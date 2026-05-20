/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * @file device.c
 * @brief Sysfs-based device discovery and initialization for SLASH FPGA devices.
 *
 * This module implements the device lifecycle for the vrtd daemon. It discovers
 * AMD Alveo V80 (SLASH) devices by globbing /dev/slash_ctl* character device
 * nodes exposed by the kernel driver, then opens each device by:
 *
 *   1. Opening the control device via libslash (slash_ctldev_open).
 *   2. Locating the matching QDMA control device by PCI BDF (bus:device prefix)
 *      via sysfs enumeration under /sys/class/misc/.
 *   3. Probing all six PCI BARs and memory-mapping the usable ones.
 *   4. Initializing subsystem drivers: clock driver, design writer, memory map.
 *
 * Teardown (cleanup_device) releases resources in reverse order to ensure
 * dependent subsystems are torn down before the underlying control device.
 */

#define _GNU_SOURCE

#include "device.h"
#include "allocator.h"
#include "clock.h"
#include "design_writer.h"
#include "hotplug.h"
#include "utils.h"

#include <assert.h>
#include <errno.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <syslog.h>
#include <systemd/sd-journal.h>

static int devices_open(struct device_ptr_array *devices, size_t pathc, char ** paths);
static int device_open(struct device **d, const char *path);
static bool devices_contains_path(const struct device_ptr_array *devices, const char *path);
static int device_read_pci_info(struct device *d, struct vrtd_pci_info *out);

/**
 * Find the /dev/ path of the qdma_ctl device sharing the same PCI bus:device
 * as the given BDF. Returns 0 on success (path written to out_path), -1 on
 * failure or no match (out_path set to NULL).
 *
 * The lookup works by:
 *   1. Extracting the bus:device prefix from ctl_bdf (e.g., "0000:65:00" from
 *      "0000:65:00.1").
 *   2. Globbing /sys/class/misc/slash_qdma_ctl_<prefix>.* to find any QDMA
 *      misc device nodes registered by the kernel driver on the same PCI slot.
 *   3. Reading the uevent file under the matched sysfs entry to extract the
 *      DEVNAME, and prepending "/dev/" to form the full device path.
 *
 * @param ctl_bdf   PCI BDF string of the control device (e.g., "0000:65:00.1").
 * @param out_path  On success, receives a heap-allocated string with the /dev/
 *                  path of the QDMA device. Set to NULL if no match is found
 *                  (which is not an error). Caller must free.
 * @return 0 on success (match found or no match), -1 on I/O or allocation error.
 */
static int find_qdma_dev_path_by_bdf(const char *ctl_bdf, char **out_path)
{
    *out_path = NULL;

    char prefix[VRTD_PCI_BDF_LEN];
    if (pci_bdf_prefix(ctl_bdf, prefix) != 0) {
        return -1;
    }

    /* Build a glob pattern to match any QDMA misc device on the same PCI slot. */
    _cleanup_(cleanup_free)
    char *pattern = NULL;
    if (asprintf(&pattern, "/sys/class/misc/slash_qdma_ctl_%s.*", prefix) < 0) {
        return -1;
    }

    _cleanup_(globfree)
    glob_t g = {0};
    int ret = glob(pattern, GLOB_ERR, NULL, &g);
    if (ret != 0) {
        return (ret == GLOB_NOMATCH) ? 0 : -1;
    }

    if (g.gl_pathc > 1) {
        LOG(
            LOG_WARNING,
            "Multiple QDMA devices found for BDF prefix %s; using first match",
            prefix
        );
    }

    const char *entry = g.gl_pathv[0];

    /* Read the uevent file to extract the kernel-assigned DEVNAME. */
    _cleanup_(cleanup_free)
    char *uevent_path = NULL;
    if (asprintf(&uevent_path, "%s/uevent", entry) < 0) {
        return -1;
    }

    FILE *f = fopen(uevent_path, "r");
    if (f == NULL) {
        return -1;
    }

    /* Parse uevent line-by-line looking for DEVNAME=<name>. */
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        static const char devname_key[] = "DEVNAME=";
        if (strncmp(line, devname_key, sizeof(devname_key) - 1) != 0) {
            continue;
        }
        const char *devname = line + sizeof(devname_key) - 1;
        size_t len = strlen(devname);
        /* Strip trailing newline/carriage return characters. */
        while (len > 0 && (devname[len - 1] == '\n' || devname[len - 1] == '\r')) {
            len--;
        }
        if (asprintf(out_path, "/dev/%.*s", (int)len, devname) < 0) {
            *out_path = NULL;
            fclose(f);
            return -1;
        }
        fclose(f);
        return 0;
    }
    fclose(f);

    return 0;
}

/**
 * Find the /dev/ path of a slash_ctl device by its full PCI BDF string.
 *
 * The slash_ctl misc device is registered by the kernel driver under a
 * stable sysfs name derived from the full PCI BDF (including function number),
 * e.g. /sys/class/misc/slash_ctl_0000:61:00.2.  The corresponding /dev/ node
 * uses an incrementing counter (slash_ctlN) that changes after each hotplug
 * remove+rescan cycle.  This function resolves the current /dev/ path by
 * reading the DEVNAME entry from the stable sysfs uevent file.
 *
 * @param bdf       Full PCI BDF string including function (e.g. "0000:61:00.2").
 * @param out_path  On success, receives a heap-allocated string with the /dev/
 *                  path of the slash_ctl device.  Set to NULL if the device is
 *                  not yet registered (which is not an error).  Caller must free.
 * @return 0 on success (device found or not yet present), -1 on I/O or
 *         allocation error.
 */
int find_slash_ctl_dev_path_by_bdf(const char *bdf, char **out_path)
{
    *out_path = NULL;

    _cleanup_(cleanup_free)
    char *uevent_path = NULL;
    if (asprintf(&uevent_path, "/sys/class/misc/slash_ctl_%s/uevent", bdf) < 0) {
        return -1;
    }

    FILE *f = fopen(uevent_path, "r");
    if (f == NULL) {
        /* Device not yet registered in sysfs — treat as no-match, not an error. */
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        static const char devname_key[] = "DEVNAME=";
        if (strncmp(line, devname_key, sizeof(devname_key) - 1) != 0) {
            continue;
        }
        const char *devname = line + sizeof(devname_key) - 1;
        size_t len = strlen(devname);
        while (len > 0 && (devname[len - 1] == '\n' || devname[len - 1] == '\r')) {
            len--;
        }
        if (asprintf(out_path, "/dev/%.*s", (int)len, devname) < 0) {
            *out_path = NULL;
            fclose(f);
            return -1;
        }
        fclose(f);
        return 0;
    }
    fclose(f);
    return 0;
}

/**
 * Discover all SLASH control devices and open them.
 *
 * Enumerates device nodes by globbing /dev/slash_ctl* and opens each one
 * that is not already present in the @p devices array. New devices are
 * appended to the array.
 *
 * @param devices  Array of already-opened device pointers; newly discovered
 *                 devices are appended here. May be empty on first call.
 * @return 0 on success (including the case where no devices are found),
 *         -1 on glob or device-open error.
 */
int devices_discover_and_open(struct device_ptr_array *devices)
{
    _cleanup_(globfree)
    glob_t g = {0};

    int ret = glob("/dev/slash_ctl*", GLOB_ERR, NULL, &g);

    if (ret == GLOB_NOMATCH) {
        LOG(
            LOG_WARNING,
            "No devices found matching /dev/slash_ctl*"
        );
        return 0; // not an error: just no devices
    }

    if (ret != 0) {
        LOG(
            LOG_ERR,
            "Error matching pattern /dev/slash_ctl*: %s",
            glob_err_to_string(ret)
        );

        return -1;
    }

    LOG(LOG_INFO, "Discovered %zu device(s) matching /dev/slash_ctl*", g.gl_pathc);

    return devices_open(devices, g.gl_pathc, g.gl_pathv);
}

/**
 * Open a set of devices given their /dev/ paths.
 *
 * Iterates over @p paths, skipping any that are already present in
 * @p devices (idempotent re-discovery). Each new device is opened via
 * device_open() and appended to the array with ownership transfer.
 *
 * @param devices  Owning array of device pointers.
 * @param pathc    Number of paths in @p paths.
 * @param paths    Array of /dev/ path strings.
 * @return 0 on success, -1 on error (logged).
 */
static int devices_open(struct device_ptr_array *devices, size_t pathc, char **paths)
{
    for (size_t i = 0; i < pathc; ++i) {
        const char *path = paths[i];

        if (devices_contains_path(devices, path)) {
            continue; // already opened, skip
        }

        _cleanup_(cleanup_devicep)
        struct device *d = NULL;

        int ret = device_open(&d, path);
        PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to open device %s", path);

        ret = device_ptr_array_push_move(devices, &d);
        PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to allocate memory for device data");
    }

    return 0;
}

/**
 * Open a single SLASH device and initialize all its subsystems.
 *
 * The initialization sequence is:
 *   1. Allocate the device struct and duplicate the path string.
 *   2. Open the libslash control device (slash_ctldev_open) for ioctl access.
 *   3. Initialize the buffer tracking array.
 *   4. Create the device memory map (for BAR-based address translation).
 *   5. Create the clock driver (Xilinx clock wizard access via BAR4).
 *   6. Read PCI info (BDF, vendor/device IDs) via ioctl, then locate and open
 *      the matching QDMA device by PCI BDF prefix.
 *   7. If QDMA is available, create the design writer (bitstream programming).
 *   8. Probe all six PCI BARs: read bar_info and mmap usable BARs.
 *
 * On success, ownership of the device is transferred to *out.
 * On failure, all partially-initialized resources are cleaned up automatically
 * via the _cleanup_ attribute on the local device pointer.
 *
 * @param out   Receives the fully initialized device pointer. Must not be NULL.
 * @param path  /dev/ path of the control device (e.g., "/dev/slash_ctl0").
 * @return 0 on success, -1 on error (logged).
 */
static int device_open(struct device **out, const char *path)
{
    PROPAGATE_ERROR_NULL_LOG(out, LOG_ERR, "Internal error: bad call of device_open: null out");
    PROPAGATE_ERROR_NULL_LOG(path, LOG_ERR, "Internal error: bad call of device_open: null path");

    _cleanup_(cleanup_devicep)
    struct device *d = calloc(1, sizeof *d);
    PROPAGATE_ERROR_NULL_STDC_LOG(d, LOG_ERR, "Failed to allocate memory for device data");

    d->path = strdup(path);
    PROPAGATE_ERROR_NULL_STDC_LOG(d->path, LOG_ERR, "Failed to allocate memory for device data");

    /* Step 1: Open the libslash control device for ioctl-based communication. */
    d->ctl = slash_ctldev_open(path);
    PROPAGATE_ERROR_NULL_STDC_LOG(d->ctl, LOG_ERR, "Error opening device %s", path);

    assert(d->ctl != NULL);

    /* Step 2: Initialize tracking structures and subsystem drivers. */
    d->buffers = buffer_ptr_array_init();

    d->memory_map = device_memory_map_create();
    PROPAGATE_ERROR_NULL_STDC_LOG(d->memory_map, LOG_ERR, "Error creating device memory map for %s", path);

    d->clock_driver = clock_driver_create(d->ctl);
    PROPAGATE_ERROR_NULL_STDC_LOG(d->clock_driver, LOG_ERR, "Error creating clock driver for %s", path);

    /* Step 3: Match the QDMA ctl device by PCI BDF (bus:device prefix). */
    {
        struct vrtd_pci_info pci_info = {0};
        int pci_ret = device_read_pci_info(d, &pci_info);
        if (pci_ret != 0) {
            LOG(
                LOG_WARNING,
                "Could not read PCI info for %s; skipping QDMA lookup",
                d->path
            );
        } else {
            _cleanup_(cleanup_free)
            char *qdma_path = NULL;
            int find_ret = find_qdma_dev_path_by_bdf(pci_info.bdf, &qdma_path);
            if (find_ret != 0) {
                LOG(
                    LOG_WARNING,
                    "Error searching for QDMA device for BDF %s (%s)",
                    pci_info.bdf, d->path
                );
            } else if (qdma_path != NULL) {
                d->qdma = slash_qdma_open(qdma_path);
                if (d->qdma == NULL) {
                    LOG(
                        LOG_WARNING,
                        "Error opening QDMA device %s (for %s): %m",
                        qdma_path, d->path
                    );
                } else {
                    LOG(
                        LOG_INFO,
                        "Matched QDMA device %s for ctldev %s (BDF %s)",
                        qdma_path, d->path, pci_info.bdf
                    );
                    /* QDMA available -- create the design writer for bitstream programming. */
                    d->design_writer = design_writer_create(d->qdma);
                    PROPAGATE_ERROR_NULL_STDC_LOG(d->design_writer, LOG_ERR, "Error creating design writer for %s", d->path);
                }
            } else {
                LOG(
                    LOG_WARNING,
                    "No QDMA device found for BDF %s (%s)",
                    pci_info.bdf, d->path
                );
            }
        }
    }

    /* Step 4: Probe all PCI BARs (0-5). Read metadata and mmap usable ones. */
    for (size_t i = 0; i < SIZEOF_ARRAY(d->bar_info); i++) {
        d->bar_info[i] = slash_bar_info_read(d->ctl, i);
        if (d->bar_info[i] == NULL) {
            LOG(
                LOG_ERR,
                "Error opening bar_info %zu on device %s: %m",
                i, d->path
            );
            continue;
        }

        assert(d->bar_info[i] != NULL);

        /* Only open (mmap) BARs that the kernel driver marks as usable. */
        if (d->bar_info[i]->usable) {
            d->bar_files[i] = slash_bar_file_open(d->ctl, i, O_CLOEXEC);
            if (d->bar_files[i] == NULL) {
                LOG(
                    LOG_ERR,
                    "Error opening bar_file %zu on device %s: %m",
                    i, d->path
                );
            }
        }
    }

    /* Transfer ownership to caller; set local to NULL to prevent cleanup. */
    *out = d;
    d = NULL;

    return 0;
}

/**
 * Read PCI identification info from the kernel driver via ioctl.
 *
 * Populates @p out with the device's BDF string, vendor/device IDs, and
 * subsystem vendor/device IDs. Also caches the info in d->pci_info.
 *
 * @param d    Device with an open control device (d->ctl).
 * @param out  Receives the PCI info. Zeroed before filling.
 * @return 0 on success, -1 on error (logged).
 */
static int device_read_pci_info(struct device *d, struct vrtd_pci_info *out)
{
    PROPAGATE_ERROR_NULL_LOG(d, LOG_ERR, "Internal error: bad call of device_read_pci_info: null device");
    PROPAGATE_ERROR_NULL_LOG(d->ctl, LOG_ERR, "Internal error: bad call of device_read_pci_info: null device->ctl");

    memset(out, 0, sizeof(*out));

    struct slash_ioctl_device_info info = {0};
    info.size = sizeof(info);

    int ret = ioctl(d->ctl->fd, SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO, &info);
    PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Could not get bar info for device: %s", d->path);

    // Copy pci information
    {
        (void) strncpy(out->bdf, info.bdf, VRTD_PCI_BDF_LEN);

        /* Strip the function digit (.F) from the kernel's BDF string.
         * vrtd identifies devices at the board level (DDDD:BB:DD), not
         * at the individual physical function level.  PF-specific BDFs
         * are constructed on the fly when needed (e.g. for hotplug ioctls)
         * using pci_bdf_set_function(). */
        {
            char *dot = strrchr(out->bdf, '.');
            if (dot != NULL) {
                LOG(LOG_INFO,
                    "Stripping PF function %s from kernel BDF %s; "
                    "device tracked as board %.*s",
                    dot, info.bdf, (int)(dot - out->bdf), out->bdf);
                *dot = '\0';
            }
        }

        out->vendor_id = info.vendor_id;
        out->device_id = info.device_id;
        out->subsystem_vendor_id = info.subsystem_vendor_id;
        out->subsystem_device_id = info.subsystem_device_id;

        d->pci_info = *out;
    }

    return 0;
}

/**
 * Check whether a device with the given /dev/ path is already in the array.
 *
 * Used during re-discovery to avoid opening the same device twice.
 *
 * @param devices  Array of opened devices.
 * @param path     /dev/ path to search for.
 * @return true if a device with a matching path exists, false otherwise.
 */
static bool devices_contains_path(const struct device_ptr_array *devices, const char *path)
{
    if (!devices || !path) return false;

    for (size_t i = 0; i < devices->len; ++i) {
        const struct device *d = devices->d[i];
        if (d && d->path && strcmp(d->path, path) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Release all resources held by a device, in reverse initialization order.
 *
 * Teardown sequence:
 *   1. Free DMA buffers.
 *   2. Destroy the design writer (QDMA bitstream programming).
 *   3. Destroy the device memory map.
 *   4. Destroy the clock driver (BAR4 clock wizard access).
 *   5. Close the QDMA device.
 *   6. Close all opened BAR file mappings.
 *   7. Free BAR info metadata.
 *   8. Close the libslash control device.
 *   9. Free the path string and the device struct itself.
 *
 * Safe to call with NULL (no-op). Errors during close are logged as
 * warnings but do not prevent further cleanup.
 *
 * @param d  Device to clean up, or NULL.
 */
void cleanup_device(struct device *d)
{
    if (d == NULL) {
        return;
    }

    LOG(LOG_DEBUG, "Cleaning up device %s", d->path ? d->path : "(unknown)");

    buffer_ptr_array_free(&d->buffers);

    if (d->design_writer != NULL) {
        cleanup_design_writer(d->design_writer);
        d->design_writer = NULL;
    }

    if (d->memory_map != NULL) {
        device_memory_map_cleanup(d->memory_map);
        d->memory_map = NULL;
    }

    if (d->clock_driver != NULL) {
        cleanup_clock_driver(d->clock_driver);
        d->clock_driver = NULL;
    }

    if (d->qdma != NULL) {
        if (slash_qdma_close(d->qdma) != 0) {
            LOG(
                LOG_WARNING,
                "Error closing qdma device for %s: %m (ignored)",
                d->path ? d->path : "(unknown)"
            );
        }
        d->qdma = NULL;
    }

    /* Close any opened BAR files */
    for (size_t i = 0; i < SIZEOF_ARRAY(d->bar_files); i++) {
        if (d->bar_files[i] != NULL) {
            if (slash_bar_file_close(d->bar_files[i]) != 0) {
                LOG(
                    LOG_WARNING,
                    "Error closing bar_file %zu for %s: %m (ignored)",
                    i, d->path ? d->path : "(unknown)"
                );
            }
            d->bar_files[i] = NULL;
        }
    }

    /* Free bar info data */
    for (size_t i = 0; i < SIZEOF_ARRAY(d->bar_info); i++) {
        if (d->bar_info[i] != NULL) {
            slash_bar_info_free(d->bar_info[i]);
            d->bar_info[i] = NULL;
        }
    }

    /* Close control device last */
    if (d->ctl != NULL) {
        if (slash_ctldev_close(d->ctl) != 0) {
            LOG(
                LOG_WARNING,
                "Error closing ctldevice %s: %m (ignored)",
                d->path ? d->path : "(unknown)"
            );
        }
        d->ctl = NULL;
    }

    free(d->path);
    d->path = NULL;

    free(d);
}

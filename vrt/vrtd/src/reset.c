/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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
 * @file reset.c
 * @brief Device reset sequence for AMD Alveo V80 using AMI + PCIe Secondary Bus Reset.
 *
 * This module implements the full reset-and-reconfiguration sequence for a
 * SLASH FPGA device.  The sequence combines two mechanisms:
 *
 *   1. AMI (Alveo Management Interface) -- a firmware-level management
 *      interface exposed through the AVED (Alveo Versal Example Design)
 *      driver on PF0.  AMI provides ioctls for device management operations
 *      such as programming boot partitions and triggering firmware-level
 *      reconfiguration.
 *      TODO(vserbu): explain AMI protocol details
 *
 *   2. PCIe hotplug via the SLASH kernel module -- after the firmware has been
 *      told to reconfigure, the PCIe device must be removed from the bus,
 *      a Secondary Bus Reset (SBR) must be toggled on the upstream bridge,
 *      and the bus must be rescanned so the newly-configured device is
 *      re-enumerated by the kernel.
 *
 * Multi-PF handling
 * -----------------
 * The Alveo V80 exposes three PCIe Physical Functions (PFs) under the same
 * bus:device address:
 *
 *   - PF0: AVED/AMI management function (used for firmware ioctls)
 *   - PF1: QDMA function (used for DMA data transfers)
 *   - PF2: Additional function
 *   TODO(vserbu): clarify PF2 role (CMC? user PF?)
 *
 * Before performing a Secondary Bus Reset, ALL three PFs must be removed from
 * the Linux PCI subsystem.  If any PF is left attached while the SBR is
 * toggled, the kernel may attempt to access a device whose configuration
 * space is no longer valid, leading to machine checks or hangs.  After the
 * SBR and a settling delay, a PCI bus rescan brings all three functions back.
 *
 * Reset sequence (step by step)
 * -----------------------------
 *   1. Compute BDF strings for PF0, PF1, PF2 from the device's BDF.
 *   2. Remove the device from vrtd's tracked device list (it is about to
 *      disappear from the bus).
 *   3. Open the AMI device on PF0, request access.
 *   4. Issue AMI_IOC_DEVICE_BOOT ioctl to tell the AMC firmware which
 *      partition to boot from on the next reset.
 *   5. Write a trigger value to BAR0 offset 0x1040000 to initiate the
 *      firmware-level reconfiguration.
 *      TODO(vserbu): explain what BAR0 register 0x1040000 controls in AMI
 *   6. Close the AMI device handle.
 *   7. Remove PF0, PF1, PF2 from the PCI bus via slash_hotplug_remove().
 *      ENODEV is tolerated (device may already have been removed by firmware).
 *   8. Toggle Secondary Bus Reset on the upstream PCIe bridge via
 *      slash_hotplug_toggle_sbr().
 *   9. Wait 5 seconds for the device to complete reconfiguration and
 *      re-train the PCIe link.
 *  10. Poll until the device is usable again, rescanning the PCI bus as we go:
 *      PF0 must answer ami_dev_find(), device discovery must re-add the device
 *      with all its resources open, and the build-ID register must report the
 *      shell the selected boot partition should have loaded.  See
 *      reset_wait_for_device().
 *  11. Record that shell on the re-discovered device.
 */

#define _GNU_SOURCE

#include "reset.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <ami.h>
#include <ami_device.h>
#include <ami_device_internal.h>
#include <ami_ioctl.h>
#include <ami_mem_access.h>
#include <vrtd/wire.h>

#include "device.h"
#include "hotplug.h"
#include "shell_build_id.h"
#include "utils.h"

#define GPIO_ALLOW_SBR 0x1040000

static void reset_emit_progress(
    cfgmem_progress_callback progress_cb,
    void *progress_ctx,
    uint32_t phase
)
{
    if (progress_cb != NULL) {
        progress_cb(progress_ctx, phase, 0, 0);
    }
}

int shell_boot_partition(enum vrtd_shell_type shell, uint32_t *partition_out)
{
    PROPAGATE_ERROR_NULL_LOG(partition_out, LOG_ERR, "Internal error: null partition_out");

    switch (shell) {
    case VRTD_SHELL_SERVICE:
        *partition_out = 0;
        return 0;
    case VRTD_SHELL_COMPUTE:
        *partition_out = 1;
        return 0;
    default:
        return -1;
    }
}

/* Inverse of shell_boot_partition(): the shell a boot partition maps to. */
static enum vrtd_shell_type shell_from_boot_partition(uint32_t partition)
{
    switch (partition) {
    case 0:
        return VRTD_SHELL_SERVICE;
    case 1:
        return VRTD_SHELL_COMPUTE;
    default:
        return VRTD_SHELL_UNKNOWN;
    }
}

bool shell_reset_required(enum vrtd_shell_type current_shell, enum vrtd_shell_type required_shell)
{
    return current_shell == VRTD_SHELL_UNKNOWN || current_shell != required_shell;
}

bool shell_switch_blocked_by_jtag(
    enum vrtd_shell_type current_shell,
    enum vrtd_shell_type required_shell,
    bool jtag
)
{
    return jtag && shell_reset_required(current_shell, required_shell);
}

enum reset_ready_state reset_ready_classify(
    bool device_present,
    bool fully_initialized,
    enum vrtd_shell_type reported_shell,
    enum vrtd_shell_type booted_shell
)
{
    if (!device_present || !fully_initialized) {
        return RESET_READY_WAIT;
    }

    if (reported_shell == VRTD_SHELL_UNKNOWN) {
        return RESET_READY_WAIT;
    }

    if (reported_shell != booted_shell) {
        return RESET_READY_SHELL_MISMATCH;
    }

    return RESET_READY_OK;
}

/* Monotonic microseconds: deadline arithmetic must not follow wall-clock steps. */
static uint64_t monotonic_us(void)
{
    struct timespec ts = {0};

    (void) clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
}

/* Locate a tracked device by BDF, or NULL when it is not (yet) present. */
static struct device *reset_find_device_by_bdf(
    struct device_ptr_array *devices,
    const char *bdf
)
{
    for (size_t i = 0; i < devices->len; i++) {
        struct device *d = devices->d[i];

        if (d != NULL && strcmp(d->pci_info.bdf, bdf) == 0) {
            return d;
        }
    }

    return NULL;
}

/*
 * Budget for the readiness poll below.  The total (5 s settle + 60 s polling)
 * is deliberately just under the ~67 s the previous fixed-sleep sequence could
 * spend, so no host that completes a reset today can start timing out.
 */
#define RESET_READY_TIMEOUT_US   60000000
#define RESET_READY_POLL_US        250000
#define RESET_RESCAN_INTERVAL_US  3000000

/**
 * Wait for a device to come back after SBR, and confirm which shell it booted.
 *
 * Coming back is not one event but several, each landing at its own moment:
 * the kernel must re-enumerate the PFs, hand them to the slash and ami drivers,
 * let those drivers create device nodes, and let udev set permissions on them;
 * only then can the BARs be mapped and the build-ID register read.  How long
 * that takes varies with the host -- device count, udev load, how big the
 * partition being loaded is.
 *
 * The previous implementation slept a fixed 10 s and then tested each of those
 * conditions exactly once, so a host slower than the guess failed a reset that
 * would have succeeded moments later.  That is the shared cause behind issues
 * #222 and #227: three separate single-shot gates, each returning
 * VRTD_RET_INTERNAL_ERROR, which users see as "Internal error in vrtd daemon or
 * local libvrtd".
 *
 * So poll the whole conjunction against a deadline instead.  A healthy host
 * converges in well under a second rather than paying the 10 s sleep; a slow
 * one keeps getting retried until the deadline.  A device reporting the wrong
 * shell still fails immediately -- see reset_ready_classify().
 *
 * @param devices      Tracked device array; the target is re-added here.
 * @param pf0_bdf      PF0 address, used to ask AMI whether the card is back.
 * @param target_bdf   Address of the device being reset.
 * @param booted_shell Shell the selected boot partition should have loaded.
 * @param progress_cb  Progress sink, kept fed so the v80-smi UI still advances.
 * @param progress_ctx Opaque context for @p progress_cb.
 * @param[out] out     Receives the re-discovered device on success.
 * @return VRTD_RET_OK, or a VRTD_RET_* error code.
 */
static uint16_t reset_wait_for_device(
    struct device_ptr_array *devices,
    const char *pf0_bdf,
    const char *target_bdf,
    enum vrtd_shell_type booted_shell,
    cfgmem_progress_callback progress_cb,
    void *progress_ctx,
    struct device **out
)
{
    const uint64_t start = monotonic_us();
    const uint64_t deadline = start + RESET_READY_TIMEOUT_US;

    uint64_t last_rescan_us = 0;
    bool rescanned = false;

    /* Remembered so a timeout can name the gate that never opened. */
    bool ami_seen = false;
    bool node_seen = false;
    bool device_completed = false;

    for (;;) {
        uint64_t now = monotonic_us();

        if (!rescanned || now - last_rescan_us >= RESET_RESCAN_INTERVAL_US) {
            reset_emit_progress(
                progress_cb,
                progress_ctx,
                VRTD_CFGMEM_PROGRAM_PHASE_RESCANNING_PCIE
            );

            if (slash_hotplug_rescan(g_hotplug) != 0) {
                LOG(LOG_ERR, "reset_with_ami: hotplug rescan failed: %m");
                return hotplug_errno_to_vrtd_ret(errno);
            }

            last_rescan_us = now;
            rescanned = true;
        }

        /*
         * Ask AMI whether PF0 is back.  The handle is released on every pass,
         * including failed ones: this loop runs far more often than the old
         * five-attempt version, so anything it retains would accumulate.
         */
        struct ami_device *ami_device = NULL;
        int find_ret = ami_dev_find(pf0_bdf, &ami_device);
        if (ami_device != NULL) {
            ami_dev_delete(&ami_device);
        }

        if (find_ret == AMI_STATUS_OK) {
            ami_seen = true;

            reset_emit_progress(
                progress_cb,
                progress_ctx,
                VRTD_CFGMEM_PROGRAM_PHASE_REDISCOVERING_DEVICE
            );

            if (devices_discover_and_open(devices) != 0) {
                LOG(LOG_WARNING, "reset_with_ami: device discovery failed, will retry");
            }

            struct device *d = reset_find_device_by_bdf(devices, target_bdf);
            const bool present = d != NULL;
            const bool complete = device_is_fully_initialized(d);

            node_seen = node_seen || present;
            device_completed = device_completed || complete;

            /*
             * A device opened before the kernel had finished bringing it up
             * keeps whatever happened to be ready at the time -- device_open()
             * only warns about a BAR it cannot map or a missing QDMA node --
             * and every later discovery pass then skips it as already present.
             * Drop it so the next pass opens it again with everything in place.
             */
            if (present && !complete) {
                device_ptr_array_rm_by_reference(devices, d);
                d = NULL;
            }

            const enum vrtd_shell_type reported = complete
                ? build_id_read_shell(d->bar_files[BUILD_ID_BAR_NUMBER])
                : VRTD_SHELL_UNKNOWN;

            switch (reset_ready_classify(present, complete, reported, booted_shell)) {
            case RESET_READY_OK:
                LOG(
                    LOG_INFO,
                    "reset_with_ami: device %s ready %u ms after SBR settle, "
                    "running the %s shell",
                    target_bdf,
                    (unsigned int)((monotonic_us() - start) / 1000u),
                    build_id_shell_name(booted_shell)
                );
                *out = d;
                return VRTD_RET_OK;

            case RESET_READY_SHELL_MISMATCH:
                LOG(
                    LOG_ERR,
                    "reset_with_ami: hardware reports the %s shell but booting the "
                    "partition for the %s shell was requested",
                    build_id_shell_name(reported),
                    build_id_shell_name(booted_shell)
                );
                return VRTD_RET_INTERNAL_ERROR;

            case RESET_READY_WAIT:
                break;
            }
        }

        if (monotonic_us() >= deadline) {
            break;
        }

        usleep(RESET_READY_POLL_US);
    }

    /*
     * Name the gate that never opened.  A single "internal error" for four
     * distinct causes is what made issues #222 and #227 expensive to diagnose.
     */
    if (!ami_seen) {
        LOG(
            LOG_ERR,
            "reset_with_ami: PF0 %s did not reappear within %u s of the reset: %s",
            pf0_bdf,
            (unsigned int)(RESET_READY_TIMEOUT_US / 1000000u),
            ami_get_last_error()
        );
    } else if (!node_seen) {
        LOG(
            LOG_ERR,
            "reset_with_ami: PF0 %s came back but no device node for %s appeared "
            "within %u s",
            pf0_bdf,
            target_bdf,
            (unsigned int)(RESET_READY_TIMEOUT_US / 1000000u)
        );
    } else if (!device_completed) {
        LOG(
            LOG_ERR,
            "reset_with_ami: device %s reappeared but never finished initialising "
            "within %u s (build-ID BAR%d, QDMA or design writer never became available)",
            target_bdf,
            (unsigned int)(RESET_READY_TIMEOUT_US / 1000000u),
            BUILD_ID_BAR_NUMBER
        );
    } else {
        LOG(
            LOG_ERR,
            "reset_with_ami: device %s opened but its build-ID register at BAR%d+0x%x "
            "never reported the %s shell within %u s",
            target_bdf,
            BUILD_ID_BAR_NUMBER,
            BUILD_ID_REG_HI,
            build_id_shell_name(booted_shell),
            (unsigned int)(RESET_READY_TIMEOUT_US / 1000000u)
        );
    }

    return VRTD_RET_INTERNAL_ERROR;
}

/**
 * Perform a full device reset using AMI firmware commands and PCIe hotplug.
 *
 * This function executes the complete reset sequence described in the file
 * header.  It takes ownership of @device (removes it from @devices) because
 * the device will be physically removed from the PCI bus during the reset.
 * After the reset and rescan, devices_discover_and_open() re-populates the
 * device list with the newly-enumerated device.
 *
 * @param device   The device to reset.  The caller must not use this pointer
 *                 after the call, as the device is removed from the tracked
 *                 list and freed.
 * @param devices  The global array of tracked device pointers.  The target
 *                 device is removed at the start; after a successful reset,
 *                 the newly-discovered device is added back.
 * @return VRTD_RET_OK on success, or a VRTD_RET_* error code on failure.
 */
uint16_t reset_with_ami_partition_progress(
    struct device *device,
    struct device_ptr_array *devices,
    uint32_t partition,
    cfgmem_progress_callback progress_cb,
    void *progress_ctx
)
{
    /*
     * Step 1: Compute BDF (Bus:Device.Function) strings for all three PFs.
     * All PFs share the same bus:device but have different function numbers.
     */
    char pf0_bdf[VRTD_PCI_BDF_LEN] = {0};
    char pf1_bdf[VRTD_PCI_BDF_LEN] = {0};
    char pf2_bdf[VRTD_PCI_BDF_LEN] = {0};

    struct ami_device *ami_device = NULL;

    /* Saved now because @device is freed once it is removed below; used after
     * rediscovery to locate the re-enumerated device and record its shell. */
    char target_bdf[VRTD_PCI_BDF_LEN] = {0};
    strncpy(target_bdf, device->pci_info.bdf, sizeof(target_bdf) - 1);

    int ret = pci_bdf_set_function(device->pci_info.bdf, 0, pf0_bdf);
    if (ret != 0) {
        LOG(LOG_ERR, "reset_with_ami: failed to compute PF0 BDF from %s", device->pci_info.bdf);
        return VRTD_RET_INTERNAL_ERROR;
    }
    ret = pci_bdf_set_function(device->pci_info.bdf, 1, pf1_bdf);
    if (ret != 0) {
        LOG(LOG_ERR, "reset_with_ami: failed to compute PF1 BDF from %s", device->pci_info.bdf);
        return VRTD_RET_INTERNAL_ERROR;
    }
    ret = pci_bdf_set_function(device->pci_info.bdf, 2, pf2_bdf);
    if (ret != 0) {
        LOG(LOG_ERR, "reset_with_ami: failed to compute PF2 BDF from %s", device->pci_info.bdf);
        return VRTD_RET_INTERNAL_ERROR;
    }

    /*
     * Step 2: Remove the device from vrtd's tracked device list.
     * The device is about to be reset and will disappear from the PCI bus,
     * so we must stop tracking it before proceeding.  After this point,
     * the @device pointer is invalid and must not be dereferenced.
     */
    reset_emit_progress(
        progress_cb,
        progress_ctx,
        VRTD_CFGMEM_PROGRAM_PHASE_RESET_PREPARING
    );

    // We are now removing this device.
    device_ptr_array_rm_by_reference(devices, device);
    device = NULL;

    /*
     * Step 3: Open the AMI management device on PF0 and request access.
     * AMI (Alveo Management Interface) runs on PF0 (the AVED function).
     * ami_dev_find() locates the AMI character device by PCI BDF, and
     * ami_dev_request_access() acquires exclusive access for management
     * operations.
     * TODO(vserbu): explain AMI access model -- is this a lock? exclusive open? capability grant?
     */
    // PF0 is AVED/AMI bdf
    ret = ami_dev_find(pf0_bdf, &ami_device);
    if (ret != AMI_STATUS_OK) {
        LOG(LOG_ERR, "reset_with_ami: ami_dev_find(%s) failed: %s", pf0_bdf, ami_get_last_error());
        return VRTD_RET_INTERNAL_ERROR;
    }

    ret = ami_dev_request_access(ami_device);
    if (ret != AMI_STATUS_OK) {
        LOG(LOG_ERR, "reset_with_ami: ami_dev_request_access(%s) failed: %s", pf0_bdf, ami_get_last_error());
        ami_dev_delete(&ami_device);
        return VRTD_RET_INTERNAL_ERROR;
    }

    /*
     * Step 4: Issue AMI_IOC_DEVICE_BOOT ioctl to select the boot partition.
     *
     * We issue AMI_IOC_DEVICE_BOOT directly rather than calling
     * ami_prog_device_boot(), even though the latter is the intended public
     * API for this operation.  The reason is that ami_prog_device_boot()
     * unconditionally calls ami_dev_hot_reset() after the ioctl succeeds.
     * ami_dev_hot_reset() performs its own full remove-device / toggle-SBR /
     * rescan cycle by opening the PCIe bridge config-space sysfs file
     * (/sys/bus/pci/devices/<port>/config) with O_RDWR.  That file is mode
     * 0600 and owned by root; vrtd runs as the unprivileged 'vrtd' user, so
     * the open always fails with EBADF regardless of any Linux capabilities
     * granted to the process.
     *
     * More fundamentally, even if the open succeeded, ami_dev_hot_reset would
     * conflict with vrtd's own hotplug reset sequence that follows immediately
     * below.  vrtd drives hotplug through the slash kernel module
     * (slash_hotplug_remove / slash_hotplug_toggle_sbr / slash_hotplug_rescan),
     * which is the authoritative hotplug path for SLASH devices.  Letting both
     * ami_dev_hot_reset and the slash hotplug sequence run would reset the
     * device twice and leave the AMI device handle in an inconsistent state.
     *
     * The correct behaviour is to issue only the AMI_IOC_DEVICE_BOOT ioctl to
     * inform the AMC firmware of the desired boot partition, then hand control
     * back to vrtd to drive the full hotplug sequence itself.  We set
     * cap_override from the device handle (populated earlier by
     * ami_dev_request_access) so that the kernel driver's per-ioctl permission
     * check passes for the unprivileged vrtd user without requiring
     * CAP_DAC_OVERRIDE or root.
     */
    reset_emit_progress(
        progress_cb,
        progress_ctx,
        VRTD_CFGMEM_PROGRAM_PHASE_SELECTING_PARTITION
    );

    {
        struct ami_ioc_data_payload boot_payload = { 0 };
        boot_payload.partition = partition;
        boot_payload.cap_override = ami_device->cap_override;

        if (ami_open_cdev(ami_device) != AMI_STATUS_OK) {
            LOG(LOG_ERR, "reset_with_ami: ami_open_cdev(%s) failed: %s", pf0_bdf, ami_get_last_error());
            ami_dev_delete(&ami_device);
            return VRTD_RET_INTERNAL_ERROR;
        }

        errno = 0;
        if (ioctl(ami_device->cdev, AMI_IOC_DEVICE_BOOT, &boot_payload) != 0) {
            LOG(LOG_ERR, "reset_with_ami: AMI_IOC_DEVICE_BOOT(%s) failed: errno %d (%s)",
                pf0_bdf, errno, strerror(errno));
            ami_dev_delete(&ami_device);
            return VRTD_RET_INTERNAL_ERROR;
        }
    }
    LOG(LOG_INFO, "reset_with_ami: AMI_IOC_DEVICE_BOOT(%s, partition=%u) OK",
        pf0_bdf, (unsigned int)partition);

    /*
     * Step 5: Write a trigger value to BAR0 register at offset 0x1040000
     * to initiate the firmware-level reconfiguration.
     *
     * This is a GPIO pin in the programmed logic that forms an AND gate
     * with the PCIe SBR signal, and needs to be turned on in order to
     * perform a scondary bus reset.
     */
    ret = ami_mem_bar_write(ami_device, 0, GPIO_ALLOW_SBR, 1);
    if (ret != AMI_STATUS_OK) {
        LOG(LOG_ERR, "reset_with_ami: ami_mem_bar_write(%s) failed: %s", pf0_bdf, ami_get_last_error());
        ami_dev_delete(&ami_device);
        return VRTD_RET_INTERNAL_ERROR;
    }

    LOG(LOG_INFO, "reset_with_ami: GPIO_ALLOW_SBR set on %s", pf0_bdf);

    /* Step 6: Close the AMI device handle -- we are done with firmware commands. */
    ami_dev_delete(&ami_device);

    /*
     * Step 7: Remove ALL three PFs from the Linux PCI subsystem.
     *
     * Every PF must be removed before we toggle SBR on the upstream bridge.
     * If any function remains bound while the bus is reset, the kernel may
     * attempt MMIO or config-space accesses to a device whose link is down,
     * which can cause machine checks or system hangs.
     *
     * ENODEV is tolerated because the firmware reconfiguration triggered in
     * step 5 may have already caused the device to disappear from the bus.
     */
    if (g_hotplug == NULL) {
        LOG(LOG_ERR, "reset_with_ami: hotplug handle not available (is slash_hotplug loaded?)");
        return VRTD_RET_INTERNAL_ERROR;
    }

    reset_emit_progress(
        progress_cb,
        progress_ctx,
        VRTD_CFGMEM_PROGRAM_PHASE_REMOVING_PCIE
    );

    ret = slash_hotplug_remove(g_hotplug, pf0_bdf);
    LOG(LOG_INFO, "reset_with_ami: removed %s (ret=%d, errno=%d)", pf0_bdf, ret, errno);
    if (ret != 0 && errno != ENODEV) {
        LOG(LOG_ERR, "reset_with_ami: hotplug remove(%s) failed: %m", pf0_bdf);
        return hotplug_errno_to_vrtd_ret(errno);
    }
    ret = slash_hotplug_remove(g_hotplug, pf1_bdf);
    LOG(LOG_INFO, "reset_with_ami: removed %s (ret=%d, errno=%d)", pf1_bdf, ret, errno);
    if (ret != 0 && errno != ENODEV) {
        LOG(LOG_ERR, "reset_with_ami: hotplug remove(%s) failed: %m", pf1_bdf);
        return hotplug_errno_to_vrtd_ret(errno);
    }
    ret = slash_hotplug_remove(g_hotplug, pf2_bdf);
    LOG(LOG_INFO, "reset_with_ami: removed %s (ret=%d, errno=%d)", pf2_bdf, ret, errno);
    if (ret != 0 && errno != ENODEV) {
        LOG(LOG_ERR, "reset_with_ami: hotplug remove(%s) failed: %m", pf2_bdf);
        return hotplug_errno_to_vrtd_ret(errno);
    }

    /*
     * Step 7a: Brief settle after PF removal, before toggling SBR.
     *
     * The AMI library's ami_dev_hot_reset() inserts a 1 ms delay here.
     * Its comment notes that "on some systems, the device that is being
     * reset disappears from the host, forcing a system reboot — adding a
     * delay before setting the SBR seems to mitigate this issue."
     */
    usleep(20000);

    /*
     * Step 8: Toggle Secondary Bus Reset (SBR) on the upstream PCIe bridge.
     *
     * SBR asserts the reset signal on the secondary side of the PCIe bridge,
     * forcing all downstream devices (our FPGA) to re-initialize.  This is
     * the mechanism that causes the FPGA to load the new configuration from
     * the boot partition selected in step 4.
     */
    LOG(LOG_INFO, "reset_with_ami: toggling SBR for %s", pf0_bdf);
    reset_emit_progress(
        progress_cb,
        progress_ctx,
        VRTD_CFGMEM_PROGRAM_PHASE_TOGGLING_SBR
    );
    ret = slash_hotplug_toggle_sbr(g_hotplug, pf0_bdf);
    if (ret != 0) {
        LOG(LOG_ERR, "reset_with_ami: hotplug toggle_sbr(%s) failed: %m", pf0_bdf);
        return hotplug_errno_to_vrtd_ret(errno);
    }
    LOG(LOG_INFO, "reset_with_ami: SBR toggle complete for %s", pf0_bdf);

    /*
     * Step 9: Wait for the FPGA to complete reconfiguration and re-train
     * the PCIe link.  5 seconds is a conservative estimate that accounts for
     * bitstream loading time and link training, mentioned in a AVED sw comment.
     */
    usleep(5000000);

    /*
     * Steps 10-13: Rescan the bus and wait for the device to become usable,
     * then record the shell it booted.
     *
     * Rediscovery creates a fresh device struct with an UNKNOWN shell, so the
     * shell has to be written back here or it would be lost after every reset.
     * The boot partition states which shell was intended, and the build-ID
     * register confirms the hardware agrees: a partition that did not take
     * effect would otherwise leave vrtd asserting a shell the card is not
     * running, and every later decision keyed on the shell -- whether a reset
     * is required, which register windows exist -- would be made against the
     * wrong design.
     */
    enum vrtd_shell_type booted_shell = shell_from_boot_partition(partition);
    struct device *new_device = NULL;

    uint16_t wait_ret = reset_wait_for_device(
        devices,
        pf0_bdf,
        target_bdf,
        booted_shell,
        progress_cb,
        progress_ctx,
        &new_device
    );
    if (wait_ret != VRTD_RET_OK) {
        return wait_ret;
    }

    new_device->current_shell = booted_shell;

    return VRTD_RET_OK;
}

uint16_t reset_with_ami_partition(
    struct device *device,
    struct device_ptr_array *devices,
    uint32_t partition
)
{
    return reset_with_ami_partition_progress(device, devices, partition, NULL, NULL);
}

uint16_t reset_with_ami(
    struct device *device,
    struct device_ptr_array *devices,
    enum vrtd_shell_type target_shell
)
{
    uint32_t boot_partition = 0;
    if (shell_boot_partition(target_shell, &boot_partition) != 0) {
        LOG(LOG_ERR, "reset_with_ami: invalid target shell %u", (unsigned int)target_shell);
        return VRTD_RET_INVALID_ARGUMENT;
    }

    /* reset_with_ami_partition_progress() records the booted shell for us. */
    return reset_with_ami_partition(device, devices, boot_partition);
}

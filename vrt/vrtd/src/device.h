/**
 * The MIT License (MIT)
 * Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
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
 * @file device.h
 * @brief SLASH FPGA device representation for the vrtd daemon.
 *
 * Each physical SLASH V80 FPGA device discovered at daemon startup is
 * represented by a @c struct @c device.  The struct aggregates the libslash
 * control and QDMA handles, per-BAR metadata and mmap'd file regions,
 * and per-device subsystems (design writer, clock driver, memory allocator).
 *
 * PCI BARs are indexed 0-5, matching the PCI specification.  Not all BARs
 * may be present on every device; absent BARs have NULL entries.
 */

#ifndef VRTD_DEVICE_H
#define VRTD_DEVICE_H

#include <stddef.h>

#include <slash/ctldev.h>
#include <slash/qdma.h>

#include "array.h"
#include "buffer.h"

struct design_writer;
struct clock_driver;
struct device_memory_map;

/**
 * @brief Represents a single discovered SLASH FPGA device.
 *
 * Owns all per-device resources: libslash handles, BAR mappings, subsystem
 * drivers, DMA buffers, and the HBM/DDR memory map allocator.
 */
struct device {
    /** @brief Sysfs path of this device (e.g. "/sys/bus/pci/devices/0000:03:00.0").
     *  Heap-allocated, owning. */
    char *path; /* owning */
    /** @brief libslash control device handle for ioctl operations (owning). */
    struct slash_ctldev *ctl;
    /** @brief libslash QDMA subsystem handle for DMA queue management (owning). */
    struct slash_qdma *qdma;
    /** @brief BAR metadata (size, flags, physical address) for each of the 6 PCI BARs.
     *  NULL if the BAR is not present or not mapped. Owning pointers. */
    struct slash_ioctl_bar_info *bar_info[6];
    /** @brief Memory-mapped BAR file regions for each of the 6 PCI BARs.
     *  NULL if the BAR is not present or not mapped. Owning pointers. */
    struct slash_bar_file *bar_files[6];
    /** @brief Asynchronous FPGA bitstream writer subsystem (owning, may be NULL). */
    struct design_writer *design_writer;
    /** @brief Clock frequency control subsystem via AXI clock wizard (owning, may be NULL). */
    struct clock_driver *clock_driver;
    /** @brief HBM/DDR address-range allocator for DMA buffer placement (owning, may be NULL). */
    struct device_memory_map *memory_map;
    /** @brief Currently allocated DMA buffers on this device (owning array). */
    struct buffer_ptr_array buffers;
    /** @brief PCI identity (vendor/device ID, BDF address) reported to clients. */
    struct vrtd_pci_info pci_info;
};

/**
 * @brief Release all resources owned by a device (handles, mappings, subsystems, buffers).
 * @param d Pointer to the device to clean up.
 */
void cleanup_device(struct device *d);

/**
 * @brief Cleanup helper for use with __attribute__((cleanup)).
 * @param d Address of a @c struct @c device pointer.
 */
static inline
void cleanup_devicep(struct device **d)
{
    cleanup_device(*d);

    *d = NULL;
}

DECLARE_OWNING_PTR_ARRAY(device_ptr_array, struct device *, cleanup_device);

/**
 * @brief Discover all SLASH FPGA devices on the system and open them.
 *
 * Enumerates PCI devices via sysfs, opens libslash control and QDMA handles,
 * maps BARs, and initializes per-device subsystems (design writer, clock
 * driver, memory allocator).
 *
 * @param[out] devices Array to populate with discovered device pointers.
 *                     The array takes ownership of all allocated devices.
 * @return 0 on success, -1 on error (partially discovered devices are cleaned up).
 */
int devices_discover_and_open(struct device_ptr_array *devices);

/**
 * @brief Find the current /dev/slash_ctlN path for a PF2 device by its full BDF.
 *
 * The /dev node suffix is assigned by an incrementing kernel counter and changes
 * after each hotplug remove+rescan cycle.  This function resolves the current
 * path by reading the stable sysfs uevent file at
 * /sys/class/misc/slash_ctl_<bdf>/uevent.
 *
 * @param bdf       Full PCI BDF including function number (e.g. "0000:61:00.2").
 * @param out_path  On success, receives a heap-allocated /dev/ path string, or
 *                  NULL if the device is not yet registered.  Caller must free.
 * @return 0 on success (device found or absent), -1 on I/O or allocation error.
 */
int find_slash_ctl_dev_path_by_bdf(const char *bdf, char **out_path);

#endif // VRTD_DEVICE_H

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
 *   4. Issue AMI_IOC_DEVICE_BOOT ioctl to tell the AMC firmware to boot
 *      from partition 1 on the next reset.
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
 *  10. Rescan the PCI bus via slash_hotplug_rescan() to re-enumerate all PFs.
 *  11. Verify the device is back by calling ami_dev_find() on PF0.
 *  12. Run device discovery to re-add the reset device to vrtd's tracked
 *      device list.
 */

#define _GNU_SOURCE

#include "reset.h"

#include <errno.h>
#include <stddef.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <ami.h>
#include <ami_device.h>
#include <ami_device_internal.h>
#include <ami_ioctl.h>
#include <ami_mem_access.h>

#include "device.h"
#include "hotplug.h"
#include "utils.h"

#define GPIO_ALLOW_SBR 0x1040000

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
uint16_t reset_with_ami(struct device *device, struct device_ptr_array  *devices)
{
    /*
     * Step 1: Compute BDF (Bus:Device.Function) strings for all three PFs.
     * All PFs share the same bus:device but have different function numbers.
     */
    char pf0_bdf[VRTD_PCI_BDF_LEN] = {0};
    char pf1_bdf[VRTD_PCI_BDF_LEN] = {0};
    char pf2_bdf[VRTD_PCI_BDF_LEN] = {0};

    struct ami_device *ami_device = NULL;

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
     * Step 4: Issue AMI_IOC_DEVICE_BOOT ioctl to select boot partition 1.
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
    {
        struct ami_ioc_data_payload boot_payload = { 0 };
        boot_payload.partition = 0;
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
    LOG(LOG_INFO, "reset_with_ami: AMI_IOC_DEVICE_BOOT(%s) OK", pf0_bdf);

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
     * Step 10-12: Rescan the PCI bus and verify the device reappears.
     * The rescan re-enumerates all functions (PF0, PF1, PF2), then we wait
     * for the kernel, drivers, and udev to fully initialize device nodes.
     * If the device has not reappeared, retry the rescan after 3 seconds,
     * up to 5 attempts total.
     */
    #define RESCAN_MAX_RETRIES 5
    #define RESCAN_RETRY_DELAY_US 3000000

    for (int attempt = 1; attempt <= RESCAN_MAX_RETRIES; attempt++) {
        ret = slash_hotplug_rescan(g_hotplug);
        if (ret != 0) {
            LOG(LOG_ERR, "reset_with_ami: hotplug rescan failed: %m");
            return hotplug_errno_to_vrtd_ret(errno);
        }
        LOG(LOG_INFO, "reset_with_ami: rescan complete (attempt %d/%d)",
            attempt, RESCAN_MAX_RETRIES);

        /*
         * After a rescan the following things need to happen:
         *
         * * The kernel needs to detect the device on the PCIe bus.
         * * The kernel needs to hand that device to the slash and ami drivers.
         * * The slash and ami drivers need to create device nodes.
         * * The kernel needs to signal to userspace systemd-udev that the device node was created.
         * * systemd-udev needs to set permisions on the device node.
         *
         * That all takes time, so we wait a generous 10 seconds for all of that to occur.
         *
         * TODO: A much more robust method would be to remove all the code bellow this
         * and rework how devices are discovered. We could bring in libudev.
         * This would allow us to get netlink notifications on device events, such as new devices
         * appearing. Then we could attempt to open these devices only after the userspace has configured them.
         */
        usleep(10000000);

        ret = ami_dev_find(pf0_bdf, &ami_device);
        if (ret == AMI_STATUS_OK) {
            LOG(LOG_INFO, "reset_with_ami: device %s found after reset", pf0_bdf);
            break;
        }

        if (attempt < RESCAN_MAX_RETRIES) {
            LOG(LOG_WARNING, "reset_with_ami: ami_dev_find(%s) failed (attempt %d/%d): %s, retrying in 3s",
                pf0_bdf, attempt, RESCAN_MAX_RETRIES, ami_get_last_error());
            usleep(RESCAN_RETRY_DELAY_US);
        } else {
            LOG(LOG_ERR, "reset_with_ami: post-reset ami_dev_find(%s) failed after %d attempts: %s",
                pf0_bdf, RESCAN_MAX_RETRIES, ami_get_last_error());
            return VRTD_RET_INTERNAL_ERROR;
        }
    }

    #undef RESCAN_MAX_RETRIES
    #undef RESCAN_RETRY_DELAY_US

    ami_dev_delete(&ami_device);

    /*
     * Step 13: Run device discovery to re-add the reset device to vrtd's
     * tracked device list.  This opens the QDMA function, sets up queues,
     * and makes the device available for user requests again.
     */
    // We now rescan for the reset device
    ret = devices_discover_and_open(devices);
    if (ret != 0) {
        LOG(LOG_ERR, "reset_with_ami: devices_discover_and_open failed after reset");
        return VRTD_RET_INTERNAL_ERROR;
    }

    return VRTD_RET_OK;
}

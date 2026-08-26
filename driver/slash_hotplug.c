/**
 * Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program; if
 * not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * @file slash_hotplug.c
 *
 * PCIe hot-plug and reset subsystem for the SLASH kernel module.
 *
 * This file manages the PCIe-level lifecycle of SLASH FPGA devices,
 * providing four operations via /dev/slash_hotplug:
 *
 *   - **RESCAN**     — rescan all PCI root buses to discover new devices.
 *   - **REMOVE**     — remove a specific device from the PCI bus.
 *   - **TOGGLE_SBR** — assert and deassert a Secondary Bus Reset on
 *                      the device's immediate upstream bridge.
 *   - **HOTPLUG**    — atomic remove + rescan cycle on the device's
 *                      immediate parent bus.
 *
 * These operations are essential for FPGA reconfiguration workflows.
 * When a new bitstream is loaded, the FPGA's PCI identity and BAR
 * layout may change, requiring the device to be removed from the bus,
 * reset via SBR, and re-enumerated.
 *
 * A typical reconfiguration flow:
 *   1. REMOVE each PCI function (PF0, PF1, PF2 ...)
 *   2. TOGGLE_SBR to reset the device and wait for link recovery
 *   3. RESCAN to discover the new configuration
 *
 * All ioctls that operate on a specific device require an explicit
 * BDF string in the request.
 */

#include "slash_hotplug_driver.h"

#include "slash.h"

#include <slash/uapi/slash_hotplug.h>

#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define SLASH_HOTPLUG_MODE 0600
#define SLASH_SBR_PDI_SETTLE_MS 5000
#define SLASH_SBR_LINK_POLL_MS 100
#define SLASH_SBR_LINK_TIMEOUT_MS 25000

/**
 * slash_hotplug_copy_request() - Copy and sanitize a hotplug request from userspace.
 * @arg: Userspace pointer to the request struct.
 * @req: Kernel-side buffer to populate.
 *
 * NUL-terminates and trims whitespace from the BDF string.
 *
 * Return: 0 on success, -EFAULT or -EINVAL on failure.
 */
static int slash_hotplug_copy_request(unsigned long arg, struct slash_hotplug_device_request *req)
{
    pr_debug("slash_hotplug: copy_request: copying %zu bytes from userspace\n", sizeof(*req));

    if (copy_from_user(req, (void __user *)arg, sizeof(*req))) {
        pr_err("slash_hotplug: copy_request: copy_from_user failed\n");
        return -EFAULT;
    }

    pr_debug("slash_hotplug: copy_request: size=%u bdf='%.*s'\n",
             req->size, (int)(SLASH_HOTPLUG_BDF_LEN - 1), req->bdf);

    if (req->size < sizeof(*req)) {
        pr_err("slash_hotplug: request size %u too small (expected %zu)\n",
               req->size, sizeof(*req));
        return -EINVAL;
    }

    /* Defend against unterminated strings from userspace. */
    req->bdf[SLASH_HOTPLUG_BDF_LEN - 1] = '\0';
    strim(req->bdf);

    if (!req->bdf[0]) {
        pr_err("slash_hotplug: empty BDF in request\n");
        return -EINVAL;
    }

    pr_debug("slash_hotplug: copy_request: sanitized BDF='%s'\n", req->bdf);
    return 0;
}

/**
 * slash_hotplug_get_pci_dev() - Look up a PCI device by BDF string.
 * @bdf:      BDF string in "DDDD:BB:SS.F" hex format.
 * @pdev_out: On success, receives a reference-counted pci_dev pointer.
 *            Caller must call pci_dev_put() when done.
 *
 * Return: 0 on success, -EINVAL if the BDF is malformed, -ENODEV if
 *         the device is not present.
 */
static int slash_hotplug_get_pci_dev(const char *bdf, struct pci_dev **pdev_out)
{
    int domain, bus, slot, func;
    struct pci_dev *pdev;

    if (sscanf(bdf, "%x:%x:%x.%x", &domain, &bus, &slot, &func) != 4) {
        pr_err("slash_hotplug: malformed BDF '%s' (expected DDDD:BB:SS.F)\n", bdf);
        return -EINVAL;
    }

    pr_info("slash_hotplug: get_pci_dev: looking up %s (domain=%04x bus=%02x slot=%02x func=%x)\n",
            bdf, domain, bus, slot, func);

    pdev = pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(slot, func));
    if (!pdev) {
        pr_err("slash_hotplug: device %s not present in PCI subsystem\n", bdf);
        return -ENODEV;
    }

    pr_info("slash_hotplug: get_pci_dev: found %s\n", pci_name(pdev));
    *pdev_out = pdev;
    return 0;
}

/**
 * slash_hotplug_handle_rescan() - Rescan all PCI root buses.
 *
 * Discovers any new or reconfigured devices on every root bus.
 *
 * Return: Always 0.
 */
static int slash_hotplug_handle_rescan(void)
{
    struct pci_bus *bus;
    int bus_count = 0;

    pr_info("slash_hotplug: rescan: acquiring pci_lock_rescan_remove\n");
    pci_lock_rescan_remove();

    list_for_each_entry(bus, &pci_root_buses, node) {
        pr_info("slash_hotplug: rescan: scanning root bus %04x:%02x\n",
                pci_domain_nr(bus), bus->number);
        pci_rescan_bus(bus);
        bus_count++;
    }

    pci_unlock_rescan_remove();
    pr_info("slash_hotplug: rescan: complete (%d root bus(es) scanned)\n", bus_count);

    return 0;
}

/**
 * slash_hotplug_handle_remove() - Remove a device from the PCI bus.
 * @bdf: BDF string identifying the device to remove.
 *
 * Stops the device, tears down its driver bindings, and removes it
 * from the PCI hierarchy.  The device can be re-discovered later via
 * a bus rescan.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_hotplug_handle_remove(const char *bdf)
{
    struct pci_dev *pdev;
    int ret;

    pr_info("slash_hotplug: remove: starting for BDF %s\n", bdf);

    ret = slash_hotplug_get_pci_dev(bdf, &pdev);
    if (ret) {
        pr_err("slash_hotplug: remove: BDF %s unavailable (%d)\n", bdf, ret);
        return ret;
    }

    if (pdev->bus && pdev->bus->self) {
        u16 bridge_ctrl;
        pci_read_config_word(pdev->bus->self, PCI_BRIDGE_CONTROL, &bridge_ctrl);
        pr_info("slash_hotplug: remove: %s upstream bridge=%s bridge_ctrl=0x%04x before remove\n",
                pci_name(pdev), pci_name(pdev->bus->self), bridge_ctrl);
    }

    pr_info("slash_hotplug: remove: acquiring pci_lock_rescan_remove\n");
    pci_lock_rescan_remove();

    pr_info("slash_hotplug: remove: clearing bus master for %s\n", pci_name(pdev));
    pci_clear_master(pdev);

    pr_info("slash_hotplug: remove: calling pci_stop_and_remove_bus_device for %s\n", pci_name(pdev));
    pci_stop_and_remove_bus_device(pdev);

    pci_unlock_rescan_remove();
    pr_info("slash_hotplug: remove: released pci_lock_rescan_remove\n");

    pci_dev_put(pdev);
    pr_info("slash_hotplug: remove: %s complete\n", bdf);

    return 0;
}

/**
 * slash_hotplug_wait_for_link() - Wait for V80 reload and a stable PCIe link.
 * @bridge: Immediate upstream bridge whose secondary link was reset.
 *
 * The V80 turns SBR into a full PDI reload, so the link can remain down well
 * after the PCI core's conventional-reset delay.  Wait for the AVED-specified
 * five-second reload interval, then require DLL Link Active to remain asserted
 * across two polls.  PCIe requires 100 ms after Gen3+ link activation before
 * configuration access, which the second poll also supplies.
 *
 * Return: 0 when the link is ready (or DLLLA reporting is unavailable),
 * -EIO on config-space access failure, or -ETIMEDOUT on link timeout.
 */
static int slash_hotplug_wait_for_link(struct pci_dev *bridge)
{
    unsigned int elapsed_ms = 0;
    bool active_once = false;
    u16 status;
    int ret;

    pr_info("slash_hotplug: toggle_sbr: waiting %u ms for V80 PDI reload\n",
            SLASH_SBR_PDI_SETTLE_MS);
    msleep(SLASH_SBR_PDI_SETTLE_MS);

    if (!pci_is_pcie(bridge) || !bridge->link_active_reporting) {
        pr_info("slash_hotplug: toggle_sbr: DLL Link Active reporting unavailable; reload delay complete\n");
        return 0;
    }

    while (elapsed_ms < SLASH_SBR_LINK_TIMEOUT_MS) {
        ret = pcie_capability_read_word(bridge, PCI_EXP_LNKSTA, &status);
        if (ret != PCIBIOS_SUCCESSFUL) {
            pr_err("slash_hotplug: toggle_sbr: failed to read link status (%d)\n",
                   ret);
            return -EIO;
        }

        if (status & PCI_EXP_LNKSTA_DLLLA) {
            if (active_once) {
                pr_info("slash_hotplug: toggle_sbr: link stable after %u ms of polling\n",
                        elapsed_ms);
                return 0;
            }
            active_once = true;
        } else {
            active_once = false;
        }

        msleep(SLASH_SBR_LINK_POLL_MS);
        elapsed_ms += SLASH_SBR_LINK_POLL_MS;
    }

    pr_err("slash_hotplug: toggle_sbr: link did not become stable within %u ms\n",
           SLASH_SBR_LINK_TIMEOUT_MS);
    return -ETIMEDOUT;
}

/**
 * slash_hotplug_handle_toggle_sbr() - Perform a Secondary Bus Reset.
 * @bdf: BDF string identifying the device (or its former location).
 *
 * Locates the immediate upstream bridge for the given BDF and toggles
 * the PCI_BRIDGE_CTL_BUS_RESET bit in the bridge's PCI_BRIDGE_CONTROL
 * register.  The sequence is:
 *
 *   1. Read the current bridge control register.
 *   2. Assert SBR (set the BUS_RESET bit).
 *   3. Wait 2 ms — the PCIe spec minimum reset hold time.
 *   4. Deassert SBR (clear the BUS_RESET bit).
 *
 * The ioctl waits for V80 PDI reload and stable link training before
 * returning, so the caller may rescan immediately on success.
 *
 * Bridge resolution:
 *   The endpoint is typically removed before SBR is toggled, so we
 *   resolve the bridge via pci_find_bus() using the bus number from
 *   the BDF.  Bus structures survive endpoint removal.  The bridge
 *   is always the immediate parent (bus->self): a root port for direct
 *   attachment, or a switch downstream port behind a PCIe switch.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_hotplug_handle_toggle_sbr(const char *bdf)
{
    struct pci_bus *ep_bus;
    struct pci_dev *bridge;
    bool coordinate_hotplug;
    bool was_ignoring_hotplug;
    int domain, bus_nr, slot, func;
    int ret;

    pr_info("slash_hotplug: toggle_sbr: starting for BDF %s\n", bdf);

    if (sscanf(bdf, "%x:%x:%x.%x", &domain, &bus_nr, &slot, &func) != 4) {
        pr_err("slash_hotplug: toggle_sbr: malformed BDF '%s'\n", bdf);
        return -EINVAL;
    }

    /*
     * Hold pci_lock_rescan_remove() through bridge lookup, reset, and link
     * recovery. pci_bridge_secondary_bus_reset() is a low-level primitive and
     * does not serialize against pciehp or concurrent topology changes.
     *
     * ponytail: target kernels do not export the temporary pciehp link-change
     * coordination added upstream.  The global lock plus ignore_hotplug is
     * the compatible ceiling for RHEL 9 / Ubuntu 22.04; replace it with the
     * per-port PCI core API if that API becomes exported to modules.
     */
    pr_info("slash_hotplug: toggle_sbr: looking up bus (domain=%04x bus=%02x)\n",
            domain, bus_nr);
    pci_lock_rescan_remove();
    ep_bus = pci_find_bus(domain, bus_nr);
    if (!ep_bus || !ep_bus->self) {
        pci_unlock_rescan_remove();
        pr_err("slash_hotplug: toggle_sbr: no upstream bridge for %s\n", bdf);
        return -ENODEV;
    }
    if (!list_empty(&ep_bus->devices)) {
        pci_unlock_rescan_remove();
        pr_err("slash_hotplug: toggle_sbr: bus %04x:%02x still has live devices; remove every function before SBR\n",
               domain, bus_nr);
        return -EBUSY;
    }
    bridge = pci_dev_get(ep_bus->self);

    pr_info("slash_hotplug: toggle_sbr: bridge=%s bus=%02x\n",
            pci_name(bridge), bus_nr);

    /*
     * SBR generates DLLSC/PDC events.  RHEL 9 backports the PCIe bandwidth
     * controller, whose shared interrupt can make pciehp consume those events
     * and power off or re-enumerate the slot while the V80 reloads.  The
     * legacy ignore_hotplug flag is honored by pciehp's IRQ handler on all
     * supported kernels.  Keep it asserted for the entire PDI/link outage,
     * not merely the two-millisecond SBR assertion.
     */
    coordinate_hotplug = bridge->is_hotplug_bridge;
    was_ignoring_hotplug = bridge->ignore_hotplug;
    if (coordinate_hotplug) {
        bridge->ignore_hotplug = 1;
        smp_mb();
        pr_info("slash_hotplug: toggle_sbr: suppressing pciehp link events during V80 reload\n");
    }

    ret = pci_bridge_secondary_bus_reset(bridge);
    if (ret) {
        pr_err("slash_hotplug: toggle_sbr: pci_bridge_secondary_bus_reset failed (%d)\n", ret);
        goto out_restore_hotplug;
    }
    pr_info("slash_hotplug: toggle_sbr: pci_bridge_secondary_bus_reset OK\n");

    ret = slash_hotplug_wait_for_link(bridge);

out_restore_hotplug:
    if (coordinate_hotplug) {
        bridge->ignore_hotplug = was_ignoring_hotplug;
        smp_mb();
        pr_info("slash_hotplug: toggle_sbr: restored pciehp link-event handling\n");
    }
    pci_dev_put(bridge);
    pci_unlock_rescan_remove();
    if (!ret)
        pr_info("slash_hotplug: toggle_sbr: %s complete\n", bdf);
    return ret;
}

/**
 * slash_hotplug_handle_hotplug() - Perform a full hot-plug cycle.
 * @bdf: BDF string identifying the device.
 *
 * Removes the device from the bus, then rescans the device's parent
 * bus to re-enumerate it.  This is an atomic remove-then-rescan,
 * useful when the device identity hasn't changed but the kernel needs
 * to rebind drivers.
 *
 * Note: this does **not** include an SBR.  If the FPGA bitstream has
 * changed and a reset is needed, call TOGGLE_SBR separately before
 * HOTPLUG.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_hotplug_handle_hotplug(const char *bdf)
{
    struct pci_dev *pdev;
    struct pci_bus *bus;
    int ret;

    pr_info("slash_hotplug: hotplug: starting for BDF %s\n", bdf);

    ret = slash_hotplug_get_pci_dev(bdf, &pdev);
    if (ret) {
        pr_err("slash_hotplug: hotplug: BDF %s unavailable (%d)\n", bdf, ret);
        return ret;
    }

    bus = pdev->bus;
    if (!bus) {
        pr_err("slash_hotplug: hotplug: no parent bus for %s\n", pci_name(pdev));
        pci_dev_put(pdev);
        return -ENODEV;
    }

    pr_info("slash_hotplug: hotplug: parent bus %04x:%02x\n",
            pci_domain_nr(bus), bus->number);

    pr_info("slash_hotplug: hotplug: acquiring pci_lock_rescan_remove\n");
    pci_lock_rescan_remove();

    pr_info("slash_hotplug: hotplug: clearing bus master for %s\n", pci_name(pdev));
    pci_clear_master(pdev);

    pr_info("slash_hotplug: hotplug: calling pci_stop_and_remove_bus_device for %s\n", pci_name(pdev));
    pci_stop_and_remove_bus_device(pdev);
    pci_dev_put(pdev);

    pr_info("slash_hotplug: hotplug: device removed, rescanning bus %04x:%02x\n",
            pci_domain_nr(bus), bus->number);
    pci_rescan_bus(bus);

    pci_unlock_rescan_remove();
    pr_info("slash_hotplug: hotplug: released pci_lock_rescan_remove\n");
    pr_info("slash_hotplug: hotplug: %s complete\n", bdf);

    return 0;
}

/**
 * slash_hotplug_ioctl() - Dispatch hotplug ioctl commands.
 * @file: Open file for the hotplug misc device.
 * @cmd:  ioctl command number.
 * @arg:  Userspace pointer to the request struct (for commands that need one).
 *
 * Return: 0 on success, negative errno on failure.
 */
static long slash_hotplug_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct slash_hotplug_device_request req = {0};
    int ret;

    pr_info("slash_hotplug: ioctl: received cmd=0x%x\n", cmd);

    switch (cmd) {
    case SLASH_HOTPLUG_IOCTL_RESCAN:
        pr_info("slash_hotplug: ioctl: dispatching RESCAN\n");
        ret = slash_hotplug_handle_rescan();
        if (!ret)
            pr_info("slash_hotplug: ioctl: RESCAN succeeded\n");
        break;
    case SLASH_HOTPLUG_IOCTL_REMOVE:
        pr_info("slash_hotplug: ioctl: dispatching REMOVE\n");
        ret = slash_hotplug_copy_request(arg, &req);
        if (ret) {
            pr_err("slash_hotplug: remove: copy_request failed (%d)\n", ret);
            break;
        }
        pr_info("slash_hotplug: remove: BDF %s\n", req.bdf);
        ret = slash_hotplug_handle_remove(req.bdf);
        if (!ret)
            pr_info("slash_hotplug: ioctl: REMOVE succeeded\n");
        break;
    case SLASH_HOTPLUG_IOCTL_TOGGLE_SBR:
        pr_info("slash_hotplug: ioctl: dispatching TOGGLE_SBR\n");
        ret = slash_hotplug_copy_request(arg, &req);
        if (ret) {
            pr_err("slash_hotplug: toggle_sbr: copy_request failed (%d)\n", ret);
            break;
        }
        pr_info("slash_hotplug: toggle_sbr: BDF %s\n", req.bdf);
        ret = slash_hotplug_handle_toggle_sbr(req.bdf);
        if (!ret)
            pr_info("slash_hotplug: ioctl: TOGGLE_SBR succeeded\n");
        break;
    case SLASH_HOTPLUG_IOCTL_HOTPLUG:
        pr_info("slash_hotplug: ioctl: dispatching HOTPLUG\n");
        ret = slash_hotplug_copy_request(arg, &req);
        if (ret) {
            pr_err("slash_hotplug: hotplug: copy_request failed (%d)\n", ret);
            break;
        }
        pr_info("slash_hotplug: hotplug: BDF %s\n", req.bdf);
        ret = slash_hotplug_handle_hotplug(req.bdf);
        if (!ret)
            pr_info("slash_hotplug: ioctl: HOTPLUG succeeded\n");
        break;
    default:
        pr_err("slash_hotplug: unknown ioctl cmd 0x%x\n", cmd);
        ret = -ENOTTY;
        break;
    }

    if (ret)
        pr_err("slash_hotplug: ioctl 0x%x returning %d\n", cmd, ret);

    return ret;
}

#ifdef CONFIG_COMPAT
/**
 * slash_hotplug_compat_ioctl() - Handle 32-bit compat ioctls.
 *
 * Converts the 32-bit userspace pointer to a native pointer and
 * delegates to the standard ioctl handler.  The request struct is
 * the same size on 32-bit and 64-bit, so no field translation is
 * needed.
 */
static long slash_hotplug_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    pr_info("slash_hotplug: compat_ioctl: cmd=0x%x (32-bit userspace)\n", cmd);
    return slash_hotplug_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations slash_hotplug_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = slash_hotplug_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = slash_hotplug_compat_ioctl,
#endif
};

static struct miscdevice slash_hotplug_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = SLASH_HOTPLUG_DEVICE_NAME,
    .fops = &slash_hotplug_fops,
    .mode = SLASH_HOTPLUG_MODE,
};

int slash_hotplug_init(void)
{
    int ret;

    pr_info("slash_hotplug: registering misc device\n");

    ret = misc_register(&slash_hotplug_misc);
    if (ret) {
        pr_err("slash_hotplug: misc_register failed: %d\n", ret);
        return ret;
    }

    pr_info("slash_hotplug: misc device registered as /dev/%s (minor %d)\n",
            slash_hotplug_misc.name, slash_hotplug_misc.minor);
    return 0;
}

void slash_hotplug_exit(void)
{
    pr_info("slash_hotplug: deregistering misc device\n");
    misc_deregister(&slash_hotplug_misc);
    pr_info("slash_hotplug: misc device unregistered\n");
}

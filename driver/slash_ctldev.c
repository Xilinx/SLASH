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
 * @file slash_ctldev.c
 *
 * Control device implementation for the SLASH kernel module.
 *
 * Creates a per-device misc character device (/dev/slash_ctl<N>) that
 * exposes device identity, BAR properties, and dma-buf-backed BAR
 * mappings to userspace via ioctl.  This is an ioctl-only interface —
 * no read/write/mmap file operations are provided on the control
 * device itself.
 *
 * The ioctl interface uses **size-versioned structs** for ABI
 * compatibility: every ioctl struct has a leading @size field that the
 * caller sets to sizeof(struct ...).  The kernel reads the minimum
 * required fields (MIN_SIZE), copies min(user_size, kernel_size)
 * bytes, and zero-fills any trailing space when a newer userspace sends
 * a larger struct than the kernel knows about.  This allows the driver
 * and userspace library to evolve independently.
 */

#include "slash_ctldev.h"

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/printk.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "slash.h"
#include "slash_dmabuf.h"

/** Compute the size of a struct member without needing an instance. */
#define SLASH_FIELD_SIZE(_type, _member) (sizeof(((_type *)0)->_member))

/*
 * Minimum struct sizes needed to read the input fields, and minimum
 * sizes needed to write back the output fields, for each ioctl.
 * These define the ABI backward-compatibility boundary: any userspace
 * that provides at least MIN_SIZE bytes is accepted.
 */

#define SLASH_IOCTL_BAR_INFO_MIN_SIZE \
    (offsetof(struct slash_ioctl_bar_info, bar_number) + SLASH_FIELD_SIZE(struct slash_ioctl_bar_info, bar_number))
#define SLASH_IOCTL_BAR_INFO_RESPONSE_SIZE \
    (offsetof(struct slash_ioctl_bar_info, length) + SLASH_FIELD_SIZE(struct slash_ioctl_bar_info, length))

#define SLASH_IOCTL_BAR_FD_MIN_SIZE \
    (offsetof(struct slash_ioctl_bar_fd_request, flags) + SLASH_FIELD_SIZE(struct slash_ioctl_bar_fd_request, flags))
#define SLASH_IOCTL_BAR_FD_RESPONSE_SIZE \
    (offsetof(struct slash_ioctl_bar_fd_request, length) + SLASH_FIELD_SIZE(struct slash_ioctl_bar_fd_request, length))

static int slash_ctldev_set_bar_info(struct pci_dev *pdev, struct slash_ctldev *ctldev);
static int slash_ctldev_create_bar_dmabufs(struct slash_ctldev *ctldev);
static int slash_ctldev_create_misc(struct slash_ctldev *ctldev);

static void slash_ctldev_destroy_misc(struct slash_ctldev *ctldev);
static void slash_ctldev_destroy_dmabufs(struct slash_ctldev *ctldev);

static long slash_ctldev_fop_ioctl(struct file *, unsigned int, unsigned long);

/**
 * struct slash_ctldev_id_entry - Stable BDF-to-number mapping entry.
 * @node:    Intrusive list linkage for @slash_ctldev_id_map.
 * @bdf:     Full PCI BDF string including function (e.g. "0000:61:00.2").
 * @number:  The /dev/slash_ctl<N> suffix permanently assigned to this BDF.
 * @in_use:  True while the device is bound to the driver.  Cleared on remove,
 *           set on probe.  A probe that finds @in_use already true indicates
 *           the kernel handed us a device that was never properly unbound —
 *           this should never happen under normal operation.
 *
 * Entries are allocated in probe and intentionally never freed.  They survive
 * hotplug remove+rescan cycles so that a device always gets back the same N.
 */
struct slash_ctldev_id_entry {
    struct list_head node;
    char bdf[32]; /* "DDDD:BB:SS.F\0" fits comfortably in 32 bytes */
    int  number;
    bool in_use;
};

/** Persistent BDF-to-number map; entries live for the module's lifetime. */
static LIST_HEAD(slash_ctldev_id_map);
/** Serialises all accesses to @slash_ctldev_id_map and @in_use fields. */
static DEFINE_MUTEX(slash_ctldev_id_map_lock);
/** Source of new numbers; only incremented when a BDF is seen for the first time. */
static atomic_t slash_ctldev_devcount = ATOMIC_INIT(0);

static struct file_operations slash_ctldev_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = slash_ctldev_fop_ioctl,
};

/**
 * slash_ctldev_create() - Create a control device for a PCI function.
 * @pdev: PCI device to create the control device for.
 *
 * Allocates the control device state, probes all PCI BARs, creates
 * dma-buf exporters for MMIO BARs, and registers a misc device.
 * The state is stored as PCI driver data on @pdev.
 *
 * Return: 0 on success, negative errno on failure.
 */
int slash_ctldev_create(struct pci_dev *pdev)
{
    int err;

    struct slash_ctldev *ctldev = kzalloc(sizeof(*ctldev), GFP_KERNEL);
    if (!ctldev) {
        dev_err(&pdev->dev, "ctldev: kzalloc failed\n");
        return -ENOMEM;
    }
    ctldev->pdev = pdev;

    dev_info(&pdev->dev, "ctldev: creating control device\n");

    /* Store early so that the ioctl handler can find us via pci_get_drvdata(). */
    pci_set_drvdata(pdev, ctldev);

    err = slash_ctldev_set_bar_info(pdev, ctldev);
    if (err) {
        dev_err(&pdev->dev, "ctldev: set_bar_info failed: %d\n", err);
        goto err_free_ctldev;
    }

    err = slash_ctldev_create_bar_dmabufs(ctldev);
    if (err) {
        dev_err(&pdev->dev, "ctldev: creating BAR dma-bufs failed: %d\n", err);
        /*
         * Some dmabufs may have been created before the failure,
         * so we must destroy whatever was successfully created.
         */
        goto err_destroy_dmabufs;
    }

    err = slash_ctldev_create_misc(ctldev);
    if (err) {
        dev_err(&pdev->dev, "ctldev: creating misc ctldev failed: %d\n", err);
        goto err_destroy_dmabufs;
    }

    dev_info(&pdev->dev, "ctldev: device created successfully\n");

    return 0;

err_destroy_dmabufs:
    slash_ctldev_destroy_dmabufs(ctldev);

err_free_ctldev:
    kfree(ctldev);

    return err;
}

/**
 * slash_ctldev_set_bar_info() - Probe and cache BAR metadata.
 * @pdev:   PCI device to read BAR info from.
 * @ctldev: Control device state to populate.
 *
 * Iterates over all 6 standard PCI BARs and records their start
 * address, size, and type (MMIO vs I/O port).  BARs with a zero
 * start address are considered unused and skipped.
 *
 * Return: Always 0 (BAR discovery cannot fail).
 */
static int slash_ctldev_set_bar_info(struct pci_dev *pdev, struct slash_ctldev *ctldev)
{
    int i;

    dev_dbg(&pdev->dev, "ctldev: probing PCI BARs\n");
    for (i = 0; i < PCI_STD_NUM_BARS; i++) {
        unsigned long flags;

        if (!pci_resource_start(pdev, i)) {
            dev_dbg(&pdev->dev, "ctldev: BAR%d unused\n", i);
            continue; /* Unused BAR */
        }

        ctldev->bars[i].active = 1;
        ctldev->bars[i].start  = pci_resource_start(pdev, i);
        ctldev->bars[i].end    = pci_resource_end(pdev, i);
        ctldev->bars[i].len    = pci_resource_len(pdev, i);
        flags                  = pci_resource_flags(pdev, i);
        ctldev->bars[i].mmio   = ((flags & IORESOURCE_MEM) != 0);


        dev_info(&pdev->dev,
                "Found BAR%d: 0x%pa - 0x%pa (size: %pa) %s\n",
                i, &ctldev->bars[i].start, &ctldev->bars[i].end, &ctldev->bars[i].len,
                (flags & IORESOURCE_MEM) ? "MMIO" :
                (flags & IORESOURCE_IO) ? "IO" : "UNKNOWN");
    }

    return 0;
}

/**
 * slash_ctldev_create_bar_dmabufs() - Create dma-buf exporters for MMIO BARs.
 * @ctldev: Control device whose BARs to export.
 *
 * Only active MMIO BARs get a dma-buf; I/O-port BARs are skipped.
 * The dma-buf lets userspace mmap the BAR for direct register access.
 *
 * Return: 0 on success, negative errno on first failure (some dmabufs
 *         may already have been created and must be cleaned up by the caller).
 */
static int slash_ctldev_create_bar_dmabufs(struct slash_ctldev *ctldev)
{
    int i;
    struct dma_buf *dmabuf;

    dev_dbg(&ctldev->pdev->dev, "ctldev: creating dma-bufs for MMIO BARs\n");
    for (i = 0; i < PCI_STD_NUM_BARS; i++) {
        if (!ctldev->bars[i].active || !ctldev->bars[i].mmio) {
            continue;
        }

        dmabuf = slash_bar_dmabuf_create(ctldev->pdev, i);
        if (IS_ERR(dmabuf)) {
            dev_err(&ctldev->pdev->dev, "ctldev: BAR%d dmabuf create failed: %ld\n", i, PTR_ERR(dmabuf));
            return PTR_ERR(dmabuf);
        }

        ctldev->bars[i].dmabuf = dmabuf;
        dev_dbg(&ctldev->pdev->dev, "ctldev: BAR%d dmabuf created\n", i);
    }

    return 0;
}

/**
 * slash_ctldev_id_get() - Look up or allocate a stable number for a BDF.
 * @bdf: Full PCI BDF string (e.g. "0000:61:00.2") from pci_name().
 *
 * Called from probe.  Returns the number permanently associated with @bdf,
 * allocating a new one if this BDF is seen for the first time.  Also marks
 * the entry as in_use = true.
 *
 * If an existing entry is found with in_use already set, the device was
 * never properly unbound before probe was called again — this indicates a
 * kernel PCI driver bug.  The function logs a loud error and returns
 * -EBUSY so that probe aborts without touching the device.
 *
 * Return: non-negative stable device number on success, negative errno on
 *         failure (-ENOMEM if allocation fails, -EBUSY if already in use).
 */
static int slash_ctldev_id_get(const char *bdf)
{
    struct slash_ctldev_id_entry *entry;
    int number;

    mutex_lock(&slash_ctldev_id_map_lock);

    list_for_each_entry(entry, &slash_ctldev_id_map, node) {
        if (strcmp(entry->bdf, bdf) != 0)
            continue;

        if (entry->in_use) {
            /*
             * This BDF is already marked in_use.  The kernel should
             * never call probe for a device that is still bound —
             * if this fires, something has gone badly wrong in the
             * PCI driver infrastructure.
             */
            pr_err("slash_ctldev: BUG: probe called for %s but entry is already in_use "
                   "(number=%d); refusing to bind\n", bdf, entry->number);
            mutex_unlock(&slash_ctldev_id_map_lock);
            return -EBUSY;
        }

        entry->in_use = true;
        number = entry->number;
        mutex_unlock(&slash_ctldev_id_map_lock);
        pr_info("slash_ctldev: reusing number %d for %s\n", number, bdf);
        return number;
    }

    /* First time we've seen this BDF — allocate a fresh entry. */
    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry) {
        mutex_unlock(&slash_ctldev_id_map_lock);
        return -ENOMEM;
    }

    strscpy(entry->bdf, bdf, sizeof(entry->bdf));
    entry->number = atomic_inc_return(&slash_ctldev_devcount) - 1;
    entry->in_use = true;
    list_add_tail(&entry->node, &slash_ctldev_id_map);

    number = entry->number;
    mutex_unlock(&slash_ctldev_id_map_lock);

    pr_info("slash_ctldev: assigned number %d to %s\n", number, bdf);
    return number;
}

/**
 * slash_ctldev_id_release() - Mark a BDF's entry as no longer in use.
 * @bdf: Full PCI BDF string passed to the matching slash_ctldev_id_get() call.
 *
 * Called from remove.  Clears in_use so that the next probe for the same
 * BDF can reuse the stored number.  The entry itself is not freed — it must
 * persist so the number remains stable across hotplug cycles.
 *
 * If no entry exists for @bdf (should never happen after a successful probe),
 * the call is a no-op and a warning is logged.
 */
static void slash_ctldev_id_release(const char *bdf)
{
    struct slash_ctldev_id_entry *entry;

    mutex_lock(&slash_ctldev_id_map_lock);

    list_for_each_entry(entry, &slash_ctldev_id_map, node) {
        if (strcmp(entry->bdf, bdf) != 0)
            continue;

        entry->in_use = false;
        mutex_unlock(&slash_ctldev_id_map_lock);
        pr_info("slash_ctldev: released number %d for %s\n", entry->number, bdf);
        return;
    }

    /* Should be unreachable: remove without a prior successful probe. */
    pr_warn("slash_ctldev: WARNING: release called for %s but no entry found\n", bdf);
    mutex_unlock(&slash_ctldev_id_map_lock);
}

/**
 * slash_ctldev_create_misc() - Register the misc character device.
 * @ctldev: Control device to register.
 *
 * Creates /dev/slash_ctl<N> with a stable index derived from the BDF-to-number
 * map.  The sysfs name includes the PCI BDF for identification; the /dev node
 * uses a numeric suffix that is stable across hotplug remove+rescan cycles.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_ctldev_create_misc(struct slash_ctldev *ctldev)
{
    int err, id;
    const char *name, *nodename;

    /* sysfs name: includes PCI BDF (e.g. "slash_ctl_0000:03:00.2"). */
    name = kasprintf(GFP_KERNEL, SLASH_CTLDEV_NAME_FMT, pci_name(ctldev->pdev));
    if (!name) {
        dev_err(&ctldev->pdev->dev, "ctldev: kasprintf(name) failed\n");
        return -ENOMEM;
    }

    /* /dev node name: stable numeric index from BDF-to-number map. */
    id = slash_ctldev_id_get(pci_name(ctldev->pdev));
    if (id < 0) {
        dev_err(&ctldev->pdev->dev, "ctldev: id_get failed: %d\n", id);
        err = id;
        goto err_free_name;
    }

    nodename = kasprintf(GFP_KERNEL, SLASH_CTLDEV_NODENAME_FMT, id);
    if (!nodename) {
        dev_err(&ctldev->pdev->dev, "ctldev: kasprintf(nodename) failed\n");
        err = -ENOMEM;
        goto err_release_id;
    }

    ctldev->misc.minor = MISC_DYNAMIC_MINOR;
    ctldev->misc.name = name;
    ctldev->misc.fops = &slash_ctldev_fops;
    ctldev->misc.parent = &ctldev->pdev->dev;
    ctldev->misc.nodename = nodename;
    ctldev->misc.mode = SLASH_CTLDEV_MODE;

    err = misc_register(&ctldev->misc);
    if (err) {
        dev_err(&ctldev->pdev->dev, "ctldev: misc_register failed: %d\n", err);
        goto err_free_nodename;
    }

    return 0;

err_free_nodename:
    kfree(nodename);

err_release_id:
    /* id_get succeeded and set in_use; undo that. */
    slash_ctldev_id_release(pci_name(ctldev->pdev));

err_free_name:
    kfree(name);

    return err;
}

void slash_ctldev_destroy(struct pci_dev *pdev)
{
    struct slash_ctldev *ctldev = pci_get_drvdata(pdev);

    dev_info(&pdev->dev, "ctldev: destroying control device\n");
    slash_ctldev_destroy_misc(ctldev);
    slash_ctldev_destroy_dmabufs(ctldev);

    kfree(ctldev);
}

static void slash_ctldev_destroy_misc(struct slash_ctldev *ctldev)
{
    dev_dbg(&ctldev->pdev->dev, "ctldev: deregistering misc device\n");
    misc_deregister(&ctldev->misc);
    slash_ctldev_id_release(pci_name(ctldev->pdev));
    kfree(ctldev->misc.name);
    kfree(ctldev->misc.nodename);
    ctldev->misc.name = NULL;
    ctldev->misc.nodename = NULL;
}

static void slash_ctldev_destroy_dmabufs(struct slash_ctldev *ctldev)
{
    int i;

    for (i = 0; i < PCI_STD_NUM_BARS; i++) {
        if (ctldev->bars[i].dmabuf) {
            dev_dbg(&ctldev->pdev->dev, "ctldev: destroying BAR%d dmabuf\n", i);
            slash_bar_dmabuf_destroy(ctldev->bars[i].dmabuf);
        }
    }
}

/**
 * slash_ctldev_fop_ioctl() - Handle control device ioctls.
 * @file: Open file for the misc device.
 * @op:   ioctl command number.
 * @arg:  Pointer to the user-space ioctl struct.
 *
 * Dispatches to one of:
 *   - GET_BAR_INFO:    Return BAR properties (start, size, usability).
 *   - GET_BAR_FD:      Return a dma-buf fd for mmap'ing a BAR.
 *   - GET_DEVICE_INFO: Return PCI identity (BDF, vendor/device IDs).
 *
 * All ioctls use the size-versioning pattern described in the file
 * header.
 *
 * Return: 0 (or positive fd for GET_BAR_FD) on success, negative errno on failure.
 */
static long slash_ctldev_fop_ioctl(struct file *file, unsigned int op, unsigned long arg)
{
    struct miscdevice *misc = file->private_data;
    struct pci_dev *pdev = to_pci_dev(misc->parent);
    struct slash_ctldev *ctldev = pci_get_drvdata(pdev);

    dev_dbg(&pdev->dev, "ctldev: ioctl op=0x%x\n", op);
    switch(op) {
    case SLASH_CTLDEV_IOCTL_GET_BAR_INFO: {
        struct slash_ioctl_bar_info bar_info = {0};
        struct slash_ctldev_bar *bar = NULL;
        u32 bar_info_alleged_size;
        size_t copy_size;

        /*
         * Size-versioning: read the leading size field first to
         * determine how much data the caller provided.
         */
        if (copy_from_user(&bar_info_alleged_size, (void __user *)arg, sizeof(bar_info_alleged_size))) {
            dev_err(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_INFO copy_from_user failed\n");
            return -EFAULT;
        }

        if (bar_info_alleged_size < SLASH_IOCTL_BAR_INFO_MIN_SIZE) {
            dev_warn(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_INFO size too small (%u)\n", bar_info_alleged_size);
            return -EINVAL;
        }

        /*
         * Copy the smaller of (user struct, kernel struct), then
         * zero-fill any kernel fields that the user struct doesn't
         * cover.  This handles older userspace gracefully.
         */
        copy_size = min_t(size_t, bar_info_alleged_size, sizeof(bar_info));
        if (copy_from_user(&bar_info, (void __user *)arg, copy_size)) {
            dev_err(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_INFO copy_from_user failed\n");
            return -EFAULT;
        }
        if (copy_size < sizeof(bar_info)) {
            memset((u8 *)&bar_info + copy_size, 0, sizeof(bar_info) - copy_size);
        }

        if (bar_info.bar_number < 0 || bar_info.bar_number >= PCI_STD_NUM_BARS) {
            dev_warn(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_INFO invalid BAR %d\n", bar_info.bar_number);
            return -EINVAL;
        }

        bar = &ctldev->bars[bar_info.bar_number];

        /* Populate output fields. */
        bar_info.usable = bar->active && bar->mmio;
        bar_info.in_use = 0;
        bar_info.start_address = bar->start;
        bar_info.length = bar->len;

        /* Tell userspace the kernel's struct size for version negotiation. */
        bar_info.size = sizeof(bar_info);

        if (bar_info_alleged_size < SLASH_IOCTL_BAR_INFO_RESPONSE_SIZE) {
            dev_warn(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_INFO response size too small (%u)\n", bar_info_alleged_size);
            return -EINVAL;
        }

        copy_size = min_t(size_t, bar_info_alleged_size, sizeof(bar_info));
        if (copy_to_user((void __user *)arg, &bar_info, copy_size)) {
            dev_err(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_INFO copy_to_user failed\n");
            return -EFAULT;
        }
        /*
         * If the user struct is larger than what we know, zero-fill
         * the tail.  This ensures newer userspace sees zeroed fields
         * when talking to an older kernel (forward compatibility).
         */
        if (bar_info_alleged_size > sizeof(bar_info)) {
            size_t extra = bar_info_alleged_size - sizeof(bar_info);
            void __user *dst = (void __user *)((unsigned long)arg + sizeof(bar_info));

            if (clear_user(dst, extra)) {
                dev_err(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_INFO clear_user failed\n");
                return -EFAULT;
            }
        }

        return 0;
    }

    case SLASH_CTLDEV_IOCTL_GET_BAR_FD: {
        struct slash_ioctl_bar_fd_request fd_request = {0};
        struct slash_ctldev_bar *bar = NULL;
        int ret;
        u32 fd_request_alleged_size;
        size_t copy_size;

        /*
         * Access control is enforced by device-node permissions
         * (udev: slash_ctl* is 0600, owned by vrtd:vrtd).
         * No capability check is needed here or at mmap() time —
         * the dma-buf mmap handler uses fault-based vmf_insert_pfn()
         * which does not require CAP_SYS_RAWIO.
         */

        /* Size-versioning: same pattern as GET_BAR_INFO above. */
        if (copy_from_user(&fd_request_alleged_size, (void __user *)arg, sizeof(fd_request_alleged_size))) {
            dev_err(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_FD copy_from_user failed\n");
            return -EFAULT;
        }

        if (fd_request_alleged_size < SLASH_IOCTL_BAR_FD_MIN_SIZE) {
            dev_warn(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_FD size too small (%u)\n", fd_request_alleged_size);
            return -EINVAL;
        }

        copy_size = min_t(size_t, fd_request_alleged_size, sizeof(fd_request));
        if (copy_from_user(&fd_request, (void __user *)arg, copy_size)) {
            dev_err(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_FD copy_from_user failed\n");
            return -EFAULT;
        }
        if (copy_size < sizeof(fd_request)) {
            memset((u8 *)&fd_request + copy_size, 0, sizeof(fd_request) - copy_size);
        }

        if (fd_request.bar_number < 0 || fd_request.bar_number >= PCI_STD_NUM_BARS) {
            dev_warn(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_FD invalid BAR %d\n", fd_request.bar_number);
            return -EINVAL;
        }
        if (fd_request.flags & ~O_CLOEXEC) {
            dev_warn(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_FD invalid flags 0x%x\n", fd_request.flags);
            return -EINVAL;
        }

        bar = &ctldev->bars[fd_request.bar_number];

        if (!bar->dmabuf) {
            dev_err(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_FD BAR%d has no dmabuf\n", fd_request.bar_number);
            return -ENODEV;
        }

        fd_request.length = bar->len;
        fd_request.size = sizeof(fd_request);

        if (fd_request_alleged_size < SLASH_IOCTL_BAR_FD_RESPONSE_SIZE) {
            dev_warn(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_FD response size too small (%u)\n", fd_request_alleged_size);
            return -EINVAL;
        }

        copy_size = min_t(size_t, fd_request_alleged_size, sizeof(fd_request));
        if (copy_to_user((void __user *)arg, &fd_request, copy_size)) {
            dev_err(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_FD copy_to_user failed\n");
            return -EFAULT;
        }
        if (fd_request_alleged_size > sizeof(fd_request)) {
            size_t extra = fd_request_alleged_size - sizeof(fd_request);
            void __user *dst = (void __user *)((unsigned long)arg + sizeof(fd_request));

            if (clear_user(dst, extra)) {
                dev_err(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_BAR_FD clear_user failed\n");
                return -EFAULT;
            }
        }

        /*
         * Take an extra reference on the dma-buf before creating the
         * fd.  The fd will hold this reference; if dma_buf_fd() fails
         * we must drop it ourselves.
         */
        get_dma_buf(bar->dmabuf);
        ret = dma_buf_fd(bar->dmabuf, fd_request.flags);
        if (ret < 0) {
            dev_err(&pdev->dev, "ctldev: GET_BAR_FD dma_buf_fd failed: %d\n", ret);
            dma_buf_put(bar->dmabuf);
            return ret;
        }

        /* The fd number is returned as the ioctl return value. */
        dev_dbg(&pdev->dev, "ctldev: GET_BAR_FD BAR%d -> fd %d\n", fd_request.bar_number, ret);
        return ret;
    }

    case SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO: {
        struct slash_ioctl_device_info info;
        u32 user_size = 0;
        size_t copy_size;

        if (copy_from_user(&user_size, (void __user *)arg, sizeof(user_size))) {
            dev_err(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO copy_from_user failed\n");
            return -EFAULT;
        }

        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);

        strscpy(info.bdf, pci_name(pdev), sizeof(info.bdf));
        info.vendor_id = pdev->vendor;
        info.device_id = pdev->device;
        info.subsystem_vendor_id = pdev->subsystem_vendor;
        info.subsystem_device_id = pdev->subsystem_device;

        copy_size = min_t(size_t, user_size, sizeof(info));
        if (copy_to_user((void __user *)arg, &info, copy_size)) {
            dev_err(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO copy_to_user failed\n");
            return -EFAULT;
        }
        if (user_size > sizeof(info)) {
            size_t extra = user_size - sizeof(info);
            void __user *dst = (void __user *)((unsigned long)arg + sizeof(info));

            if (clear_user(dst, extra)) {
                dev_err(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO clear_user failed\n");
                return -EFAULT;
            }
        }

        return 0;
    }

    default:
        dev_warn(&pdev->dev, "ctldev: unknown ioctl op=0x%x\n", op);
        return -ENOTTY;
    }
}

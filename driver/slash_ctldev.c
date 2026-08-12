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
 * Creates a per-device character device (/dev/slash_ctl<N>) that
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

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/printk.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "slash.h"
#include "slash_chrdev.h"
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

/*
 * GET_DEVICE_INFO is pure output: there are no input fields beyond `size`
 * itself, so the smallest meaningful user_size is the size field on its
 * own. A caller passing size==0 has either forgotten to initialise the
 * struct or claimed an incoherent "my struct has zero bytes" — either way
 * the kernel rejects with -EINVAL rather than silently writing 0 bytes
 * back.
 */
#define SLASH_IOCTL_DEVICE_INFO_MIN_SIZE \
    SLASH_FIELD_SIZE(struct slash_ioctl_device_info, size)

static int slash_ctldev_set_bar_info(struct pci_dev *pdev, struct slash_ctldev *ctldev);
static int slash_ctldev_create_bar_dmabufs(struct slash_ctldev *ctldev);
static int slash_ctldev_create_chrdev(struct slash_ctldev *ctldev);

static void slash_ctldev_destroy_dmabufs(struct slash_ctldev *ctldev);
static void slash_ctldev_release(struct kref *ref);

static int slash_ctldev_fop_open(struct inode *, struct file *);
static int slash_ctldev_fop_release(struct inode *, struct file *);
static long slash_ctldev_fop_ioctl(struct file *, unsigned int, unsigned long);
static long slash_ctldev_ioctl(struct slash_ctldev *, unsigned int,
                               unsigned long);

static struct file_operations slash_ctldev_fops = {
    .owner = THIS_MODULE,
    .open = slash_ctldev_fop_open,
    .release = slash_ctldev_fop_release,
    .unlocked_ioctl = slash_ctldev_fop_ioctl,
};

/**
 * slash_ctldev_create() - Create a control device for a PCI function.
 * @pdev: PCI device to create the control device for.
 *
 * Allocates the control device state, probes all PCI BARs, creates
 * dma-buf exporters for MMIO BARs, and registers a character device.
 * The state is stored as PCI driver data on @pdev.
 *
 * Return: 0 on success, negative errno on failure.
 */
int slash_ctldev_create(struct pci_dev *pdev)
{
    struct slash_ctldev *ctldev;
    int err;

    pci_set_drvdata(pdev, NULL);
    ctldev = kzalloc(sizeof(*ctldev), GFP_KERNEL);
    if (!ctldev) {
        dev_err(&pdev->dev, "ctldev: kzalloc failed\n");
        return -ENOMEM;
    }
    ctldev->pdev = pdev;
    ctldev->slot = -1;
    kref_init(&ctldev->ref);
    mutex_init(&ctldev->lock);

    dev_info(&pdev->dev, "ctldev: creating control device\n");

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

    err = slash_ctldev_create_chrdev(ctldev);
    if (err) {
        dev_err(&pdev->dev, "ctldev: creating character device failed: %d\n",
                err);
        goto err_destroy_dmabufs;
    }

    pci_set_drvdata(pdev, ctldev);
    dev_info(&pdev->dev, "ctldev: device created successfully\n");

    return 0;

err_destroy_dmabufs:
    slash_ctldev_destroy_dmabufs(ctldev);

err_free_ctldev:
    pci_set_drvdata(pdev, NULL);
    kref_put(&ctldev->ref, slash_ctldev_release);

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
 * slash_ctldev_create_chrdev() - Register the control character device.
 * @ctldev: Control device to register.
 *
 * The shared board allocator gives matching PF1/PF2 endpoints the same stable
 * card number.  Sysfs keeps the full PF2 BDF while the class devnode callback
 * names the node /dev/slash_ctl<N>.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_ctldev_create_chrdev(struct slash_ctldev *ctldev)
{
    char name[64];

    ctldev->slot = slash_chrdev_board_get(ctldev->pdev);
    if (ctldev->slot < 0)
        return ctldev->slot;

    snprintf(name, sizeof(name), SLASH_CTLDEV_NAME_FMT,
             pci_name(ctldev->pdev));
    ctldev->device =
        slash_chrdev_add(&ctldev->cdev, &slash_ctldev_fops,
                         SLASH_CTLDEV_MINOR(ctldev->slot),
                         &ctldev->pdev->dev, ctldev, name);
    if (!IS_ERR(ctldev->device))
        return 0;

    slash_chrdev_board_put(ctldev->pdev);
    ctldev->slot = -1;
    return PTR_ERR(ctldev->device);
}

void slash_ctldev_destroy(struct pci_dev *pdev)
{
    struct slash_ctldev *ctldev = pci_get_drvdata(pdev);

    if (!ctldev)
        return;

    dev_info(&pdev->dev, "ctldev: destroying control device\n");
    pci_set_drvdata(pdev, NULL);

    mutex_lock(&ctldev->lock);
    ctldev->dead = true;
    mutex_unlock(&ctldev->lock);

    slash_chrdev_del(&ctldev->cdev, ctldev->device);
    ctldev->device = NULL;
    slash_chrdev_board_put(ctldev->pdev);
    ctldev->slot = -1;
    slash_ctldev_destroy_dmabufs(ctldev);

    kref_put(&ctldev->ref, slash_ctldev_release);
}

static void slash_ctldev_release(struct kref *ref)
{
    struct slash_ctldev *ctldev =
        container_of(ref, struct slash_ctldev, ref);

    mutex_destroy(&ctldev->lock);
    kfree(ctldev);
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

static int slash_ctldev_fop_open(struct inode *inode, struct file *file)
{
    struct slash_ctldev *ctldev =
        container_of(inode->i_cdev, struct slash_ctldev, cdev);

    mutex_lock(&ctldev->lock);
    if (ctldev->dead) {
        mutex_unlock(&ctldev->lock);
        return -ENODEV;
    }
    kref_get(&ctldev->ref);
    mutex_unlock(&ctldev->lock);

    file->private_data = ctldev;
    return 0;
}

static int slash_ctldev_fop_release(struct inode *inode, struct file *file)
{
    struct slash_ctldev *ctldev = file->private_data;

    (void)inode;

    if (ctldev)
        kref_put(&ctldev->ref, slash_ctldev_release);
    file->private_data = NULL;
    return 0;
}

static long slash_ctldev_fop_ioctl(struct file *file, unsigned int op,
                                   unsigned long arg)
{
    struct slash_ctldev *ctldev = file->private_data;
    long ret;

    if (!ctldev)
        return -ENODEV;

    mutex_lock(&ctldev->lock);
    if (ctldev->dead) {
        mutex_unlock(&ctldev->lock);
        return -ENODEV;
    }
    ret = slash_ctldev_ioctl(ctldev, op, arg);
    mutex_unlock(&ctldev->lock);
    return ret;
}

/**
 * slash_ctldev_ioctl() - Handle control device ioctls.
 * @ctldev: Control device for the open file.
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
static long slash_ctldev_ioctl(struct slash_ctldev *ctldev, unsigned int op,
                               unsigned long arg)
{
    struct pci_dev *pdev = ctldev->pdev;

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

        if (user_size < SLASH_IOCTL_DEVICE_INFO_MIN_SIZE) {
            dev_warn(&pdev->dev, "ctldev: SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO size too small (%u)\n", user_size);
            return -EINVAL;
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

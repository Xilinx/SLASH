/**
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
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
 * @file slash_chrdev.c
 *
 * Shared SLASH character-device registration and board numbering.
 */

#include "slash_chrdev.h"

#include <slash/uapi/slash_hotplug.h>

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "slash.h"
#include "slash_compat.h"

/**
 * struct slash_board_slot - Stable board-BDF to device-number mapping.
 * @node:       Link in the module-lifetime mapping list.
 * @domain:     PCI domain of the board endpoints.
 * @bus:        PCI bus of the board endpoints.
 * @slot:       PCI device number shared by PF1 and PF2.
 * @number:     Stable card number used in the character-device minors.
 * @active_pfs: Function bits currently bound to a SLASH PCI driver.
 */
struct slash_board_slot {
    struct list_head node;
    unsigned int domain;
    unsigned int bus;
    unsigned int slot;
    unsigned int number;
    unsigned long active_pfs;
};

static dev_t slash_chrdev_base;
static struct class *slash_chrdev_class;

static LIST_HEAD(slash_board_slots);
static DEFINE_MUTEX(slash_board_slots_lock);
static unsigned int slash_board_slot_count;

#if defined(SLASH_HAVE_CLASS_DEVNODE_CONST)
static char *slash_chrdev_devnode(const struct device *device, umode_t *mode)
#else
static char *slash_chrdev_devnode(struct device *device, umode_t *mode)
#endif
{
    unsigned int minor = MINOR(device->devt);
    unsigned int card;

    if (mode)
        *mode = SLASH_CHRDEV_MODE;

    if (minor == SLASH_HOTPLUG_MINOR)
        return kstrdup(SLASH_HOTPLUG_DEVICE_NAME, GFP_KERNEL);
    card = SLASH_CARD_FROM_MINOR(minor);
    if (minor & 1)
        return kasprintf(GFP_KERNEL, SLASH_CTLDEV_NODENAME_FMT, card);
    return kasprintf(GFP_KERNEL, SLASH_QDMA_CTLDEV_NODENAME_FMT, card);
}

int __init slash_chrdev_init(void)
{
    int err;

    err = alloc_chrdev_region(&slash_chrdev_base, 0, SLASH_CHRDEV_MINORS,
                              SLASH_NAME);
    if (err)
        return err;

    slash_chrdev_class = slash_class_create(SLASH_NAME);
    if (IS_ERR(slash_chrdev_class)) {
        err = PTR_ERR(slash_chrdev_class);
        slash_chrdev_class = NULL;
        unregister_chrdev_region(slash_chrdev_base, SLASH_CHRDEV_MINORS);
        return err;
    }

    slash_chrdev_class->devnode = slash_chrdev_devnode;
    return 0;
}

void slash_chrdev_exit(void)
{
    struct slash_board_slot *entry;
    struct slash_board_slot *tmp;

    class_destroy(slash_chrdev_class);
    slash_chrdev_class = NULL;
    unregister_chrdev_region(slash_chrdev_base, SLASH_CHRDEV_MINORS);

    mutex_lock(&slash_board_slots_lock);
    list_for_each_entry_safe(entry, tmp, &slash_board_slots, node) {
        list_del(&entry->node);
        kfree(entry);
    }
    slash_board_slot_count = 0;
    mutex_unlock(&slash_board_slots_lock);
}

struct device *slash_chrdev_add(struct cdev *cdev,
                                const struct file_operations *fops,
                                unsigned int minor,
                                struct device *parent,
                                void *drvdata,
                                const char *name)
{
    struct device *device;
    dev_t devt = MKDEV(MAJOR(slash_chrdev_base), minor);
    int err;

    cdev_init(cdev, fops);
    cdev->owner = fops->owner;

    err = cdev_add(cdev, devt, 1);
    if (err)
        return ERR_PTR(err);

    device = device_create(slash_chrdev_class, parent, devt, drvdata,
                           "%s", name);
    if (IS_ERR(device))
        cdev_del(cdev);

    return device;
}

void slash_chrdev_del(struct cdev *cdev, struct device *device)
{
    device_destroy(slash_chrdev_class, device->devt);
    cdev_del(cdev);
}

int slash_chrdev_board_get(struct pci_dev *pdev)
{
    struct slash_board_slot *entry;
    unsigned int domain = pci_domain_nr(pdev->bus);
    unsigned int bus = pdev->bus->number;
    unsigned int slot = PCI_SLOT(pdev->devfn);
    unsigned int function = PCI_FUNC(pdev->devfn);
    int number;

    mutex_lock(&slash_board_slots_lock);

    list_for_each_entry(entry, &slash_board_slots, node) {
        if (entry->domain != domain || entry->bus != bus ||
            entry->slot != slot)
            continue;

        if (entry->active_pfs & BIT(function)) {
            mutex_unlock(&slash_board_slots_lock);
            return -EBUSY;
        }

        entry->active_pfs |= BIT(function);
        number = entry->number;
        mutex_unlock(&slash_board_slots_lock);
        return number;
    }

    if (slash_board_slot_count == SLASH_MAX_CARDS) {
        mutex_unlock(&slash_board_slots_lock);
        return -ENOSPC;
    }

    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry) {
        mutex_unlock(&slash_board_slots_lock);
        return -ENOMEM;
    }

    entry->domain = domain;
    entry->bus = bus;
    entry->slot = slot;
    entry->number = slash_board_slot_count++;
    entry->active_pfs = BIT(function);
    list_add_tail(&entry->node, &slash_board_slots);

    number = entry->number;
    mutex_unlock(&slash_board_slots_lock);
    return number;
}

void slash_chrdev_board_put(struct pci_dev *pdev)
{
    struct slash_board_slot *entry;
    unsigned int domain = pci_domain_nr(pdev->bus);
    unsigned int bus = pdev->bus->number;
    unsigned int slot = PCI_SLOT(pdev->devfn);
    unsigned int function = PCI_FUNC(pdev->devfn);

    mutex_lock(&slash_board_slots_lock);

    list_for_each_entry(entry, &slash_board_slots, node) {
        if (entry->domain != domain || entry->bus != bus ||
            entry->slot != slot)
            continue;

        entry->active_pfs &= ~BIT(function);
        mutex_unlock(&slash_board_slots_lock);
        return;
    }

    mutex_unlock(&slash_board_slots_lock);
    dev_warn(&pdev->dev, "character-device board slot was not allocated\n");
}

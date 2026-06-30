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
 * @file slash_dmabuf.c
 *
 * DMA-buf exporter for PCI BAR regions.
 *
 * This file implements a dma-buf wrapper around a PCI BAR, allowing
 * userspace to obtain a file descriptor (via the control device ioctl)
 * and mmap the BAR for direct MMIO register access.
 *
 * For BARs that are registered with pci_p2pdma_add_resource(), this
 * exporter also supports kernel-side dma-buf attachment and mapping so
 * peer devices can issue P2P DMA transactions directly to the BAR.
 *
 * Phase-1 policy in this driver serializes userspace mmap access and
 * P2P DMA access: both modes are supported, but not simultaneously.
 *
 * Cache attribute selection:
 *   - **Prefetchable BARs** → write-combine mapping (pgprot_writecombine).
 *     Suitable for frame buffers or bulk data regions where write
 *     coalescing improves throughput.
 *   - **Non-prefetchable BARs** → device/uncached mapping (pgprot_device).
 *     Required for control registers where every write must hit the
 *     device immediately and in order.
 */

#include "slash_dmabuf.h"

#include "slash.h"

#include <linux/atomic.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#ifdef CONFIG_PCI_P2PDMA
#include <linux/pci-p2pdma.h>
#endif
#include <linux/printk.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

/**
 * struct slash_bar_dmabuf_data - Private data attached to each BAR dma-buf.
 * @bar_number: Which PCI BAR (0-5) this dma-buf represents.
 * @len:        Size of the BAR region in bytes.
 * @pdev:       PCI device owning the BAR.  Held via pci_dev_get()
 *              for the lifetime of this struct.
 * @p2pdma_registered: True when this BAR was successfully registered via
 *                     pci_p2pdma_add_resource().
 * @cpu_mmap_count: Number of active userspace VMAs for this BAR.
 * @p2p_map_count: Number of active P2P sg mappings exported to importers.
 * @remove_in_progress: Set while remove path is draining this dmabuf.
 * @mode_lock: Serializes access-mode checks and state transitions.
 * @p2p_idle_wq: Woken when p2p_map_count transitions to zero.
 */
struct slash_bar_dmabuf_data {
    int bar_number;
    resource_size_t len;

    struct pci_dev *pdev;

    bool p2pdma_registered;
    atomic_t cpu_mmap_count;
    atomic_t p2p_map_count;
    atomic_t remove_in_progress;
    struct mutex mode_lock;
    wait_queue_head_t p2p_idle_wq;
};

static int slash_bar_dmabuf_attach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
    struct slash_bar_dmabuf_data *priv = dmabuf->priv;

#ifdef CONFIG_PCI_P2PDMA
    struct device *clients[1] = { attach->dev };

    mutex_lock(&priv->mode_lock);

    if (atomic_read(&priv->remove_in_progress)) {
        mutex_unlock(&priv->mode_lock);
        return -ENODEV;
    }

    if (!priv->p2pdma_registered) {
        mutex_unlock(&priv->mode_lock);
        dev_warn_ratelimited(&priv->pdev->dev,
                             "slash: BAR%d dma-buf attach rejected: "
                             "BAR not registered with pci_p2pdma_add_resource() "
                             "(BAR not prefetchable, or kernel lacks CONFIG_PCI_P2PDMA)\n",
                             priv->bar_number);
        return -EOPNOTSUPP;
    }

    if (atomic_read(&priv->cpu_mmap_count) > 0) {
        mutex_unlock(&priv->mode_lock);
        dev_warn_ratelimited(&priv->pdev->dev,
                             "slash: BAR%d dma-buf attach rejected: "
                             "%d active CPU mmap(s) (Phase-1 exclusivity)\n",
                             priv->bar_number,
                             atomic_read(&priv->cpu_mmap_count));
        return -EBUSY;
    }

    mutex_unlock(&priv->mode_lock);

    if (pci_p2pdma_distance_many(priv->pdev, clients, ARRAY_SIZE(clients), true) < 0) {
        dev_warn(attach->dev,
                 "slash: P2P not supported for BAR%d topology\n",
                 priv->bar_number);
        return -EOPNOTSUPP;
    }

    mutex_lock(&priv->mode_lock);
    if (atomic_read(&priv->remove_in_progress)) {
        mutex_unlock(&priv->mode_lock);
        return -ENODEV;
    }
    if (atomic_read(&priv->cpu_mmap_count) > 0) {
        mutex_unlock(&priv->mode_lock);
        return -EBUSY;
    }
    mutex_unlock(&priv->mode_lock);

    return 0;
#else
    dev_warn_once(attach->dev,
                  "slash: BAR%d dma-buf attach rejected: "
                  "kernel built without CONFIG_PCI_P2PDMA\n",
                  priv->bar_number);
    return -EOPNOTSUPP;
#endif
}

static void slash_bar_dmabuf_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
    struct slash_bar_dmabuf_data *priv = dmabuf->priv;

    dev_dbg(attach->dev, "slash: dmabuf detach BAR%d\n", priv->bar_number);
}

static struct sg_table *slash_bar_dmabuf_map(struct dma_buf_attachment *attach,
                                        enum dma_data_direction dir)
{
    struct slash_bar_dmabuf_data *priv = attach->dmabuf->priv;

#ifdef CONFIG_PCI_P2PDMA
    resource_size_t bar_start;
    unsigned long first_pfn;
    unsigned long page_off;
    unsigned long span;
    unsigned long npages;
    unsigned long i;
    struct page **pages;
    struct sg_table *sgt;
    int ret;

    mutex_lock(&priv->mode_lock);
    if (atomic_read(&priv->remove_in_progress)) {
        mutex_unlock(&priv->mode_lock);
        return ERR_PTR(-ENODEV);
    }
    if (!priv->p2pdma_registered) {
        mutex_unlock(&priv->mode_lock);
        return ERR_PTR(-EOPNOTSUPP);
    }
    if (atomic_read(&priv->cpu_mmap_count) > 0) {
        mutex_unlock(&priv->mode_lock);
        return ERR_PTR(-EBUSY);
    }
    mutex_unlock(&priv->mode_lock);

    bar_start = pci_resource_start(priv->pdev, priv->bar_number);
    page_off = offset_in_page(bar_start);
    span = page_off + priv->len;
    npages = DIV_ROUND_UP(span, PAGE_SIZE);

    if (npages > INT_MAX)
        return ERR_PTR(-E2BIG);

    pages = kvmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
    if (!pages)
        return ERR_PTR(-ENOMEM);

    first_pfn = PFN_DOWN(bar_start);
    for (i = 0; i < npages; i++) {
        pages[i] = pfn_to_page(first_pfn + i);
    }

    sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
    if (!sgt) {
        kvfree(pages);
        return ERR_PTR(-ENOMEM);
    }

    ret = sg_alloc_table_from_pages(sgt, pages, npages, page_off, priv->len,
                                    GFP_KERNEL);
    kvfree(pages);
    if (ret) {
        kfree(sgt);
        return ERR_PTR(ret);
    }

    ret = dma_map_sgtable(attach->dev, sgt, dir, DMA_ATTR_SKIP_CPU_SYNC);
    if (ret) {
        sg_free_table(sgt);
        kfree(sgt);
        return ERR_PTR(ret);
    }

    atomic_inc(&priv->p2p_map_count);

    return sgt;
#else
    dev_dbg(attach->dev, "%s: CONFIG_PCI_P2PDMA disabled", SLASH_NAME);
    return ERR_PTR(-EOPNOTSUPP);
#endif
}

static void slash_bar_dmabuf_unmap(struct dma_buf_attachment *attach,
                              struct sg_table *sgl, enum dma_data_direction dir)
{
    struct slash_bar_dmabuf_data *priv = attach->dmabuf->priv;

    if (!sgl)
        return;

    dma_unmap_sgtable(attach->dev, sgl, dir, DMA_ATTR_SKIP_CPU_SYNC);
    sg_free_table(sgl);
    kfree(sgl);

    if (atomic_read(&priv->p2p_map_count) <= 0) {
        dev_warn(&priv->pdev->dev,
                 "slash: BAR%d unmap underflow in p2p_map_count\n",
                 priv->bar_number);
        return;
    }

    if (atomic_dec_and_test(&priv->p2p_map_count))
        wake_up_all(&priv->p2p_idle_wq);
}

/**
 * slash_bar_dmabuf_fault() - Page-fault handler for BAR mappings.
 * @vmf: Fault information provided by the kernel.
 *
 * Called on the first access to each page in the VMA.  Computes the
 * physical page frame number (PFN) for the faulting address within
 * the PCI BAR and inserts it into the page tables via vmf_insert_pfn().
 *
 * This avoids io_remap_pfn_range(), whose remap_pfn_range() security
 * path requires CAP_SYS_RAWIO.  The fault-based approach (VM_PFNMAP +
 * vmf_insert_pfn) is the standard pattern used by DRM/GPU and VFIO
 * drivers for mapping device I/O memory to unprivileged userspace.
 *
 * Lifetime safety: priv->pdev is held via pci_dev_get() for the lifetime
 * of the dma-buf.  The VMA holds a file reference on the dma-buf, so priv
 * and priv->pdev remain valid for any fault during the VMA's lifetime.
 * After device removal pci_resource_start() returns stale-but-valid cached
 * values from the pci_dev struct; MMIO reads will return 0xFFFFFFFF (PCIe
 * completion timeout) which is the expected degraded behavior.
 *
 * Return: VM_FAULT_NOPAGE on success, VM_FAULT_SIGBUS on out-of-range
 *         access or insertion failure.
 */
static vm_fault_t slash_bar_dmabuf_fault(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    struct slash_bar_dmabuf_data *priv = vma->vm_private_data;
    unsigned long page_index;
    unsigned long obj_pgoff;
    resource_size_t bar_start;
    unsigned long pfn;

    /* Page offset within the VMA (0 for the first page of the mapping). */
    page_index = (vmf->address - vma->vm_start) >> PAGE_SHIFT;

    /* BAR-relative page offset: mmap offset + position within mapping. */
    obj_pgoff = vma->vm_pgoff + page_index;

    /* Bounds check: do not map beyond the physical BAR. */
    if ((obj_pgoff << PAGE_SHIFT) >= priv->len)
        return VM_FAULT_SIGBUS;

    bar_start = pci_resource_start(priv->pdev, priv->bar_number);
    pfn = (bar_start >> PAGE_SHIFT) + obj_pgoff;

    return vmf_insert_pfn(vma, vmf->address, pfn);
}

static void slash_bar_dmabuf_vma_close(struct vm_area_struct *vma)
{
    struct slash_bar_dmabuf_data *priv = vma->vm_private_data;

    mutex_lock(&priv->mode_lock);
    if (atomic_read(&priv->cpu_mmap_count) > 0)
        atomic_dec(&priv->cpu_mmap_count);
    else
        dev_warn(&priv->pdev->dev,
                 "slash: BAR%d cpu_mmap_count underflow on close\n",
                 priv->bar_number);
    mutex_unlock(&priv->mode_lock);
}

static const struct vm_operations_struct slash_bar_dmabuf_vm_ops = {
    .fault = slash_bar_dmabuf_fault,
    .close = slash_bar_dmabuf_vma_close,
};

/**
 * slash_bar_dmabuf_mmap() - Set up a BAR region for fault-based mapping.
 * @dmabuf: The BAR dma-buf being mapped.
 * @vma:    The VMA describing the mapping request.
 *
 * Configures the VMA with appropriate flags, cache attributes, and a
 * custom vm_operations_struct whose .fault handler uses vmf_insert_pfn()
 * to lazily insert PFNs on first access.  This avoids
 * io_remap_pfn_range() and its CAP_SYS_RAWIO requirement, allowing
 * unprivileged userspace to mmap BAR regions.
 *
 * Cache attribute selection:
 *   - Prefetchable BAR → write-combine (pgprot_writecombine): allows
 *     the CPU to coalesce writes for better throughput on bulk data BARs.
 *   - Non-prefetchable BAR → device/uncached (pgprot_device): strict
 *     ordering for control registers.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_bar_dmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
    struct slash_bar_dmabuf_data *priv = dmabuf->priv;
    unsigned long size = vma->vm_end - vma->vm_start;
    u64 offset = (u64)vma->vm_pgoff << PAGE_SHIFT;
    bool wc;
    int ret = 0;

    /* Ensure the requested range lies fully within the BAR. */
    if (offset > priv->len || size > priv->len - offset)
        return -EINVAL;

    mutex_lock(&priv->mode_lock);

    if (atomic_read(&priv->remove_in_progress)) {
        ret = -ENODEV;
        goto out_unlock;
    }

    if (atomic_read(&priv->p2p_map_count) > 0) {
        ret = -EBUSY;
        goto out_unlock;
    }

    atomic_inc(&priv->cpu_mmap_count);
    mutex_unlock(&priv->mode_lock);

    /*
     * VM_PFNMAP    — raw PFN mapping, required for vmf_insert_pfn().
     * VM_IO        — I/O memory (blocks /proc/pid/mem, core dump).
     * VM_DONTDUMP  — explicit core-dump exclusion (redundant w/ VM_IO).
     * VM_DONTEXPAND — prevents mremap beyond BAR boundary.
     * VM_DONTCOPY  — do not inherit across fork(); BAR register
     *                mappings should not be silently shared with children.
     */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    vm_flags_set(vma, VM_PFNMAP | VM_IO | VM_DONTDUMP |
                      VM_DONTEXPAND | VM_DONTCOPY);
#else
    vma->vm_flags |= VM_PFNMAP | VM_IO | VM_DONTDUMP |
                     VM_DONTEXPAND | VM_DONTCOPY;
#endif

    wc = !!(pci_resource_flags(priv->pdev, priv->bar_number) & IORESOURCE_PREFETCH);
    vma->vm_page_prot = wc ? pgprot_writecombine(vma->vm_page_prot)
                           : pgprot_device(vma->vm_page_prot);

    vma->vm_ops = &slash_bar_dmabuf_vm_ops;
    vma->vm_private_data = priv;

    dev_dbg(&priv->pdev->dev,
            "slash: mmap BAR%d wc=%d pgoff=0x%lx len=0x%lx (fault-based)\n",
            priv->bar_number, wc, vma->vm_pgoff, size);

    return 0;

out_unlock:
    mutex_unlock(&priv->mode_lock);
    return ret;
}

/**
 * slash_bar_dmabuf_release() - Free resources when all references are dropped.
 * @dmabuf: The BAR dma-buf being released.
 *
 * Drops the PCI device reference taken in slash_bar_dmabuf_create()
 * and frees the private data.
 */
static void slash_bar_dmabuf_release(struct dma_buf *dmabuf)
{
    struct slash_bar_dmabuf_data *priv = dmabuf->priv;

    dev_dbg(&priv->pdev->dev, "slash: dmabuf release (BAR%d)\n", priv->bar_number);

    if (atomic_read(&priv->p2p_map_count) > 0) {
        dev_warn(&priv->pdev->dev,
                 "slash: BAR%d released with %d active P2P maps\n",
                 priv->bar_number,
                 atomic_read(&priv->p2p_map_count));
    }

    if (atomic_read(&priv->cpu_mmap_count) > 0) {
        dev_warn(&priv->pdev->dev,
                 "slash: BAR%d released with %d active CPU mmaps\n",
                 priv->bar_number,
                 atomic_read(&priv->cpu_mmap_count));
    }

    pci_dev_put(priv->pdev);
    kfree(priv);
}

static const struct dma_buf_ops slash_bar_dmabuf_ops = {
    .attach        = slash_bar_dmabuf_attach,
    .detach        = slash_bar_dmabuf_detach,
    .map_dma_buf   = slash_bar_dmabuf_map,
    .unmap_dma_buf = slash_bar_dmabuf_unmap,
    .mmap          = slash_bar_dmabuf_mmap,
    .release       = slash_bar_dmabuf_release,
};

/**
 * slash_bar_dmabuf_create() - Export a PCI BAR as a dma-buf.
 * @pdev:       PCI device owning the BAR.
 * @bar_number: BAR index (0-5).  Must be present and MMIO.
 * @p2pdma_registered: True when this BAR has P2PDMA page backing.
 *
 * Allocates private state, takes a reference on @pdev, and registers a
 * dma-buf exporter.  The DEFINE_DMA_BUF_EXPORT_INFO macro initializes
 * the export info struct with sensible defaults; we override ops, size,
 * flags, priv, and exp_name.
 *
 * Return: Pointer to the new dma_buf on success, ERR_PTR on failure.
 */
struct dma_buf *slash_bar_dmabuf_create(struct pci_dev *pdev, int bar_number,
                                        bool p2pdma_registered)
{
    long err;
    resource_size_t len;
    DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
    struct dma_buf *dmabuf;
    struct slash_bar_dmabuf_data *priv;

    if (bar_number < 0 || bar_number >= PCI_STD_NUM_BARS) {
        dev_err(&pdev->dev, "slash: invalid BAR %d\n", bar_number);
        return ERR_PTR(-EINVAL);
    }
    if (!pci_resource_start(pdev, bar_number)) {
        dev_err(&pdev->dev, "slash: BAR%d not present\n", bar_number);
        return ERR_PTR(-ENODEV);
    }
    if ((pci_resource_flags(pdev, bar_number) & IORESOURCE_MEM) == 0) {
        dev_err(&pdev->dev, "slash: BAR%d is not MMIO\n", bar_number);
        return ERR_PTR(-ENODEV);
    }

    len = pci_resource_len(pdev, bar_number);

    dev_dbg(&pdev->dev, "slash: exporting BAR%d as dma-buf (size=%pa)\n", bar_number, &len);

    priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv) {
        dev_err(&pdev->dev, "slash: kzalloc(priv) failed\n");
        return ERR_PTR(-ENOMEM);
    }

    priv->bar_number = bar_number;
    priv->len = len;
    priv->p2pdma_registered = p2pdma_registered;

    atomic_set(&priv->cpu_mmap_count, 0);
    atomic_set(&priv->p2p_map_count, 0);
    atomic_set(&priv->remove_in_progress, 0);
    mutex_init(&priv->mode_lock);
    init_waitqueue_head(&priv->p2p_idle_wq);

    /* Hold a PCI device reference for the lifetime of the dma-buf. */
    priv->pdev = pci_dev_get(pdev);

    exp_info.ops = &slash_bar_dmabuf_ops;
    exp_info.size = len;
    exp_info.flags = O_RDWR;
    exp_info.priv = priv;
    exp_info.exp_name = SLASH_NAME;

    dmabuf = dma_buf_export(&exp_info);
    if (IS_ERR(dmabuf)) {
        err = PTR_ERR(dmabuf);
        dev_err(&pdev->dev, "slash: dma_buf_export failed: %ld\n", err);
        goto err_free_priv;
    }

    dev_info(&pdev->dev, "slash: BAR%d exported as dma-buf (size=%pa)\n", bar_number, &len);
    return dmabuf;

err_free_priv:
    pci_dev_put(priv->pdev);
    kfree(priv);

    return ERR_PTR(err);
}

void slash_bar_dmabuf_begin_remove(struct dma_buf *dmabuf)
{
    struct slash_bar_dmabuf_data *priv;

    if (!dmabuf)
        return;

    priv = dmabuf->priv;
    atomic_set(&priv->remove_in_progress, 1);
}

void slash_bar_dmabuf_invalidate(struct dma_buf *dmabuf)
{
    if (!dmabuf)
        return;

    slash_bar_dmabuf_begin_remove(dmabuf);
    dma_buf_move_notify(dmabuf);
}

bool slash_bar_dmabuf_wait_p2p_idle(struct dma_buf *dmabuf,
                                    unsigned long timeout_jiffies)
{
    struct slash_bar_dmabuf_data *priv;
    long ret;

    if (!dmabuf)
        return true;

    priv = dmabuf->priv;

    if (atomic_read(&priv->p2p_map_count) == 0)
        return true;

    ret = wait_event_timeout(priv->p2p_idle_wq,
                             atomic_read(&priv->p2p_map_count) == 0,
                             timeout_jiffies);

    if (ret > 0)
        return true;

    return atomic_read(&priv->p2p_map_count) == 0;
}

unsigned int slash_bar_dmabuf_p2p_map_count(struct dma_buf *dmabuf)
{
    struct slash_bar_dmabuf_data *priv;

    if (!dmabuf)
        return 0;

    priv = dmabuf->priv;
    return atomic_read(&priv->p2p_map_count);
}

bool slash_bar_dmabuf_in_use(struct dma_buf *dmabuf)
{
    struct slash_bar_dmabuf_data *priv;

    if (!dmabuf)
        return false;

    priv = dmabuf->priv;
    return atomic_read(&priv->cpu_mmap_count) > 0 ||
           atomic_read(&priv->p2p_map_count) > 0;
}

/**
 * slash_bar_dmabuf_destroy() - Release the driver's reference on a BAR dma-buf.
 * @dmabuf: dma-buf returned by slash_bar_dmabuf_create().
 *
 * Drops one reference.  The dma-buf (and its private data) are actually
 * freed only when the last holder — including any userspace fd — closes.
 */
void slash_bar_dmabuf_destroy(struct dma_buf *dmabuf)
{
    pr_debug("slash: dmabuf_destroy()\n");
    dma_buf_put(dmabuf);
}

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
 * @file slash_dmabuf.h
 *
 * DMA-buf exporter for PCI BAR regions.
 *
 * Provides a dma-buf wrapper around a PCI BAR so that userspace can
 * obtain a file descriptor and mmap the BAR for direct MMIO access.
 * If P2PDMA registration is available for the BAR, kernel-side device
 * attachment is also supported for peer-to-peer DMA imports.
 */

#ifndef SLASH_DMABUF_H
#define SLASH_DMABUF_H

#include <linux/dma-buf.h>
#include <linux/pci.h>

/**
 * slash_bar_dmabuf_create() - Export a PCI BAR as a dma-buf.
 * @pdev:       PCI device owning the BAR.
 * @bar_number: BAR index (0-5).  Must be a valid MMIO BAR.
 * @p2pdma_registered: true if pci_p2pdma_add_resource() succeeded for
 *                     this BAR.
 *
 * Creates a dma-buf exporter backed by the physical address range of
 * the specified BAR.  Takes a reference on @pdev (via pci_dev_get())
 * that is released when the dma-buf is freed.
 *
 * Return: Pointer to the new dma_buf on success, ERR_PTR on failure.
 */
struct dma_buf *slash_bar_dmabuf_create(struct pci_dev *pdev, int bar_number,
                                        bool p2pdma_registered);

/**
 * slash_bar_dmabuf_destroy() - Release a BAR dma-buf.
 * @dmabuf: dma-buf returned by slash_bar_dmabuf_create().
 *
 * Drops the driver's reference on the dma-buf.  The underlying
 * resources (PCI device reference, private data) are freed when the
 * last user closes their fd / drops their reference.
 */
void slash_bar_dmabuf_destroy(struct dma_buf *dmabuf);

/** Mark a BAR dma-buf as being in remove path and reject new mappings. */
void slash_bar_dmabuf_begin_remove(struct dma_buf *dmabuf);

/** Notify importers to drop mappings through dynamic-attach move_notify. */
void slash_bar_dmabuf_invalidate(struct dma_buf *dmabuf);

/** Wait for active P2P maps to drain. Returns true when idle before timeout. */
bool slash_bar_dmabuf_wait_p2p_idle(struct dma_buf *dmabuf,
                                    unsigned long timeout_jiffies);

/** Snapshot active P2P map count for diagnostics. */
unsigned int slash_bar_dmabuf_p2p_map_count(struct dma_buf *dmabuf);

/** Returns true when BAR dmabuf has active CPU or P2P mappings. */
bool slash_bar_dmabuf_in_use(struct dma_buf *dmabuf);

#endif /* SLASH_DMABUF_H */

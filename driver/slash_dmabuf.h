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
 * Only userspace mmap is supported; kernel-side device attachment is
 * intentionally rejected because a PCI BAR is device I/O memory, not
 * system RAM.
 */

#ifndef SLASH_DMABUF_H
#define SLASH_DMABUF_H

#include <linux/dma-buf.h>
#include <linux/pci.h>

/**
 * slash_bar_dmabuf_create() - Export a PCI BAR as a dma-buf.
 * @pdev:       PCI device owning the BAR.
 * @bar_number: BAR index (0-5).  Must be a valid MMIO BAR.
 *
 * Creates a dma-buf exporter backed by the physical address range of
 * the specified BAR.  Takes a reference on @pdev (via pci_dev_get())
 * that is released when the dma-buf is freed.
 *
 * Return: Pointer to the new dma_buf on success, ERR_PTR on failure.
 */
struct dma_buf *slash_bar_dmabuf_create(struct pci_dev *pdev, int bar_number);

/**
 * slash_bar_dmabuf_destroy() - Release a BAR dma-buf.
 * @dmabuf: dma-buf returned by slash_bar_dmabuf_create().
 *
 * Drops the driver's reference on the dma-buf.  The underlying
 * resources (PCI device reference, private data) are freed when the
 * last user closes their fd / drops their reference.
 */
void slash_bar_dmabuf_destroy(struct dma_buf *dmabuf);

#endif /* SLASH_DMABUF_H */

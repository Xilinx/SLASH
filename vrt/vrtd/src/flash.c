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
 * @file flash.c
 * @brief AMI cfgmem programming backend for vrtd.
 */

#define _GNU_SOURCE

#include "flash.h"

#include <pthread.h>
#include <stdio.h>
#include <sys/syslog.h>

#include <ami.h>
#include <ami_device.h>
#include <ami_program.h>
#include <vrtd/wire.h>

#include "device.h"
#include "hotplug.h"
#include "reset.h"
#include "utils.h"

static pthread_mutex_t g_progress_mutex = PTHREAD_MUTEX_INITIALIZER;
static cfgmem_progress_callback g_progress_cb;
static void *g_progress_ctx;

static void cfgmem_set_progress_sink(cfgmem_progress_callback progress_cb, void *progress_ctx)
{
    (void) pthread_mutex_lock(&g_progress_mutex);
    g_progress_cb = progress_cb;
    g_progress_ctx = progress_ctx;
    (void) pthread_mutex_unlock(&g_progress_mutex);
}

static void cfgmem_emit_progress(
    uint32_t phase,
    uint64_t bytes_written,
    uint64_t bytes_total
)
{
    (void) pthread_mutex_lock(&g_progress_mutex);
    cfgmem_progress_callback progress_cb = g_progress_cb;
    void *progress_ctx = g_progress_ctx;
    (void) pthread_mutex_unlock(&g_progress_mutex);

    if (progress_cb != NULL) {
        progress_cb(progress_ctx, phase, bytes_written, bytes_total);
    }
}

/**
 * Progress callback for ami_prog_download_pdi().
 *
 * AMI runs this from an internal watcher thread for the duration of the PDI
 * download.  @p data points at AMI's @ref ami_pdi_progress, whose
 * @c bytes_to_write is pre-populated; on each successful event @p ctr carries
 * the bytes covered by that event, which we accumulate into @c bytes_written
 * (matching AMI's own reference handlers).
 *
 * The download runs for minutes with no other output, so we journal the
 * completed percentage to give operators visibility via `journalctl -u vrtd`.
 * To avoid flooding the journal we only emit a line when the completed
 * percentage crosses the next 10% mark, using @c reserved (which AMI leaves
 * for handler use) to carry the last-logged decile across calls.
 */
static void cfgmem_program_progress_handler(enum ami_event_status status, uint64_t ctr, void *data)
{
    struct ami_pdi_progress *prog = data;
    if (prog == NULL) {
        return;
    }

    if (status == AMI_EVENT_STATUS_OK) {
        prog->bytes_written += (uint32_t)ctr;
    }

    if (prog->bytes_to_write == 0) {
        return;
    }

    if (prog->bytes_written > prog->bytes_to_write) {
        prog->bytes_written = prog->bytes_to_write;
    }

    unsigned int percent = (unsigned int)(((uint64_t)prog->bytes_written * 100ULL) / prog->bytes_to_write);
    unsigned int decile = percent / 10u;
    unsigned int last_decile = (unsigned int)prog->reserved;

    cfgmem_emit_progress(
        VRTD_CFGMEM_PROGRAM_PHASE_DOWNLOADING_PDI,
        prog->bytes_written,
        prog->bytes_to_write
    );

    if (decile > last_decile) {
        prog->reserved = decile;
        LOG(LOG_INFO, "cfgmem_program_with_ami: PDI download %u%% (%u/%u bytes)",
            percent, (unsigned int)prog->bytes_written, (unsigned int)prog->bytes_to_write);
    }
}

uint16_t cfgmem_program_with_ami(
    struct device *device,
    struct device_ptr_array *devices,
    int input_fd,
    uint8_t boot_device,
    uint32_t partition,
    cfgmem_progress_callback progress_cb,
    void *progress_ctx
)
{
    if (device == NULL || devices == NULL || input_fd < 0) {
        return VRTD_RET_BAD_REQUEST;
    }

    char pf0_bdf[VRTD_PCI_BDF_LEN] = {0};
    int ret = pci_bdf_set_function(device->pci_info.bdf, 0, pf0_bdf);
    if (ret != 0) {
        LOG(LOG_ERR, "cfgmem_program_with_ami: failed to compute PF0 BDF from %s", device->pci_info.bdf);
        return VRTD_RET_INTERNAL_ERROR;
    }

    struct ami_device *ami_device = NULL;
    if (progress_cb != NULL) {
        progress_cb(
            progress_ctx,
            VRTD_CFGMEM_PROGRAM_PHASE_OPENING_AMI,
            0,
            0
        );
    }

    ret = ami_dev_find(pf0_bdf, &ami_device);
    if (ret != AMI_STATUS_OK) {
        LOG(LOG_ERR, "cfgmem_program_with_ami: ami_dev_find(%s) failed: %s", pf0_bdf, ami_get_last_error());
        return VRTD_RET_INTERNAL_ERROR;
    }

    ret = ami_dev_request_access(ami_device);
    if (ret != AMI_STATUS_OK) {
        LOG(LOG_ERR, "cfgmem_program_with_ami: ami_dev_request_access(%s) failed: %s", pf0_bdf, ami_get_last_error());
        ami_dev_delete(&ami_device);
        return VRTD_RET_INTERNAL_ERROR;
    }

    char fd_path[64] = {0};
    ret = snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", input_fd);
    if (ret < 0 || (size_t)ret >= sizeof(fd_path)) {
        LOG(LOG_ERR, "cfgmem_program_with_ami: failed to build fd path for fd %d", input_fd);
        ami_dev_delete(&ami_device);
        return VRTD_RET_INTERNAL_ERROR;
    }

    LOG(LOG_INFO, "cfgmem_program_with_ami: programming %s boot_device=%u partition=%u",
        pf0_bdf, (unsigned int)boot_device, (unsigned int)partition);

    cfgmem_set_progress_sink(progress_cb, progress_ctx);
    ret = ami_prog_download_pdi(ami_device, fd_path, boot_device, partition,
        cfgmem_program_progress_handler);
    cfgmem_set_progress_sink(NULL, NULL);
    if (ret != AMI_STATUS_OK) {
        LOG(LOG_ERR, "cfgmem_program_with_ami: ami_prog_download_pdi(%s, partition=%u) failed: %s",
            pf0_bdf, (unsigned int)partition, ami_get_last_error());
        ami_dev_delete(&ami_device);
        return VRTD_RET_INTERNAL_ERROR;
    }

    ami_dev_delete(&ami_device);

    LOG(LOG_INFO, "cfgmem_program_with_ami: programming complete for %s, resetting into partition %u",
        pf0_bdf, (unsigned int)partition);

    return reset_with_ami_partition_progress(device, devices, partition, progress_cb, progress_ctx);
}

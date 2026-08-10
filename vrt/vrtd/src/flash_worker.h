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
 * @file flash_worker.h
 * @brief Asynchronous cfgmem programming and reset execution for SLASH devices.
 *
 * Programming a device's configuration memory and resetting a board both take
 * long enough to block the daemon's single-threaded sd-event loop.  The AMI
 * PDI download runs for minutes, and the subsequent SBR-based reset sequence
 * adds tens of seconds of settle/rescan waits.  Running that work directly
 * inside the event loop would starve the systemd watchdog keepalive (sent at
 * half of WatchdogSec) and gets vrtd killed with SIGABRT.
 *
 * The flash_worker offloads @ref cfgmem_program_with_ami and standalone
 * reset jobs to a dedicated background thread and exposes an async polling API,
 * mirroring the design writer:
 *
 *   1. @c flash_worker_submit_async / @c flash_worker_submit_reset_async --
 *      hand off the job parameters to the background thread.  Return
 *      immediately.
 *   2. @c flash_worker_poll_result -- non-blocking check: has the job
 *      finished?  If yes, retrieves the VRTD_RET_* result code.
 *
 * Only one job may run at a time: reset mutates the daemon's shared device
 * list, so the caller must also ensure no other request that touches device
 * state runs concurrently.
 */

#ifndef VRTD_FLASH_WORKER_H
#define VRTD_FLASH_WORKER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <vrtd/wire.h>

struct device;
struct device_ptr_array;

/** @brief Opaque handle for the asynchronous cfgmem programming worker. */
struct flash_worker;

/**
 * @brief Create a flash worker and start its background thread.
 *
 * @return Heap-allocated flash_worker on success, NULL on failure.  The caller
 *         must eventually release it with cleanup_flash_worker().
 */
struct flash_worker *flash_worker_create(void);

/**
 * @brief Submit a cfgmem programming job for asynchronous execution.
 *
 * Hands the parameters and PDI file descriptor to the background worker and
 * returns immediately.  On success, ownership of @p input_fd transfers to the
 * worker, which closes it when the job completes -- the caller must not close
 * it.  Only one job may be in flight at a time.
 *
 * @param worker       The flash worker instance.
 * @param device       The device to program (non-owning; must remain valid
 *                     until the job completes -- the reset step consumes it).
 * @param devices      The daemon's tracked device array (non-owning).
 * @param input_fd     Open file descriptor of the PDI to program.
 * @param boot_device  AMI boot-device selector.
 * @param partition    Flash partition to program and boot.
 * @param owner_conn_id Client connection ID that owns status polling for this job.
 * @param job_id_out   Output: job identifier for status polling.
 * @return 0 on success (job enqueued), -1 if the worker is busy/stopping or on
 *         invalid arguments.
 */
int flash_worker_submit_async(
    struct flash_worker *worker,
    struct device *device,
    struct device_ptr_array *devices,
    int input_fd,
    uint8_t boot_device,
    uint32_t partition,
    uint64_t owner_conn_id,
    uint64_t *job_id_out
);

/**
 * @brief Submit a reset job for asynchronous execution.
 *
 * Runs the same AMI/SBR reset flow used after cfgmem programming, without
 * programming a PDI first.  Only one job may be in flight at a time.
 *
 * @param worker        The flash worker instance.
 * @param device        The device to reset (non-owning; consumed by the reset
 *                      step when removed from @p devices).
 * @param devices       The daemon's tracked device array (non-owning).
 * @param partition     Flash partition to select and boot.
 * @param owner_conn_id Client connection ID that owns this job.
 * @param job_id_out    Output: job identifier for result/status bookkeeping.
 * @return 0 on success (job enqueued), -1 if the worker is busy/stopping or on
 *         invalid arguments.
 */
int flash_worker_submit_reset_async(
    struct flash_worker *worker,
    struct device *device,
    struct device_ptr_array *devices,
    uint32_t partition,
    uint64_t owner_conn_id,
    uint64_t *job_id_out
);

/**
 * @brief Poll the latest progress snapshot for a worker job.
 *
 * @param worker  The flash worker instance.
 * @param job_id  Job identifier returned by flash_worker_submit_async().
 * @param status  Output progress snapshot.
 * @return 0 on success, -1 on invalid arguments, mutex error, or unknown job.
 */
int flash_worker_poll_status(
    struct flash_worker *worker,
    uint64_t job_id,
    struct vrtd_cfgmem_program_status *status
);

/**
 * @brief Poll the latest progress snapshot for an owner-scoped worker job.
 *
 * @param worker        The flash worker instance.
 * @param job_id        Job identifier returned by flash_worker_submit_async().
 * @param owner_conn_id Client connection ID that submitted the job.
 * @param status        Output progress snapshot.
 * @return 0 on success, -1 on invalid arguments, mutex error, unknown job
 *         (errno=ENOENT), or wrong owner (errno=EACCES).
 */
int flash_worker_poll_status_for_owner(
    struct flash_worker *worker,
    uint64_t job_id,
    uint64_t owner_conn_id,
    struct vrtd_cfgmem_program_status *status
);

/**
 * @brief Poll for the result of an asynchronous worker job.
 *
 * @param worker  The flash worker instance.
 * @param done    Output: true if no job is in progress (completed or never
 *                started), false if still running.
 * @param result  Output: the VRTD_RET_* code produced by the last job, or
 *                VRTD_RET_INTERNAL_ERROR if the worker was stopped mid-job.
 * @return 0 on success, -1 on invalid arguments or mutex error.
 */
int flash_worker_poll_result(struct flash_worker *worker, bool *done, uint16_t *result);

/**
 * @brief Query whether a worker job is currently in progress.
 *
 * @param worker  The flash worker instance (NULL-safe: returns false).
 * @return true if a job is in flight, false otherwise.
 */
bool flash_worker_is_busy(struct flash_worker *worker);

/**
 * @brief Destroy a flash worker and free all associated resources.
 *
 * Signals the worker thread to stop, cancels and joins it, and frees the
 * struct.  NULL-safe.
 *
 * @param worker  The flash worker to destroy (may be NULL).
 */
void cleanup_flash_worker(struct flash_worker *worker);

/**
 * @brief Cleanup helper for use with __attribute__((cleanup)).
 * @param workerp Address of a @c struct @c flash_worker pointer.
 */
static inline
void cleanup_flash_workerp(struct flash_worker **workerp)
{
    if (workerp == NULL) {
        return;
    }

    cleanup_flash_worker(*workerp);
    *workerp = NULL;
}

#endif /* VRTD_FLASH_WORKER_H */

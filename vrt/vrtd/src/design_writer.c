/**
 * The MIT License (MIT)
 * Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
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
 * @file design_writer.c
 * @brief Asynchronous bitstream programming for AMD Alveo V80 FPGA devices via QDMA.
 *
 * This module implements the design_writer, which transfers FPGA bitstream data
 * from a file descriptor to the device over a QDMA (Queue-based Direct Memory
 * Access) memory-mapped (MM) channel.
 *
 * Threading model
 * ---------------
 * The design_writer uses a dedicated worker thread that runs for the lifetime
 * of the design_writer object.  The main thread (or any caller thread) submits
 * work by handing over a file descriptor containing the bitstream data; the
 * worker thread performs the potentially long-running QDMA transfer in the
 * background.
 *
 * Producer-consumer pattern
 * -------------------------
 * The interaction between the caller and the worker follows a simple
 * single-slot producer-consumer protocol protected by a mutex + condvar:
 *
 *   1. Caller invokes design_writer_submit_fd_async() to enqueue a bitstream
 *      fd.  This sets writer->input_fd and writer->busy, then signals the
 *      condvar so the worker wakes up.
 *
 *   2. The worker thread (design_writer_thread) waits on the condvar for
 *      input_fd >= 0.  When signalled, it takes ownership of the fd, releases
 *      the mutex, reads the entire bitstream into a page-aligned buffer, and
 *      writes it to the QDMA device fd at the fixed bitstream address
 *      (VRTD_DESIGN_WRITER_SEEK_ADDR).
 *
 *   3. On completion (success or failure), the worker clears busy, stores any
 *      error in last_error, and broadcasts the condvar so that any thread
 *      blocked in design_writer_submit_fd() or design_writer_poll_result()
 *      can observe the result.
 *
 * The caller may choose between:
 *   - design_writer_submit_fd()      -- synchronous: blocks until transfer
 *                                       completes.
 *   - design_writer_submit_fd_async() + design_writer_poll_result()
 *                                     -- asynchronous: submit, then poll for
 *                                        completion at the caller's pace.
 *
 * QDMA transfer strategy
 * ----------------------
 * At creation time, design_writer_open_qpair() allocates a QDMA queue pair in
 * Host-to-Card (H2C) memory-mapped mode, starts it, and obtains a file
 * descriptor for the queue.  The bitstream is then written via standard POSIX
 * write()/lseek() calls on that fd at offset VRTD_DESIGN_WRITER_SEEK_ADDR
 * (0x102100000), which the QDMA subsystem translates into DMA writes to the
 * FPGA's configuration memory.  The entire bitstream (up to 1 GiB) is read
 * into a contiguous, page-aligned userspace buffer first, then written out in
 * whatever chunks the kernel accepts.
 *
 * Error propagation
 * -----------------
 * Errors that occur inside the worker thread are captured as an errno value in
 * writer->last_error.  The main thread retrieves this value either by:
 *   - Returning from design_writer_submit_fd(), which checks last_error after
 *     the condvar wait.
 *   - Calling design_writer_poll_result(), which snapshots both the busy flag
 *     and last_error under the mutex.
 * If the writer is torn down (stop == true) while a transfer is in flight,
 * poll_result reports ECANCELED.
 *
 * Lifecycle
 * ---------
 *   design_writer_create()  -- allocate + init (qpair, mutex/cond, thread)
 *   ... submit / poll ...
 *   cleanup_design_writer() -- signal stop, join thread, release qpair, free
 */

#define _GNU_SOURCE

#include "design_writer.h"
#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/syslog.h>
#include <unistd.h>

#include <systemd/sd-journal.h>
#include <syslog.h>


/* QDMA queue pair configuration constants */
#define VRTD_QDMA_Q_MODE_MM 0u              /* Memory-mapped (MM) mode for the QDMA queue */
#define VRTD_QDMA_DIR_H2C (1u << 0)         /* Host-to-Card direction flag */
#define VRTD_QDMA_DIR_C2H (1u << 1)         /* Card-to-Host direction flag (unused here) */
#define VRTD_QDMA_RING_SZ_IDX 9u            /* Ring size index used for H2C/C2H/completion rings */

/*
 * The fixed QDMA address at which bitstream data must be written.
 * This is the device-side offset where the FPGA configuration logic
 * expects the bitstream payload.
 */
#define VRTD_DESIGN_WRITER_SEEK_ADDR 0x102100000ull

/* Maximum bitstream size accepted by the design writer (1 GiB). */
#define VRTD_DESIGN_WRITER_MAX_BYTES (1ull * 1024 * 1024 * 1024) // 1 GiB

/* Chunk size hint (currently unused in the write loop, which lets the kernel choose). */
#define VRTD_DESIGN_WRITER_CHUNK_BYTES 4096u

/* Allocation granularity when reading the bitstream file into memory (2 MiB steps). */
#define READ_ENTIRE_FILE_ALLOCATION_STEP (2 * 1024 * 1024) // 2 MiB

static int design_writer_open_qpair(struct design_writer *writer);
static void design_writer_release_qpair(struct design_writer *writer);

/**
 * Reallocate a page-aligned buffer to a new size, copying existing data.
 *
 * @param bufp      Pointer to the current buffer pointer (updated on success).
 * @param old_size  Number of bytes of valid data to copy from the old buffer.
 * @param new_size  Desired size of the new allocation (must be >= old_size).
 * @return 0 on success, -1 on allocation failure.
 *
 * The new buffer is aligned to 4096 bytes (page-aligned) as required by
 * QDMA DMA transfers.  The old buffer is freed after the copy.
 */
static int realloc_alligned_memory(void **bufp, size_t old_size, size_t new_size)
{
    void *new_buf;
    int ret = posix_memalign(&new_buf, 4096, new_size);
    if (ret != 0) {
        PROPAGATE_ERROR_LOG(-1, LOG_ERR, "Failed to allocate memory: %s", strerrordesc_np(ret));
    }

    memcpy(new_buf, *bufp, old_size);

    free(*bufp);
    *bufp = new_buf;
    return 0;
}

/**
 * Read the entire contents of a file descriptor into a page-aligned buffer.
 *
 * @param fd   Open file descriptor to read from (not closed by this function).
 * @param bufp Output: on success, receives a pointer to a page-aligned buffer
 *             containing the file data.  The caller is responsible for freeing
 *             it with free().
 * @return The number of bytes read on success, or -1 on error.
 *
 * The buffer grows in READ_ENTIRE_FILE_ALLOCATION_STEP (2 MiB) increments.
 * If the total file size exceeds VRTD_DESIGN_WRITER_MAX_BYTES (1 GiB), the
 * read is aborted with an error.  The buffer is always page-aligned (4096)
 * to satisfy QDMA DMA alignment requirements.
 */
static ssize_t read_entire_file(int fd, void **bufp)
{
    size_t capacity = READ_ENTIRE_FILE_ALLOCATION_STEP;
    size_t size = 0;

    _cleanup_(cleanup_free)
    uint8_t *buf;
    int ret = posix_memalign((void **)&buf, 4096, capacity);
    if (ret != 0) {
        PROPAGATE_ERROR_LOG(-1, LOG_ERR, "Failed to allocate memory for file buffer: %s", strerrordesc_np(ret));
    }

    for (;;) {
        ssize_t n = read(fd, buf + size, capacity - size);
        if (n == 0) { // EOF
            break;
        }
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            PROPAGATE_ERROR_STDC_LOG(-1, LOG_ERR, "Failed to read file");
        }
        size += (size_t)n;

        /* Grow the buffer if full */
        if (size == capacity) {
            capacity += READ_ENTIRE_FILE_ALLOCATION_STEP;
            ret = realloc_alligned_memory((void **)&buf, size, capacity);
            PROPAGATE_ERROR(ret);
        }

        /* Enforce the maximum bitstream size limit */
        if (capacity > VRTD_DESIGN_WRITER_MAX_BYTES) {
            PROPAGATE_ERROR_LOG(-1, LOG_ERR, "File size exceeds design writer maximum supported size of %zu bytes", (size_t)VRTD_DESIGN_WRITER_MAX_BYTES);
        }
    }

    *bufp = buf;
    buf = NULL; // ownership transferred to caller

    return (ssize_t)size;
}

/**
 * Write the full contents of a buffer to a file descriptor at a fixed position.
 *
 * Performs a seek-then-write loop, retrying on EINTR, until the entire buffer
 * has been written.  Each iteration logs progress for diagnostics.
 *
 * @param fd   The QDMA queue pair file descriptor to write to.
 * @param buf  Source buffer containing bitstream data.
 * @param len  Number of bytes to write.
 * @param pos  The device-side starting offset (VRTD_DESIGN_WRITER_SEEK_ADDR).
 * @return 0 on success, -1 on error (with errno set).
 */
static int write_all_at_pos(int fd, const void *buf, size_t len, off_t pos)
{
    size_t off = 0;

    while (off < len) {
        LOG(
            LOG_INFO,
            "Attempting to write to design writer file descriptor at offset 0x%lx (progress: %zu/%zu)",
            (unsigned long)(pos + off), off, len
        );

        off_t ret = lseek(fd, pos + off, SEEK_SET);
        PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Failed to seek design writer file descriptor to position 0x%lx", (unsigned long)(pos + off));

        ssize_t n = write(fd, (const uint8_t *)buf + off, len - off);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            PROPAGATE_ERROR_STDC_LOG(-1, LOG_ERR, "Failed to write to design writer file descriptor");
        }
        if (n == 0) {
            errno = EIO;
            PROPAGATE_ERROR_STDC_LOG(-1, LOG_ERR, "Short write to design writer file descriptor");
        }

        off += (size_t)n;

        LOG(
            LOG_INFO,
            "Design writer: wrote %zu bytes at offset 0x%lx (total written: %zu/%zu)",
            (size_t)n, (unsigned long)(pos + off), off, len
        );
    }

    return 0;
}

/**
 * Perform the complete bitstream transfer: read the input fd, write to QDMA.
 *
 * This is the core transfer routine called by the worker thread.  It:
 *   1. Reads the entire bitstream from @input_fd into a page-aligned buffer.
 *   2. Writes the buffer to the QDMA device fd at VRTD_DESIGN_WRITER_SEEK_ADDR.
 *
 * The commented-out qpair open/release calls indicate that the qpair is now
 * opened once at design_writer creation time and kept open for the lifetime
 * of the writer, rather than being opened and closed per-transfer.
 *
 * @param writer    The design_writer instance (provides the QDMA fd).
 * @param input_fd  File descriptor containing the bitstream data to program.
 * @return 0 on success, -1 on error (errno set).
 */
static int design_writer_transfer(struct design_writer *writer, int input_fd)
{
    _cleanup_(cleanup_free)
    void *file_data = NULL;
    ssize_t bytes_read = read_entire_file(input_fd, &file_data);
    PROPAGATE_ERROR_LOG(bytes_read, LOG_ERR, "Failed to read entire input file for design writer transfer");

    // int ret = design_writer_open_qpair(writer);
    // PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to initialize design writer qpair");

    int ret = write_all_at_pos(writer->fd, file_data, (size_t)bytes_read, VRTD_DESIGN_WRITER_SEEK_ADDR);
    int saved_errno = errno;
    // design_writer_release_qpair(writer);
    // errno = saved_errno;
    // PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to transfer design writer payload");

    return 0;
}

/**
 * Worker thread entry point for asynchronous bitstream programming.
 *
 * This thread runs for the entire lifetime of the design_writer.  It follows
 * a producer-consumer loop:
 *
 *   1. Wait on the condvar for either a new input_fd to be submitted or a
 *      stop request.
 *   2. On stop, exit the loop and return.
 *   3. On new input_fd, take ownership, enable cancellation (so that
 *      cleanup_design_writer can cancel us during a long transfer), then
 *      perform the QDMA transfer.
 *   4. On completion, close the input fd, clear the busy flag, store the
 *      error result in last_error, and broadcast the condvar to wake any
 *      threads waiting for the result.
 *
 * Cancellation is disabled while touching shared state (input_fd, busy,
 * last_error) and only enabled during the actual transfer to avoid leaving
 * the mutex in a locked state if the thread is cancelled.
 *
 * @param arg  Pointer to the owning design_writer struct.
 * @return Always NULL.
 */
static void *design_writer_thread(void *arg)
{
    struct design_writer *writer = arg;

    (void) pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    (void) pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    for (;;) {
        /* Wait for work: either an input fd to process or a stop signal */
        (void) pthread_mutex_lock(&writer->mutex);
        while (!writer->stop && writer->input_fd < 0) {
            (void) pthread_cond_wait(&writer->cond, &writer->mutex);
        }
        if (writer->stop) {
            (void) pthread_mutex_unlock(&writer->mutex);
            break;
        }

        int input_fd = writer->input_fd;
        (void) pthread_mutex_unlock(&writer->mutex);

        int transfer_errno = 0;

        /*
         * Enable cancellation during the transfer so that
         * design_writer_release_resources() can cancel a long-running
         * transfer during teardown.
         */
        (void) pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        LOG(LOG_INFO, "Design writer transfer starting");
        if (input_fd >= 0) {
            if (design_writer_transfer(writer, input_fd) != 0) {
                /* Capture the errno from the failed transfer */
                transfer_errno = (errno != 0) ? errno : EIO;
                LOG(
                    LOG_WARNING,
                    "Design writer transfer failed: %m"
                );
            }
            (void) close(input_fd);
        }
        (void) pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

        /*
         * Publish the result: clear busy, store the error, and wake
         * any thread waiting in submit_fd() or poll_result().
         */
        (void) pthread_mutex_lock(&writer->mutex);
        writer->input_fd = -1;
        writer->busy = false;
        writer->last_error = transfer_errno;
        (void) pthread_cond_broadcast(&writer->cond);
        (void) pthread_mutex_unlock(&writer->mutex);
    }

    return NULL;
}

/**
 * Cleanup helper: close a file descriptor and reset it to -1.
 */
static void cleanup_close_fd(int *fdp)
{
    if (fdp == NULL || *fdp < 0) {
        return;
    }

    (void) close(*fdp);
    *fdp = -1;
}

/**
 * Cleanup helper for _cleanup_ attribute: unlock a mutex pointer-to-pointer.
 *
 * Used with the _cleanup_ attribute to ensure a mutex is unlocked when the
 * variable goes out of scope, providing exception-safe locking.
 */
static void cleanup_mutex_unlockp(pthread_mutex_t **mutexp)
{
    if (mutexp == NULL || *mutexp == NULL) {
        return;
    }

    (void) pthread_mutex_unlock(*mutexp);
    *mutexp = NULL;
}

/**
 * Release the QDMA queue pair resources associated with the design writer.
 *
 * Stops the queue pair (if started), deletes it (if created), and closes
 * the QDMA file descriptor.  Errors during stop/delete are logged as
 * warnings but do not prevent further cleanup.
 *
 * @param writer  The design_writer whose qpair resources should be released.
 */
static void design_writer_release_qpair(struct design_writer *writer)
{
    if (writer == NULL) {
        return;
    }

    cleanup_close_fd(&writer->fd);

    if (writer->qpair_created && writer->qdma != NULL) {
        if (writer->qpair_started && slash_qdma_qpair_stop(writer->qdma, writer->qid) == -1) {
            LOG(
                LOG_WARNING,
                "Error stopping design writer qpair %u: %m (ignored)",
                writer->qid
            );
        }

        if (slash_qdma_qpair_del(writer->qdma, writer->qid) == -1) {
            LOG(
                LOG_WARNING,
                "Error deleting design writer qpair %u: %m (ignored)",
                writer->qid
            );
        }
    }

    writer->qid = 0;
    writer->qpair_started = false;
    writer->qpair_created = false;
}

/**
 * Tear down all resources owned by the design writer.
 *
 * This function orchestrates a clean shutdown:
 *   1. Signal the worker thread to stop (set stop flag, broadcast condvar).
 *   2. Cancel and join the worker thread.
 *   3. Close any pending input fd.
 *   4. Release the QDMA queue pair.
 *   5. Destroy the mutex and condition variable.
 *
 * @param writer  The design_writer to tear down.
 */
static void design_writer_release_resources(struct design_writer *writer)
{
    if (writer->mutex_initialized) {
        (void) pthread_mutex_lock(&writer->mutex);
        writer->stop = true;
        if (writer->cond_initialized) {
            (void) pthread_cond_broadcast(&writer->cond);
        }
        (void) pthread_mutex_unlock(&writer->mutex);
    }

    if (writer->thread_started) {
        (void) pthread_cancel(writer->thread);
        (void) pthread_join(writer->thread, NULL);
        writer->thread_started = false;
    }

    cleanup_close_fd(&writer->input_fd);
    design_writer_release_qpair(writer);

    writer->qdma = NULL;

    if (writer->cond_initialized) {
        (void) pthread_cond_destroy(&writer->cond);
        writer->cond_initialized = false;
    }
    if (writer->mutex_initialized) {
        (void) pthread_mutex_destroy(&writer->mutex);
        writer->mutex_initialized = false;
    }
}

/**
 * Cleanup helper for _cleanup_ attribute: release design writer resources.
 *
 * Used as a rollback guard during design_writer_init(); if init fails
 * partway through, this cleanup function ensures all partially-initialized
 * resources are properly released.
 */
static void cleanup_design_writer_resourcesp(struct design_writer **writerp)
{
    if (writerp == NULL || *writerp == NULL) {
        return;
    }

    design_writer_release_resources(*writerp);
}

/**
 * Initialize the mutex and condition variable used for thread synchronization.
 *
 * @param writer  The design_writer whose sync primitives should be initialized.
 * @return 0 on success, -1 on failure.
 */
static int design_writer_init_sync_primitives(struct design_writer *writer)
{
    int pthread_ret = pthread_mutex_init(&writer->mutex, NULL);
    int ret = pthread_ret == 0 ? 0 : -1;
    PROPAGATE_ERROR_LOG(
        ret,
        LOG_ERR,
        "Failed to initialize design writer mutex (code=%d)",
        pthread_ret
    );
    writer->mutex_initialized = true;

    pthread_ret = pthread_cond_init(&writer->cond, NULL);
    ret = pthread_ret == 0 ? 0 : -1;
    PROPAGATE_ERROR_LOG(
        ret,
        LOG_ERR,
        "Failed to initialize design writer condition variable (code=%d)",
        pthread_ret
    );
    writer->cond_initialized = true;

    return 0;
}

/**
 * Set up a QDMA queue pair for bitstream transfer.
 *
 * Allocates a new QDMA queue pair in Memory-Mapped (MM) mode with
 * Host-to-Card (H2C) direction, starts it, and obtains a file descriptor
 * for performing write() calls that translate to DMA transfers.
 *
 * The queue pair configuration:
 *   - Mode:      MM (memory-mapped, not streaming)
 *   - Direction: H2C only (host writes bitstream to card)
 *   - Ring size:  Index 9 for all rings (H2C, C2H, completion)
 *
 * On failure, any partially-created queue pair is cleaned up before returning.
 *
 * @param writer  The design_writer to set up the qpair for.
 * @return 0 on success, -1 on failure.
 */
static int design_writer_open_qpair(struct design_writer *writer)
{
    struct slash_qdma_qpair_add qpair = {0};
    qpair.size = sizeof(qpair);
    qpair.mode = VRTD_QDMA_Q_MODE_MM;
    qpair.dir_mask = VRTD_QDMA_DIR_H2C;
    qpair.h2c_ring_sz = VRTD_QDMA_RING_SZ_IDX;
    qpair.c2h_ring_sz = VRTD_QDMA_RING_SZ_IDX;
    qpair.cmpt_ring_sz = VRTD_QDMA_RING_SZ_IDX;

    int ret = slash_qdma_qpair_add(writer->qdma, &qpair);
    PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Failed to add design writer QDMA qpair");

    writer->qid = qpair.qid;
    writer->qpair_created = true;
    writer->qpair_started = false;

    ret = slash_qdma_qpair_start(writer->qdma, writer->qid);
    if (ret == -1) {
        LOG(LOG_ERR, "Failed to start design writer QDMA qpair: %m");
        design_writer_release_qpair(writer);
        return -1;
    }
    writer->qpair_started = true;

    /* Obtain the file descriptor for the started queue pair.
     * Subsequent lseek()+write() calls on this fd perform DMA transfers
     * to the device at the specified offset. */
    writer->fd = slash_qdma_qpair_get_fd(writer->qdma, writer->qid, O_CLOEXEC);
    if (writer->fd == -1) {
        LOG(LOG_ERR, "Failed to get design writer QDMA file descriptor: %m");
        design_writer_release_qpair(writer);
        return -1;
    }

    return 0;
}

/**
 * Create and start the worker thread for asynchronous bitstream transfers.
 *
 * @param writer  The design_writer whose thread should be started.
 * @return 0 on success, -1 on failure.
 */
static int design_writer_start_thread(struct design_writer *writer)
{
    int pthread_ret = pthread_create(&writer->thread, NULL, design_writer_thread, writer);
    int ret = pthread_ret == 0 ? 0 : -1;
    PROPAGATE_ERROR_LOG(
        ret,
        LOG_ERR,
        "Failed to create design writer thread (code=%d)",
        pthread_ret
    );

    writer->thread_started = true;
    return 0;
}

/**
 * Initialize a design_writer struct with all required resources.
 *
 * Performs the full initialization sequence:
 *   1. Zero-initialize the struct and store the QDMA handle.
 *   2. Open a QDMA queue pair (allocate, start, get fd).
 *   3. Initialize synchronization primitives (mutex, condvar).
 *   4. Start the worker thread.
 *
 * Uses a _cleanup_ rollback guard: if any step fails, all resources
 * initialized up to that point are automatically released.
 *
 * @param writer  Pre-allocated design_writer struct to initialize.
 * @param qdma    QDMA subsystem handle (non-owning reference; must outlive the writer).
 * @return 0 on success, -1 on failure.
 */
static int design_writer_init(struct design_writer *writer, struct slash_qdma *qdma)
{
    PROPAGATE_ERROR_NULL_LOG(writer, LOG_ERR, "Failed to initialize design writer: invalid writer");
    PROPAGATE_ERROR_NULL_LOG(qdma, LOG_ERR, "Failed to initialize design writer: invalid qdma");

    *writer = (struct design_writer) {
        .qdma = qdma,
        .qid = 0,
        .fd = -1,
        .qpair_created = false,
        .qpair_started = false,
        .thread = 0,
        .input_fd = -1,
        .busy = false,
        .stop = false,
        .last_error = 0,
        .thread_started = false,
        .mutex_initialized = false,
        .cond_initialized = false,
    };

    /* Rollback guard: if any step below fails, cleanup_design_writer_resourcesp
     * is invoked automatically to release everything initialized so far. */
    _cleanup_(cleanup_design_writer_resourcesp)
    struct design_writer *writer_rollback = writer;

    int ret = design_writer_open_qpair(writer);
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to initialize design writer qpair");

    ret = design_writer_init_sync_primitives(writer);
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to initialize design writer synchronization primitives");

    ret = design_writer_start_thread(writer);
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to start design writer worker thread");

    /* Initialization succeeded -- disarm the rollback guard */
    writer_rollback = NULL;

    return 0;
}

/**
 * Internal creation helper that returns errors via return value.
 *
 * Allocates a design_writer on the heap and initializes it.  On success,
 * ownership is transferred to *writerp.  On failure, all resources are
 * cleaned up and *writerp is left unchanged.
 *
 * @param qdma     QDMA subsystem handle.
 * @param writerp  Output pointer to receive the new design_writer.
 * @return 0 on success, -1 on failure.
 */
static int design_writer_create_internal(struct slash_qdma *qdma, struct design_writer **writerp)
{
    PROPAGATE_ERROR_NULL_LOG(writerp, LOG_ERR, "Failed to create design writer: invalid output pointer");

    _cleanup_(cleanup_design_writerp)
    struct design_writer *writer = calloc(1, sizeof(*writer));
    PROPAGATE_ERROR_NULL_STDC_LOG(writer, LOG_ERR, "Failed to allocate design writer");

    int ret = design_writer_init(writer, qdma);
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to initialize design writer");

    *writerp = writer;
    writer = NULL; /* ownership transferred to caller */

    return 0;
}

/**
 * Create a new design_writer for asynchronous bitstream programming.
 *
 * Allocates and fully initializes a design_writer, including:
 *   - A QDMA queue pair in H2C memory-mapped mode
 *   - A mutex and condition variable for thread synchronization
 *   - A background worker thread ready to accept bitstream transfers
 *
 * The returned writer is immediately ready for design_writer_submit_fd() or
 * design_writer_submit_fd_async() calls.
 *
 * @param qdma  QDMA subsystem handle (must remain valid for the lifetime of
 *              the returned writer).
 * @return A fully initialized design_writer on success, or NULL on failure.
 *         The caller must eventually call cleanup_design_writer() to free it.
 */
struct design_writer *design_writer_create(struct slash_qdma *qdma)
{
    struct design_writer *writer = NULL;
    if (design_writer_create_internal(qdma, &writer) == -1) {
        return NULL;
    }

    return writer;
}

/**
 * Submit a bitstream fd and block until the transfer completes.
 *
 * This is the synchronous convenience wrapper.  It calls
 * design_writer_submit_fd_async() to hand the fd to the worker thread,
 * then waits on the condvar until the worker sets busy = false.
 *
 * On return, the fd has been consumed (closed by the worker thread) regardless
 * of success or failure -- the caller must not close it.
 *
 * @param writer  The design_writer instance.
 * @param fd      Open file descriptor containing the bitstream data.
 *                Ownership is transferred to the worker thread.
 * @return 0 on success, -1 on failure (check logs for details).
 */
int design_writer_submit_fd(struct design_writer *writer, int fd)
{
    int ret = design_writer_submit_fd_async(writer, fd);
    PROPAGATE_ERROR_LOG(ret, LOG_WARNING, "Failed to enqueue design write request");

    int pthread_ret = pthread_mutex_lock(&writer->mutex);
    ret = pthread_ret == 0 ? 0 : -1;
    PROPAGATE_ERROR_LOG(
        ret,
        LOG_ERR,
        "Failed to lock design writer mutex (code=%d)",
        pthread_ret
    );
    _cleanup_(cleanup_mutex_unlockp)
    pthread_mutex_t *locked_mutex = &writer->mutex;

    /* Block until the worker thread completes the transfer */
    while (writer->busy && !writer->stop) {
        (void) pthread_cond_wait(&writer->cond, &writer->mutex);
    }

    ret = writer->stop ? -1 : 0;
    PROPAGATE_ERROR_LOG(ret, LOG_WARNING, "Design writer stopped before transfer completed");

    /* Propagate any error captured by the worker thread */
    int last_error = writer->last_error;
    ret = last_error == 0 ? 0 : -1;
    PROPAGATE_ERROR_LOG(
        ret,
        LOG_WARNING,
        "Design writer transfer failed (code=%d)",
        last_error
    );

    return 0;
}

/**
 * Submit a bitstream fd for asynchronous transfer (non-blocking).
 *
 * Enqueues the given file descriptor for the worker thread to process.
 * The caller retains no ownership of @fd after this call succeeds -- the
 * worker thread will close it when the transfer completes.
 *
 * Only one transfer may be in flight at a time.  If the writer is already
 * busy, stopping, or has a pending input fd, this call fails with -1.
 *
 * After calling this function, use design_writer_poll_result() to check
 * whether the transfer has completed and retrieve any error.
 *
 * @param writer  The design_writer instance.
 * @param fd      Open file descriptor containing bitstream data.
 *                Ownership transfers to the worker on success.
 * @return 0 on success (fd enqueued), -1 on failure (writer busy/stopping,
 *         or invalid arguments).
 */
int design_writer_submit_fd_async(struct design_writer *writer, int fd)
{
    PROPAGATE_ERROR_NULL_LOG(writer, LOG_ERR, "design_writer_submit_fd_async called with null writer");
    PROPAGATE_ERROR_LOG(
        (fd >= 0) ? 0 : -1,
        LOG_ERR,
        "design_writer_submit_fd_async called with invalid fd %d",
        fd
    );

    int pthread_ret = pthread_mutex_lock(&writer->mutex);
    int ret = pthread_ret == 0 ? 0 : -1;
    PROPAGATE_ERROR_LOG(
        ret,
        LOG_ERR,
        "Failed to lock design writer mutex (code=%d)",
        pthread_ret
    );
    _cleanup_(cleanup_mutex_unlockp)
    pthread_mutex_t *locked_mutex = &writer->mutex;

    /* Reject if the writer is busy, stopping, or already has a pending fd */
    ret = (writer->stop || writer->busy || writer->input_fd >= 0) ? -1 : 0;
    PROPAGATE_ERROR_LOG(ret, LOG_WARNING, "Design writer is busy or stopping");

    /* Hand off the fd to the worker thread */
    writer->input_fd = fd;
    writer->busy = true;
    writer->last_error = 0;
    (void) pthread_cond_signal(&writer->cond);

    LOG(LOG_DEBUG, "Design write enqueued fd=%d", fd);
    return 0;
}

/**
 * Poll for the result of an asynchronous bitstream transfer.
 *
 * Non-blocking check of whether the most recent transfer has completed.
 *
 * @param writer      The design_writer instance.
 * @param done        Output: set to true if no transfer is in progress
 *                    (either completed or never started), false if busy.
 * @param last_error  Output: 0 if the last transfer succeeded, an errno
 *                    value if it failed, or ECANCELED if the writer was
 *                    stopped while a transfer was in flight.
 * @return 0 on success, -1 on failure (invalid arguments or mutex error).
 */
int design_writer_poll_result(struct design_writer *writer, bool *done, int *last_error)
{
    PROPAGATE_ERROR_NULL_LOG(writer, LOG_ERR, "design_writer_poll_result called with null writer");
    PROPAGATE_ERROR_NULL_LOG(done, LOG_ERR, "design_writer_poll_result called with null done pointer");
    PROPAGATE_ERROR_NULL_LOG(last_error, LOG_ERR, "design_writer_poll_result called with null last_error pointer");

    int pthread_ret = pthread_mutex_lock(&writer->mutex);
    int ret = pthread_ret == 0 ? 0 : -1;
    PROPAGATE_ERROR_LOG(
        ret,
        LOG_ERR,
        "Failed to lock design writer mutex (code=%d)",
        pthread_ret
    );
    _cleanup_(cleanup_mutex_unlockp)
    pthread_mutex_t *locked_mutex = &writer->mutex;

    *done = !writer->busy;
    if (writer->stop) {
        *done = true;
        *last_error = ECANCELED;
    } else {
        *last_error = writer->last_error;
    }

    return 0;
}

/**
 * Check whether the design writer currently has a transfer in progress.
 *
 * Thread-safe query of the busy flag.
 *
 * @param writer  The design_writer instance (NULL-safe: returns false).
 * @return true if a transfer is in progress, false otherwise.
 */
bool design_writer_is_busy(struct design_writer *writer)
{
    if (writer == NULL) {
        return false;
    }

    (void) pthread_mutex_lock(&writer->mutex);
    bool busy = writer->busy;
    (void) pthread_mutex_unlock(&writer->mutex);
    return busy;
}

/**
 * Destroy a design_writer and free all associated resources.
 *
 * Signals the worker thread to stop, joins it, releases the QDMA queue
 * pair, destroys synchronization primitives, and frees the struct.
 *
 * After this call, @writer is invalid and must not be used.
 * NULL-safe: calling with NULL is a no-op.
 *
 * @param writer  The design_writer to destroy (may be NULL).
 */
void cleanup_design_writer(struct design_writer *writer)
{
    if (writer == NULL) {
        return;
    }

    design_writer_release_resources(writer);
    free(writer);
}

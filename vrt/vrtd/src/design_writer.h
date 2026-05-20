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
 * @file design_writer.h
 * @brief Asynchronous FPGA bitstream writer for SLASH devices.
 *
 * Writing a bitstream to an FPGA can take several seconds.  To avoid blocking
 * the single-threaded event loop, the design_writer offloads the actual QDMA
 * transfer to a background pthread and exposes an async polling API:
 *
 *   1. @c design_writer_submit_fd_async -- hand off a bitstream fd to the
 *      background thread.  Returns immediately.
 *   2. @c design_writer_poll_result -- non-blocking check: has the write
 *      finished?  If yes, retrieves the result code.
 *   3. The event loop re-arms a deferred timer to keep polling until done.
 *
 * A synchronous convenience wrapper (@c design_writer_submit_fd) is also
 * provided for cases where blocking is acceptable.
 */

#ifndef VRTD_DESIGN_WRITER_H
#define VRTD_DESIGN_WRITER_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <slash/qdma.h>

/**
 * @brief State for the asynchronous FPGA bitstream writer.
 *
 * Internally manages a dedicated pthread that performs the blocking QDMA
 * write.  Synchronization between the event-loop thread and the worker
 * thread uses a mutex + condition variable pair.
 */
struct design_writer {
    /** @brief QDMA subsystem handle (non-owning, borrowed from struct device). */
    struct slash_qdma *qdma; /* non-owning */
    /** @brief QDMA queue ID allocated for bitstream writes. */
    uint32_t qid;
    /** @brief File descriptor for the QDMA queue pair character device. */
    int fd;
    /** @brief True if the QDMA queue pair has been created. */
    bool qpair_created;
    /** @brief True if the QDMA queue pair has been started. */
    bool qpair_started;

    /* Worker thread and synchronization primitives */

    /** @brief Background worker pthread handle. */
    pthread_t thread;
    /** @brief Mutex protecting shared state (input_fd, busy, stop, last_error). */
    pthread_mutex_t mutex;
    /** @brief Condition variable signaled when new work is submitted or stop is requested. */
    pthread_cond_t cond;

    /** @brief Bitstream file descriptor passed to the worker thread for the current write. */
    int input_fd;
    /** @brief True while the worker thread is performing a write operation. */
    bool busy;
    /** @brief Set to true to request the worker thread to exit its loop and terminate. */
    bool stop;
    /** @brief Result code from the most recent write (0 = success, negative = error). */
    int last_error;

    /* Initialization tracking flags */

    /** @brief True if the worker pthread has been successfully started. */
    bool thread_started;
    /** @brief True if @c mutex has been initialized (for cleanup safety). */
    bool mutex_initialized;
    /** @brief True if @c cond has been initialized (for cleanup safety). */
    bool cond_initialized;
};

/**
 * @brief Create and initialize a design writer for the given QDMA subsystem.
 *
 * Allocates a QDMA queue pair for bitstream transfer, initializes the
 * mutex/condvar, and starts the background worker thread.
 *
 * @param qdma QDMA subsystem handle (borrowed, must outlive the design_writer).
 * @return Heap-allocated design_writer on success, NULL on failure.
 */
struct design_writer *design_writer_create(struct slash_qdma *qdma);

/**
 * @brief Submit a bitstream file descriptor for asynchronous writing.
 *
 * Hands the fd to the background worker thread and returns immediately.
 * Use @c design_writer_poll_result to check for completion.
 *
 * @param writer The design writer instance.
 * @param fd     Open file descriptor for the bitstream (caller retains ownership).
 * @return 0 on success, -1 if the writer is already busy or on error.
 */
int design_writer_submit_fd_async(struct design_writer *writer, int fd);

/**
 * @brief Submit a bitstream file descriptor and block until the write completes.
 *
 * Synchronous convenience wrapper around the async API.  Blocks the calling
 * thread until the bitstream transfer finishes.
 *
 * @param writer The design writer instance.
 * @param fd     Open file descriptor for the bitstream.
 * @return 0 on success, negative errno on failure.
 */
int design_writer_submit_fd(struct design_writer *writer, int fd);

/**
 * @brief Poll for completion of an asynchronous bitstream write.
 *
 * Non-blocking check.  When the write has finished, @p done is set to true
 * and @p last_error receives the result code.
 *
 * @param writer          The design writer instance.
 * @param[out] done       Set to true if the write has completed, false if still in progress.
 * @param[out] last_error Set to the write result (0 = success, negative = error) when done.
 * @return 0 on success, -1 on mutex error.
 */
int design_writer_poll_result(struct design_writer *writer, bool *done, int *last_error);

/**
 * @brief Check whether the design writer is currently performing a write.
 * @param writer The design writer instance.
 * @return True if a write is in progress, false otherwise.
 */
bool design_writer_is_busy(struct design_writer *writer);

/**
 * @brief Release all resources owned by the design writer.
 *
 * Signals the worker thread to stop, joins it, destroys synchronization
 * primitives, and tears down the QDMA queue pair.
 *
 * @param writer Pointer to the design_writer to clean up. May be NULL (no-op).
 */
void cleanup_design_writer(struct design_writer *writer);

/**
 * @brief Cleanup helper for use with __attribute__((cleanup)).
 * @param writerp Address of a @c struct @c design_writer pointer.
 */
static inline
void cleanup_design_writerp(struct design_writer **writerp)
{
    cleanup_design_writer(*writerp);
    *writerp = NULL;
}

#endif // VRTD_DESIGN_WRITER_H

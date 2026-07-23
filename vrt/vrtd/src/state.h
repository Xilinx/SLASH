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

#ifndef VRTD_STATE_H
#define VRTD_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "device.h"
#include "config.h"
#include "serve.h"

struct flash_worker;

struct vrtd {
    struct config *config;

    struct client_ptr_array clients;
    uint64_t next_conn_id;

    struct device_ptr_array devices;

    /**
     * @brief Background worker that runs cfgmem (flash) programming off the
     * event-loop thread (owning, may be NULL).
     */
    struct flash_worker *flash_worker;

    /**
     * @brief True while a long-running device operation is running on @ref flash_worker.
     *
     * Cfgmem programming and standalone reset both mutate @ref devices, so
     * while this is set the event loop defers dispatching every other client
     * request (they stay queued until the operation finishes).  This keeps the
     * loop alive to feed the systemd watchdog without racing the worker on the
     * device list.
     */
    bool async_device_op_in_progress;

    /**
     * @brief Client connection IDs whose buffer cleanup is deferred while an
     * async device operation owns @ref devices from the worker thread.
     */
    struct uint64_array deferred_buffer_cleanup_conn_ids;
};

#endif // VRTD_STATE_H

// ################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//  and associated documentation files (the "Software"), to deal in the Software without
//  restriction, including without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all copies or
//  substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ################################################################################################

#pragma once

#include "config.h"

namespace slash_sysemu {

/**
 * @brief Request a clean shutdown of the running daemon.
 *
 * Thread-safe.  Signals the running sd-event loop (via an eventfd) to exit; the
 * request is latched, so a call that lands just before run_daemon() creates the
 * eventfd is still honored.  Has no effect if no daemon is running.  Real
 * SIGTERM/SIGINT delivery drives the same exit path via sd-event's signal
 * handling; this is the programmatic equivalent (also used by tests).
 */
void request_shutdown() noexcept;

/**
 * @brief Run the emulation daemon with a resolved configuration.
 *
 * The daemon runs only under systemd.  Bring-up:
 *   1. Block SIGTERM/SIGINT (before any subsystem thread spawns) and create the
 *      sd-event loop, its signal sources, and an eventfd source for
 *      request_shutdown().
 *   2. Bring up the daemon-level slash_hotplug socket and its lifecycle worker.
 *   3. Trigger the startup RESCAN (instantiates every configured accelerator),
 *      then notify systemd READY=1.
 *   4. If the watchdog is enabled, arm a health-gated WATCHDOG=1 keepalive timer.
 *   5. Run the sd-event loop until SIGTERM/SIGINT or request_shutdown().
 *   6. Notify STOPPING=1, tear down the hotplug subsystem (drains the lifecycle
 *      queue and tears down every accelerator in order).
 *
 * systemd owns the runtime directory (RuntimeDirectory=) and socket ownership /
 * permissions (User=/Group= + UMask=), so the daemon neither creates its base
 * directory, cold-reboot-cleans it, nor chowns/chmods its sockets.
 *
 * @param config Fully-resolved daemon configuration.
 * @return 0 on a clean shutdown, non-zero on a fatal bring-up error.
 */
int run_daemon(const DaemonConfig& config);

} // namespace slash_sysemu

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

#include <csignal>
#include <functional>

namespace slash_emu {

/**
 * @brief Callable type for installing a signal handler.
 *
 * Matches the signature of POSIX sigaction(2).  The default implementation
 * delegates directly to ::sigaction; tests may inject a stub that fails on
 * demand (e.g. for SIGKILL) or that records which signals were installed.
 */
using SignalInstaller = std::function<int(int, const struct sigaction *, struct sigaction *)>;

/**
 * @brief Request a clean shutdown of the running daemon.
 *
 * Thread-safe.  May be called from a signal handler or from test code.
 * Has no effect if the daemon is not currently running.
 */
void request_shutdown() noexcept;

/**
 * @brief Run the emulation daemon.
 *
 * Installs SIGINT/SIGTERM handlers via @p install_signal (defaults to the
 * real ::sigaction), logs a startup line, blocks until request_shutdown() is
 * called (either directly or via a signal), then logs a shutdown line and
 * returns 0.
 *
 * Returns 0 on a clean shutdown, non-zero on a fatal error (e.g. signal
 * installation failure).
 *
 * Separated from main() so that it can be exercised directly from unit tests.
 * The injectable @p install_signal seam allows tests to exercise the
 * sigaction-failure path without needing to attempt installing SIGKILL.
 */
int run_daemon(SignalInstaller install_signal = nullptr);

} // namespace slash_emu

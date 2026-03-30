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
 * @file signals.c
 * @brief Signal handling for graceful shutdown and live configuration reload.
 *
 * vrtd uses sd_event_add_signal() to receive signals through the event loop
 * (via signalfd) rather than traditional async signal handlers.  This avoids
 * the usual signal-safety pitfalls and lets us call arbitrary library
 * functions from the handler.
 *
 * Handled signals:
 *  - SIGINT / SIGTERM  -- Initiate graceful shutdown by exiting the event loop.
 *  - SIGHUP            -- Reload the configuration file without restarting the
 *                         daemon (all connected clients have their resolved
 *                         roles invalidated so they are re-evaluated against
 *                         the new config on the next request).
 *  - SIGPIPE           -- Ignored (set to SIG_IGN in main.c/configure_signals)
 *                         because clients can disconnect at any time and we
 *                         must not be killed by a write to a broken socket.
 */

#define _GNU_SOURCE

#include "signals.h"

#include <assert.h>
#include <stdio.h>
#include <systemd/sd-journal.h>
#include <sys/syslog.h>

#include "state.h"
#include "config.h"
#include "utils.h"

int reload_config(struct vrtd *state);

/**
 * sd_event signal callback dispatched when SIGINT, SIGTERM, SIGHUP, or
 * SIGQUIT is received via the signalfd.
 *
 * - SIGINT / SIGTERM: request a clean exit from the event loop so that
 *   destructors run and STOPPING=1 is sent to systemd.
 * - SIGHUP: live-reload the configuration from disk.
 * - Others: logged as unhandled (should not occur given the signal mask
 *   set up in main.c).
 */
int on_event_signal(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata)
{
    int sig = si->ssi_signo;

    struct vrtd *state = userdata;
    assert(state != NULL);

    // Log or act based on the signal
    switch (sig) {
    case SIGINT:
    case SIGTERM: {
        // Stop the event loop gracefully
        LOG(LOG_INFO, "Received signal %s (%d), shutting down", sigabbrev_np(sig), sig);
        sd_event *event = sd_event_source_get_event(s);
        if (event) {
            sd_event_exit(event, 0);
        }
        break;
    }

    case SIGHUP: {
        LOG(LOG_INFO, "Received SIGHUP, reloading configuration");
        reload_config(state);
        break;
    }

    default: {
        LOG(LOG_WARNING, "Unhandled signal: %s (%d)\n", sigabbrev_np(sig), sig);

        break;
    }
    }

    return 0;
}

/**
 * Reload the daemon's configuration from disk.
 *
 * Existing client role assignments are invalidated (cleaned up) so that each
 * client's role is re-resolved from the new configuration on its next request.
 * This avoids disconnecting clients simply because the config file changed.
 *
 * If loading the new configuration fails, the old config has already been
 * freed -- the daemon continues to run but all role lookups will fail until
 * a subsequent successful reload or restart.
 */
int reload_config(struct vrtd *state)
{
    /* Invalidate cached roles for every connected client so they are
     * re-evaluated against the incoming configuration. */
    for (size_t i = 0; i < state->clients.len; i++) {
        struct client *client = state->clients.d[i];
        assert(client != NULL);

        cleanup_rolep(&client->role);
    }

    cleanup_configp(&state->config);

    int ret = config_load(&state->config);
    PROPAGATE_ERROR(ret);

    LOG(LOG_INFO, "Configuration reloaded successfully");

    return 0;
}

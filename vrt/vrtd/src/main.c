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

#define _GNU_SOURCE

#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-daemon.h>
#include <systemd/sd-event.h>
#include <systemd/sd-journal.h>

#include "config.h"
#include "array.h"
#include "utils.h"
#include "state.h"
#include "accept.h"
#include "device.h"
#include "flash_worker.h"
#include "signals.h"
#include "hotplug.h"

/*
 * The deferred work timer fires every 20ms to poll for completion of
 * asynchronous design writes (e.g. bitstream loads to the FPGA fabric).
 * These operations are initiated by client requests but complete
 * asynchronously via the QDMA subsystem; a 20ms polling interval strikes
 * a balance between responsiveness and CPU overhead -- fast enough that
 * clients see sub-frame latency, slow enough to avoid busy-spinning.
 */
#define VRTD_DEFERRED_WORK_INTERVAL_USEC (20ULL * 1000ULL)

static void check_journal_and_abort_if_needed(void);
static int configure_watchdog(sd_event *ev);
static int configure_signals(sd_event *ev, struct vrtd *state);
static int configure_sockets(sd_event *ev, struct vrtd *state);
static int configure_background_tasks(sd_event *ev, struct vrtd *state);
static int block_signals(const int *signals, size_t n);
static int create_listening_unix_socket(const char *path);
static int register_listening_socket(sd_event *ev, struct vrtd *state, int fd, const char *name);

void globals_init();
void globals_destroy();

static char *created_socket_path = NULL;

int main(void)
{
    struct vrtd state = {0};

    /*
     * Verify the systemd journal is reachable before doing anything else.
     * If logging is broken we want to fail *before* sd_notify(READY=1),
     * so the service never appears started and the sysadmin gets a clear
     * error from systemctl.  See the detailed rationale above
     * check_journal_and_abort_if_needed().
     */
    check_journal_and_abort_if_needed();

    globals_init();

    int ret = config_load(&state.config);
    if (ret == -1) {
        LOG(LOG_CRIT, "Failed to load config");
        exit(EXIT_FAILURE);
    }

    ret = devices_discover_and_open(&state.devices);
    if (ret == -1) {
        LOG(LOG_CRIT, "Failed to load devices");
        exit(EXIT_FAILURE);
    }

    LOG(LOG_INFO, "Discovered %zu device(s)", state.devices.len);

    /*
     * The flash worker runs long cfgmem programming jobs (AMI PDI download +
     * reset) off the event-loop thread so the loop stays responsive enough to
     * feed the systemd watchdog while a device is being reprogrammed.
     */
    state.flash_worker = flash_worker_create();
    if (state.flash_worker == NULL) {
        LOG(LOG_CRIT, "Failed to create flash worker");
        exit(EXIT_FAILURE);
    }

    _cleanup_(sd_event_unrefp)
    sd_event *ev = NULL;
    ret = sd_event_default(&ev);
    if (ret < 0) {
        LOG(LOG_CRIT, "Failed to allocate event loop: %s", strerrordesc_np(-ret));
        exit(EXIT_FAILURE);
    }

    /*
     * Enable the systemd watchdog so that systemd can detect if vrtd
     * becomes unresponsive (e.g. blocked on a stuck QDMA ioctl or
     * deadlocked).  sd_event_set_watchdog() automatically sends
     * keepalive pings at half the interval configured in the unit file
     * (WatchdogSec=); if we stop pinging, systemd will restart us.
     */
    ret = configure_watchdog(ev);
    if (ret == -1) {
        LOG(LOG_CRIT, "Failed to configure watchdog");
        exit(EXIT_FAILURE);
    }

    ret = configure_signals(ev, &state);
    if (ret == -1) {
        LOG(LOG_CRIT, "Failed to configure signals");
        exit(EXIT_FAILURE);
    }

    ret = configure_sockets(ev, &state);
    if (ret == -1) {
        LOG(LOG_CRIT, "Failed to configure sockets");
        exit(EXIT_FAILURE);
    }

    ret = configure_background_tasks(ev, &state);
    if (ret == -1) {
        LOG(LOG_CRIT, "Failed to configure background tasks");
        exit(EXIT_FAILURE);
    }

    ret = sd_notify(0, "READY=1");
    if (ret < 0) {
        LOG(LOG_CRIT, "Failed to notify ready: %s", strerrordesc_np(-ret));
        exit(EXIT_FAILURE);
    } else if (ret == 0) {
        LOG(LOG_INFO, "No notification socket");
    }

    ret = sd_event_loop(ev);
    if (ret < 0) {
        LOG(LOG_CRIT, "Critical error: %s", strerrordesc_np(-ret));
        exit(EXIT_FAILURE);
    }

    (void) sd_notify(0, "STOPPING=1");
    if (created_socket_path != NULL) {
        (void) unlink(created_socket_path);
        free(created_socket_path);
        created_socket_path = NULL;
    }

    cleanup_flash_worker(state.flash_worker);
    state.flash_worker = NULL;
    uint64_array_free(&state.deferred_buffer_cleanup_conn_ids);

    globals_destroy();

    return ret;
}


/**
 * In vrtd we do all our logging through the systemd-journal.
 * This is very convenient as it allows inspecting with journalctl -u
 * in the usual way, saves us from having to manage our own files in
 * /var/log (with rotation, compression etc.) and is nice QoL all around.
 * 
 * The problem is that logging can fail, which raises the question about
 * how we are to handle that failure.
 *
 * It is important to note that if the systemd-journal is not active,
 * the logging functions will succeed, and silently do nothing. This is
 * a systemd design choice. For now, we simply accept this behaviour.
 *
 * The logging functions can fail if:
 *
 * 1) We call them with invalid parameters (EINVAL).
 * 2) We send a message that's too big.
 * 3) We run out of memory (ENOMEM).
 * 4) Some other process limit is reached.
 * 5) An I/O error occurs.
 * 6) The internal sendmsg syscall is interrupted by a signal (EINTR).
 *
 * Aborting the program if logging fails is not a good idea. We are left
 * with two choices:
 *
 * a) generally ignore logging errors
 * b) generally check logging errors
 *
 * Our current approach is to generally ignore logging errors, checking
 * only once (in the function below) at the very beginning of the program,
 * mostly to catch errors of type (5), and failing if we cannot log anything
 * at all. Because this happens before we notify READY=1, the service will
 * never appear started and systemctl start will fail, making it obvious to
 * the sysadmin that something is wrong.
 *
 * If we decide to check (which would massively increase complexity and may
 * slightly affect performance), we should assert against (1); assert against (2)
 * when there is no user-provided parameters (and fall back to a message without them
 * if there are); ignore (3); fall back to some other (stderr?) logging if (4) or (5)
 * and quietly retry (6).
 *
 * The reason to ignore (3) is because logging code is not a structurally sane
 * place to recover from ENOMEM. If we're limited, we'll hit ENOMEM again later
 * and we can do a better job at recovering then.
 */
static void check_journal_and_abort_if_needed()
{
    int ret = sd_journal_print(LOG_INFO, "Starting vrtd...");
    if (ret < 0) {
        (void) fprintf(stderr, "Failed to access systemd journal\n");
        exit(EXIT_FAILURE);
    }
}

static int configure_signals(sd_event *ev, struct vrtd *state)
{
    struct sigaction sa_ignore = { .sa_handler = SIG_IGN };
    int ret = sigemptyset(&sa_ignore.sa_mask);
    PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Error manipulating signal set");
    
    ret = sigaction(SIGPIPE, &sa_ignore, NULL);
    PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Failed to ignore SIGPIPE");

    int signals[] = {SIGINT, SIGTERM, SIGQUIT, SIGHUP};

    ret = block_signals(signals, SIZEOF_ARRAY(signals));
    PROPAGATE_ERROR(ret);

    for (size_t i = 0; i < SIZEOF_ARRAY(signals); i++) {
        ret = sd_event_add_signal(ev, NULL, signals[i], on_event_signal, state);
        PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to add event source: %s", sigabbrev_np(signals[i]));
    }

    return 0;
}

static int block_signals(const int *signals, size_t n)
{
    sigset_t set;
    sigemptyset(&set);
    for (size_t i = 0; i < n; i++) {
        sigaddset(&set, signals[i]);
    }

    int ret = sigprocmask(SIG_BLOCK, &set, NULL);
    PROPAGATE_ERROR_STDC_LOG(ret,LOG_CRIT, "Failed to mask signals");

    return 0;
}

static int configure_sockets(sd_event *ev, struct vrtd *state)  
{
    _cleanup_(cleanup_argv)
    char **names = NULL;

    int ret = sd_listen_fds_with_names(1, &names);
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Could not list listen fds");
    int n_fds = ret;

    for (int i = 0; i < n_fds; i++) {
        int fd = SD_LISTEN_FDS_START + i;
        const char *name = (names != NULL && names[i] != NULL) ? names[i] : "systemd";

        ret = sd_is_socket(fd, AF_UNIX, SOCK_SEQPACKET, 1);
        PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to get state of socket %s", name);
        if (ret == 0) {
            LOG(LOG_ERR, "Bad socket type %s", name);
            return -1;
        }

        ret = register_listening_socket(ev, state, fd, name);
        PROPAGATE_ERROR(ret);
    }

    const char *env_socket_path = getenv("VRTD_SOCKET");
    if (env_socket_path != NULL && env_socket_path[0] != '\0') {
        int fd = create_listening_unix_socket(env_socket_path);
        PROPAGATE_ERROR(fd);

        ret = register_listening_socket(ev, state, fd, env_socket_path);
        if (ret == -1) {
            (void) close(fd);
            (void) unlink(env_socket_path);
            return -1;
        }
    } else if (n_fds == 0) {
        LOG(LOG_ERR, "No socket provided");
        return -1;
    }

    return 0;
}

static int create_listening_unix_socket(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        LOG(LOG_ERR, "VRTD_SOCKET must not be empty");
        return -1;
    }

    struct sockaddr_un addr = {0};
    size_t path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        LOG(LOG_ERR, "VRTD_SOCKET path is too long: %s", path);
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    PROPAGATE_ERROR_STDC_LOG(fd, LOG_ERR, "Failed to create VRTD_SOCKET listener %s", path);

    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, path_len + 1);

    (void) unlink(path);

    int ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == -1) {
        LOG(LOG_ERR, "Failed to bind VRTD_SOCKET listener %s: %m", path);
        (void) close(fd);
        return -1;
    }

    ret = chmod(path, 0666);
    if (ret == -1) {
        LOG(LOG_ERR, "Failed to chmod VRTD_SOCKET listener %s: %m", path);
        (void) close(fd);
        (void) unlink(path);
        return -1;
    }

    ret = listen(fd, SOMAXCONN);
    if (ret == -1) {
        LOG(LOG_ERR, "Failed to listen on VRTD_SOCKET listener %s: %m", path);
        (void) close(fd);
        (void) unlink(path);
        return -1;
    }

    free(created_socket_path);
    created_socket_path = strdup(path);
    if (created_socket_path == NULL) {
        LOG(LOG_ERR, "Failed to remember VRTD_SOCKET path %s: %m", path);
        (void) close(fd);
        (void) unlink(path);
        return -1;
    }

    return fd;
}

static int register_listening_socket(sd_event *ev, struct vrtd *state, int fd, const char *name)
{
    int flags = fcntl(fd, F_GETFL, 0);
    PROPAGATE_ERROR_STDC_LOG(flags, LOG_ERR, "Failed to get fcntl for fd=%d (%s)", fd, name);

    int ret = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Failed to set fcntl for fd=%d (%s)", fd, name);

    _cleanup_(sd_event_source_unrefp)
    sd_event_source *source = NULL;
    ret = sd_event_add_io(ev, &source, fd, EPOLLIN, on_event_new_connection, state);
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to set up listening for socket %s", name);

    _cleanup_(cleanup_free)
    char *description = NULL;

    ret = asprintf(&description, "Unix socket %s", name);
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Could not allocate description for socket %s", name);

    ret = sd_event_source_set_description(source, description);
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Could not set description for socket %s", name);

    ret = sd_event_source_set_io_fd_own(source, 1);
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to set up fd ownership for socket %s", name);

    ret = sd_event_source_set_floating(source, 1);
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to set up floating source for socket %s", name);

    ret = sd_event_source_set_exit_on_failure(source, 1);
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to set up exit on failure for socket %s", name);

    LOG(LOG_INFO, "Listening on unix socket %s", name);

    return 0;
}

static int configure_watchdog(sd_event *ev)
{
    int ret = sd_event_set_watchdog(ev, 1);
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to enable watchdog");

    return 0;
}

static int configure_background_tasks(sd_event *ev, struct vrtd *state)
{
    uint64_t now = 0;
    int ret = sd_event_now(ev, CLOCK_MONOTONIC, &now);
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to read event loop clock");

    _cleanup_(sd_event_source_unrefp)
    sd_event_source *source = NULL;
    ret = sd_event_add_time(
        ev,
        &source,
        CLOCK_MONOTONIC,
        now + VRTD_DEFERRED_WORK_INTERVAL_USEC,
        VRTD_DEFERRED_WORK_INTERVAL_USEC / 2,
        on_event_deferred_work,
        state
    );
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to add deferred work timer");

    ret = sd_event_source_set_description(source, "Deferred request poll");
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to set deferred work timer description");

    ret = sd_event_source_set_floating(source, 1);
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to float deferred work timer source");

    ret = sd_event_source_set_exit_on_failure(source, 1);
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to set exit-on-failure for deferred work timer");

    return 0;
}

void globals_init(void)
{
    hotplug_global_init();
}

void globals_destroy(void)
{
    hotplug_global_destroy();
}

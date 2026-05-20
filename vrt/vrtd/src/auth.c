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
 * @file auth.c
 * @brief Per-request authorization enforcement for vrtd.
 *
 * This file implements the authorization layer of the V80 Runtime Daemon
 * (vrtd). Every incoming client request passes through one of the
 * auth_request_*() functions before the daemon executes the corresponding
 * operation. Each function checks that the client's effective role grants
 * the required permissions.
 *
 * Permission model:
 *   - A connected client carries a UID and a set of GIDs (obtained from the
 *     Unix domain socket credentials at connection time).
 *   - On the first authorization check, ensure_role() lazily constructs the
 *     client's effective (merged) role by combining:
 *       1. The **default user** roles (applied to every client).
 *       2. Any **user-specific** roles whose UID matches the client's UID.
 *       3. Any **group-specific** roles whose GID matches one of the client's
 *          supplementary GIDs.
 *     Merging uses logical OR for each permission flag, so the client receives
 *     the union (most permissive) of all applicable roles.
 *   - The merged role is cached on the client struct for the lifetime of
 *     the connection.
 *
 * Authorization check categories:
 *   - **Query-only** operations (get_device_info, get_device_by_bdf,
 *     get_num_devices, get_bar_info, qdma_get_info): require only the
 *     `query` permission.
 *   - **Device-access** operations (get_bar_fd, qdma_qpair_add/op/get_fd,
 *     buffer_open/close, design_write, clock_op): require `query` plus
 *     the corresponding per-device per-subsystem permission (bar, qdma,
 *     buffer, design-write, clock) as defined in the role's device_policies.
 *   - **Hotplug** operations (device_hotplug_op): require `query` plus
 *     per-device pcie-hotplug permission or the global pcie_hotplug flag.
 *
 * Return convention for auth_request_*() functions:
 *   - 1  = authorized (proceed with the operation)
 *   - 0  = denied (the daemon should send an error response)
 *   - <0 = internal error (propagated via PROPAGATE_ERROR)
 */

#define _GNU_SOURCE

#include "auth.h"
#include "config.h"
#include "device.h"
#include "state.h"
#include "utils.h"

#include <assert.h>
#include <sys/syslog.h>
#include <stdio.h>
#include <string.h>

int ensure_role(struct client *client);

/**
 * @brief Identifies which subsystem permission to check on a device.
 */
enum auth_subsystem {
    AUTH_SUBSYSTEM_BAR,
    AUTH_SUBSYSTEM_QDMA,
    AUTH_SUBSYSTEM_BUFFER,
    AUTH_SUBSYSTEM_DESIGN_WRITE,
    AUTH_SUBSYSTEM_CLOCK,
    AUTH_SUBSYSTEM_PCIE_HOTPLUG,
    AUTH_SUBSYSTEM_RAW_MEM_ACCESS,
};

/**
 * @brief Human-readable names for auth_subsystem values (for log messages).
 */
static const char *auth_subsystem_name(enum auth_subsystem subsystem)
{
    switch (subsystem) {
    case AUTH_SUBSYSTEM_BAR:          return "bar-access";
    case AUTH_SUBSYSTEM_QDMA:         return "qdma";
    case AUTH_SUBSYSTEM_BUFFER:       return "buffer";
    case AUTH_SUBSYSTEM_DESIGN_WRITE: return "design-write";
    case AUTH_SUBSYSTEM_CLOCK:        return "clock";
    case AUTH_SUBSYSTEM_PCIE_HOTPLUG:  return "pcie-hotplug";
    case AUTH_SUBSYSTEM_RAW_MEM_ACCESS: return "raw-mem-access";
    default:                           return "unknown";
    }
}

/**
 * @brief Check whether a device_policy grants a specific subsystem permission.
 */
static bool device_policy_check(const struct device_policy *dp, enum auth_subsystem subsystem)
{
    switch (subsystem) {
    case AUTH_SUBSYSTEM_BAR:          return dp->bar;
    case AUTH_SUBSYSTEM_QDMA:         return dp->qdma;
    case AUTH_SUBSYSTEM_BUFFER:       return dp->buffer;
    case AUTH_SUBSYSTEM_DESIGN_WRITE: return dp->design_write;
    case AUTH_SUBSYSTEM_CLOCK:        return dp->clock;
    case AUTH_SUBSYSTEM_PCIE_HOTPLUG:  return dp->pcie_hotplug;
    case AUTH_SUBSYSTEM_RAW_MEM_ACCESS: return dp->raw_mem_access;
    default:                           return false;
    }
}

/**
 * @brief Build a comma-separated string of all role names applicable to a client.
 *
 * Iterates through the default user's roles, user-specific roles (matched by
 * UID), and group-specific roles (matched by GID) to produce a human-readable
 * summary. This is used in denial log messages to help administrators
 * understand which roles a client actually has.
 *
 * @param client  The client whose roles should be collected.
 * @return Heap-allocated comma-separated role name string, or NULL if the
 *         client has no roles or on allocation failure. Caller must free().
 */
static char *auth_collect_role_names(const struct client *client)
{
    assert(client->state != NULL);
    assert(client->state->config != NULL);
    const struct config *config = client->state->config;

    char *roles_str = NULL;
    size_t roles_len = 0;
    bool any_role = false;

    #define APPEND_ROLE(r) \
        do { \
            const char *rname = (r)->name; \
            if (rname == NULL) break; \
            size_t rlen = strlen(rname); \
            size_t need = roles_len + (any_role ? 2 : 0) + rlen + 1; \
            char *tmp = realloc(roles_str, need); \
            if (tmp == NULL) { free(roles_str); return NULL; } \
            roles_str = tmp; \
            if (any_role) { \
                memcpy(roles_str + roles_len, ", ", 2); \
                roles_len += 2; \
            } \
            memcpy(roles_str + roles_len, rname, rlen + 1); \
            roles_len += rlen; \
            any_role = true; \
        } while (0)

    /* Collect roles from the default (wildcard) user -- these apply to everyone. */
    if (config->default_user != NULL) {
        for (size_t i = 0; i < config->default_user->roles.len; i++) {
            APPEND_ROLE(config->default_user->roles.d[i]);
        }
    }

    /* Collect roles from user entries matching this client's UID. */
    for (size_t i = 0; i < config->users.len; i++) {
        const struct user_config *uc = config->users.d[i];
        if (uc == NULL || uc->uid != client->uid) {
            continue;
        }
        for (size_t j = 0; j < uc->roles.len; j++) {
            APPEND_ROLE(uc->roles.d[j]);
        }
    }

    /* Collect roles from group entries matching any of the client's GIDs. */
    for (size_t i = 0; i < config->groups.len; i++) {
        const struct group_config *gc = config->groups.d[i];
        if (gc == NULL) {
            continue;
        }
        for (size_t j = 0; j < client->gids.len; j++) {
            if (gc->gid != client->gids.d[j]) {
                continue;
            }
            for (size_t k = 0; k < gc->roles.len; k++) {
                APPEND_ROLE(gc->roles.d[k]);
            }
            break;
        }
    }

    #undef APPEND_ROLE

    return roles_str;
}

/**
 * @brief Log a permission-denied message with the client's identity and roles.
 *
 * Emits a LOG_WARNING with the denied operation and the specific permission
 * that was missing, then a LOG_INFO listing all roles the client holds (to
 * aid debugging and auditing).
 *
 * @param client              The client that was denied.
 * @param operation           Human-readable name of the denied operation.
 * @param missing_permission  The specific permission flag the client lacks.
 */
static void auth_log_denied(
    struct client *client,
    const char *operation,
    const char *missing_permission
)
{
    char pwbuf[1024];
    const char *username = uid_to_username(client->uid, pwbuf, sizeof(pwbuf));

    LOG(
        LOG_WARNING,
        "Permission denied for uid %u(%s): '%s' requires '%s'",
        (unsigned int) client->uid,
        username,
        operation,
        missing_permission
    );

    char *roles_str = auth_collect_role_names(client);
    if (roles_str != NULL) {
        LOG(
            LOG_INFO,
            "User uid %u(%s) has roles: %s",
            (unsigned int) client->uid,
            username,
            roles_str
        );
        free(roles_str);
    } else {
        LOG(
            LOG_INFO,
            "User uid %u(%s) has no roles",
            (unsigned int) client->uid,
            username
        );
    }
}

/* ========================================================================
 * Per-device, per-subsystem authorization check
 *
 * This is the core authorization helper for all device-access operations.
 * It resolves the target device's BDF from the device index, then looks up
 * the client's role for a matching device_policy entry (exact BDF match
 * first, then "any" wildcard fallback).
 * ======================================================================== */

/**
 * @brief Check whether the client's role grants a specific subsystem
 *        permission on the device identified by @p dev_number.
 *
 * Resolution logic:
 *   1. Verify the client has the "query" permission (prerequisite for all
 *      device operations).
 *   2. Resolve @p dev_number to the device's board-level BDF string.
 *   3. Search the role's device_policies for an exact BDF match.
 *   4. If no exact match, search for an "any" wildcard entry.
 *   5. Check the requested subsystem flag on the matching policy.
 *   6. For PCIE_HOTPLUG, also check the global role->pcie_hotplug flag.
 *
 * @param client     The requesting client (carries the role and device state).
 * @param dev_number The 0-based device index from the request body.
 * @param subsystem  Which subsystem permission to check.
 * @param operation  Human-readable operation name (for denial logging).
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
static int auth_check_device_permission(
    struct client *client,
    uint32_t dev_number,
    enum auth_subsystem subsystem,
    const char *operation
)
{
    assert(client != NULL);
    assert(client->role != NULL);

    /* All device-access operations require the query permission. */
    if (!client->role->query) {
        auth_log_denied(client, operation, "query");
        return 0;
    }

    /* Resolve device index to BDF string. */
    assert(client->state != NULL);
    if (dev_number >= client->state->devices.len) {
        /* Invalid device index -- this is a request validation error,
         * not an auth error, but deny it here for safety. */
        return 0;
    }

    const struct device *dev = client->state->devices.d[dev_number];
    assert(dev != NULL);
    const char *dev_bdf = dev->pci_info.bdf;

    /* Search for a device_policy matching this device's BDF. */
    const struct device_policy *dp = NULL;
    const struct device_policy *any_dp = NULL;

    for (size_t i = 0; i < client->role->device_policies.len; i++) {
        const struct device_policy *candidate = client->role->device_policies.d[i];
        if (strcmp(candidate->bdf, dev_bdf) == 0) {
            dp = candidate;
            break;
        }
        if (strcmp(candidate->bdf, "any") == 0) {
            any_dp = candidate;
        }
    }

    /* Fall back to "any" wildcard if no exact match. */
    if (dp == NULL) {
        dp = any_dp;
    }

    if (dp == NULL) {
        /* No device policy matches -- denied. */
        char denied_msg[128];
        snprintf(denied_msg, sizeof(denied_msg), "%s (device %s)",
                 auth_subsystem_name(subsystem), dev_bdf);
        auth_log_denied(client, operation, denied_msg);
        return 0;
    }

    if (!device_policy_check(dp, subsystem)) {
        char denied_msg[128];
        snprintf(denied_msg, sizeof(denied_msg), "%s (device %s)",
                 auth_subsystem_name(subsystem), dev_bdf);
        auth_log_denied(client, operation, denied_msg);
        return 0;
    }

    return 1;
}

/* ========================================================================
 * Query-only authorization checks
 *
 * These operations only require the "query" permission, which allows
 * enumerating and inspecting devices without modifying state or accessing
 * device memory.
 * ======================================================================== */

/**
 * @brief Authorize a get_device_info request.
 *
 * Requires: query permission.
 *
 * @param client    The requesting client.
 * @param req_body  The request payload (unused for auth, present for signature consistency).
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_get_device_info(
    struct client *client,
    const struct vrtd_req_get_device_info *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    assert(client->role != NULL);

    if (client->role->query) {
        return 1;
    } else {
        auth_log_denied(client, "get_device_info", "query");
        return 0;
    }
}

/**
 * @brief Authorize a get_device_by_bdf request.
 *
 * Requires: query permission.
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_get_device_by_bdf(
    struct client *client,
    const struct vrtd_req_get_device_by_bdf *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    assert(client->role != NULL);

    if (client->role->query) {
        return 1;
    } else {
        auth_log_denied(client, "get_device_by_bdf", "query");
        return 0;
    }
}

/**
 * @brief Authorize a get_num_devices request.
 *
 * Requires: query permission.
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_get_num_devices(
    struct client *client,
    const struct vrtd_req_get_num_devices *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    assert(client->role != NULL);

    if (client->role->query) {
        return 1;
    } else {
        auth_log_denied(client, "get_num_devices", "query");
        return 0;
    }
}

/**
 * @brief Authorize a get_bar_info request.
 *
 * Requires: query permission. This only retrieves BAR metadata (size,
 * address); it does not grant memory-mapped access.
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_get_bar_info(
    struct client *client,
    const struct vrtd_req_get_bar_info *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    assert(client->role != NULL);

    if (client->role->query) {
        return 1;
    } else {
        auth_log_denied(client, "get_bar_info", "query");
        return 0;
    }
}

/* ========================================================================
 * Device-access authorization checks
 *
 * These operations provide direct access to device resources (BAR file
 * descriptors, QDMA queue pairs, DMA buffers, design programming, clocks).
 * Each operation checks the corresponding per-device per-subsystem permission
 * via auth_check_device_permission().
 * ======================================================================== */

/**
 * @brief Authorize a get_bar_fd request (memory-mapped BAR access).
 *
 * Requires: query + bar-access permission on the target device.
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_get_bar_fd(
    struct client *client,
    const struct vrtd_req_get_bar_fd *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    return auth_check_device_permission(
        client, req_body->dev_number, AUTH_SUBSYSTEM_BAR, "get_bar_fd"
    );
}

/**
 * @brief Authorize a qdma_get_info request (QDMA subsystem query).
 *
 * Requires: query permission only (informational, no data-plane access).
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_qdma_get_info(
    struct client *client,
    const struct vrtd_req_qdma_get_info *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    assert(client->role != NULL);

    if (client->role->query) {
        return 1;
    } else {
        auth_log_denied(client, "qdma_get_info", "query");
        return 0;
    }
}

/**
 * @brief Authorize a qdma_qpair_add request (create a QDMA queue pair).
 *
 * Requires: query + qdma permission on the target device.
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_qdma_qpair_add(
    struct client *client,
    const struct vrtd_req_qdma_qpair_add *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    return auth_check_device_permission(
        client, req_body->dev_number, AUTH_SUBSYSTEM_QDMA, "qdma_qpair_add"
    );
}

/**
 * @brief Authorize a qdma_qpair_op request (start/stop a QDMA queue pair).
 *
 * Requires: query + qdma permission on the target device.
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_qdma_qpair_op(
    struct client *client,
    const struct vrtd_req_qdma_qpair_op *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    return auth_check_device_permission(
        client, req_body->dev_number, AUTH_SUBSYSTEM_QDMA, "qdma_qpair_op"
    );
}

/**
 * @brief Authorize a qdma_qpair_get_fd request (get fd for a QDMA queue pair).
 *
 * Requires: query + qdma permission on the target device.
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_qdma_qpair_get_fd(
    struct client *client,
    const struct vrtd_req_qdma_qpair_get_fd *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    return auth_check_device_permission(
        client, req_body->dev_number, AUTH_SUBSYSTEM_QDMA, "qdma_qpair_get_fd"
    );
}

/**
 * @brief Authorize a buffer_open request (allocate a DMA buffer).
 *
 * Requires: query + buffer permission on the target device.
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_buffer_open(
    struct client *client,
    const struct vrtd_req_buffer_open *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    return auth_check_device_permission(
        client, req_body->dev_number, AUTH_SUBSYSTEM_BUFFER, "buffer_open"
    );
}

/**
 * @brief Authorize a buffer_open_raw request (open a raw DMA buffer, bypassing the allocator).
 *
 * Requires: query + raw-mem-access permission on the target device.
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_buffer_open_raw(
    struct client *client,
    const struct vrtd_req_buffer_open_raw *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    return auth_check_device_permission(
        client, req_body->dev_number, AUTH_SUBSYSTEM_RAW_MEM_ACCESS, "buffer_open_raw"
    );
}

/**
 * @brief Authorize a buffer_close request (release a DMA buffer).
 *
 * Requires: query + buffer permission on the target device.
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_buffer_close(
    struct client *client,
    const struct vrtd_req_buffer_close *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    return auth_check_device_permission(
        client, req_body->dev_number, AUTH_SUBSYSTEM_BUFFER, "buffer_close"
    );
}

/**
 * @brief Authorize a design_write request (program an FPGA bitstream).
 *
 * Requires: query + design-write permission on the target device.
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_design_write(
    struct client *client,
    const struct vrtd_req_design_write *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    return auth_check_device_permission(
        client, req_body->dev_number, AUTH_SUBSYSTEM_DESIGN_WRITE, "design_write"
    );
}

/* ========================================================================
 * Hotplug authorization check
 *
 * PCIe hotplug is a destructive control-plane operation (removing/adding
 * devices from the bus). It requires a per-device pcie-hotplug flag in a
 * device_policy (specified via [role:name:bdf] or [role:name:any]).
 * ======================================================================== */

/**
 * @brief Authorize a device_hotplug_op request (PCIe hot-plug/remove).
 *
 * Requires: query + pcie-hotplug permission on the target device.
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_device_hotplug_op(
    struct client *client,
    const struct vrtd_req_device_hotplug_op *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    return auth_check_device_permission(
        client, req_body->dev_number, AUTH_SUBSYSTEM_PCIE_HOTPLUG, "device_hotplug_op"
    );
}

/**
 * @brief Authorize a clock_op request (read/modify device clock settings).
 *
 * Requires: query + clock permission on the target device.
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_clock_op(
    struct client *client,
    const struct vrtd_req_clock_op *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    return auth_check_device_permission(
        client, req_body->dev_number, AUTH_SUBSYSTEM_CLOCK, "clock_op"
    );
}

/**
 * @brief Authorize a get_sensor_info request.
 *
 * Requires: query permission only (informational, read-only sensor data).
 *
 * @param client    The requesting client.
 * @param req_body  The request payload.
 * @return 1 if authorized, 0 if denied, <0 on internal error.
 */
int auth_request_get_sensor_info(
    struct client *client,
    const struct vrtd_req_get_sensor_info *req_body
)
{
    assert(client != NULL);
    assert(req_body != NULL);

    int ret = ensure_role(client);
    PROPAGATE_ERROR(ret);

    assert(client->role != NULL);

    if (client->role->query) {
        return 1;
    } else {
        auth_log_denied(client, "get_sensor_info", "query");
        return 0;
    }
}

/* ========================================================================
 * Role resolution (lazy, per-client)
 *
 * ensure_role() is called at the top of every auth_request_*() function.
 * On first invocation for a given client it builds the merged effective role;
 * on subsequent calls it returns immediately (the role is cached on the
 * client struct).
 *
 * Merging order:
 *   1. Start with an empty role (all permissions false).
 *   2. OR in the default_user's roles (wildcard -- applies to everyone).
 *   3. OR in roles from any [user:<name>] entry whose UID matches the
 *      client's UID.
 *   4. OR in roles from any [group:<name>] entry whose GID matches one
 *      of the client's supplementary GIDs.
 *
 * Because merging uses OR, a permission granted by any single matching
 * role cannot be revoked by another role (highest privilege wins).
 * ======================================================================== */

/**
 * @brief Lazily construct the merged effective role for a client.
 *
 * If client->role is already set, returns immediately. Otherwise, creates a
 * new role via role_merge_new() and merges in permissions from all applicable
 * sources: default user roles, UID-matched user roles, and GID-matched group
 * roles.
 *
 * The resulting role is stored on client->role and persists for the lifetime
 * of the client connection (it is freed when the client is cleaned up).
 *
 * @param client  The client whose role should be resolved.
 * @return 0 on success, <0 on allocation or merge error.
 */
int ensure_role(struct client *client)
{
    assert(client != NULL);

    /* If a role has already been computed for this client, use the cached one. */
    if (client->role != NULL) {
        return 0;
    }

    _cleanup_(cleanup_free)
    char *role_name = NULL;

    int ret = asprintf(&role_name, "Internal role for user: %u", (unsigned int) client->uid);
    PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Allocation error when creating internal role for user");

    _cleanup_(cleanup_rolep)
    struct role *role = NULL;

    ret = role_merge_new(&role, "TODO: Change this string");
    PROPAGATE_ERROR(ret);

    assert(client->state != NULL);
    assert(client->state->config != NULL);

    const struct config *config = client->state->config;

    /* Step 1: merge in default user roles (apply to every client). */
    ret = role_merge_add_array(role, &config->default_user->roles);
    PROPAGATE_ERROR(ret);

    /* Step 2: merge in roles from user entries matching this client's UID. */
    for (size_t i = 0; i < config->users.len; i++) {
        const struct user_config *user_config = config->users.d[i];
        assert(user_config != NULL);

        if (user_config->uid == client->uid) {
            ret = role_merge_add_array(role, &user_config->roles);
            PROPAGATE_ERROR(ret);
        }
    }

    /* Step 3: merge in roles from group entries matching any of the client's GIDs. */
    for (size_t i = 0; i < config->groups.len; i++) {
        const struct group_config *group_config = config->groups.d[i];
        assert(group_config != NULL);

        for (size_t j = 0; j < client->gids.len; j++) {
            gid_t gid = client->gids.d[j];

            if (group_config->gid == gid) {
                ret = role_merge_add_array(role, &group_config->roles);
                PROPAGATE_ERROR(ret);
            }
        }
    }

    /* Transfer ownership of the merged role to the client. */
    client->role = role;
    role = NULL;

    return 0;
}

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
 * @file config.c
 * @brief INI configuration loader for vrtd role-based access control.
 *
 * This file implements the configuration subsystem for the V80 Runtime Daemon
 * (vrtd). The daemon uses an INI-style configuration file (parsed via inih) to
 * define:
 *
 *   - **Roles**: named permission bundles that control what operations a client
 *     may perform (e.g. query devices, access BARs, perform PCIe hotplug).
 *   - **Users**: system user accounts mapped (by UID) to one or more roles.
 *   - **Groups**: system groups mapped (by GID) to one or more roles.
 *   - **Default user** (wildcard `*`): roles that apply to every client,
 *     regardless of UID/GID match.
 *
 * A single client may match multiple roles (via user entry, group entries, and
 * the default user). At authorization time the effective permission set is the
 * **union** of all matched roles -- i.e. highest privilege wins. Role merging
 * is performed by role_merge_add_role() which ORs the boolean permission
 * fields.
 *
 * Configuration loading flow (config_load):
 *   1. Determine config file path from $VRTD_CONFIG or the compiled-in
 *      default (/etc/vrt/vrtd.conf).
 *   2. Parse the INI file via inih; the callback dispatches each key/value
 *      to the appropriate handler based on section type:
 *        - Top-level `include` / `include-glob`: recursively parse other files.
 *        - `[role:<name>]`: create / update a role definition.
 *        - `[user:<name>]` or `[user:*]`: map a user (or wildcard) to roles.
 *        - `[group:<name>]`: map a group to roles.
 *   3. After all files are parsed, resolve the role name strings stored in
 *      each user/group config to pointers into the role array
 *      (assign_users_roles / assign_groups_roles).
 *
 * The config supports circular-include detection via a visited_files list, and
 * glob-based includes for drop-in configuration directories.
 */

#define _GNU_SOURCE

#define VRTD_DEFAULT_CONFIG_PATH "/etc/vrt/vrtd.conf"

#include "array.h"
#include "config.h"
#include "utils.h"

#include <assert.h>
#include <errno.h>
#include <grp.h>
#include <glob.h>
#include <pwd.h>
#include <sys/syslog.h>
#include <stdbool.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <ini.h>
#include <systemd/sd-journal.h>

// This is on Ubuntu
static_assert(INI_HANDLER_LINENO == 0, "vrtd does not support INI_HANDLER_LINENO = 1");

/**
 * Sentinel user name for the default (wildcard) user entry.
 * Any client that does not match a specific [user:<name>] section will still
 * receive the roles assigned to [user:*].
 */
static const char DEFAULT_USER_NAME[] = "*";

/**
 * @brief Transient state carried through a single config_load() invocation.
 *
 * @param config        Pointer to the config being populated.
 * @param visited_files Canonical paths of files already parsed, used to
 *                      prevent infinite include cycles.
 */
struct config_parse_state {
    struct config *config;

    struct str_array visited_files;
};

/* Forward declarations for internal helpers. */
static int parse_file_glob(struct config_parse_state *state, const char *pattern);
static int resolve_relative_pattern(struct config_parse_state *state, const char *pattern, char **abs_pattern);
static int parse_file(struct config_parse_state *state, const char *path);
static int parse_file_unique(struct config_parse_state *state, const char *path);
static int parse_config_callback(void *user, const char *section, const char *name, const char *value);
static int role_find_and_add_value(struct config *config, const char *objname, const char *name, const char *value);
static int role_add_value(struct role *role, const char *name, const char *value);
static int role_device_find_and_add_value(struct config *config, const char *role_name, const char *dev_selector, const char *name, const char *value);
static int device_policy_add_value(struct device_policy *dp, const char *name, const char *value);
static int user_find_and_add_value(struct config *config, const char *objname, const char *name, const char *value);
static int user_add_value(struct user_config *user, const char *name, const char *value);
static int group_find_and_add_value(struct config *config, const char *objname, const char *name, const char *value);
static int group_add_value(struct group_config *group, const char *name, const char *value);
static int set_user_uid(uid_t *uid, const char *name);
static int set_group_gid(gid_t *gid, const char *name);
static int assign_users_roles(struct config *config);
static int assign_user_roles(struct config *config, struct user_config *user);
static int assign_groups_roles(struct config *config);
static int assign_group_roles(struct config *config, struct group_config *group);
static struct role *role_find_or_create(struct config *config, const char *role_name);
static int normalize_bdf(const char *input, char *out, size_t out_len);
static struct device_policy *find_device_policy(const struct device_policy_ptr_array *policies, const char *bdf);
static int find_or_create_device_policy(struct device_policy_ptr_array *policies, const char *bdf, struct device_policy **out);

/* ========================================================================
 * Cleanup helpers
 *
 * These functions are used with the _cleanup_ GCC attribute to provide
 * automatic resource cleanup on scope exit (RAII-style). Each frees all
 * owned memory within the given struct and NULLs dangling pointers.
 * ======================================================================== */

/**
 * @brief Free all resources owned by a device_policy.
 *
 * Releases the BDF string and the struct itself.
 *
 * @param dp  Pointer to the device_policy to clean up, or NULL (no-op).
 */
void cleanup_device_policy(struct device_policy *dp)
{
    if (dp == NULL) {
        return;
    }

    free(dp->bdf);
    dp->bdf = NULL;

    free(dp);
}

/**
 * @brief Free all resources owned by a role.
 *
 * Releases the role name string, the device_policies array, and the role
 * struct itself.
 *
 * @param role  Pointer to the role to clean up, or NULL (no-op).
 */
void cleanup_role(struct role *role)
{
    if (role == NULL) {
        return;
    }

    free(role->name);
    role->name = NULL;

    device_policy_ptr_array_free(&role->device_policies);

    free(role);
}

/**
 * @brief Free all resources owned by a user_config.
 *
 * Releases the user name, the role_names string array (used during parsing),
 * the resolved roles reference array, and the struct itself.
 *
 * @param user  Pointer to the user_config to clean up, or NULL (no-op).
 */
void cleanup_user_config(struct user_config *user)
{
    if (user == NULL) {
        return;
    }

    free(user->name);
    user->name = NULL;

    str_array_free(&user->role_names);
    role_ref_array_free(&user->roles);

    free(user);
}

/**
 * @brief Free all resources owned by a group_config.
 *
 * Releases the group name, role_names, resolved roles, and the struct itself.
 *
 * @param group  Pointer to the group_config to clean up, or NULL (no-op).
 */
void cleanup_group_config(struct group_config *group)
{
    if (group == NULL) {
        return;
    }

    free(group->name);
    group->name = NULL;

    str_array_free(&group->role_names);
    role_ref_array_free(&group->roles);

    free(group);
}

/**
 * @brief Free all resources owned by a config.
 *
 * Releases the roles array (which owns the role structs), the default user,
 * the users array, and the groups array.
 *
 * @param config  Pointer to the config to clean up, or NULL (no-op).
 */
void cleanup_config(struct config *config)
{
    if (config == NULL) {
        return;
    }

    role_ptr_array_free(&config->roles);

    cleanup_user_config(config->default_user);

    user_config_ptr_array_free(&config->users);
    group_config_ptr_array_free(&config->groups);

    free(config);
}

/**
 * @brief Clean up the stack-allocated parse state.
 *
 * Frees the visited_files string array. The config pointer is simply NULLed
 * (ownership is transferred to the caller on success).
 *
 * @param state  Pointer to the parse state to clean up.
 */
static inline
void cleanup_parse_state_stack(struct config_parse_state *state)
{
    state->config = NULL;

    str_array_free(&state->visited_files);
}

/* ========================================================================
 * Role merging
 *
 * A client can hold multiple roles (from user entries, group entries, and
 * the default user). The effective permission set is the union (logical OR)
 * of all individual roles. These helpers construct the merged role.
 * ======================================================================== */

/**
 * @brief Allocate a new empty role with the given name.
 *
 * All permission flags start as false/zero. The caller receives ownership
 * of the role via *rolep.
 *
 * @param[out] rolep  Receives the newly allocated role.
 * @param      name   Human-readable name for the role (copied).
 * @return 0 on success, -1 on allocation failure.
 */
int role_merge_new(struct role **rolep, const char *name)
{
    assert(rolep != NULL);

    _cleanup_(cleanup_rolep)
    struct role *role = calloc(1, sizeof *role);
    PROPAGATE_ERROR_NULL_STDC_LOG(role, LOG_ERR, "Error allocating new role");

    _cleanup_(cleanup_free)
    char *s = strdup(name);
    PROPAGATE_ERROR_NULL_STDC_LOG(s, LOG_ERR, "Error allocating new role");

    role->name = s;
    s = NULL;

    *rolep = role;
    role = NULL;

    return 0;
}

/**
 * @brief Merge a source role's permissions into a destination role.
 *
 * For each boolean permission field, the destination retains `true` if either
 * the destination or source had it set ("highest privilege wins" via OR).
 * Per-device policies are merged by BDF: for each source device_policy,
 * a matching entry in the destination is found (or created), and each
 * subsystem flag is ORed independently.
 *
 * @param dst  The role accumulating permissions (modified in place).
 * @param src  The role whose permissions are being merged in.
 * @return 0 on success, -1 if either argument is NULL or on allocation error.
 */
int role_merge_add_role(struct role *dst, const struct role *src)
{
    if (dst == NULL || src == NULL) {
        assert(false);
        return -1;
    }

    /* Highest privilege wins => OR the global booleans. */
    dst->query = dst->query || src->query;

    /* Merge per-device policies: find-or-create in dst, OR each subsystem flag. */
    for (size_t i = 0; i < src->device_policies.len; i++) {
        const struct device_policy *src_dp = src->device_policies.d[i];
        assert(src_dp != NULL);

        struct device_policy *dst_dp = NULL;
        int ret = find_or_create_device_policy(&dst->device_policies, src_dp->bdf, &dst_dp);
        PROPAGATE_ERROR(ret);

        dst_dp->bar          = dst_dp->bar          || src_dp->bar;
        dst_dp->qdma         = dst_dp->qdma         || src_dp->qdma;
        dst_dp->buffer       = dst_dp->buffer       || src_dp->buffer;
        dst_dp->design_write = dst_dp->design_write || src_dp->design_write;
        dst_dp->clock        = dst_dp->clock        || src_dp->clock;
        dst_dp->pcie_hotplug    = dst_dp->pcie_hotplug    || src_dp->pcie_hotplug;
        dst_dp->raw_mem_access  = dst_dp->raw_mem_access  || src_dp->raw_mem_access;
    }

    return 0;
}

/**
 * @brief Merge an array of roles into a destination role.
 *
 * Iterates over @p roles and calls role_merge_add_role() for each element,
 * accumulating the union of all permissions into @p dst.
 *
 * @param dst    The role accumulating permissions.
 * @param roles  Array of role pointers to merge in.
 * @return 0 on success, -1 on error.
 */
int role_merge_add_array(struct role *dst, const struct role_ref_array *roles)
{
    if (dst == NULL || roles == NULL) {
        assert(false);
        return -1;
    }

    for (size_t i = 0; i < roles->len; ++i) {
        const struct role *r = roles->d[i];
        assert(r != NULL);

        int ret = role_merge_add_role(dst, r);
        PROPAGATE_ERROR(ret);
    }

    return 0;
}



/**
 * @brief Load the vrtd configuration from disk.
 *
 * Entry point for the configuration subsystem. Performs the following steps:
 *   1. Allocates a config struct and the default (wildcard) user entry.
 *   2. Determines the config file path: first checks the VRTD_CONFIG
 *      environment variable, then falls back to VRTD_DEFAULT_CONFIG_PATH
 *      (/etc/vrt/vrtd.conf).
 *   3. Parses the config file (and any included files) via inih callbacks,
 *      populating config->roles, config->users, config->groups, and
 *      config->default_user with parsed role names.
 *   4. Resolves the string-based role references in each user and group
 *      to actual role struct pointers (assign_users_roles / assign_groups_roles).
 *   5. On success, transfers ownership of the config to the caller via *configp.
 *      On error, all allocated memory is automatically freed via _cleanup_.
 *
 * @param[out] configp  Receives the fully loaded configuration. Caller owns it
 *                      and must eventually free it with cleanup_config().
 * @return 0 on success, -1 on error.
 */
int config_load(struct config **configp)
{
    _cleanup_(cleanup_parse_state_stack)
    struct config_parse_state state = {0};

    // Cleanup on error
    _cleanup_(cleanup_configp)
    struct config *config = calloc(1, sizeof(**configp));

    config->default_user = calloc(1, sizeof(*config->default_user));
    PROPAGATE_ERROR_NULL_LOG(config->default_user, LOG_ERR, "Memory error assigning default user");

    config->default_user->name = strdup(DEFAULT_USER_NAME);
    PROPAGATE_ERROR_NULL_LOG(config->default_user->name, LOG_ERR, "Memory error assigning default user name");

    state.config = config;

    const char *path = getenv("VRTD_CONFIG");
    if (path == NULL || path[0] == '\0') {
        path = VRTD_DEFAULT_CONFIG_PATH;
    }

    int ret = parse_file(&state, path);

    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to parse config file");

    /* Phase 2: resolve role name strings to role struct pointers. */
    ret = assign_users_roles(config);
    PROPAGATE_ERROR(ret);

    ret = assign_groups_roles(config);
    PROPAGATE_ERROR(ret);

    LOG(LOG_INFO, "Configuration loaded successfully");

    // No error, do not cleanup
    *configp = config;
    config = NULL;

    return 0;
}

/**
 * @brief Resolve a relative glob/include pattern against the current config file's directory.
 *
 * If @p pattern is a relative path (does not start with '/') and there is a
 * current file being parsed (tracked in state->visited_files), this function
 * constructs an absolute path by prepending the directory of the current
 * config file.
 *
 * If @p pattern is already absolute or there is no current file context,
 * *abs_pattern is set to NULL and the caller should use @p pattern directly.
 *
 * @param      state        Current parse state (provides the visited_files stack).
 * @param      pattern      The include pattern from the config file.
 * @param[out] abs_pattern  Receives the heap-allocated absolute pattern, or NULL.
 *                          Caller must free() if non-NULL.
 * @return 0 on success, -1 on allocation failure.
 */
static int resolve_relative_pattern(struct config_parse_state *state, const char *pattern, char **abs_pattern)
{
    *abs_pattern = NULL;

    if (pattern[0] == '/' || state->visited_files.len == 0) {
        return 0;
    }

    /* Use the most recently entered config file as the base directory. */
    const char *current_file = state->visited_files.d[state->visited_files.len - 1];

    _cleanup_(cleanup_free)
    char *dir = strdup(current_file);
    PROPAGATE_ERROR_NULL_STDC_LOG(dir, LOG_ERR, "Error resolving glob pattern %s", pattern);

    /* Truncate after the last '/' to get the directory portion. */
    char *slash = strrchr(dir, '/');
    if (slash == NULL) {
        return 0;
    }
    slash[1] = '\0';

    int r = asprintf(abs_pattern, "%s%s", dir, pattern);
    PROPAGATE_ERROR_LOG(r, LOG_ERR, "Error resolving glob pattern %s", pattern);

    return 0;
}

/**
 * @brief Expand a glob pattern and parse each matched config file.
 *
 * Used to implement `include-glob = <pattern>` directives. Relative patterns
 * are resolved against the directory of the currently-parsed config file.
 * If no files match the pattern, this is silently treated as success.
 *
 * @param state    Current parse state.
 * @param pattern  A shell glob pattern (e.g. "/etc/vrt/conf.d/*.conf").
 * @return 0 on success, -1 on error parsing any matched file.
 */
static int parse_file_glob(struct config_parse_state *state, const char *pattern)
{
    _cleanup_(globfree)
    glob_t glob_state;
    memset(&glob_state, 0, sizeof(glob_state));

    /* Resolve relative patterns against the including config file's directory. */
    _cleanup_(cleanup_free)
    char *abs_pattern = NULL;
    int r = resolve_relative_pattern(state, pattern, &abs_pattern);
    PROPAGATE_ERROR(r);
    if (abs_pattern != NULL) {
        pattern = abs_pattern;
    }

    int ret = glob(pattern, GLOB_ERR, NULL, &glob_state);
    if (ret == GLOB_NOMATCH) {
        return 0;
    } else if (ret != 0) {
        LOG(
            LOG_WARNING,
            "Error matching pattern %s: %s",
            pattern,
            glob_err_to_string(ret)
        );

        return 0;
    }

    for (size_t i = 0; i < glob_state.gl_pathc; i++) {
        ret = parse_file(state, glob_state.gl_pathv[i]);
        PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Found by pattern %s", pattern);
    }

    return 0;
}

/**
 * @brief Parse a config file, with circular-include detection.
 *
 * Resolves the given path to its canonical form, then checks whether this
 * file has already been parsed (by searching state->visited_files). If it
 * has, the function returns immediately to avoid infinite include loops.
 * Otherwise, the canonical path is recorded and the file is parsed.
 *
 * @param state  Current parse state.
 * @param path   Path to the config file (may be relative or contain symlinks).
 * @return 0 on success, -1 on error.
 */
static int parse_file(struct config_parse_state *state, const char *path)
{
    _cleanup_(cleanup_free)
    char *full_path = realpath(path, NULL);
    PROPAGATE_ERROR_NULL_STDC_LOG(full_path, LOG_ERR, "Error obtaining the cannonical path for %s", path);

    /* Check for circular includes: skip if we have already visited this file. */
    for (size_t i = 0; i < state->visited_files.len; i++) {
        if (strcmp(full_path, state->visited_files.d[i]) == 0) {
            /* We have already parsed this file -- exit as OK */
            return 0;
        }
    }

    char *full_path_ref = full_path;

    int ret = str_array_push_move(&state->visited_files, &full_path);
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Error processing %s", full_path);

    LOG(LOG_DEBUG, "Parsing config file %s", full_path_ref);

    ret = parse_file_unique(state, full_path_ref);
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Error parsing file %s", full_path_ref);

    return 0;
}

/**
 * @brief Invoke the inih parser on a single config file.
 *
 * Wraps ini_parse() and translates its error codes into log messages.
 * The parse_config_callback is called for each key/value pair encountered.
 *
 * @param state  Current parse state (passed as user data to the callback).
 * @param path   Canonical path to the file to parse.
 * @return 0 on success, -1 on parse error or file-open failure.
 */
static int parse_file_unique(struct config_parse_state *state, const char *path)
{
    int ret = ini_parse(path, parse_config_callback, state);
    if (ret != 0) {
        if (ret > 0) {
            LOG(LOG_ERR, "Parse error at %s:%d", path, ret);
            return -1;
        } else if (ret == -1) {
            LOG(LOG_ERR, "Could not open file %s", path);
            return -1;
        } else if (ret == -2) {
            LOG(LOG_ERR, "Out of memory reading file %s", path);
            return -1;
        } else {
            LOG(LOG_WARNING, "Unknown error reading file %s", path);
            return 0;
        }
    }

    return 0;
}

/**
 * @brief inih callback: dispatches each INI key/value to the right handler.
 *
 * Section formats and their handlers:
 *   - Top-level (empty section):
 *       `include = <path>`        -> parse_file()       (recursive include)
 *       `include-glob = <pattern>` -> parse_file_glob()  (glob-based include)
 *       `enable-mock-device = <bool>` -> sets config->mock_device
 *   - `[role:<name>]`  -> role_find_and_add_value()  (role permission keys)
 *   - `[user:<name>]`  -> user_find_and_add_value()  (user-to-role mapping)
 *   - `[group:<name>]` -> group_find_and_add_value() (group-to-role mapping)
 *
 * Uses inih convention: returns 1 on success, 0 on error.
 *
 * @param user     Opaque pointer to config_parse_state.
 * @param section  INI section name (e.g. "role:admin", "user:jdoe", "").
 * @param name     Key name within the section.
 * @param value    Value string associated with the key.
 * @return 1 on success, 0 on error (per inih convention).
 */
// This callback uses 0 for error and 1 for success, as per inih spec
static int parse_config_callback(void *user, const char *section, const char *name, const char *value)
{
    #define MATCH(s, n) (strcmp(section, s) == 0 && strcmp(name, n) == 0)
    /**
     * MATCH_OBJECT: matches sections of the form "prefix:objname".
     * On match, sets the local `n` pointer to the substring after the colon
     * (the object's name). For example, [role:admin] yields objname = "admin".
     */
    #define MATCH_OBJECT(c, n) \
    ({ const char *colon__ = strchr(section, ':'); \
       colon__ && (size_t)(colon__ - section) == strlen(c) && \
       memcmp(section, (c), strlen(c)) == 0 && \
       (n = colon__ + 1, n[0] != '\0'); \
    })

    int ret;
    const char *objname;
    struct config_parse_state *state = user;

    if (MATCH("", "include")) {
        ret = parse_file(state, value);
        if (ret == -1) {
            return 0;
        }
    } else if (MATCH("", "include-glob")) {
        ret = parse_file_glob(state, value);
        if (ret == -1) {
            return 0;
        }
    } else if (MATCH("", "enable-mock-device")) {
        state->config->mock_device = string_to_bool(value);
    } else if (MATCH_OBJECT("role", objname)) {
        /* Check if objname contains a device selector (e.g. "admin:0000:03:00").
         * Role names must not contain colons, so the first colon in objname
         * separates the role name from the device selector. */
        const char *dev_sep = strchr(objname, ':');
        if (dev_sep != NULL) {
            char *role_name = strndup(objname, (size_t)(dev_sep - objname));
            if (role_name == NULL) {
                LOG(LOG_ERR, "Could not allocate role name");
                return 0;
            }
            const char *dev_selector = dev_sep + 1;
            ret = role_device_find_and_add_value(state->config, role_name, dev_selector, name, value);
            free(role_name);
        } else {
            ret = role_find_and_add_value(state->config, objname, name, value);
        }
        if (ret == -1) {
            return 0;
        }
    } else if (MATCH_OBJECT("user", objname)) {
        ret = user_find_and_add_value(state->config, objname, name, value);
        if (ret == -1) {
            return 0;
        }
    } else if (MATCH_OBJECT("group", objname)) {
        ret = group_find_and_add_value(state->config, objname, name, value);
        if (ret == -1) {
            return 0;
        }
    } else {
        LOG(LOG_WARNING, "Unknown section/key: [%s] %s", section, name);
        return 1;
    }

    return 1;

    #undef MATCH
    #undef MATCH_OBJECT
}

/* ========================================================================
 * Role parsing
 *
 * Roles are defined in INI sections like [role:admin]. Each key sets a
 * permission flag on the role struct. If the role already exists (from an
 * earlier key or included file), the new value is applied to the existing
 * role; otherwise a new role is created and appended to config->roles.
 * ======================================================================== */

/**
 * @brief Find or create a role by name and set a key/value on it.
 *
 * Searches config->roles for a role with the given objname. If found, applies
 * the key/value via role_add_value(). If not found, allocates a new role,
 * applies the value, and appends it to config->roles.
 *
 * @param config   The global config being built.
 * @param objname  Role name (from the section header, e.g. "admin").
 * @param name     Key name (e.g. "pcie-hotplug", "bar-access", "device").
 * @param value    Value string (e.g. "yes", "full", "any").
 * @return 0 on success, -1 on error.
 */
static int role_find_and_add_value(struct config *config, const char *objname, const char *name, const char *value)
{
    for (size_t i = 0; i < config->roles.len; ++i) {
        if (strcmp(config->roles.d[i]->name, objname) == 0) {
            return role_add_value(config->roles.d[i], name, value);
        }
    }

    _cleanup_(cleanup_rolep)
    struct role *role = calloc(1, sizeof *role);
    PROPAGATE_ERROR_NULL_STDC_LOG(role, LOG_ERR, "Could not allocate role");

    role->name = strdup(objname);
    PROPAGATE_ERROR_NULL_STDC_LOG(role->name, LOG_ERR, "Could not allocate role name");

    int ret = role_add_value(role, name, value);
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Invalid key/value for role %s: '%s' = '%s'", objname, name, value);

    ret = role_ptr_array_push_move(&config->roles, &role);
    PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Could not store role %s", objname);

    return 0;
}

/**
 * @brief Apply a single key/value pair to a role (global section keys).
 *
 * Handles keys that appear in a plain @c [role:\<name\>] section (without a
 * device selector).  Supported keys:
 *   - "query-devices":"yes" | "no"  -- controls device enumeration / info queries.
 *
 * All other permissions (bar-access, qdma, buffer, design-write, clock,
 * pcie-hotplug) must be specified in device-scoped sections
 * @c [role:\<name\>:\<bdf\>].
 *
 * @param role   The role to modify.
 * @param name   Key name.
 * @param value  Value string.
 * @return 0 on success, -1 on unknown key or invalid value.
 */
static int role_add_value(struct role *role, const char *name, const char *value)
{
    if (strcmp(name, "query-devices") == 0) {
        if (strcmp(value, "yes") == 0) {
            role->query = true;
            return 0;
        } else if (strcmp(value, "no") == 0) {
            role->query = false;
            return 0;
        } else {
            LOG(LOG_ERR, "Invalid value for role query-devices: '%s'", value);
            return -1;
        }
    } else {
        LOG(LOG_ERR, "Unknown role key: '%s'", name);
        return -1;
    }
}

/* ========================================================================
 * BDF normalization and device policy helpers
 *
 * These utilities support the per-device permission model. BDF strings
 * are normalized to the canonical "DDDD:BB:DD" board-level format.
 * Device policies are stored in a per-role array and looked up by BDF.
 * ======================================================================== */

/**
 * @brief Normalize a BDF string to canonical board-level form "DDDD:BB:DD".
 *
 * Handles:
 *   - "any" is passed through unchanged.
 *   - Short-form "BB:DD" is expanded to "0000:BB:DD".
 *   - Full-form "DDDD:BB:DD" is copied as-is.
 *   - A trailing ".F" function suffix is stripped (board-level only).
 *
 * @param input   The raw BDF string from the config file.
 * @param out     Output buffer for the normalized BDF.
 * @param out_len Size of the output buffer (must be >= 13 for "DDDD:BB:DD\0").
 * @return 0 on success, -1 on invalid format or buffer too small.
 */
static int normalize_bdf(const char *input, char *out, size_t out_len)
{
    assert(input != NULL);
    assert(out != NULL);

    if (strcmp(input, "any") == 0) {
        if (out_len < 4) {
            return -1;
        }
        strcpy(out, "any");
        return 0;
    }

    /* Work on a mutable copy so we can strip function suffix. */
    char buf[64];
    size_t len = strlen(input);
    if (len >= sizeof(buf)) {
        LOG(LOG_ERR, "BDF string too long: '%s'", input);
        return -1;
    }
    memcpy(buf, input, len + 1);

    /* Strip ".F" function suffix if present. */
    char *dot = strrchr(buf, '.');
    if (dot != NULL) {
        *dot = '\0';
    }

    /* Count colons to determine format. */
    int colons = 0;
    for (const char *p = buf; *p; p++) {
        if (*p == ':') {
            colons++;
        }
    }

    if (colons == 1) {
        /* Short-form "BB:DD" -> "0000:BB:DD" */
        int ret = snprintf(out, out_len, "0000:%s", buf);
        if (ret < 0 || (size_t)ret >= out_len) {
            LOG(LOG_ERR, "BDF buffer too small for '%s'", input);
            return -1;
        }
    } else if (colons == 2) {
        /* Full-form "DDDD:BB:DD" */
        if (strlen(buf) >= out_len) {
            LOG(LOG_ERR, "BDF buffer too small for '%s'", input);
            return -1;
        }
        strcpy(out, buf);
    } else {
        LOG(LOG_ERR, "Invalid BDF format: '%s'", input);
        return -1;
    }

    return 0;
}

/**
 * @brief Find an existing device_policy by BDF in the policies array.
 *
 * @param policies  The array to search.
 * @param bdf       The normalized BDF string to match.
 * @return Pointer to the matching device_policy, or NULL if not found.
 */
static struct device_policy *find_device_policy(
    const struct device_policy_ptr_array *policies,
    const char *bdf
)
{
    for (size_t i = 0; i < policies->len; i++) {
        if (strcmp(policies->d[i]->bdf, bdf) == 0) {
            return policies->d[i];
        }
    }
    return NULL;
}

/**
 * @brief Find or create a device_policy entry for the given BDF.
 *
 * Searches the policies array for an existing entry with the given BDF.
 * If not found, allocates a new device_policy with all flags false and
 * appends it to the array.
 *
 * @param      policies  The array to search/modify.
 * @param      bdf       The normalized BDF string.
 * @param[out] out       Receives the found or newly created device_policy.
 * @return 0 on success, -1 on allocation error.
 */
static int find_or_create_device_policy(
    struct device_policy_ptr_array *policies,
    const char *bdf,
    struct device_policy **out
)
{
    struct device_policy *dp = find_device_policy(policies, bdf);
    if (dp != NULL) {
        *out = dp;
        return 0;
    }

    /* Allocate a new device_policy with all flags false. */
    dp = calloc(1, sizeof(*dp));
    PROPAGATE_ERROR_NULL_STDC_LOG(dp, LOG_ERR, "Could not allocate device_policy");

    dp->bdf = strdup(bdf);
    if (dp->bdf == NULL) {
        free(dp);
        LOG(LOG_ERR, "Could not allocate BDF string: %s", strerror(errno));
        return -1;
    }

    int ret = device_policy_ptr_array_push(policies, dp);
    if (ret != 0) {
        free(dp->bdf);
        free(dp);
        LOG(LOG_ERR, "Could not store device_policy: %s", strerror(errno));
        return -1;
    }

    *out = dp;
    return 0;
}

/**
 * @brief Find or create a role by name in the config.
 *
 * Searches config->roles for an existing role with the given name.
 * If not found, allocates a new role and appends it to the array.
 *
 * @param config     The global config being built.
 * @param role_name  Name of the role to find or create.
 * @return Pointer to the role, or NULL on allocation error.
 */
static struct role *role_find_or_create(struct config *config, const char *role_name)
{
    for (size_t i = 0; i < config->roles.len; ++i) {
        if (strcmp(config->roles.d[i]->name, role_name) == 0) {
            return config->roles.d[i];
        }
    }

    struct role *role = calloc(1, sizeof(*role));
    if (role == NULL) {
        LOG(LOG_ERR, "Could not allocate role: %s", strerror(errno));
        return NULL;
    }

    role->name = strdup(role_name);
    if (role->name == NULL) {
        free(role);
        LOG(LOG_ERR, "Could not allocate role name: %s", strerror(errno));
        return NULL;
    }

    int ret = role_ptr_array_push(&config->roles, role);
    if (ret != 0) {
        free(role->name);
        free(role);
        LOG(LOG_ERR, "Could not store role %s: %s", role_name, strerror(errno));
        return NULL;
    }

    return role;
}

/**
 * @brief Handle a key/value from a device-scoped role section [role:name:bdf].
 *
 * Finds or creates the role, normalizes the device selector BDF, finds or
 * creates the device_policy entry, and sets the appropriate flag.
 *
 * @param config        The global config being built.
 * @param role_name     The role name (portion before the device selector).
 * @param dev_selector  The device selector ("any", "0000:03:00", "03:00", etc.).
 * @param name          Key name (e.g. "bar-access", "qdma", "buffer").
 * @param value         Value string (e.g. "full", "yes", "no").
 * @return 0 on success, -1 on error.
 */
static int role_device_find_and_add_value(
    struct config *config,
    const char *role_name,
    const char *dev_selector,
    const char *name,
    const char *value
)
{
    struct role *role = role_find_or_create(config, role_name);
    PROPAGATE_ERROR_NULL_LOG(role, LOG_ERR, "Could not find or create role %s", role_name);

    /* Normalize the device selector to canonical BDF form. */
    char normalized_bdf[32];
    int ret = normalize_bdf(dev_selector, normalized_bdf, sizeof(normalized_bdf));
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Invalid device selector '%s' for role %s", dev_selector, role_name);

    struct device_policy *dp = NULL;
    ret = find_or_create_device_policy(&role->device_policies, normalized_bdf, &dp);
    PROPAGATE_ERROR(ret);

    ret = device_policy_add_value(dp, name, value);
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Invalid key/value for role %s device %s: '%s' = '%s'",
                        role_name, normalized_bdf, name, value);

    return 0;
}

/**
 * @brief Apply a single key/value pair to a device_policy.
 *
 * Supported keys:
 *   - "bar-access":    "full"       -- grants BAR mmap access.
 *   - "qdma":          "yes" | "no" -- controls QDMA queue pair operations.
 *   - "buffer":        "yes" | "no" -- controls DMA buffer operations.
 *   - "design-write":  "yes" | "no" -- controls FPGA bitstream programming.
 *   - "clock":         "yes" | "no" -- controls clock get/set operations.
 *   - "pcie-hotplug":  "yes" | "no" -- controls per-device hotplug operations.
 *   - "raw-mem-access": "yes" | "no" -- controls raw DMA buffer open (bypasses allocator).
 *
 * @param dp     The device_policy to modify.
 * @param name   Key name.
 * @param value  Value string.
 * @return 0 on success, -1 on unknown key or invalid value.
 */
static int device_policy_add_value(struct device_policy *dp, const char *name, const char *value)
{
    if (strcmp(name, "bar-access") == 0) {
        if (strcmp(value, "full") == 0) {
            dp->bar = true;
            return 0;
        } else {
            LOG(LOG_ERR, "Invalid value for device bar-access: '%s'", value);
            return -1;
        }
    } else if (strcmp(name, "qdma") == 0) {
        if (strcmp(value, "yes") == 0) {
            dp->qdma = true;
            return 0;
        } else if (strcmp(value, "no") == 0) {
            dp->qdma = false;
            return 0;
        } else {
            LOG(LOG_ERR, "Invalid value for device qdma: '%s'", value);
            return -1;
        }
    } else if (strcmp(name, "buffer") == 0) {
        if (strcmp(value, "yes") == 0) {
            dp->buffer = true;
            return 0;
        } else if (strcmp(value, "no") == 0) {
            dp->buffer = false;
            return 0;
        } else {
            LOG(LOG_ERR, "Invalid value for device buffer: '%s'", value);
            return -1;
        }
    } else if (strcmp(name, "design-write") == 0) {
        if (strcmp(value, "yes") == 0) {
            dp->design_write = true;
            return 0;
        } else if (strcmp(value, "no") == 0) {
            dp->design_write = false;
            return 0;
        } else {
            LOG(LOG_ERR, "Invalid value for device design-write: '%s'", value);
            return -1;
        }
    } else if (strcmp(name, "clock") == 0) {
        if (strcmp(value, "yes") == 0) {
            dp->clock = true;
            return 0;
        } else if (strcmp(value, "no") == 0) {
            dp->clock = false;
            return 0;
        } else {
            LOG(LOG_ERR, "Invalid value for device clock: '%s'", value);
            return -1;
        }
    } else if (strcmp(name, "pcie-hotplug") == 0) {
        if (strcmp(value, "yes") == 0) {
            dp->pcie_hotplug = true;
            return 0;
        } else if (strcmp(value, "no") == 0) {
            dp->pcie_hotplug = false;
            return 0;
        } else {
            LOG(LOG_ERR, "Invalid value for device pcie-hotplug: '%s'", value);
            return -1;
        }
    } else if (strcmp(name, "raw-mem-access") == 0) {
        if (strcmp(value, "yes") == 0) {
            dp->raw_mem_access = true;
            return 0;
        } else if (strcmp(value, "no") == 0) {
            dp->raw_mem_access = false;
            return 0;
        } else {
            LOG(LOG_ERR, "Invalid value for device raw-mem-access: '%s'", value);
            return -1;
        }
    } else {
        LOG(LOG_ERR, "Unknown device policy key: '%s'", name);
        return -1;
    }
}

/* ========================================================================
 * User parsing
 *
 * Users are defined in INI sections like [user:jdoe]. The special name "*"
 * refers to the default (wildcard) user whose roles apply to every client.
 * Each user section maps to one or more `role = <rolename>` entries.
 * The UID is resolved from the system passwd database at parse time.
 * ======================================================================== */

/**
 * @brief Find or create a user config by name and set a key/value on it.
 *
 * The wildcard user "*" is stored in config->default_user rather than the
 * users array. For named users, this function searches the existing users
 * array; if not found, it creates a new user_config, resolves the UID via
 * getpwnam_r, and appends it to config->users.
 *
 * @param config   The global config being built.
 * @param objname  User name (from section header) or "*" for default.
 * @param name     Key name (currently only "role" is supported).
 * @param value    Value string (e.g. the role name to assign).
 * @return 0 on success, -1 on error.
 */
static int user_find_and_add_value(struct config *config, const char *objname, const char *name, const char *value)
{
    /* The wildcard user "*" is stored separately as default_user. */
    if (strcmp(objname, "*") == 0) {
        return user_add_value(config->default_user, name, value);
    }

    for (size_t i = 0; i < config->users.len; ++i) {
        if (strcmp(config->users.d[i]->name, objname) == 0) {
            return user_add_value(config->users.d[i], name, value);
        }
    }

    _cleanup_(cleanup_user_configp)
    struct user_config *user = calloc(1, sizeof *user);
    PROPAGATE_ERROR_NULL_STDC_LOG(user, LOG_ERR, "Could not allocate user");

    user->name = strdup(objname);
    PROPAGATE_ERROR_NULL_STDC_LOG(user->name, LOG_ERR, "Could not allocate user name");

    /* Resolve the system UID for this username at config-load time. */
    int ret = set_user_uid(&user->uid, objname);
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Could not find uid for user %s", objname);

    ret = user_add_value(user, name, value);
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Invalid key/value for user %s: '%s' = '%s'", objname, name, value);

    ret = user_config_ptr_array_push_move(&config->users, &user);
    PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Could not store user %s", objname);

    return 0;
}

/**
 * @brief Apply a single key/value pair to a user config.
 *
 * Currently the only supported key is "role", which appends a role name
 * string to the user's role_names list. Multiple `role = <name>` lines
 * allow a user to hold several roles (whose permissions are merged at
 * authorization time).
 *
 * @param user   The user config to modify.
 * @param name   Key name (must be "role").
 * @param value  The role name to add.
 * @return 0 on success, -1 on unknown key or allocation failure.
 */
static int user_add_value(struct user_config *user, const char *name, const char *value)
{
    if (strcmp(name, "role") == 0) {
        _cleanup_(cleanup_free)
        char *role = strdup(value);
        PROPAGATE_ERROR_NULL_STDC_LOG(role, LOG_ERR, "Could not allocate role name");

        int ret = str_array_push_move(&user->role_names, &role);
        PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Could not store role name for user");

        return 0;
    } else {
        return -1;
    }
}

/* ========================================================================
 * Group parsing
 *
 * Groups are defined in INI sections like [group:fpga-users]. Each group
 * section maps to one or more `role = <rolename>` entries. The GID is
 * resolved from the system group database at parse time. At authorization
 * time, if any of the client's supplementary GIDs matches a group's GID,
 * that group's roles are merged into the client's effective permissions.
 * ======================================================================== */

/**
 * @brief Find or create a group config by name and set a key/value on it.
 *
 * Searches config->groups for an existing group with the given name. If not
 * found, allocates a new group_config, resolves the GID via getgrnam_r, and
 * appends it to config->groups.
 *
 * @param config   The global config being built.
 * @param objname  Group name (from section header, e.g. "fpga-users").
 * @param name     Key name (currently only "role" is supported).
 * @param value    Value string (e.g. the role name to assign).
 * @return 0 on success, -1 on error.
 */
static int group_find_and_add_value(struct config *config, const char *objname, const char *name, const char *value)
{
    for (size_t i = 0; i < config->groups.len; ++i) {
        if (strcmp(config->groups.d[i]->name, objname) == 0) {
            return group_add_value(config->groups.d[i], name, value);
        }
    }

    _cleanup_(cleanup_group_configp)
    struct group_config *group = calloc(1, sizeof *group);
    PROPAGATE_ERROR_NULL_STDC_LOG(group, LOG_ERR, "Could not allocate group");

    group->name = strdup(objname);
    PROPAGATE_ERROR_NULL_STDC_LOG(group->name, LOG_ERR, "Could not allocate group name");

    /* Resolve the system GID for this group name at config-load time. */
    int ret = set_group_gid(&group->gid, objname);
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Could not find gid for group %s", objname);

    ret = group_add_value(group, name, value);
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Invalid key/value for group %s: '%s' = '%s'", objname, name, value);

    ret = group_config_ptr_array_push_move(&config->groups, &group);
    PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Could not store group %s", objname);

    return 0;
}

/**
 * @brief Apply a single key/value pair to a group config.
 *
 * Currently the only supported key is "role", which appends a role name
 * to the group's role_names list.
 *
 * @param group  The group config to modify.
 * @param name   Key name (must be "role").
 * @param value  The role name to add.
 * @return 0 on success, -1 on unknown key or allocation failure.
 */
static int group_add_value(struct group_config *group, const char *name, const char *value)
{
    if (strcmp(name, "role") == 0) {
        _cleanup_(cleanup_free)
        char *role = strdup(value);
        PROPAGATE_ERROR_NULL_STDC_LOG(role, LOG_ERR, "Could not allocate role name");

        int ret = str_array_push_move(&group->role_names, &role);
        PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Could not store role name for group");

        return 0;
    } else {
        return -1;
    }
}

/* ========================================================================
 * UID / GID resolution
 *
 * These helpers look up a system user or group by name (via the reentrant
 * POSIX getpwnam_r / getgrnam_r) and extract the numeric UID or GID. They
 * handle ERANGE by doubling the buffer and retrying, and EINTR by retrying
 * in place.
 * ======================================================================== */

/**
 * @brief Resolve a username to its numeric UID via getpwnam_r.
 *
 * @param[out] uid   Receives the resolved UID.
 * @param      name  The username to look up.
 * @return 0 on success, -1 if the user is not found or on system error.
 */
static int set_user_uid(uid_t *uid, const char *name)
{
    size_t bufsz = BUFSIZ;
    int ret;
    struct passwd pwd;
    struct passwd *result;

    do {
        char *buf = malloc(bufsz);
        PROPAGATE_ERROR_NULL_STDC_LOG(buf, LOG_ERR, "Failed malloc in get_user_uid");

retry:
        ret = getpwnam_r(name, &pwd, buf, bufsz, &result);
        if (ret == EINTR) {
            goto retry;
        }

        free(buf);

        bufsz *= 2;
    } while (ret == ERANGE);

    PROPAGATE_ERROR_NULL_STDC_LOG(result, LOG_ERR, "User %s not found", name);

    *uid = result->pw_uid;
    return 0;
}

/**
 * @brief Resolve a group name to its numeric GID via getgrnam_r.
 *
 * @param[out] gid   Receives the resolved GID.
 * @param      name  The group name to look up.
 * @return 0 on success, -1 if the group is not found or on system error.
 */
static int set_group_gid(gid_t *gid, const char *name)
{
    size_t bufsz = BUFSIZ;
    int ret;
    struct group pwd;
    struct group *result;

    do {
        char *buf = malloc(bufsz);
        PROPAGATE_ERROR_NULL_STDC_LOG(buf, LOG_ERR, "Failed malloc in set_group_gid");

retry:
        ret = getgrnam_r(name, &pwd, buf, bufsz, &result);
        if (ret == EINTR) {
            goto retry;
        }

        free(buf);

        bufsz *= 2;
    } while (ret == ERANGE);

    PROPAGATE_ERROR_NULL_STDC_LOG(result, LOG_ERR, "Group %s not found", name);

    *gid = result->gr_gid;
    return 0;
}

/* ========================================================================
 * Role assignment (post-parse phase)
 *
 * During INI parsing, user and group configs accumulate role names as
 * strings (role_names array). After all files are parsed, these functions
 * resolve each name to a pointer into config->roles, populating the `roles`
 * reference array. This two-phase approach allows roles to be defined in
 * any order or across multiple included files.
 * ======================================================================== */

/**
 * @brief Resolve role name references for all users (including the default user).
 *
 * @param config  The fully parsed config with populated role_names arrays.
 * @return 0 on success, -1 on error.
 */
static int assign_users_roles(struct config *config)
{
    /* Resolve the default (wildcard) user first. */
    int ret = assign_user_roles(config, config->default_user);
    PROPAGATE_ERROR(ret);

    for (size_t i = 0; i < config->users.len; i++) {
        ret = assign_user_roles(config, config->users.d[i]);
        PROPAGATE_ERROR(ret);
    }

    return 0;
}

/**
 * @brief Resolve role name references for a single user config.
 *
 * For each role name string in user->role_names, searches config->roles for
 * a matching role and pushes a reference pointer into user->roles. If a role
 * name is not found, a warning is logged but processing continues (the user
 * simply will not receive that role's permissions).
 *
 * @param config  The global config (provides the roles array to search).
 * @param user    The user config whose role names are being resolved.
 * @return 0 on success, -1 on allocation error.
 */
static int assign_user_roles(struct config *config, struct user_config *user)
{
    for (size_t j = 0; j < user->role_names.len; j++) {
        bool found_role_name = false;

        for (size_t k = 0; k < config->roles.len; k++) {
            if (strcmp(user->role_names.d[j], config->roles.d[k]->name) == 0) {
                int ret = role_ref_array_push(&user->roles, config->roles.d[k]);
                PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed allocation in assign_user_roles");

                found_role_name = true;
                break;
            }
        }

        if (!found_role_name) {
            LOG(LOG_WARNING, "Failed to find user role %s for user %s", user->role_names.d[j], user->name);
        }
    }

    return 0;
}

/**
 * @brief Resolve role name references for all groups.
 *
 * @param config  The fully parsed config with populated role_names arrays.
 * @return 0 on success, -1 on error.
 */
static int assign_groups_roles(struct config *config)
{
    for (size_t i = 0; i < config->groups.len; i++) {
        int ret = assign_group_roles(config, config->groups.d[i]);
        PROPAGATE_ERROR(ret);
    }

    return 0;
}

/**
 * @brief Resolve role name references for a single group config.
 *
 * Mirrors assign_user_roles() but operates on a group_config. Unresolved
 * role names produce a warning but do not cause a hard failure.
 *
 * @param config  The global config (provides the roles array to search).
 * @param group   The group config whose role names are being resolved.
 * @return 0 on success, -1 on allocation error.
 */
static int assign_group_roles(struct config *config, struct group_config *group)
{
    for (size_t j = 0; j < group->role_names.len; j++) {
        bool found_role_name = false;

        for (size_t k = 0; k < config->roles.len; k++) {
            if (strcmp(group->role_names.d[j], config->roles.d[k]->name) == 0) {
                int ret = role_ref_array_push(&group->roles, config->roles.d[k]);
                PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed allocation in assign_group_roles");

                found_role_name = true;
                break;
            }
        }

        if (!found_role_name) {
            LOG(LOG_WARNING, "Failed to find group role %s for group %s", group->role_names.d[j], group->name);
        }
    }

    return 0;
}

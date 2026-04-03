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
 * @file config.h
 * @brief Configuration data model for the vrtd daemon.
 *
 * The daemon reads a configuration file that defines:
 *   - Named @em roles, each granting a specific set of permissions (which
 *     devices may be accessed, which BARs, whether queries or PCIe hotplug
 *     operations are allowed).
 *   - @em User entries that map a system UID to one or more roles.
 *   - @em Group entries that map a system GID to one or more roles.
 *   - A @em default user entry applied when no explicit UID/GID match is found.
 *
 * Role names inside user/group entries are resolved lazily: the config parser
 * stores the names first, then links them to the actual @c struct @c role
 * objects in a second pass via @c role_merge_add_array.
 */

#ifndef VRTD_CONFIG_H
#define VRTD_CONFIG_H

#include <stdbool.h>
#include <sys/types.h>

#include "array.h"

/**
 * @brief Per-device permission flags.
 *
 * Each device_policy entry maps a device (identified by board-level BDF
 * string, e.g. "0000:03:00") to a set of subsystem-level permissions.
 * The special BDF string "any" acts as a wildcard matching all devices.
 *
 * During authorization, the auth layer looks up the target device's BDF
 * in the role's device_policies array: first an exact match, then a
 * fallback to the "any" entry if present.
 */
struct device_policy {
    /** @brief Normalized BDF string ("DDDD:BB:DD") or "any" (heap-allocated, owning). */
    char *bdf; /* owning */
    /** @brief If true, the client may mmap BAR regions on this device. */
    bool bar;
    /** @brief If true, the client may create/operate QDMA queue pairs on this device. */
    bool qdma;
    /** @brief If true, the client may allocate/release DMA buffers on this device. */
    bool buffer;
    /** @brief If true, the client may program FPGA bitstreams on this device. */
    bool design_write;
    /** @brief If true, the client may get/set clock frequencies on this device. */
    bool clock;
    /** @brief If true, the client may perform PCIe hotplug operations on this device. */
    bool pcie_hotplug;
    /** @brief If true, the client may open raw DMA buffers at caller-specified device addresses (bypasses allocator). */
    bool raw_mem_access;
};

/**
 * @brief Release all resources owned by a device_policy (bdf string).
 * @param dp Pointer to the device_policy to clean up.
 */
void cleanup_device_policy(struct device_policy *dp);

/** @brief Owning array of device_policy pointers. */
DECLARE_OWNING_PTR_ARRAY(device_policy_ptr_array, struct device_policy *, cleanup_device_policy)

/**
 * @brief A named permission set that governs what a client may do.
 *
 * Roles are the central unit of the vrtd access-control model.  Each
 * connecting client is assigned a merged role derived from its UID and GID
 * credentials.  The role determines:
 *   - Which devices the client may access and with which subsystem
 *     permissions, via the @c device_policies array.
 *   - Whether the client may issue informational queries (@c query).
 *
 * Per-device permissions (bar-access, qdma, buffer, design-write, clock,
 * pcie-hotplug) are specified in the config file using sub-sections of the
 * form @c [role:\<name\>:\<bdf\>], where @c \<bdf\> is a board-level PCI
 * address or "any".
 */
struct role {
    /** @brief Human-readable name of this role (heap-allocated, owning). */
    char *name; /* owning */

    /** @brief Per-device permission policies (owning array). */
    struct device_policy_ptr_array device_policies;

    /** @brief If true, the role permits device enumeration and info queries. */
    bool query;
};

/**
 * @brief Release all resources owned by a role (name string, device_policies array).
 * @param role Pointer to the role to clean up.
 */
void cleanup_role(struct role *role);

/**
 * @brief Cleanup helper for use with __attribute__((cleanup)).
 * @param rolep Address of a @c struct @c role pointer.
 */
static inline
void cleanup_rolep(struct role **rolep)
{
    if (rolep == NULL) {
        return;
    }

    cleanup_role(*rolep);

    *rolep = NULL;
}

/** @brief Non-owning array of role pointers (for referencing roles without ownership). */
DECLARE_ARRAY(role_ref_array, struct role *)
/** @brief Owning array of role pointers (frees roles on cleanup). */
DECLARE_OWNING_PTR_ARRAY(role_ptr_array, struct role *, cleanup_role)

/**
 * @brief Maps a system user (by UID) to a set of roles.
 *
 * During configuration loading, role names are stored in @c role_names for
 * lazy resolution.  After all roles are parsed, @c roles is populated with
 * direct pointers.
 */
struct user_config {
    /** @brief Username string (heap-allocated, owning). */
    char *name; /* owning */
    /** @brief Numeric user ID resolved from @c name via getpwnam(). */
    uid_t uid;

    /** @brief Role names from the config file, used for lazy resolution. */
    struct str_array role_names; /* Used for lazy loading roles */
    /** @brief Resolved role pointers (non-owning references into config.roles). */
    struct role_ref_array roles;
};

/**
 * @brief Release all resources owned by a user_config.
 * @param user Pointer to the user_config to clean up.
 */
void cleanup_user_config(struct user_config *user);

/**
 * @brief Cleanup helper for use with __attribute__((cleanup)).
 * @param userp Address of a @c struct @c user_config pointer.
 */
static inline
void cleanup_user_configp(struct user_config **userp)
{
    if (userp == NULL) {
        return;
    }

    cleanup_user_config(*userp);

    *userp = NULL;
}

DECLARE_OWNING_PTR_ARRAY(user_config_ptr_array, struct user_config *, cleanup_user_config)

/**
 * @brief Maps a system group (by GID) to a set of roles.
 *
 * Analogous to @c struct @c user_config, but keyed on GID.  Role names are
 * lazily resolved the same way.
 */
struct group_config {
    /** @brief Group name string (heap-allocated, owning). */
    char *name; /* owning */
    /** @brief Numeric group ID resolved from @c name via getgrnam(). */
    gid_t gid;

    /** @brief Role names from the config file, used for lazy resolution. */
    struct str_array role_names; /* Used for lazy loading roles */
    /** @brief Resolved role pointers (non-owning references into config.roles). */
    struct role_ref_array roles;
};

/**
 * @brief Release all resources owned by a group_config.
 * @param group Pointer to the group_config to clean up.
 */
void cleanup_group_config(struct group_config *group);

/**
 * @brief Cleanup helper for use with __attribute__((cleanup)).
 * @param groupp Address of a @c struct @c group_config pointer.
 */
static inline
void cleanup_group_configp(struct group_config **groupp)
{
    if (groupp == NULL) {
        return;
    }

    cleanup_group_config(*groupp);

    *groupp = NULL;
}

DECLARE_OWNING_PTR_ARRAY(group_config_ptr_array, struct group_config *, cleanup_group_config)

/**
 * @brief Top-level daemon configuration container.
 *
 * Owns all roles, user mappings, and group mappings.  The @c default_user
 * entry (if present) is applied to any connecting client whose UID/GIDs do
 * not match an explicit user or group entry.
 */
struct config {
    /** @brief All defined roles (owning array). */
    struct role_ptr_array roles;

    /** @brief Fallback user entry applied when no UID/GID match is found (non-owning,
     *         points into @c users or is a standalone allocation). */
    struct user_config *default_user;

    /** @brief Per-UID user configuration entries (owning array). */
    struct user_config_ptr_array users;
    /** @brief Per-GID group configuration entries (owning array). */
    struct group_config_ptr_array groups;

    /** @brief If true, use mock devices instead of real hardware (for testing). */
    bool mock_device;
};

/**
 * @brief Release all resources owned by the config (roles, users, groups).
 * @param config Pointer to the config to clean up.
 */
void cleanup_config(struct config *config);

/**
 * @brief Cleanup helper for use with __attribute__((cleanup)).
 * @param configp Address of a @c struct @c config pointer.
 */
static inline
void cleanup_configp(struct config **configp)
{
    if (configp == NULL) {
        return;
    }

    cleanup_config(*configp);

    *configp = NULL;
}

/**
 * @brief Load the daemon configuration from the default config file.
 *
 * Parses the configuration file, resolves UIDs/GIDs, and lazily links role
 * name references to actual @c struct @c role objects.
 *
 * @param[out] config On success, receives a heap-allocated config. Caller
 *                    must free with cleanup_config().
 * @return 0 on success, -1 on error (logged via sd_journal).
 */
int config_load(struct config **config);

/**
 * @brief Allocate a new empty role and assign it a name.
 *
 * Used during configuration loading and role merging to create a fresh role
 * that can then be populated via @c role_merge_add_role.
 *
 * @param[out] rolep  Receives the newly allocated role on success.
 * @param      name   Name to assign to the role (copied).
 * @return 0 on success, -1 on allocation failure.
 */
int role_merge_new(struct role **rolep, const char *name);

/**
 * @brief Merge permissions from one role into another (union of permissions).
 *
 * Adds @p src's device_policies and boolean permissions into @p dst.
 * This implements the "most permissive wins" merging semantic: boolean
 * flags are ORed, and per-device policy entries are merged by BDF with
 * each subsystem flag ORed independently.
 *
 * @param dst Destination role to merge into (modified in place).
 * @param src Source role to merge from (not modified).
 * @return 0 on success, -1 on error.
 */
int role_merge_add_role(struct role *dst, const struct role *src);

/**
 * @brief Merge an array of roles into a single destination role.
 *
 * Iterates over @p roles and calls @c role_merge_add_role for each entry.
 *
 * @param dst   Destination role to merge into.
 * @param roles Array of role pointers to merge from.
 * @return 0 on success, -1 on error.
 */
int role_merge_add_array(struct role *dst, const struct role_ref_array *roles);

#endif // VRTD_CONFIG_H

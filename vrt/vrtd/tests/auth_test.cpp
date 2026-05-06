/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
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

#include <gtest/gtest.h>

#include <cstring>

extern "C" {
#include "auth.h"
#include "config.h"
#include "device.h"
#include "state.h"
}

static struct device_policy *make_dp(const char *bdf, bool bar, bool qdma, bool buffer,
                                     bool design_write, bool clock, bool pcie_hotplug,
                                     bool raw_mem) {
    auto *dp = static_cast<struct device_policy *>(calloc(1, sizeof(struct device_policy)));
    dp->bdf = strdup(bdf);
    dp->bar = bar;
    dp->qdma = qdma;
    dp->buffer = buffer;
    dp->design_write = design_write;
    dp->clock = clock;
    dp->pcie_hotplug = pcie_hotplug;
    dp->raw_mem_access = raw_mem;
    return dp;
}

class AuthTest : public ::testing::Test {
   protected:
    struct config cfg{};
    struct device dev{};
    struct vrtd state{};
    struct client cl{};
    struct user_config default_user{};
    struct role *fullaccess_role = nullptr;
    struct role *info_role = nullptr;

    void SetUp() override {
        memset(&cfg, 0, sizeof(cfg));
        memset(&dev, 0, sizeof(dev));
        memset(&state, 0, sizeof(state));
        memset(&cl, 0, sizeof(cl));
        memset(&default_user, 0, sizeof(default_user));

        strncpy(dev.pci_info.bdf, "0000:03:00", sizeof(dev.pci_info.bdf) - 1);

        ASSERT_EQ(role_merge_new(&fullaccess_role, "fullaccess"), 0);
        fullaccess_role->query = true;
        struct device_policy *dp = make_dp("any", true, true, true, true, true, true, true);
        struct device_policy *dp_ptr = dp;
        ASSERT_EQ(device_policy_ptr_array_push_move(&fullaccess_role->device_policies, &dp_ptr), 0);

        ASSERT_EQ(role_merge_new(&info_role, "info"), 0);
        info_role->query = true;

        struct role *fa_ref = fullaccess_role;
        ASSERT_EQ(role_ptr_array_push_move(&cfg.roles, &fa_ref), 0);
        struct role *info_ref = info_role;
        ASSERT_EQ(role_ptr_array_push_move(&cfg.roles, &info_ref), 0);

        default_user.name = strdup("*");
        cfg.default_user = &default_user;

        struct device *dev_ptr = &dev;
        ASSERT_EQ(device_ptr_array_push(&state.devices, dev_ptr), 0);

        state.config = &cfg;

        cl.uid = getuid();
        cl.state = &state;
        cl.role = nullptr;
        cl.fd = -1;
    }

    void TearDown() override {
        if (cl.role != nullptr) {
            cleanup_role(cl.role);
            cl.role = nullptr;
        }
        gid_t_array_free(&cl.gids);

        // dev is stack-allocated, so we must not call the owning
        // device_ptr_array_free (which would free(dev)).
        free(state.devices.d);
        state.devices.d = nullptr;
        state.devices.len = 0;
        state.devices.cap = 0;

        role_ptr_array_free(&cfg.roles);

        str_array_free(&default_user.role_names);
        role_ref_array_free(&default_user.roles);
        free(default_user.name);

        user_config_ptr_array_free(&cfg.users);
        group_config_ptr_array_free(&cfg.groups);
    }

    void assignRole(struct role *role_template) {
        struct role *merged = nullptr;
        ASSERT_EQ(role_merge_new(&merged, "merged"), 0);
        ASSERT_EQ(role_merge_add_role(merged, role_template), 0);
        cl.role = merged;
    }
};

// --- Query-only operations ---

TEST_F(AuthTest, GetNumDevicesAllowedWithQuery) {
    assignRole(info_role);
    struct vrtd_req_get_num_devices req{};
    EXPECT_EQ(auth_request_get_num_devices(&cl, &req), 1);
}

TEST_F(AuthTest, GetNumDevicesDeniedWithoutQuery) {
    struct role *empty = nullptr;
    ASSERT_EQ(role_merge_new(&empty, "empty"), 0);
    cl.role = empty;
    struct vrtd_req_get_num_devices req{};
    EXPECT_EQ(auth_request_get_num_devices(&cl, &req), 0);
}

TEST_F(AuthTest, GetDeviceInfoAllowed) {
    assignRole(info_role);
    struct vrtd_req_get_device_info req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_get_device_info(&cl, &req), 1);
}

TEST_F(AuthTest, GetDeviceByBdfAllowed) {
    assignRole(info_role);
    struct vrtd_req_get_device_by_bdf req{};
    EXPECT_EQ(auth_request_get_device_by_bdf(&cl, &req), 1);
}

TEST_F(AuthTest, GetBarInfoAllowed) {
    assignRole(info_role);
    struct vrtd_req_get_bar_info req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_get_bar_info(&cl, &req), 1);
}

TEST_F(AuthTest, QdmaGetInfoAllowed) {
    assignRole(info_role);
    struct vrtd_req_qdma_get_info req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_qdma_get_info(&cl, &req), 1);
}

TEST_F(AuthTest, GetSensorInfoAllowed) {
    assignRole(info_role);
    struct vrtd_req_get_sensor_info req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_get_sensor_info(&cl, &req), 1);
}

// --- Device-access operations: fullaccess role ---

TEST_F(AuthTest, GetBarFdAllowedFullaccess) {
    assignRole(fullaccess_role);
    struct vrtd_req_get_bar_fd req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_get_bar_fd(&cl, &req), 1);
}

TEST_F(AuthTest, QdmaQpairAddAllowed) {
    assignRole(fullaccess_role);
    struct vrtd_req_qdma_qpair_add req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_qdma_qpair_add(&cl, &req), 1);
}

TEST_F(AuthTest, BufferOpenAllowed) {
    assignRole(fullaccess_role);
    struct vrtd_req_buffer_open req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_buffer_open(&cl, &req), 1);
}

TEST_F(AuthTest, BufferCloseAllowed) {
    assignRole(fullaccess_role);
    struct vrtd_req_buffer_close req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_buffer_close(&cl, &req), 1);
}

TEST_F(AuthTest, DesignWriteAllowed) {
    assignRole(fullaccess_role);
    struct vrtd_req_design_write req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_design_write(&cl, &req), 1);
}

TEST_F(AuthTest, ClockOpAllowed) {
    assignRole(fullaccess_role);
    struct vrtd_req_clock_op req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_clock_op(&cl, &req), 1);
}

TEST_F(AuthTest, HotplugOpAllowed) {
    assignRole(fullaccess_role);
    struct vrtd_req_device_hotplug_op req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_device_hotplug_op(&cl, &req), 1);
}

TEST_F(AuthTest, BufferOpenRawAllowed) {
    assignRole(fullaccess_role);
    struct vrtd_req_buffer_open_raw req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_buffer_open_raw(&cl, &req), 1);
}

// --- Device-access denied with info-only role ---

TEST_F(AuthTest, GetBarFdDeniedInfoOnly) {
    assignRole(info_role);
    struct vrtd_req_get_bar_fd req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_get_bar_fd(&cl, &req), 0);
}

TEST_F(AuthTest, BufferOpenDeniedInfoOnly) {
    assignRole(info_role);
    struct vrtd_req_buffer_open req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_buffer_open(&cl, &req), 0);
}

TEST_F(AuthTest, DesignWriteDeniedInfoOnly) {
    assignRole(info_role);
    struct vrtd_req_design_write req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_design_write(&cl, &req), 0);
}

TEST_F(AuthTest, ClockOpDeniedInfoOnly) {
    assignRole(info_role);
    struct vrtd_req_clock_op req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_clock_op(&cl, &req), 0);
}

TEST_F(AuthTest, HotplugOpDeniedInfoOnly) {
    assignRole(info_role);
    struct vrtd_req_device_hotplug_op req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_device_hotplug_op(&cl, &req), 0);
}

// --- Exact BDF match vs "any" wildcard ---

TEST_F(AuthTest, ExactBdfMatchTakesPriority) {
    struct role *role = nullptr;
    ASSERT_EQ(role_merge_new(&role, "mixed"), 0);
    role->query = true;

    struct device_policy *any_dp = make_dp("any", false, false, false, false, false, false, false);
    struct device_policy *any_ptr = any_dp;
    ASSERT_EQ(device_policy_ptr_array_push_move(&role->device_policies, &any_ptr), 0);

    struct device_policy *exact_dp = make_dp("0000:03:00", true, true, true, true, true, true, true);
    struct device_policy *exact_ptr = exact_dp;
    ASSERT_EQ(device_policy_ptr_array_push_move(&role->device_policies, &exact_ptr), 0);

    cl.role = role;

    struct vrtd_req_get_bar_fd req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_get_bar_fd(&cl, &req), 1);
}

TEST_F(AuthTest, WildcardFallbackWhenNoExactMatch) {
    struct role *role = nullptr;
    ASSERT_EQ(role_merge_new(&role, "wildcard_only"), 0);
    role->query = true;

    struct device_policy *any_dp = make_dp("any", true, false, false, false, false, false, false);
    struct device_policy *any_ptr = any_dp;
    ASSERT_EQ(device_policy_ptr_array_push_move(&role->device_policies, &any_ptr), 0);

    cl.role = role;

    struct vrtd_req_get_bar_fd req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_get_bar_fd(&cl, &req), 1);
}

TEST_F(AuthTest, NoPolicyMatchDenied) {
    struct role *role = nullptr;
    ASSERT_EQ(role_merge_new(&role, "no_devices"), 0);
    role->query = true;

    struct device_policy *other_dp = make_dp("0000:99:00", true, true, true, true, true, true, true);
    struct device_policy *other_ptr = other_dp;
    ASSERT_EQ(device_policy_ptr_array_push_move(&role->device_policies, &other_ptr), 0);

    cl.role = role;

    struct vrtd_req_get_bar_fd req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_get_bar_fd(&cl, &req), 0);
}

// --- Invalid device index ---

TEST_F(AuthTest, DeviceIndexOutOfRange) {
    assignRole(fullaccess_role);
    struct vrtd_req_get_bar_fd req{};
    req.dev_number = 999;
    EXPECT_EQ(auth_request_get_bar_fd(&cl, &req), 0);
}

// --- ensure_role: lazy role merging via default_user ---

TEST_F(AuthTest, EnsureRoleMergesDefaultUser) {
    ASSERT_EQ(role_ref_array_push(&default_user.roles, fullaccess_role), 0);

    struct vrtd_req_get_num_devices req{};
    EXPECT_EQ(auth_request_get_num_devices(&cl, &req), 1);
    EXPECT_NE(cl.role, nullptr);
    EXPECT_TRUE(cl.role->query);
}

TEST_F(AuthTest, EnsureRoleMergesUidUser) {
    auto *uid_user = static_cast<struct user_config *>(calloc(1, sizeof(struct user_config)));
    uid_user->name = strdup("testuser");
    uid_user->uid = getuid();
    ASSERT_EQ(role_ref_array_push(&uid_user->roles, fullaccess_role), 0);

    struct user_config *uid_ptr = uid_user;
    ASSERT_EQ(user_config_ptr_array_push_move(&cfg.users, &uid_ptr), 0);

    struct vrtd_req_buffer_open req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_buffer_open(&cl, &req), 1);
}

TEST_F(AuthTest, EnsureRoleMergesGidGroup) {
    gid_t gid = getgid();
    ASSERT_EQ(gid_t_array_push(&cl.gids, gid), 0);

    auto *grp = static_cast<struct group_config *>(calloc(1, sizeof(struct group_config)));
    grp->name = strdup("testgroup");
    grp->gid = gid;
    ASSERT_EQ(role_ref_array_push(&grp->roles, fullaccess_role), 0);

    struct group_config *grp_ptr = grp;
    ASSERT_EQ(group_config_ptr_array_push_move(&cfg.groups, &grp_ptr), 0);

    struct vrtd_req_design_write req{};
    req.dev_number = 0;
    EXPECT_EQ(auth_request_design_write(&cl, &req), 1);
}

TEST_F(AuthTest, EnsureRoleEmptyDefaultDeniesAll) {
    struct vrtd_req_get_num_devices req{};
    EXPECT_EQ(auth_request_get_num_devices(&cl, &req), 0);
}

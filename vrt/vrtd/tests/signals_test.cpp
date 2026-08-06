/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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

#include "test_helpers.hpp"

extern "C" {
#include "signals.h"
#include "state.h"
}

#include <filesystem>

namespace {

class ReloadConfigTest : public testing::Test {
   protected:
    void SetUp() override {
        dir_ = makeTempDir("vrtd-reload");
        good_ = writeTempFile(dir_, "good.conf",
                              "[role:probe]\nquery-devices = yes\n");
        /* A section header inih cannot close, so the parse fails on the file
         * rather than on anything the daemon does with it. */
        bad_ = writeTempFile(dir_, "bad.conf", "[role:probe\n");
    }

    void TearDown() override {
        cleanup_config(state_.config);
        state_.config = nullptr;
        client_ptr_array_free(&state_.clients);
        std::filesystem::remove_all(dir_);
    }

    void loadGoodConfig() {
        ScopedEnv config("VRTD_CONFIG", good_);
        ASSERT_EQ(config_load(&state_.config), 0);
        ASSERT_NE(state_.config, nullptr);
    }

    vrtd state_{};
    std::filesystem::path dir_;
    std::string good_;
    std::string bad_;
};

TEST_F(ReloadConfigTest, KeepsTheLiveConfigWhenTheNewOneWillNotParse) {
    state_.clients = client_ptr_array_init();
    ASSERT_NO_FATAL_FAILURE(loadGoodConfig());
    const config* live = state_.config;
    const size_t roles = state_.config->roles.len;
    ASSERT_GT(roles, 0u);

    ScopedEnv broken("VRTD_CONFIG", bad_);
    EXPECT_EQ(reload_config(&state_), -1);

    /* A typo in the config file must not leave the daemon with nothing to
     * authorise requests against. */
    ASSERT_EQ(state_.config, live);
    EXPECT_EQ(state_.config->roles.len, roles);
    EXPECT_STREQ(state_.config->roles.d[0]->name, "probe");
    EXPECT_TRUE(state_.config->roles.d[0]->query);
}

TEST_F(ReloadConfigTest, ReplacesTheLiveConfigWhenTheNewOneParses) {
    state_.clients = client_ptr_array_init();
    ASSERT_NO_FATAL_FAILURE(loadGoodConfig());
    const config* live = state_.config;

    ScopedEnv replacement("VRTD_CONFIG", good_);
    EXPECT_EQ(reload_config(&state_), 0);

    EXPECT_NE(state_.config, live);
    ASSERT_GT(state_.config->roles.len, 0u);
    EXPECT_STREQ(state_.config->roles.d[0]->name, "probe");
}

}  // namespace

// ################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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

// Integration guard for the shipped default model server: launch the *real*
// vpp_sim binary (the one packed into the default VBIN) and drive it through the
// daemon's ModelClient.  This pins the production model to the same happy-path
// dialect the client speaks, so the shipped default and the client can't drift.
// SLASH_SYSEMU_MODEL_BIN is the built binary's path, injected by tests/CMakeLists.txt.

#include "model_client.h"

#include <chrono>
#include <cstdint>
#include <numeric>
#include <span>
#include <string>
#include <vector>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#ifndef SLASH_SYSEMU_MODEL_BIN
#error "SLASH_SYSEMU_MODEL_BIN must be defined by the build system"
#endif

using slash_sysemu::ModelClient;

namespace {

std::span<const uint8_t> as_span(const std::vector<uint8_t>& v) {
    return std::span<const uint8_t>(v.data(), v.size());
}

// Launches the model binary on an ipc endpoint and reaps it on destruction.
class LaunchedModel {
public:
    explicit LaunchedModel(std::string endpoint) : endpoint_(std::move(endpoint)) {
        pid_ = ::fork();
        if (pid_ == 0) {
            // Child: exec the model with the endpoint as argv[1].
            ::execl(SLASH_SYSEMU_MODEL_BIN, SLASH_SYSEMU_MODEL_BIN, endpoint_.c_str(),
                    static_cast<char*>(nullptr));
            _exit(127); // exec failed
        }
    }

    ~LaunchedModel() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
    }

    [[nodiscard]] pid_t pid() const { return pid_; }
    // Reap a model that exited on its own; returns true if it did within a grace.
    bool reap_within(std::chrono::milliseconds grace) {
        for (int i = 0; i < grace.count() / 10 + 1; ++i) {
            int   status = 0;
            pid_t r      = ::waitpid(pid_, &status, WNOHANG);
            if (r == pid_) {
                pid_ = -1;
                return true;
            }
            ::usleep(10 * 1000);
        }
        return false;
    }

private:
    std::string endpoint_;
    pid_t       pid_ = -1;
};

TEST(ModelServerIntegrationTest, RoundTripsThroughModelClient) {
    const std::string ep = "ipc:///tmp/slash_sysemu_model_test_" +
                           std::to_string(::getpid()) + ".sock";

    LaunchedModel model(ep);
    ASSERT_GT(model.pid(), 0) << "failed to fork model";

    auto c = ModelClient::connect(ep, std::chrono::milliseconds(2000));
    ASSERT_TRUE(c.has_value()) << c.error().message;
    ModelClient& client = c.value();

    // start
    ASSERT_TRUE(client.start().has_value());

    // register round-trip
    ASSERT_TRUE(client.reg_write(0x40, 0xcafef00du).has_value());
    auto scalar = client.fetch_scalar(0x40);
    ASSERT_TRUE(scalar.has_value()) << scalar.error().message;
    EXPECT_EQ(0xcafef00du, scalar.value());

    // buffer round-trip
    std::vector<uint8_t> data(64);
    std::iota(data.begin(), data.end(), 1);
    ASSERT_TRUE(client.populate(0x1000, as_span(data)).has_value());
    auto buf = client.fetch_buffer(0x1000, data.size());
    ASSERT_TRUE(buf.has_value()) << buf.error().message;
    EXPECT_EQ(data, buf.value());

    // unwritten memory reads back as zero
    auto zeros = client.fetch_buffer(0x9000, 8);
    ASSERT_TRUE(zeros.has_value());
    EXPECT_EQ(std::vector<uint8_t>(8, 0), zeros.value());

    // exit → model replies OK then terminates on its own
    ASSERT_TRUE(client.exit().has_value());
    EXPECT_TRUE(model.reap_within(std::chrono::milliseconds(2000)))
        << "model did not exit after `exit` verb";
}

} // namespace

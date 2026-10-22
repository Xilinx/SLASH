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

/// @file debug/hotplug.cpp
/// @brief Implementation of the debug hotplug-op command.

#include "hotplug.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <vrtd/session.hpp>

#include "../bdf.hpp"

namespace {

std::string toLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

vrtd::HotplugOp parseOp(const std::string_view text) {
    const std::string normalized = toLower(text);
    if (normalized == "rescan") {
        return vrtd::HotplugOp::Rescan;
    }
    if (normalized == "remove") {
        return vrtd::HotplugOp::Remove;
    }
    if (normalized == "toggle-sbr") {
        return vrtd::HotplugOp::ToggleSbr;
    }
    if (normalized == "hotplug") {
        return vrtd::HotplugOp::Hotplug;
    }
    if (normalized == "link-disable") {
        return vrtd::HotplugOp::LinkDisable;
    }
    if (normalized == "link-enable") {
        return vrtd::HotplugOp::LinkEnable;
    }

    throw std::invalid_argument(
        "op must be one of: rescan, remove, toggle-sbr, hotplug, link-disable, link-enable");
}

bool isFunctionLevelOp(vrtd::HotplugOp op) {
    return op == vrtd::HotplugOp::Remove ||
           op == vrtd::HotplugOp::ToggleSbr ||
           op == vrtd::HotplugOp::Hotplug;
}

// Device-targeted but always acts on all PFs (vrtd rejects anything else).
bool isBoardLevelOp(vrtd::HotplugOp op) {
    return op == vrtd::HotplugOp::LinkDisable;
}

bool defaultsToAllPfs(vrtd::HotplugOp op) {
    return op == vrtd::HotplugOp::Remove ||
           op == vrtd::HotplugOp::Hotplug ||
           op == vrtd::HotplugOp::LinkDisable;
}

const char* opName(vrtd::HotplugOp op) {
    switch (op) {
    case vrtd::HotplugOp::Rescan:
        return "rescan";
    case vrtd::HotplugOp::Remove:
        return "remove";
    case vrtd::HotplugOp::ToggleSbr:
        return "toggle-sbr";
    case vrtd::HotplugOp::Hotplug:
        return "hotplug";
    case vrtd::HotplugOp::LinkDisable:
        return "link-disable";
    case vrtd::HotplugOp::LinkEnable:
        return "link-enable";
    case vrtd::HotplugOp::ResetSequence:
        break;
    }

    return "unknown";
}

vrtd::HotplugOp validateOptions(const Hotplug::Options& options) {
    const vrtd::HotplugOp op = parseOp(options.opText);
    const bool needsFunction = isFunctionLevelOp(op);
    const bool needsDevice = needsFunction || isBoardLevelOp(op);

    if (op == vrtd::HotplugOp::ToggleSbr && !options.function.has_value()) {
        throw std::invalid_argument("--function is required for toggle-sbr");
    }
    if (!needsFunction && options.function.has_value()) {
        throw std::invalid_argument("--function is only valid for remove, toggle-sbr, and hotplug");
    }
    if (needsDevice && options.bdf.empty()) {
        throw std::invalid_argument(
            "--device is required for remove, toggle-sbr, hotplug, and link-disable");
    }
    if (!needsDevice && !options.bdf.empty()) {
        throw std::invalid_argument(
            "--device is only valid for remove, toggle-sbr, hotplug, and link-disable");
    }

    return op;
}

} // namespace

int Hotplug::run(const Options& options) {
    const vrtd::HotplugOp op = validateOptions(options);

    vrtd::Session session;
    if (op == vrtd::HotplugOp::Rescan) {
        session.hotplugRescan();
        std::cout << "hotplug_op=rescan\n";
        return 0;
    }
    if (op == vrtd::HotplugOp::LinkEnable) {
        // Re-enables the link disabled by an earlier link-disable on *this*
        // vrtd connection.  Since every v80-smi invocation is its own
        // connection, vrtd has already re-enabled a link left disabled by a
        // crashed process; this op exists for scripted use of the libvrtdpp
        // API and for completeness.
        session.hotplugLinkEnable();
        std::cout << "hotplug_op=link-enable\n";
        return 0;
    }

    const std::string bdf = resolveBoardBdf(options.bdf, "debug hotplug-op");
    auto device = session.getDeviceByBdf(bdf);
    const uint8_t function = options.function.value_or(vrtd::HotplugFunctionAll);
    device.hotplugOp(op, function);

    std::cout << "hotplug_op=" << opName(op) << " bdf=" << bdf;
    if (options.function.has_value()) {
        std::cout << " function=" << static_cast<unsigned int>(*options.function);
    } else if (defaultsToAllPfs(op)) {
        std::cout << " function=all";
    }
    std::cout << '\n';

    return 0;
}

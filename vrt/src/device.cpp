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

/**
 * @file device.cpp
 * @brief Device class implementation.
 */

#include <vrt/device.hpp>

#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>
#include <vrtd/bar.hpp>

#include <vrt/utils/filesystem_cache.hpp>

namespace vrt {
namespace impl {

namespace {

// Normalize a user-supplied BDF to board-level "DDDD:BB:DD" for vrtd lookup.
// Strips function digit (.F) if present, prepends domain 0000: if missing.
std::string normalizeBdfForVrtd(const std::string& bdf) {
    std::string result = bdf;

    // Strip function digit
    auto dot = result.rfind('.');
    if (dot != std::string::npos) {
        std::cerr << "Warning: BDF '" << bdf
                  << "' contains a PF function number; "
                  << "stripping " << result.substr(dot)
                  << " — use board address instead"
                  << std::endl;
        result = result.substr(0, dot);
    }

    // Prepend domain if missing (only one colon means short BDF)
    const auto firstColon = result.find(':');
    const auto lastColon = result.rfind(':');
    if (firstColon == std::string::npos || firstColon == lastColon) {
        result = "0000:" + result;
    }
    return result;
}

// Normalize a user-supplied BDF to board-level "BB:DD" (no domain, no function).
std::string normalizeBdfLegacy(const std::string& bdf) {
    std::string result = bdf;

    // Strip function digit
    auto dot = result.rfind('.');
    if (dot != std::string::npos) {
        result = result.substr(0, dot);
    }

    // Strip domain
    const auto firstColon = result.find(':');
    const auto lastColon = result.rfind(':');
    if (firstColon != std::string::npos && firstColon != lastColon) {
        result = result.substr(firstColon + 1);
    }
    return result;
}

std::string shellQuote(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

std::string makeExecFromBinaryDirCommand(const std::string& execPath) {
    const std::filesystem::path path(execPath);
    const std::string dir = path.parent_path().string();
    const std::string file = path.filename().string();
    if (dir.empty() || file.empty()) {
        return shellQuote(execPath);
    }
    return "cd " + shellQuote(dir) + " && exec ./" + shellQuote(file);
}

bool parseEmuArgIndex(const std::string& argName, std::size_t& outIdx) {
    if (argName.size() < 4 || argName.rfind("arg", 0) != 0) {
        return false;
    }
    std::size_t value = 0;
    bool hasDigit = false;
    for (std::size_t i = 3; i < argName.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(argName[i]);
        if (!std::isdigit(c)) {
            return false;
        }
        hasDigit = true;
        value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    if (!hasDigit) {
        return false;
    }
    outIdx = value;
    return true;
}

void applyEmuManifestToKernels(const std::string& manifestPath, std::map<std::string, Kernel>& kernels) {
    if (manifestPath.empty()) {
        throw std::runtime_error("EMU manifest missing from vrtbin");
    }

    std::ifstream in(manifestPath);
    if (!in.is_open()) {
        throw std::runtime_error("EMU manifest path unreadable: " + manifestPath);
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(in, root) || !root.isObject()) {
        throw std::runtime_error("Failed to parse EMU manifest: " + manifestPath);
    }

    std::size_t appliedCallKinds = 0;
    std::size_t appliedFetchRoutes = 0;

    const Json::Value manifestKernels = root["kernels"];
    if (!manifestKernels.isArray()) {
        throw std::runtime_error("EMU manifest missing required array: kernels");
    }
    if (manifestKernels.isArray()) {
        for (const auto& k : manifestKernels) {
            if (!k.isObject()) continue;
            const std::string instance = k.get("instance", "").asString();
            if (instance.empty()) continue;
            auto it = kernels.find(instance);
            if (it == kernels.end()) continue;

            std::vector<std::string> kinds;
            const Json::Value callArgs = k["call_args"];
            if (callArgs.isArray()) {
                for (const auto& ca : callArgs) {
                    if (!ca.isObject()) continue;
                    const std::string argName = ca.get("arg", "").asString();
                    const std::string kind = ca.get("kind", "").asString();
                    std::size_t idx = 0;
                    if (kind.empty() || !parseEmuArgIndex(argName, idx)) continue;
                    if (idx >= kinds.size()) {
                        kinds.resize(idx + 1);
                    }
                    kinds[idx] = kind;
                }
            }
            if (!kinds.empty()) {
                it->second.setEmuCallArgKinds(kinds);
                appliedCallKinds += 1;
            }
        }
    }

    std::map<std::string, std::map<uint32_t, std::string>> fetchRoutesByKernel;
    const Json::Value fetch = root["fetch"];
    if (!fetch.isObject()) {
        throw std::runtime_error("EMU manifest missing required object: fetch");
    }
    const Json::Value fetchScalar = fetch["scalar"];
    if (!fetchScalar.isArray()) {
        throw std::runtime_error("EMU manifest missing required array: fetch.scalar");
    }
    if (fetchScalar.isArray()) {
        for (const auto& route : fetchScalar) {
            if (!route.isObject()) continue;
            const std::string functionName = route.get("function", "").asString();
            const std::string argName = route.get("arg", "").asString();
            if (functionName.empty() || argName.empty()) continue;
            const Json::Value source = route["source"];
            if (!source.isObject()) continue;
            const Json::Value regOff = source["register_offset"];
            if (!regOff.isUInt() && !regOff.isInt()) continue;
            const uint32_t offset = regOff.asUInt();
            fetchRoutesByKernel[functionName][offset] = argName;
        }
    }

    for (auto& kv : fetchRoutesByKernel) {
        auto it = kernels.find(kv.first);
        if (it == kernels.end()) continue;
        it->second.setEmuFetchScalarArgByOffset(kv.second);
        appliedFetchRoutes += kv.second.size();
    }

    utils::Logger::log(utils::LogLevel::DEBUG, __PRETTY_FUNCTION__,
                       "Applied EMU manifest metadata: call-kind kernels={}, fetch routes={}",
                       appliedCallKinds, appliedFetchRoutes);
}

}  // namespace

Device::Device(const std::string& bdf, const std::string& vrtbinPath, bool program,
               ProgramType programType)
    : vrtbin(vrtbinPath, bdf) {
    this->bdf = normalizeBdfLegacy(bdf);
    this->bdfFull = normalizeBdfForVrtd(bdf);
    this->allocator = new Allocator();
    this->systemMap = this->vrtbin.getSystemMapPath();
    this->pdiPath = this->vrtbin.getPdiPath();
    this->pdiPaths = this->vrtbin.getPdiPaths();
    this->programType = programType;
    this->zmqServer = std::make_shared<ZmqServer>();
    this->platform = vrtbin.getPlatform();
    if (platform == Platform::HARDWARE) {
        vrtdSession = std::make_shared<vrtd::Session>();
        vrtdDevice = vrtdSession->getDeviceByBdf(bdfFull);
        if (program) {
            programDevice();
        }
        parseSystemMap();
        if (program && !kernels.empty()) {
            sleep(1); // wait for device to be ready after programming before accessing BAR

        }
        if (vrtdDevice.has_value()) {
            if (clockFreq > CLOCK_MAX_FREQ) {
                utils::Logger::log(utils::LogLevel::WARN, __PRETTY_FUNCTION__,
                           "Clock frequency {} exceeds maximum frequency {}", clockFreq, CLOCK_MAX_FREQ);
                vrtdDevice->setUserClockRate(static_cast<uint32_t>(CLOCK_MAX_FREQ));
            }
        }
    } else if (platform == Platform::EMULATION) {
        parseSystemMap();
        applyEmuManifestToKernels(this->vrtbin.getEmulationManifest(), kernels);
        std::string emulationExecPath = this->vrtbin.getEmulationExec();
        if (emulationExecPath.empty()) {
            throw std::runtime_error("Emulation executable vpp_emu not found in vrtbin");
        }
        if (::access(emulationExecPath.c_str(), X_OK) != 0) {
            throw std::runtime_error("Emulation executable is not runnable: " + emulationExecPath +
                                     " (" + std::strerror(errno) + ")");
        }

        const std::string emuCommand = makeExecFromBinaryDirCommand(emulationExecPath);
        runtimeThread = std::thread([emuCommand]() {
            int rc = std::system(emuCommand.c_str());
            if (rc != 0) {
                utils::Logger::log(utils::LogLevel::WARN, __PRETTY_FUNCTION__,
                    "Emulation process exited with code {}", rc);
            }
        });

    } else {
        parseSystemMap();
        std::string simulationExecPath = this->vrtbin.getSimulationExec();
        if (simulationExecPath.empty()) {
            throw std::runtime_error("Simulation executable vpp_sim not found in vrtbin");
        }
        if (::access(simulationExecPath.c_str(), X_OK) != 0) {
            throw std::runtime_error("Simulation executable is not runnable: " +
                                     simulationExecPath + " (" + std::strerror(errno) + ")");
        }

        const std::string simCommand = makeExecFromBinaryDirCommand(simulationExecPath);
        runtimeThread = std::thread([simCommand]() {
            int rc = std::system(simCommand.c_str());
            if (rc != 0) {
                utils::Logger::log(utils::LogLevel::WARN, __PRETTY_FUNCTION__,
                    "Simulation process exited with code {}", rc);
            }
        });
        Json::Value command;
        command["command"] = "start";
        zmqServer->sendCommand(command);
    }
    if (platform == Platform::HARDWARE && vrtdDevice.has_value()) {
        for (auto& qdmaCon : qdmaConnections) {
            qdmaIntfs.emplace_back(new QdmaIntf(*vrtdDevice, qdmaCon.getQid(),
                                                qdmaCon.getDirection()));
        }
    }
}

Device::~Device() {
    cleanup();
    delete allocator;
    allocator = nullptr;
}

void Device::parseSystemMap() {
    XMLParser parser(systemMap);
    parser.parseXML();
    clockFreq = parser.getClockFrequency();
    this->platform = parser.getPlatform();
    kernels = parser.getKernels();

    std::optional<vrtd::Bar> barHandle = std::nullopt;
    if (platform == Platform::HARDWARE && vrtdDevice.has_value()) {
        barHandle = vrtdDevice->getBar(bar);
    }

    for (auto&kernel : kernels) {
        kernel.second.setPlatform(platform);
        kernel.second.setVrtdBar(barHandle);
        kernel.second.setServer(zmqServer);
    }
    this->qdmaConnections = parser.getQdmaConnections();
}

Kernel Device::getKernel(const std::string& name) {
    auto it = kernels.find(name);
    if (it == kernels.end()) {
        throw std::runtime_error("Kernel '" + name + "' not found in system_map metadata");
    }
    return it->second;
}

void Device::cleanup() {
    if (cleanupDone) {
        return;
    }
    cleanupDone = true;

    if (platform == Platform::HARDWARE) {
        for (auto qdmaIntf_ : qdmaIntfs) {
            delete qdmaIntf_;
        }
        qdmaIntfs.clear();
    } else if (platform == Platform::EMULATION || platform == Platform::SIMULATION) {
        Json::Value exit;
        exit["command"] = "exit";
        zmqServer->sendCommand(exit);
    }
    if (runtimeThread.joinable()) {
        runtimeThread.join();
    }
}

std::string Device::getBdf() { return bdf; }

void Device::programDevice() {
    if (pdiPaths.empty() && !pdiPath.empty()) {
        pdiPaths.push_back(pdiPath);
    }
    if (pdiPaths.empty()) {
        throw std::runtime_error("No PDI files found for programming");
    }

    for (const auto& pdi : pdiPaths) {
        utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                           "Programming PDI via vrtd design writer {}", pdi);
        getVrtdDevice().designWriteFile(pdi);
    }
}

void Device::setFrequency(uint64_t freq) {
    if (platform == Platform::HARDWARE) {
        if (freq > clockFreq) {
            utils::Logger::log(utils::LogLevel::WARN, __PRETTY_FUNCTION__,
                               "Setting frequency {}, which is higher than max frequency {}", freq,
                               clockFreq);
        }
        if (freq > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("Requested frequency exceeds vrtd clock API limits");
        }
        getVrtdDevice().setUserClockRate(static_cast<uint32_t>(freq));
    }
}

uint64_t Device::getFrequency() {
    if (platform == Platform::HARDWARE) {
        return getVrtdDevice().getUserClockRate();
    } else {
        return 0;
    }
}

uint64_t Device::getMaxFrequency() {
    if (platform == Platform::HARDWARE) {
        return clockFreq;
    } else {
        return 0;
    }
}

void Device::findPlatform() {
    XMLParser parser(systemMap);
    parser.parseXML();
    this->platform = parser.getPlatform();
}

Platform Device::getPlatform() { return platform; }

std::shared_ptr<ZmqServer> Device::getZmqServer() { return zmqServer; }

std::vector<QdmaConnection> Device::getQdmaConnections() { return qdmaConnections; }

Allocator* Device::getAllocator() { return allocator; }

vrtd::Device& Device::getVrtdDevice() {
    if (!vrtdDevice.has_value()) {
        throw std::runtime_error("vrtd device not initialized");
    }
    return *vrtdDevice;
}

const vrtd::Device& Device::getVrtdDevice() const {
    if (!vrtdDevice.has_value()) {
        throw std::runtime_error("vrtd device not initialized");
    }
    return *vrtdDevice;
}

std::vector<QdmaIntf*> Device::getQdmaInterfaces() { return qdmaIntfs; }

}  // namespace impl
}  // namespace vrt

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
 * @file kernel.cpp
 * @brief Kernel class implementation.
 */

#include <vrt/kernel.hpp>

#include <vrt/device.hpp>

#include <algorithm>
#include <sstream>

namespace vrt {
namespace {

MemoryConfig parseMemoryTarget(const std::string& target) {
    if (target.rfind("DDR", 0) == 0) {
        return {MemoryRangeType::DDR, std::nullopt};
    }
    if (target.rfind("HBM", 0) == 0) {
        std::string bankStr = target.substr(3);
        if (!bankStr.empty()) {
            return {MemoryRangeType::HBM, static_cast<uint8_t>(std::stoul(bankStr))};
        }
        return {MemoryRangeType::HBM_VNOC, std::nullopt};
    }
    if (target.rfind("MEM", 0) == 0) {
        return {MemoryRangeType::HBM_VNOC, std::nullopt};
    }
    throw std::runtime_error("Unknown memory target '" + target + "'");
}

uint64_t resolveBarOffset(uint64_t absoluteAddr, uint64_t accessSize, uint64_t barLen) {
    if (barLen == 0) {
        throw std::runtime_error("BAR length is zero");
    }

    // Design model: BAR maps a contiguous AXI window. Kernel base addresses
    // are absolute within that window; register offsets are relative to kernel base.
    const uint64_t barWindowBase = absoluteAddr - (absoluteAddr % barLen);
    const uint64_t barOffset = absoluteAddr - barWindowBase;
    if (barOffset + accessSize > barLen) {
        throw std::runtime_error("BAR access out of range");
    }
    return barOffset;
}

}  // namespace

Kernel::Kernel(const std::string& name, uint64_t baseAddr, uint64_t range,
               const std::vector<Register>& registers,
               const std::vector<FunctionalArg>& functionalArgs) {
    this->name = name;
    this->baseAddr = baseAddr;
    this->range = range;
    this->registers = registers;
    this->functionalArgs = functionalArgs;
    std::sort(this->functionalArgs.begin(), this->functionalArgs.end(),
              [](const FunctionalArg& a, const FunctionalArg& b) { return a.idx < b.idx; });
}

Kernel::Kernel(Device device, const std::string& kernelName)
    : Kernel(device.getKernel(kernelName)) {}

vrtd::BarFile& Kernel::getOrOpenBarFile() {
    if (!vrtdBar.has_value()) {
        throw std::runtime_error("vrtd BAR handle not initialized");
    }
    if (!vrtdBarFile || vrtdBarFile->isClosed()) {
        vrtdBarFile = std::make_shared<vrtd::BarFile>(vrtdBar->openBarFile());
    }
    return *vrtdBarFile;
}

void Kernel::write(uint32_t offset, uint32_t value) {
    if (platform == Platform::HARDWARE) {
        utils::Logger::log(utils::LogLevel::DEBUG, __PRETTY_FUNCTION__,
                           "Writing to device {} kernel: {} at offset: {x} value: {x}", deviceBdf,
                           name, offset, value);
        auto& barFile = getOrOpenBarFile();
        const uint64_t absoluteAddr = baseAddr + static_cast<uint64_t>(offset);
        uint64_t barOffset = resolveBarOffset(absoluteAddr, sizeof(uint32_t), barFile.getLen());
        auto ptr = barFile.getPtr<uint32_t>(vrtd::BarFile::Direction::Write,
                                            static_cast<size_t>(barOffset));
        *ptr = value;
        return;
    } else if (platform == Platform::SIMULATION) {
        server->sendScalar(baseAddr + offset, value);
    }
}

uint32_t Kernel::read(uint32_t offset) {
    if (platform == Platform::HARDWARE) {
        if (offset != 0)
            utils::Logger::log(utils::LogLevel::DEBUG, __PRETTY_FUNCTION__,
                               "Reading from device {} kernel: {} at offset: {x}", deviceBdf, name,
                               offset);
        auto& barFile = getOrOpenBarFile();
        const uint64_t absoluteAddr = baseAddr + static_cast<uint64_t>(offset);
        uint64_t barOffset = resolveBarOffset(absoluteAddr, sizeof(uint32_t), barFile.getLen());
        auto ptr = barFile.getPtr<uint32_t>(vrtd::BarFile::Direction::Read,
                                            static_cast<size_t>(barOffset));
        return *ptr;
    } else if (platform == Platform::EMULATION) {
        return server->readRegister(name, offset);
    } else if (platform == Platform::SIMULATION) {
        return server->fetchScalarSim(baseAddr + offset);
    }
    return 0;
}

void Kernel::setVrtdBar(const std::optional<vrtd::Bar>& bar) { this->vrtdBar = bar; }

void Kernel::setServer(std::shared_ptr<ZmqServer> server) { this->server = server; }

void Kernel::setFunctionalArgs(const std::vector<FunctionalArg>& args) {
    functionalArgs = args;
    std::sort(functionalArgs.begin(), functionalArgs.end(),
              [](const FunctionalArg& a, const FunctionalArg& b) { return a.idx < b.idx; });
}

bool Kernel::hasFunctionalArgs() const { return !functionalArgs.empty(); }

const std::vector<FunctionalArg>& Kernel::getFunctionalArgs() const {
    return functionalArgs;
}

std::string Kernel::buildArgApiUsageMessage(std::string_view reason, std::string_view opName) const {
    std::ostringstream oss;
    oss << "Kernel argument API misuse for kernel '" << name << "': " << reason << "\n";
    oss << "Usage model:\n";
    oss << "1) Positional launch: kernel." << opName << "(arg0, arg1, ...)\n";
    oss << "2) Staged launch: kernel.setArg(idx_or_name, value) ... then kernel." << opName << "()\n";
    oss << "Rules:\n";
    oss << "- Choose exactly one style per launch; do not mix setArg(...) with "
        << opName << "(...)\n";
    oss << "- If " << opName
        << "() is used (no positional args) and functional_args exist, every writable arg must be set via setArg\n";
    oss << "- If no functional_args metadata exists, argument APIs are unavailable; use write(offset, value) then "
        << opName << "()\n";

    if (!functionalArgs.empty()) {
        oss << "Expected functional_args:\n";
        for (const FunctionalArg& arg : functionalArgs) {
            oss << "  - idx=" << arg.idx << ", name='" << arg.name << "', type='" << arg.type
                << "', offset=0x" << std::hex << arg.offset << std::dec
                << ", range_bits=" << arg.range << "\n";
        }
    }

    return oss.str();
}

[[noreturn]] void Kernel::throwArgApiMisuse(std::string_view reason, std::string_view opName) const {
    throw std::runtime_error(buildArgApiUsageMessage(reason, opName));
}

void Kernel::ensureNoSetArgValuesWhenPassingArgs(std::size_t providedArgCount,
                                                 std::string_view opName) const {
    if (providedArgCount > 0 && !setArgValues.empty()) {
        throwArgApiMisuse(
            "Positional arguments were passed while staged setArg(...) values are already present.",
            opName);
    }
}

void Kernel::ensureSetArgValuesCompleteForLaunch(std::string_view opName) const {
    std::vector<std::string> missing;
    for (const FunctionalArg& argMeta : functionalArgs) {
        if (!argMeta.writable) {
            continue;
        }
        if (setArgValues.find(argMeta.idx) == setArgValues.end()) {
            missing.push_back("'" + argMeta.name + "'(idx " + std::to_string(argMeta.idx) + ")");
        }
    }
    if (!missing.empty()) {
        std::ostringstream reason;
        reason << "Not all functional args were provided via setArg before " << opName << "(). Missing: ";
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i != 0) {
                reason << ", ";
            }
            reason << missing[i];
        }
        throwArgApiMisuse(reason.str(), opName);
    }
}

const FunctionalArg& Kernel::functionalArgByIdx(uint32_t idx) const {
    auto it = std::find_if(functionalArgs.begin(), functionalArgs.end(),
                           [idx](const FunctionalArg& arg) { return arg.idx == idx; });
    if (it == functionalArgs.end()) {
        throwArgApiMisuse("setArg(idx, value) referenced unknown arg index " + std::to_string(idx) +
                              ".",
                          "setArg");
    }
    return *it;
}

uint32_t Kernel::functionalArgIdxByName(std::string_view argName) const {
    if (argName.empty()) {
        throwArgApiMisuse("setArg(name, value) received an empty argument name.", "setArg");
    }

    const std::string requestedName(argName);
    bool found = false;
    uint32_t foundIdx = 0;
    for (const FunctionalArg& argMeta : functionalArgs) {
        if (argMeta.name == requestedName) {
            if (found) {
                throwArgApiMisuse("setArg(name, value) matched multiple args for name '" +
                                      requestedName + "'.",
                                  "setArg");
            }
            found = true;
            foundIdx = argMeta.idx;
        }
    }

    // Vitis HLS appends _r to m_axi (buffer) register names.
    // Allow users to use the original HLS parameter name (e.g. "in" -> "in_r").
    if (!found) {
        const std::string withSuffix = requestedName + "_r";
        for (const FunctionalArg& argMeta : functionalArgs) {
            if (argMeta.name == withSuffix) {
                if (found) {
                    throwArgApiMisuse("setArg(name, value) matched multiple args for name '" +
                                          requestedName + "' (resolved to '" + withSuffix + "').",
                                      "setArg");
                }
                found = true;
                foundIdx = argMeta.idx;
            }
        }
    }

    if (!found) {
        throwArgApiMisuse("setArg(name, value) referenced unknown arg name '" + requestedName +
                              "'.",
                          "setArg");
    }
    return foundIdx;
}

void Kernel::setArgResolved(uint32_t idx, uint64_t value) {
    if (functionalArgs.empty()) {
        throwArgApiMisuse(
            "setArg(...) was used but this kernel has no functional_args metadata in system_map.xml.",
            "setArg");
    }
    const FunctionalArg& argMeta = functionalArgByIdx(idx);
    setArgValues[argMeta.idx] = value;
}

void Kernel::writeArgToRegisterMap(const FunctionalArg& argMeta, uint64_t value) {
    const uint32_t words = argWordCount(argMeta);
    for (uint32_t i = 0; i < words; ++i) {
        const uint32_t off = argMeta.offset + i * sizeof(uint32_t);
        registerMap[off] = argWordValue(value, i);
    }
}

void Kernel::writeArgToSimulation(const FunctionalArg& argMeta, uint64_t value) {
    const uint32_t words = argWordCount(argMeta);
    for (uint32_t i = 0; i < words; ++i) {
        const uint32_t off = argMeta.offset + i * sizeof(uint32_t);
        write(off, argWordValue(value, i));
    }
}

void Kernel::writeArgToEmulation(Json::Value& command, const FunctionalArg& argMeta,
                                 uint64_t value) const {
    const std::string emuKind = normalizeArgType(argMeta.type);
    const uint32_t emuArgIdx = argMeta.idx;

    if (emuKind == "buffer") {
        command["args"]["arg" + std::to_string(emuArgIdx)]["type"] = "buffer";
        command["args"]["arg" + std::to_string(emuArgIdx)]["name"] = std::to_string(value);
        return;
    }
    if (emuKind == "scalar") {
        command["args"]["arg" + std::to_string(emuArgIdx)]["type"] = "scalar";
        command["args"]["arg" + std::to_string(emuArgIdx)]["value"] =
            static_cast<Json::UInt64>(value);
        return;
    }
    throw std::runtime_error("Unsupported functional arg type '" + argMeta.type +
                             "' for kernel '" + name + "' at idx " +
                             std::to_string(argMeta.idx));
}

void Kernel::applySetArgsToRegisterMap() {
    registerMap.clear();
    for (const FunctionalArg& argMeta : functionalArgs) {
        if (!argMeta.writable) {
            continue;
        }
        const uint64_t value = setArgValues.at(argMeta.idx);
        writeArgToRegisterMap(argMeta, value);
    }
}

void Kernel::applySetArgsToSimulation() {
    for (const FunctionalArg& argMeta : functionalArgs) {
        if (!argMeta.writable) {
            continue;
        }
        const uint64_t value = setArgValues.at(argMeta.idx);
        writeArgToSimulation(argMeta, value);
    }
}

void Kernel::applySetArgsToEmulation(Json::Value& command) const {
    for (const FunctionalArg& argMeta : functionalArgs) {
        if (!argMeta.writable) {
            continue;
        }
        const uint64_t value = setArgValues.at(argMeta.idx);
        writeArgToEmulation(command, argMeta, value);
    }
}

void Kernel::setEmuCallArgKinds(const std::vector<std::string>& kinds) { emuCallArgKinds = kinds; }

void Kernel::setEmuFetchScalarArgByOffset(const std::map<uint32_t, std::string>& routes) {
    emuFetchScalarArgByOffset = routes;
}

void Kernel::setConnections(const std::map<std::string, std::string>& conns) {
    connections = conns;
}

void Kernel::validateBufferMemoryType(const FunctionalArg& argMeta, MemoryRangeType memType,
                                      uint8_t hbmPort) const {
    if (argMeta.port.empty()) {
        return;
    }
    auto it = connections.find(argMeta.port);
    if (it == connections.end()) {
        return;
    }
    const std::string& target = it->second;

    if (target.rfind("DDR", 0) == 0) {
        if (memType != MemoryRangeType::DDR) {
            throw std::runtime_error(
                "Memory type mismatch for kernel '" + name + "' argument '" + argMeta.name +
                "' (port " + argMeta.port + "): target is " + target +
                " but buffer is not DDR");
        }
    } else if (target.rfind("HBM", 0) == 0) {
        if (memType == MemoryRangeType::DDR) {
            throw std::runtime_error(
                "Memory type mismatch for kernel '" + name + "' argument '" + argMeta.name +
                "' (port " + argMeta.port + "): target is " + target +
                " but buffer is DDR");
        }
        if (memType == MemoryRangeType::HBM_VNOC) {
            throw std::runtime_error(
                "Memory type mismatch for kernel '" + name + "' argument '" + argMeta.name +
                "' (port " + argMeta.port + "): target is " + target +
                " but buffer is HBM_VNOC (use a specific HBM region buffer)");
        }
        // HBM with specific bank — extract bank number and verify exact match
        std::string bankStr = target.substr(3);
        if (!bankStr.empty()) {
            unsigned long expectedBank = std::stoul(bankStr);
            if (static_cast<unsigned long>(hbmPort) != expectedBank) {
                throw std::runtime_error(
                    "Memory type mismatch for kernel '" + name + "' argument '" + argMeta.name +
                    "' (port " + argMeta.port + "): target is " + target +
                    " but buffer is HBM" + std::to_string(hbmPort));
            }
        }
    } else if (target == "MEM") {
        if (memType != MemoryRangeType::HBM_VNOC && memType != MemoryRangeType::HBM) {
            throw std::runtime_error(
                "Memory type mismatch for kernel '" + name + "' argument '" + argMeta.name +
                "' (port " + argMeta.port + "): target is MEM but buffer is DDR"
                " (use an HBM_VNOC or HBM buffer)");
        }
    }
}

MemoryConfig Kernel::portMemoryConfig(std::string_view portName) const {
    auto it = connections.find(std::string(portName));
    if (it == connections.end()) {
        throw std::runtime_error("Kernel '" + name + "' has no memory connection for port '" +
                                 std::string(portName) + "'");
    }
    return parseMemoryTarget(it->second);
}

MemoryConfig Kernel::argMemoryConfig(std::string_view argName) const {
    const uint32_t idx = functionalArgIdxByName(argName);
    const FunctionalArg& arg = functionalArgByIdx(idx);
    if (arg.port.empty()) {
        throw std::runtime_error("Kernel '" + name + "' argument '" + std::string(argName) +
                                 "' has no associated AXI port");
    }
    return portMemoryConfig(arg.port);
}

void Kernel::wait() {
    if (platform == Platform::EMULATION) {
        Json::Value command;
        command["command"] = "wait";
        command["function"] = name;
        server->sendCommand(command);
        return;
    }
    // ap_ctrl_hs: wait for ap_done (CTRL[1]) instead of checking exact control word values.
    while ((read(0x00) & 0x2u) == 0u) {
    }
}

void Kernel::start() { this->start<>(); }

void Kernel::startKernel(bool autorestart) {
    if (autorestart) {
        write(0x00, 0x81);
    } else {
        write(0x00, 0x01);
    }
}

Kernel::~Kernel() {}

void Kernel::setPlatform(Platform platform) { this->platform = platform; }

void Kernel::writeBatch() {
    if (platform != Platform::HARDWARE) {
        return;
    }
    if (registerMap.empty()) {
        return;
    }

    uint32_t maxOffset = 0;
    for (const auto& [offset, _] : registerMap) {
        maxOffset = std::max(maxOffset, offset);
    }
    uint32_t noOfPhysicalRegisters = (maxOffset + sizeof(uint32_t)) / sizeof(uint32_t);

    uint32_t* buf = (uint32_t*)calloc(noOfPhysicalRegisters, sizeof(uint32_t));
    for (const auto& [offset, value] : registerMap) {
        const std::size_t wordIdx = static_cast<std::size_t>(offset / sizeof(uint32_t));
        if (wordIdx >= noOfPhysicalRegisters) {
            continue;
        }
        buf[wordIdx] = value;
        utils::Logger::log(utils::LogLevel::DEBUG, __PRETTY_FUNCTION__,
                           "Kernel {}, reg at offset {x}, value: {x}", name, offset, value);
    }
    vrtd::BarFile* barFilePtr = nullptr;
    try {
        barFilePtr = &getOrOpenBarFile();
    } catch (...) {
        free(buf);
        throw;
    }
    auto& barFile = *barFilePtr;
    uint64_t byteCount = static_cast<uint64_t>(noOfPhysicalRegisters) * sizeof(uint32_t);
    uint64_t barOffset = 0;
    try {
        barOffset = resolveBarOffset(baseAddr, byteCount, barFile.getLen());
    } catch (const std::runtime_error&) {
        free(buf);
        throw std::runtime_error("BAR write range out of range");
    }
    auto ptr = barFile.getPtr<uint32_t>(vrtd::BarFile::Direction::Write,
                                        static_cast<size_t>(barOffset));
    for (uint32_t i = 0; i < noOfPhysicalRegisters; ++i) {
        ptr[i] = buf[i];
    }
    free(buf);
    return;
}
std::string Kernel::getName() const { return name; }
uint64_t Kernel::getPhysAddr() const { return baseAddr; }

}  // namespace vrt

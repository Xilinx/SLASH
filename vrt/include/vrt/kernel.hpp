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
 * @file kernel.hpp
 * @brief Kernel class — hardware kernel execution and argument management.
 */

#ifndef VRT_KERNEL_HPP
#define VRT_KERNEL_HPP

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <vrt/allocator/allocator.hpp>
#include <vrt/register/register.hpp>
#include <vrt/utils/logger.hpp>
#include <vrt/utils/platform.hpp>
#include <vrt/utils/zmq_server.hpp>
#include <vrtd/bar.hpp>
#include <vrtd/bar_file.hpp>

namespace vrt {
class Device;
template <typename T>
class Buffer;

/**
 * @brief Kernel argument metadata parsed from system_map.xml.
 */
struct FunctionalArg {
    uint32_t idx = 0;           ///< Argument index
    std::string name;           ///< Argument name
    std::string type;           ///< C type (e.g. "int", "float*")
    uint32_t offset = 0;        ///< Register offset for this argument
    uint32_t range = 32;        ///< Bit width (32 or 64; 0 treated as 32)
    bool readable = false;      ///< Whether the argument register is readable
    bool writable = false;      ///< Whether the argument register is writable
    std::string port;           ///< AXI port name (for memory arguments)
};

/**
 * @brief Class representing a kernel.
 */
class Kernel {
    uint8_t bar = 0;                                          ///< Base Address Register (BAR)
    std::string name;                                         ///< Name of the kernel
    uint64_t baseAddr = 0;                                    ///< Base address of the kernel
    uint64_t range = 0;                                       ///< Address range of the kernel
    std::vector<Register> registers;                          ///< List of registers in the kernel
    std::vector<FunctionalArg> functionalArgs;                ///< Parsed function arguments from system_map.xml
    size_t currentArgIndex = 0;                               ///< Current call argument index
    std::string deviceBdf;                     ///< BDF of the device
    Platform platform = Platform::UNKNOWN;     ///< Platform of the device
    std::shared_ptr<ZmqServer> server;         ///< Pointer to ZeroMQ server for communication
    std::map<uint32_t, uint32_t> registerMap;  ///< Map of register offsets to values
    std::map<uint32_t, uint64_t> setArgValues; ///< Values assigned through setArg(idx/name, value)
    std::optional<vrtd::Bar> vrtdBar;          ///< vrtd BAR handle for hardware access
    /// Cached BAR file mapping. Wrapped in shared_ptr because BarFile is
    /// non-copyable and Kernel currently uses defaulted copy semantics.
    /// TODO: Consider making Kernel move-only and using unique_ptr instead.
    std::shared_ptr<vrtd::BarFile> vrtdBarFile;

    vrtd::BarFile& getOrOpenBarFile();
    std::vector<std::string> emuCallArgKinds;  ///< Optional EMU arg kind metadata from emu_manifest.json
    std::map<uint32_t, std::string> emuFetchScalarArgByOffset;  ///< Optional EMU fetch routing by register offset
    std::map<std::string, std::string> connections;  ///< Port-to-target memory mappings from system_map.xml

    template <typename T, typename = void>
    struct HasPhysAddr : std::false_type {};

    template <typename T>
    struct HasPhysAddr<T, std::void_t<decltype(std::declval<const T&>().getPhysAddr())>>
        : std::true_type {};

    template <typename T, typename = void>
    struct HasMemoryInfo : std::false_type {};

    template <typename T>
    struct HasMemoryInfo<T, std::void_t<decltype(std::declval<const T&>().getMemoryRangeType()),
                                       decltype(std::declval<const T&>().getHBMPort())>>
        : std::true_type {};

    void validateBufferMemoryType(const FunctionalArg& argMeta, MemoryRangeType memType,
                                  uint8_t hbmPort) const;

    template <typename T>
    void validateBufferArg(const FunctionalArg& argMeta, const T& arg, std::true_type) const {
        validateBufferMemoryType(argMeta, arg.getMemoryRangeType(), arg.getHBMPort());
    }

    template <typename T>
    void validateBufferArg(const FunctionalArg& /*argMeta*/, const T& /*arg*/,
                           std::false_type) const {}

    template <typename T>
    static uint64_t resolveKernelArgImpl(T&& arg, std::true_type) {
        return static_cast<uint64_t>(arg.getPhysAddr());
    }

    template <typename T>
    static decltype(auto) resolveKernelArgImpl(T&& arg, std::false_type) {
        return std::forward<T>(arg);
    }

    template <typename T>
    static decltype(auto) resolveKernelArg(T&& arg) {
        using ArgT = std::remove_reference_t<T>;
        return resolveKernelArgImpl(std::forward<T>(arg), HasPhysAddr<ArgT>{});
    }

    static uint32_t argWordCount(const FunctionalArg& arg) {
        const uint32_t bits = (arg.range == 0) ? 32u : arg.range;
        return std::max<uint32_t>(1u, (bits + 31u) / 32u);
    }

    static uint32_t argWordValue(uint64_t value, uint32_t wordIdx) {
        if (wordIdx == 0) return static_cast<uint32_t>(value & 0xFFFFFFFFULL);
        if (wordIdx == 1) return static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFULL);
        return 0u;
    }

    static std::string normalizeArgType(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    void ensureFunctionalArgsForCall(std::size_t providedArgCount, std::string_view opName) const {
        if (providedArgCount == 0) {
            return;
        }
        if (functionalArgs.empty()) {
            throwArgApiMisuse(
                "This kernel has no functional_args metadata in system_map.xml, "
                "so argument-based launch is unavailable.",
                opName);
        }
        if (providedArgCount > functionalArgs.size()) {
            throwArgApiMisuse(
                "Too many positional arguments were provided (" + std::to_string(providedArgCount) +
                    "); kernel metadata defines " + std::to_string(functionalArgs.size()) +
                    " functional argument(s).",
                opName);
        }
    }

    std::string buildArgApiUsageMessage(std::string_view reason, std::string_view opName) const;
    [[noreturn]] void throwArgApiMisuse(std::string_view reason, std::string_view opName) const;
    void ensureNoSetArgValuesWhenPassingArgs(std::size_t providedArgCount, std::string_view opName) const;
    void ensureSetArgValuesCompleteForLaunch(std::string_view opName) const;
    const FunctionalArg& functionalArgByIdx(uint32_t idx) const;
    uint32_t functionalArgIdxByName(std::string_view argName) const;
    void setArgResolved(uint32_t idx, uint64_t value);
    void writeArgToRegisterMap(const FunctionalArg& argMeta, uint64_t value);
    void writeArgToSimulation(const FunctionalArg& argMeta, uint64_t value);
    void writeArgToEmulation(Json::Value& command, const FunctionalArg& argMeta, uint64_t value) const;
    void applySetArgsToRegisterMap();
    void applySetArgsToSimulation();
    void applySetArgsToEmulation(Json::Value& command) const;
   public:
    /**
     * @brief Constructor for Kernel.
     * @param name The name of the kernel.
     * @param baseAddr The base address of the kernel.
     * @param range The address range of the kernel.
     * @param registers The list of registers in the kernel.
     * @param functionalArgs Parsed function-argument metadata from system_map.xml.
     */
    Kernel(const std::string& name, uint64_t baseAddr, uint64_t range,
           const std::vector<Register>& registers,
           const std::vector<FunctionalArg>& functionalArgs = {});

    /**
     * @brief Default constructor for Kernel.
     */
    Kernel() = default;

    /**
     * @brief Default copy constructor for Kernel.
     */
    Kernel(const Kernel&) = default;

    /**
     * @brief Default move constructor for Kernel.
     */
    Kernel(Kernel&&) = default;

    /**
     * @brief Constructor for Kernel using a Device object.
     * @param device The Device object.
     * @param kernelName The name of the kernel.
     */
    Kernel(vrt::Device device, const std::string& kernelName);

    /**
     * @brief Sets the vrtd BAR handle for hardware access.
     * @param bar The vrtd BAR handle.
     */
    void setVrtdBar(const std::optional<vrtd::Bar>& bar);

    /**
     * @brief Sets the ZeroMQ server for emulation and simulation.
     * @param server The ZeroMQ server handle.
     */
    void setServer(std::shared_ptr<ZmqServer> server);

    /**
     * @brief Writes a value to a register.
     * @param offset The offset of the register.
     * @param value The value to write.
     */
    void write(uint32_t offset, uint32_t value);

    /**
     * @brief Reads a value from a register.
     * @param offset The offset of the register.
     * @return The value read from the register.
     */
    uint32_t read(uint32_t offset);

    /**
     * @brief Waits for the kernel to complete.
     */
    void wait();

    /**
     * @brief Starts the kernel.
     * @param autorestart Flag indicating whether to enable autorestart.
     */
    void startKernel(bool autorestart = false);

    /**
     * @brief Sets the platform for the kernel.
     * @param platform The platform to set.
     */
    void setPlatform(Platform platform);

    /**
     * @brief Sets parsed function argument metadata.
     */
    void setFunctionalArgs(const std::vector<FunctionalArg>& args);

    /**
     * @brief Returns true when function argument metadata is available.
     */
    bool hasFunctionalArgs() const;

    /**
     * @brief Get information about the functional arguments.
     */
    const std::vector<FunctionalArg>& getFunctionalArgs() const;

    /**
     * @brief Sets EMU call argument kinds loaded from emu_manifest.json.
     *        Index corresponds to argN in EMU call JSON.
     */
    void setEmuCallArgKinds(const std::vector<std::string>& kinds);
    /**
     * @brief Sets EMU scalar fetch routing keyed by register offset.
     *        Used by Kernel::read() in EMULATION mode.
     */
    void setEmuFetchScalarArgByOffset(const std::map<uint32_t, std::string>& routes);

    /**
     * @brief Sets port-to-target memory connection mappings from system_map.xml.
     */
    void setConnections(const std::map<std::string, std::string>& conns);

    /**
     * @brief Returns the memory configuration for a named AXI port.
     *
     * Looks up the port in the kernel's connection map (populated from system_map.xml)
     * and returns a MemoryConfig that can be passed directly to the Buffer constructor.
     *
     * @param portName The AXI port name (e.g. "m_axi_gmem0").
     * @throws std::runtime_error if the port has no connection entry.
     */
    MemoryConfig portMemoryConfig(std::string_view portName) const;

    /**
     * @brief Returns the memory configuration for a named kernel argument.
     *
     * Resolves the argument to its AXI port via functional_args metadata and then
     * delegates to portMemoryConfig(). The returned MemoryConfig can be passed
     * directly to the Buffer constructor.
     *
     * @param argName The argument name from functional_args metadata.
     * @throws std::runtime_error if the argument is not found or has no AXI port.
     */
    MemoryConfig argMemoryConfig(std::string_view argName) const;

    /**
     * @brief Set argument value by argument index from functional_args metadata.
     */
    template <typename T>
    void setArg(int idx, T&& value) {
        if (idx < 0) {
            throwArgApiMisuse("setArg(index, value) received a negative argument index.",
                              "setArg");
        }
        using RawT = std::remove_cv_t<std::remove_reference_t<T>>;
        if constexpr (HasMemoryInfo<RawT>::value) {
            const FunctionalArg& argMeta = functionalArgByIdx(static_cast<uint32_t>(idx));
            validateBufferArg(argMeta, value, HasMemoryInfo<RawT>{});
        }
        decltype(auto) resolvedValue = resolveKernelArg(std::forward<T>(value));
        setArgResolved(static_cast<uint32_t>(idx), static_cast<uint64_t>(resolvedValue));
    }

    /**
     * @brief Set argument value by argument name from functional_args metadata.
     */
    template <typename T>
    void setArg(std::string_view argName, T&& value) {
        using RawT = std::remove_cv_t<std::remove_reference_t<T>>;
        if constexpr (HasMemoryInfo<RawT>::value) {
            uint32_t idx = functionalArgIdxByName(argName);
            const FunctionalArg& argMeta = functionalArgByIdx(idx);
            validateBufferArg(argMeta, value, HasMemoryInfo<RawT>{});
        }
        decltype(auto) resolvedValue = resolveKernelArg(std::forward<T>(value));
        setArgResolved(functionalArgIdxByName(argName), static_cast<uint64_t>(resolvedValue));
    }

    /**
     * @brief Writes batch register to PCIe BAR.
     */
    void writeBatch();

    /**
     * @brief Calls the kernel and waits for it to complete.
     * @param args The arguments to pass to the kernel.
     */
    template <typename... Args>
    void call(Args&&... args) {
        const std::size_t providedArgCount = sizeof...(Args);
        currentArgIndex = 0;
        registerMap.clear();
        ensureNoSetArgValuesWhenPassingArgs(providedArgCount, "call");
        ensureFunctionalArgsForCall(providedArgCount, "call");

        if (platform == Platform::HARDWARE) {
            if constexpr (sizeof...(Args) > 0) {
                (processArg(std::forward<Args>(args)), ...);
                this->writeBatch();
            } else if (!setArgValues.empty()) {
                ensureSetArgValuesCompleteForLaunch("call");
                applySetArgsToRegisterMap();
                this->writeBatch();
            } else if (!functionalArgs.empty()) {
                throwArgApiMisuse(
                    "call() was invoked without positional args and no setArg values were provided, "
                    "but this kernel has functional arguments.",
                    "call");
            }
            this->startKernel();
            this->wait();
        } else if (platform == Platform::EMULATION) {
            Json::Value command;
            command["command"] = "call";
            command["function"] = name;
            if constexpr (sizeof...(Args) > 0) {
                (processEmuArg(std::forward<Args>(args), command), ...);
            } else if (!setArgValues.empty()) {
                ensureSetArgValuesCompleteForLaunch("call");
                applySetArgsToEmulation(command);
            } else if (!functionalArgs.empty()) {
                throwArgApiMisuse(
                    "call() was invoked without positional args and no setArg values were provided, "
                    "but this kernel has functional arguments.",
                    "call");
            }
            server->sendCommand(command);
        } else if (platform == Platform::SIMULATION) {
            if constexpr (sizeof...(Args) > 0) {
                (processSimArg(std::forward<Args>(args)), ...);
            } else if (!setArgValues.empty()) {
                ensureSetArgValuesCompleteForLaunch("call");
                applySetArgsToSimulation();
            } else if (!functionalArgs.empty()) {
                throwArgApiMisuse(
                    "call() was invoked without positional args and no setArg values were provided, "
                    "but this kernel has functional arguments.",
                    "call");
            }
            this->startKernel();
            this->wait();
        }
    }

    /**
     * @brief Starts the kernel.
     */
    void start();

    /**
     * @brief Starts the kernel with arguments.
     * @param args The arguments to pass to the kernel.
     */
    template <typename... Args>
    void start(Args&&... args) {
        const std::size_t providedArgCount = sizeof...(Args);
        currentArgIndex = 0;
        registerMap.clear();
        ensureNoSetArgValuesWhenPassingArgs(providedArgCount, "start");
        ensureFunctionalArgsForCall(providedArgCount, "start");
        if (platform == Platform::HARDWARE) {
            if constexpr (sizeof...(Args) > 0) {
                (processArg(std::forward<Args>(args)), ...);
                this->writeBatch();
            } else if (!setArgValues.empty()) {
                ensureSetArgValuesCompleteForLaunch("start");
                applySetArgsToRegisterMap();
                this->writeBatch();
            } else if (!functionalArgs.empty()) {
                throwArgApiMisuse(
                    "start() was invoked without positional args and no setArg values were provided, "
                    "but this kernel has functional arguments.",
                    "start");
            }
            this->startKernel();

        } else if (platform == Platform::EMULATION) {
            Json::Value command;
            command["command"] = "start";
            command["function"] = name;
            if constexpr (sizeof...(Args) > 0) {
                (processEmuArg(std::forward<Args>(args), command), ...);
            } else if (!setArgValues.empty()) {
                ensureSetArgValuesCompleteForLaunch("start");
                applySetArgsToEmulation(command);
            } else if (!functionalArgs.empty()) {
                throwArgApiMisuse(
                    "start() was invoked without positional args and no setArg values were provided, "
                    "but this kernel has functional arguments.",
                    "start");
            }
            server->sendCommand(command);
        } else if (platform == Platform::SIMULATION) {
            if constexpr (sizeof...(Args) > 0) {
                (processSimArg(std::forward<Args>(args)), ...);
            } else if (!setArgValues.empty()) {
                ensureSetArgValuesCompleteForLaunch("start");
                applySetArgsToSimulation();
            } else if (!functionalArgs.empty()) {
                throwArgApiMisuse(
                    "start() was invoked without positional args and no setArg values were provided, "
                    "but this kernel has functional arguments.",
                    "start");
            }
            this->startKernel();
        }
    }
    /**
     * @brief Helper method which processes an argument.
     * @tparam T The type of the argument.
     * @param arg The argument to process.
     */
    template <typename T>
    void processArg(T&& arg) {
        if (currentArgIndex >= functionalArgs.size()) {
            throwArgApiMisuse(
                "Positional argument index " + std::to_string(currentArgIndex) +
                    " exceeds available functional_args entries.",
                "start/call");
        }

        const FunctionalArg& argMeta = functionalArgs.at(currentArgIndex);
        using RawT = std::remove_cv_t<std::remove_reference_t<T>>;
        validateBufferArg(argMeta, arg, HasMemoryInfo<RawT>{});
        decltype(auto) resolvedArg = resolveKernelArg(std::forward<T>(arg));
        const uint64_t value = static_cast<uint64_t>(resolvedArg);
        writeArgToRegisterMap(argMeta, value);
        currentArgIndex++;
    }

    /**
     * @brief Helper method which processes an argument for simulation.
     * @tparam T The type of the argument.
     * @param arg The argument to process.
     */
    template <typename T>
    void processSimArg(T&& arg) {
        if (currentArgIndex >= functionalArgs.size()) {
            throwArgApiMisuse(
                "Positional argument index " + std::to_string(currentArgIndex) +
                    " exceeds available functional_args entries.",
                "start/call");
        }

        const FunctionalArg& argMeta = functionalArgs.at(currentArgIndex);
        using RawT = std::remove_cv_t<std::remove_reference_t<T>>;
        validateBufferArg(argMeta, arg, HasMemoryInfo<RawT>{});
        decltype(auto) resolvedArg = resolveKernelArg(std::forward<T>(arg));
        const uint64_t value = static_cast<uint64_t>(resolvedArg);
        writeArgToSimulation(argMeta, value);
        currentArgIndex++;
    }

    /**
     * @brief Helper method which processes an argument for emulation.
     * @tparam T The type of the argument.
     * @param arg The argument to process.
     * @param command The JSON command to update.
     */
    template <typename T>
    void processEmuArg(T&& arg, Json::Value& command) {
        if (currentArgIndex >= functionalArgs.size()) {
            throwArgApiMisuse(
                "Positional argument index " + std::to_string(currentArgIndex) +
                    " exceeds available functional_args entries.",
                "start/call");
        }

        const FunctionalArg& argMeta = functionalArgs.at(currentArgIndex);
        using RawT = std::remove_cv_t<std::remove_reference_t<T>>;
        validateBufferArg(argMeta, arg, HasMemoryInfo<RawT>{});
        decltype(auto) resolvedArg = resolveKernelArg(std::forward<T>(arg));
        writeArgToEmulation(command, argMeta, static_cast<uint64_t>(resolvedArg));
        currentArgIndex++;
    }

    /**
     * @brief Getter for the kernel name.
     * @return The name of the kernel.
     */
    std::string getName() const;

    /**
     * @brief Getter for the kernel base physical address.
     * @return The physical base address of the kernel.
     */
    uint64_t getPhysAddr() const;

    /**
     * @brief Destructor for Kernel.
     */
    ~Kernel();

    /**
     * @brief Copy assignment operator.
     *
     * @param other The kernel to copy from.
     * @return Reference to this kernel.
     */
    Kernel& operator=(const Kernel& other) = default;

    /**
     * @brief Move assignment operator.
     *
     * @param other The kernel to move from.
     * @return Reference to this kernel.
     */
    Kernel& operator=(Kernel&& other) noexcept = default;
};

}  // namespace vrt

#endif  // VRT_KERNEL_HPP

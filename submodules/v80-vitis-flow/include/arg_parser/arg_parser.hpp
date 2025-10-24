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

#ifndef ARG_PARSER_HPP
#define ARG_PARSER_HPP

#include <algorithm>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "kernel.hpp"
#include "logger.hpp"
#include "xml_parser.hpp"

/**
 * @brief Structure representing the Tcl scripts to inject into the Vivado build scripts.
 */
struct TclInjections {
    std::vector<std::string> scriptsPreSynth; ///< The name of the scripts to inject before the synthesis phase
    std::vector<std::string> scriptsPostBuild; ///< The name of the scripts to inject after the build phase
};

/**
 * @brief Structure representing a connection element within a hardware design.
 */
struct ConnectionElement {
    std::string kernelName;     ///< The name of the kernel
    std::string interfaceName;  ///< The name of the interface on the kernel
};

/**
 * @brief Structure representing a connection between two elements in a hardware design.
 */
struct Connection {
    ConnectionElement src;  ///< The source connection element
    ConnectionElement dst;  ///< The destination connection element
};

/**
 * @brief Enum class representing the direction of a streaming interface.
 */
enum class StreamDirection {
    HOST_TO_DEVICE,  ///< Stream from host to device
    DEVICE_TO_HOST   ///< Stream from device to host
};

/**
 * @brief Structure representing a streaming connection in a hardware design.
 *
 * Streaming connections are used for high-throughput data transfer between
 * the host and device or between kernels.
 */
struct StreamingConnection {
    std::string kernelName;     ///< The name of the kernel
    uint32_t qid;               ///< Queue ID for the streaming connection
    std::string interfaceName;  ///< The name of the interface on the kernel
    StreamDirection direction;  ///< Direction of the stream
};

/**
 * @brief Enum class representing the platform type for execution.
 */
enum class Platform {
    EMULATOR,   ///< Software emulation platform
    SIMULATOR,  ///< Hardware simulation platform
    HARDWARE    ///< Actual hardware execution platform
};

/**
 * @brief Class for parsing command line arguments and configuration files.
 *
 * This class parses command line arguments and configuration files to extract
 * information about kernels, connections, and platform settings for hardware
 * acceleration designs.
 */
class ArgParser {
   public:
    /**
     * @brief Path to the XML configuration file.
     */
    static const std::string XML_PATH;

    /**
     * @brief Constructor for the ArgParser.
     * @param argc Number of command line arguments.
     * @param argv Array of command line arguments.
     */
    ArgParser(int argc, char** argv);

    /**
     * @brief Gets the list of kernels defined in the configuration.
     * @return Vector of kernel objects.
     */
    std::vector<Kernel> getKernels();

    /**
     * @brief Gets the path to the configuration file.
     * @return String containing the path to the configuration file.
     */
    std::string getConfigFile() const;

    /**
     * @brief Gets the mapping between kernel names and their corresponding entity names.
     * @return Map from kernel names to entity names.
     */
    std::map<std::string, std::string> getKernelEntities() const;

    /**
     * @brief Gets the list of connections defined in the configuration.
     * @return Vector of connection objects.
     */
    std::vector<Connection> getConnections() const;

    /**
     * @brief Gets the clock frequency in Hertz.
     * @return Clock frequency in Hertz.
     */
    uint64_t getFreqHz() const;

    /**
     * @brief Checks if the design is segmented.
     * @return True if the design is segmented, false otherwise.
     */
    bool isSegmented() const;

    /**
     * @brief Gets the target platform.
     * @return The target platform (Emulator, Simulator, or Hardware).
     */
    Platform getPlatform();

    /**
     * @brief Gets the list of kernel file paths.
     * @return Vector of paths to kernel files.
     */
    std::vector<std::string> getKernelPaths();

    /**
     * @brief Gets the set of Tcl files to inject.
     * @return Structure representing the set of Tcl files to inject.
     */
    const TclInjections &getTclInjections() const;

   private:
    std::vector<std::string> kernelPaths;  ///< Paths to kernel files
    std::string configFile;                ///< Path to the configuration file
    std::vector<Kernel> kernels;           ///< List of kernels
    std::map<std::string, std::string>
        kernelEntities;                   ///< Mapping from kernel names to entity names
    std::vector<Connection> connections;  ///< List of connections
    uint64_t freqHz;                      ///< Clock frequency in Hz
    bool segmented;                       ///< Flag indicating if the design is segmented
    Platform platform;                    ///< Target platform
    TclInjections tclInjections;          ///< Tcl files to inject                

    /**
     * @brief Parses kernel information from configuration files.
     * @return Vector of parsed kernel objects.
     */
    std::vector<Kernel> parseKernels();

    /**
     * @brief Parses the configuration file.
     */
    void parseConfig();
};

#endif  // ARG_PARSER_HPP
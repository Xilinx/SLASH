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

#ifndef VRT_VRTBIN_HPP
#define VRT_VRTBIN_HPP

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <vrt/parser/xml_parser.hpp>
#include <vrt/utils/logger.hpp>
#include <vrt/utils/platform.hpp>
#include <vrt/utils/filesystem_cache.hpp>

namespace vrt {

/**
 * @brief Class for handling VRTBIN operations.
 */
class Vrtbin {
    std::string vrtbinPath;                                         ///< Path to the VRTBIN tar file
    std::string systemMapPath;                                      ///< Path to the system map file
    std::string pdiPath;                                            ///< Path to the PDI file
    std::vector<std::string> pdiPaths;                              ///< Paths to all discovered PDI files
    std::string tempExtractPath;                                    ///< Temporary extraction path
    std::string emulationExecPath;                                  ///< Path to the emulation executable
    std::string emulationManifestPath;                              ///< Path to emu manifest (if present)
    std::string simulationExecPath;                                 ///< Path to the simulation executable
    std::string utilizationReportPath;                              ///< Path to utilization report (if present)
    Platform platform;                                              ///< Platform type
    /**
     * @brief Copies a file from source to destination.
     * @param source The source file path.
     * @param destination The destination file path.
     */
    void copy(const std::string& source, const std::string& destination);
    void discoverPdiFiles();
    std::filesystem::path findExtractedFile(const std::string& filename) const;
    std::filesystem::path findExtractedFileByPrefix(const std::string& prefix,
                                                    const std::string& extension) const;
    static std::string sanitizeForPath(const std::string& input);

   public:
    /**
     * @brief Constructor for Vrtbin.
     * @param vrtbinPath The path to the VRTBIN file.
     * @param bdf The Bus:Device.Function identifier.
     */
    Vrtbin(std::string vrtbinPath, const std::string& bdf);

    /**
     * @brief Extracts the VRTBIN file.
     */
    void extract();

    /**
     * @brief Gets the path to the system map file.
     * @return The path to the system map file.
     */
    std::string getSystemMapPath();

    /**
     * @brief Gets the path to the PDI file.
     * @return The path to the PDI file.
     */
    std::string getPdiPath();

    /**
     * @brief Gets the paths to all discovered PDI files.
     * @return A list of paths to PDI files.
     */
    std::vector<std::string> getPdiPaths();

    /**
     * @brief Gets the emulation executable file.
     * @return The path to the emulation executable file.
     */
    std::string getEmulationExec();

    /**
     * @brief Gets the emulation manifest file (if present in EMU vrtbin).
     * @return The path to the emulation manifest file, or empty string if absent.
     */
    std::string getEmulationManifest();

    /**
     * @brief Gets the simulation executable file.
     * @return The path to the simulation executable file.
     */
    std::string getSimulationExec();

    /**
     * @brief Gets the path to the utilization report (if present in the vbin).
     * @return The path to the utilization report, or empty string if absent.
     */
    std::string getUtilizationReportPath() const;

    /**
     * @brief Gets the platform type parsed from the system map.
     * @return The platform type.
     */
    Platform getPlatform() const;

    /**
     * @brief Gets the path to the system map last loaded on a bdf.
     * @param bdf The bdf to query.
     * @return The path to the system map.
     */
    static std::string getSystemMapPathFromBdf(const std::string& bdf);

    /**
     * @brief Gets the path to the utilization report last loaded on a bdf.
     * @param bdf The bdf to query.
     * @return The path to the utilization report.
     */
    static std::string getUtilizationReportPathFromBdf(const std::string& bdf);
};

}  // namespace vrt

#endif  // VRT_VRTBIN_HPP

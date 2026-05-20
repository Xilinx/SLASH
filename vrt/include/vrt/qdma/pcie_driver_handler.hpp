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

#ifndef VRT_PCIE_DRIVER_HANDLER_HPP
#define VRT_PCIE_DRIVER_HANDLER_HPP

#include <fcntl.h>
#include <unistd.h>

#include <stdexcept>
#include <string>

#include <vrt/utils/logger.hpp>

namespace vrt {
/**
 * @brief Class for handling PCIe driver commands.
 */
class PcieDriverHandler {
   public:
    /**
     * @brief Enum for PCIe driver commands.
     */
    enum class Command {
        REMOVE,      ///< Remove command
        TOGGLE_SBR,  ///< Toggle Secondary Bus Reset command
        RESCAN,      ///< Rescan command
        HOTPLUG      ///< Hotplug command
    };

    /**
     * @brief Constructor for PcieDriverHandler.
     * @param bdf The BDF of the PCIe device.
     */
    PcieDriverHandler(const std::string& bdf);

    /**
     * @brief Sends a command to the PCIe driver.
     * @param cmd The command to send.
     */
    void sendCommand(Command cmd);

    /**
     * @brief Executes a PCIe driver command.
     * @param cmd The command to execute.
     */
    void execute(Command cmd);

   private:
    /**
     * @brief Helper method to convert enum to string.
     * @param cmd The command to convert.
     * @return The string representation of the command.
     */
    std::string commandToString(Command cmd);
    std::string bdf;                                        ///< The BDF of the PCIe device.
    std::string driverPath;                                 ///< The path to the PCIe driver.
    std::string pcieHotplugRootPath = "/dev/pcie_hotplug";  ///< The root path for PCIe hotplug.
};

}  // namespace vrt

#endif  // VRT_PCIE_DRIVER_HANDLER_HPP
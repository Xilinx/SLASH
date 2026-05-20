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

#include <vrt/qdma/pcie_driver_handler.hpp>

namespace vrt {

PcieDriverHandler::PcieDriverHandler(const std::string& bdf) {
    this->bdf = bdf;
    driverPath = pcieHotplugRootPath + "_0000:" + bdf;
}

void PcieDriverHandler::execute(Command cmd) {
    utils::Logger::log(utils::LogLevel::DEBUG, __PRETTY_FUNCTION__,
                       "Executing command: {} for PCIe device {}", commandToString(cmd), bdf);
    std::string cmdStr = commandToString(cmd);
    int fd = open(driverPath.c_str(), O_WRONLY);
    if (fd < 0) {
        throw std::runtime_error("Could not open device");
    }

    if (write(fd, cmdStr.c_str(), cmdStr.size()) < 0) {
        close(fd);
        throw std::runtime_error("Could not write to device " + driverPath);
    }
    close(fd);
}

std::string PcieDriverHandler::commandToString(Command cmd) {
    switch (cmd) {
        case Command::REMOVE:
            return "remove";
        case Command::TOGGLE_SBR:
            return "toggle_sbr";
        case Command::RESCAN:
            return "rescan";
        case Command::HOTPLUG:
            return "hotplug";
        default:
            throw std::invalid_argument("Invalid command");
    }
}
}  // namespace vrt
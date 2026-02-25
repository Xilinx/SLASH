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

#include "api/device.hpp"

#include "utils/filesystem_cache.hpp"

namespace vrt {

Device::Device(const std::string& bdf, const std::string& vrtbinPath, bool program,
               ProgramType programType)
    : vrtbin(vrtbinPath, bdf), clkWiz(nullptr, "", 0, 0, 0), pcieHandler(bdf) {
    lockPcieDevice(bdf);
    this->bdf = bdf;
    this->allocator = new Allocator(4096);
    this->systemMap = this->vrtbin.getSystemMapPath();
    this->pdiPath = this->vrtbin.getPdiPath();
    this->programType = programType;
    this->qdmaIntf = QdmaIntf(bdf);
    this->zmqServer = std::make_shared<ZmqServer>();
    findPlatform();
    if (platform == Platform::HARDWARE) {
        createAmiDev();
        findVrtbinType();
        if (program) {
            programDevice();
        }
        parseSystemMap();
        this->clkWiz.setRateHz(200000000, false);
    } else if (platform == Platform::EMULATION) {
        parseSystemMap();
        std::string emulationExecPath = this->vrtbin.getEmulationExec() + " >/dev/null";

        std::thread([emulationExecPath]() { std::system(emulationExecPath.c_str()); }).detach();

    } else {
        parseSystemMap();
        std::string simulationExecPath = this->vrtbin.getSimulationExec() + " >/dev/null";

        std::thread([simulationExecPath]() { std::system(simulationExecPath.c_str()); }).detach();
        Json::Value command;
        command["command"] = "start";
        zmqServer->sendCommand(command);
    }
    for (auto& qdmaCon : qdmaConnections) {
        qdmaIntfs.emplace_back(new QdmaIntf(bdf, qdmaCon.getQid()));
    }
}

Device::~Device() {
    unlockPcieDevice(bdf);
}

void Device::parseSystemMap() {
    XMLParser parser(systemMap);
    parser.parseXML();
    clockFreq = parser.getClockFrequency();
    this->platform = parser.getPlatform();
    this->clkWiz = ClkWiz(dev, "clk_wiz", CLK_WIZ_BASE, CLK_WIZ_OFFSET, clockFreq);
    this->clkWiz.setPlatform(platform);
    kernels = parser.getKernels();
    for (auto& kernel : kernels) {
        kernel.second.setDevice(dev);
    }
    this->qdmaConnections = parser.getQdmaConnections();
}

Kernel Device::getKernel(const std::string& name) { return kernels[name]; }

void Device::cleanup() {
    if (platform == Platform::HARDWARE) {
        for (auto qdmaIntf_ : qdmaIntfs) {
            delete qdmaIntf_;
        }
        ami_dev_delete(&dev);
        unlockPcieDevice(bdf);
    } else if (platform == Platform::EMULATION || platform == Platform::SIMULATION) {
        Json::Value exit;
        exit["command"] = "exit";
        zmqServer->sendCommand(exit);
    }
}

std::string Device::getBdf() { return bdf; }

void Device::programDevice() {
    if (vrtbinType == VrtbinType::FLAT) {
        if (programType == ProgramType::FLASH) {
            char current_uuid[33];
            std::string logic_uuid = vrtbin.getUUID();
            int found_current_uuid = AMI_STATUS_ERROR;
            found_current_uuid = ami_dev_read_uuid(dev, current_uuid);
            if (found_current_uuid == AMI_STATUS_OK) {
                std::string current_uuid_str(current_uuid);
                current_uuid_str = current_uuid_str.substr(0, 32);
                utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__, "Current UUID: {}",
                                   current_uuid_str);
                utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__, "New UUID: {}",
                                   logic_uuid);
                if (current_uuid_str == logic_uuid) {
                    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                                       "Device already programmed with the same image");
                    bootDevice();
                    return;
                }
            }
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "Programming device {} in FLASH mode...This might take a while",
                               bdf);
            if (ami_prog_download_pdi(dev, pdiPath.c_str(), 0, 1, nullptr, false) !=
                AMI_STATUS_OK) {
                throw std::runtime_error("Failed to program device");
            }
            bootDevice();
        } else {
            int found_current_uuid = AMI_STATUS_ERROR;
            char current_uuid[33];
            std::string logic_uuid = vrtbin.getUUID();
            found_current_uuid = ami_dev_read_uuid(dev, current_uuid);
            if (found_current_uuid == AMI_STATUS_OK) {
                std::string current_uuid_str(current_uuid);
                current_uuid_str = current_uuid_str.substr(0, 32);
                utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__, "Current UUID: {}",
                                   current_uuid_str);
                utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__, "New UUID: {}",
                                   logic_uuid);
                if (current_uuid_str == logic_uuid) {
                    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                                       "Device already programmed with the same image");
                    bootDevice();
                    return;
                }
            }
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "Programming device {} in JTAG mode...This might take a while", bdf);
            std::string cmd = JTAG_PROGRAM_PATH + pdiPath;
            system(cmd.c_str());
            bootDevice();
        }
    } else if (vrtbinType == VrtbinType::SEGMENTED) {
        utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                           "Programming device {} in SEGMENTED mode...This might take a while",
                           bdf);
        char current_uuid[33];
        std::string logic_uuid = vrtbin.getUUID();
        int found_current_uuid = AMI_STATUS_ERROR;
        found_current_uuid = ami_dev_read_uuid(dev, current_uuid);
        if (found_current_uuid == AMI_STATUS_OK) {
            std::string current_uuid_str(current_uuid);
            current_uuid_str = current_uuid_str.substr(0, 32);
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__, "Current UUID: {}",
                               current_uuid_str);
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__, "New UUID: {}",
                               logic_uuid);
            if (current_uuid_str == logic_uuid) {
                utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                                   "Device already programmed with the same image");
                utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                                   "Refreshing qdma handle");
                pcieHandler.execute(PcieDriverHandler::Command::HOTPLUG);
                XMLParser parser(systemMap);
                parser.parseXML();
                auto qdmaConns = parser.getQdmaConnections();
                std::string cmd = "sudo bash " + std::string(QDMA_SETUP_QUEUES) + bdf + " --mm 0 bi";
                for (auto& qdmaConn : qdmaConns) {
                    uint32_t qid = qdmaConn.getQid();
                    std::string direction =
                        (qdmaConn.getDirection() == StreamDirection::HOST_TO_DEVICE ? "h2c"
                                                                                    : "c2h");
                    cmd += " --st " + std::to_string(qid) + " --dir " + direction;
                }
                system(cmd.c_str());
                return;
            }
        }
        bootDevice();
    }
}

void Device::bootDevice() {
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__, "Booting device...");
    if (vrtbinType == VrtbinType::FLAT) {
        if (programType == ProgramType::FLASH) {
            int ret = ami_prog_device_boot(&dev, 1);
            if (ret != AMI_STATUS_OK && geteuid() == 0) {  // for root users this should not matter
                throw std::runtime_error("Failed to boot device");
            } else {
                utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                                   "Booting into PDI...");
                utils::Logger::log(utils::LogLevel::DEBUG, __PRETTY_FUNCTION__,
                                   "Writing PMC GPIO...");
                ami_mem_bar_write(dev, 0, 0x1040000, 1);
                destroyAmiDev();
                pcieHandler.execute(PcieDriverHandler::Command::REMOVE);
                pcieHandler.execute(PcieDriverHandler::Command::TOGGLE_SBR);
                usleep(5000000);
                pcieHandler.execute(PcieDriverHandler::Command::RESCAN);
                pcieHandler.execute(PcieDriverHandler::Command::HOTPLUG);
                createAmiDev();
                utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                                   "New PDI booted successfully");
                XMLParser parser(systemMap);
                parser.parseXML();
                auto qdmaConns = parser.getQdmaConnections();
                std::string cmd = "sudo bash " + std::string(QDMA_SETUP_QUEUES) + bdf + " --mm 0 bi";
                for (auto& qdmaConn : qdmaConns) {
                    uint32_t qid = qdmaConn.getQid();
                    std::string direction =
                        (qdmaConn.getDirection() == StreamDirection::HOST_TO_DEVICE ? "h2c"
                                                                                    : "c2h");
                    cmd += " --st " + std::to_string(qid) + " --dir " + direction;
                }
                system(cmd.c_str());
                utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                                   "QDMA queues setup successfully");
            }
        } else {
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__, "Booting into PDI...");
            destroyAmiDev();
            pcieHandler.execute(PcieDriverHandler::Command::REMOVE);
            pcieHandler.execute(PcieDriverHandler::Command::RESCAN);
            pcieHandler.execute(PcieDriverHandler::Command::HOTPLUG);
            createAmiDev();
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "New PDI booted successfully");
            XMLParser parser(systemMap);
            parser.parseXML();
            auto qdmaConns = parser.getQdmaConnections();
            std::string cmd = "sudo bash " + std::string(QDMA_SETUP_QUEUES) + bdf + " --mm 0 bi";
            for (auto& qdmaConn : qdmaConns) {
                uint32_t qid = qdmaConn.getQid();
                std::string direction =
                    (qdmaConn.getDirection() == StreamDirection::HOST_TO_DEVICE ? "h2c" : "c2h");
                cmd += " --st " + std::to_string(qid) + " --dir " + direction;
            }
            system(cmd.c_str());
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "QDMA queues setup successfully");
        }
    } else if (vrtbinType == VrtbinType::SEGMENTED) {
        int ret = ami_prog_device_boot(
            &dev, 1);  // make sure we are on partition one, this contains the segmented base pdi
        if (ret != AMI_STATUS_OK && geteuid() == 0) {
            throw std::runtime_error("Failed to boot into base segmented PDI");
        } else {
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "Booting into base segmented PDI...");
            utils::Logger::log(utils::LogLevel::DEBUG, __PRETTY_FUNCTION__, "Writing PMC GPIO...");
            // PMC GPIO. this is needed for reset PDI into partition 1
            ami_mem_bar_write(dev, 0, 0x1040000, 1);
            destroyAmiDev();
            pcieHandler.execute(PcieDriverHandler::Command::REMOVE);
            pcieHandler.execute(PcieDriverHandler::Command::TOGGLE_SBR);
            usleep(5000000);
            pcieHandler.execute(PcieDriverHandler::Command::RESCAN);
            pcieHandler.execute(PcieDriverHandler::Command::HOTPLUG);
            createAmiDev();
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "Base segmented PDI booted successfully");
            if (ami_prog_download_pdi(dev, pdiPath.c_str(), 0, 1, nullptr, true) != AMI_STATUS_OK) {
                throw std::runtime_error("Failed to program partial device");
            }
            destroyAmiDev();
            pcieHandler.execute(PcieDriverHandler::Command::REMOVE);
            usleep(2 * DELAY_PARTIAL_BOOT);  // enough time for the device to reset
            pcieHandler.execute(PcieDriverHandler::Command::RESCAN);
            pcieHandler.execute(PcieDriverHandler::Command::HOTPLUG);
            createAmiDev();
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "PLD PDI booted successfully");
            XMLParser parser(systemMap);
            parser.parseXML();
            auto qdmaConns = parser.getQdmaConnections();
            std::string cmd = "sudo bash " + std::string(QDMA_SETUP_QUEUES) + bdf + " --mm 0 bi";
            for (auto& qdmaConn : qdmaConns) {
                uint32_t qid = qdmaConn.getQid();
                std::string direction =
                    (qdmaConn.getDirection() == StreamDirection::HOST_TO_DEVICE ? "h2c" : "c2h");
                cmd += " --st " + std::to_string(qid) + " --dir " + direction;
            }
            system(cmd.c_str());
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "QDMA queues setup successfully");
        }
    }
}

void Device::getNewHandle() {
    ami_device* new_dev = NULL;
    int ret = AMI_STATUS_ERROR;
    ret = ami_dev_find_next(&new_dev, AMI_PCI_BUS(pci_bdf), AMI_PCI_DEV(pci_bdf),
                            AMI_PCI_FUNC(pci_bdf), NULL);
    if (ret == AMI_STATUS_OK) {
        if (ami_sensor_discover(new_dev) == AMI_STATUS_OK) {
            dev = new_dev;
        } else {
            throw std::runtime_error("Failed to discover sensors");
        }
    } else {
        throw std::runtime_error("Failed to find device");
    }
}

void Device::createAmiDev() {
    if (ami_dev_find(bdf.c_str(), &dev) != AMI_STATUS_OK) {
        throw std::runtime_error("Failed to find device " + bdf);
    }
    ami_dev_get_pci_bdf(dev, &pci_bdf);
    if (ami_dev_request_access(dev) != AMI_STATUS_OK) {
        throw std::runtime_error("Failed to request elevated access to device");
    }
}

void Device::destroyAmiDev() { ami_dev_delete(&dev); }

void Device::setFrequency(uint64_t freq) {
    if (platform == Platform::HARDWARE) {
        if (freq > clockFreq) {
            utils::Logger::log(utils::LogLevel::WARN, __PRETTY_FUNCTION__,
                               "Setting frequency {}, which is higher than max frequency {}", freq,
                               clockFreq);
        }
        clkWiz.setRateHz(freq);
    }
}

uint64_t Device::getFrequency() {
    if (platform == Platform::HARDWARE) {
        return clkWiz.getClockRate();
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

ami_device* Device::getAmiDev() { return dev; }

void Device::findVrtbinType() {
    XMLParser parser(systemMap);
    parser.parseXML();
    this->vrtbinType = parser.getVrtbinType();
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

std::vector<QdmaIntf*> Device::getQdmaInterfaces() { return qdmaIntfs; }

void Device::lockPcieDevice(const std::string& bdf) {
    std::string lockFile = FilesystemCache::getRuntimePath() / ("pcie_device_" + bdf + ".lock");
    int fd = open(lockFile.c_str(), O_CREAT | O_WRONLY, 0666);
    if (fd == -1) {
        throw std::runtime_error("Failed to lock PCIe device " + bdf);
    }
    int ret = flock(fd, LOCK_EX | LOCK_NB);
    if (ret < 0) {
        close(fd);
        throw std::runtime_error("Device " + bdf + " locked by another instance");
    }
}

void Device::unlockPcieDevice(const std::string& bdf) {
    std::string lockFile = FilesystemCache::getRuntimePath() / ("pcie_device_" + bdf + ".lock");
    int fd = open(lockFile.c_str(), O_WRONLY, 0666);
    if (fd == -1) {
        throw std::runtime_error("Failed to lock PCIe device " + bdf);
    }
    int ret = flock(fd, LOCK_UN);
    if (ret < 0) {
        throw std::runtime_error("Device " + bdf + " cannot be unlocked");
    }
    close(fd);
}

}  // namespace vrt

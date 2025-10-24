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

#include "bd_builder.hpp"

#include "arg_parser.hpp"
#include "system_map.hpp"
#include <sstream>

bool BdBuilder::hasAximmIntf = false;

BdBuilder::BdBuilder(std::vector<Kernel> kernels, std::vector<Connection> connections)
    : systemMap(false, Platform::HARDWARE) {
    this->kernels = kernels;
    this->streamConnections = connections;
}

BdBuilder::BdBuilder(std::vector<Kernel> kernels, std::vector<Connection> connections,
                     double targetClockFreq, bool segmented, Platform platform,
                     TclInjections tclInjections)
    : systemMap(segmented, platform) {
    this->kernels = kernels;
    this->streamConnections = connections;
    this->targetClockFreq = targetClockFreq;
    this->segmented = segmented;
    this->platform = platform;
    this->tclInjections = std::move(tclInjections);

    systemMap.setClockFreq(targetClockFreq);
    StreamingConnection qdmaStreamConnection;
    for (auto sc = streamConnections.begin(); sc != streamConnections.end(); sc++) {
        if (sc->src.kernelName == "cips") {
            qdmaStreamConnection.interfaceName = sc->dst.interfaceName;
            std::regex re("qdma_(\\d)");
            std::smatch match;
            if (std::regex_search(sc->src.interfaceName, match, re) && match.size() > 1) {
                qdmaStreamConnection.qid = std::stoi(match.str(1));
            } else {
                throw std::runtime_error("Invalid QDMA interface name: " + sc->src.interfaceName);
            }
            qdmaStreamConnection.kernelName = sc->dst.kernelName;
            qdmaStreamConnection.direction = StreamDirection::HOST_TO_DEVICE;
            // streamConnections.erase(sc--);
            systemMap.addStreamConnection(qdmaStreamConnection);

        } else if (sc->dst.kernelName == "cips") {
            qdmaStreamConnection.interfaceName = sc->src.interfaceName;
            std::regex re("qdma_(\\d)");
            std::smatch match;
            if (std::regex_search(sc->dst.interfaceName, match, re) && match.size() > 1) {
                qdmaStreamConnection.qid = std::stoi(match.str(1));
            } else {
                throw std::runtime_error("Invalid QDMA interface name: " + sc->src.interfaceName);
            }
            qdmaStreamConnection.kernelName = sc->src.kernelName;
            qdmaStreamConnection.direction = StreamDirection::DEVICE_TO_HOST;
            // streamConnections.erase(sc--);
            systemMap.addStreamConnection(qdmaStreamConnection);
        }
    }
}

void BdBuilder::buildBlockDesign() {
    std::ifstream inputBlockDesignFile;
    if (platform == Platform::SIMULATOR) {
        inputBlockDesignFile.open(INPUT_FILE_SIM);
    }
    std::ofstream blockDesignFile;
    std::ofstream postBuildScriptFile;
    if (platform == Platform::EMULATOR) {
        blockDesignFile.open("/dev/null");
        postBuildScriptFile.open("/dev/null");
    } else {
        blockDesignFile.open(PRE_OUTPUT_FILE);
        postBuildScriptFile.open(POST_OUTPUT_FILE);
    }

    if (platform == Platform::HARDWARE) {
        std::string line;
        blockDesignFile << addRunPreHeader();
        blockDesignFile << setupQdmaStreaming();
        blockDesignFile << addQdmaLogic();
        blockDesignFile << setupClkWiz();
        blockDesignFile << setupSysRst();
        blockDesignFile << configNumberOfAXILiteSlaves();  // done

        blockDesignFile << configureUserClock();

        blockDesignFile << connectClkWiz();
        blockDesignFile << connectQdmaLogic();
        // blockDesignFile << connectQdmaToRouter();

        // do this for each kernel to be added
        int axilite_idx = 0, axifull_idx = 0;
        for (int i = 0; i < kernels.size(); i++) {
            auto kernel = kernels.at(i);
            blockDesignFile << createIp(i);
        }
        for (int i = 0; i < kernels.size(); i++) {
            auto kernel = kernels.at(i);
            for (auto& el : kernel.getInterfaces()) {
                if (el.getInterfaceType() == "axi4lite") {
                    blockDesignFile << connectInterface(kernel.getName(), el, axilite_idx++);
                } else if (el.getInterfaceType() == "clock" || el.getInterfaceType() == "reset") {
                    blockDesignFile << connectInterface(kernel.getName(), el, axilite_idx - 1);
                } else if (el.getInterfaceType() == "axi4stream") {
                    blockDesignFile << connectAxis(kernel.getName());
                }
            }
        }

        blockDesignFile << configNumberOfAXIFullSlaves();

        // Do QoS for AXI NoC
        uint16_t bw = calculateBw();
        blockDesignFile << setHBMConfig();
        blockDesignFile << genQoS(0, bw);
        blockDesignFile << genQoS(1, bw);
        blockDesignFile << genQoS(2, bw);
        blockDesignFile << genQoS(3, bw);
        if (segmented) {
            blockDesignFile << genQoS(4, bw);
        } else {
            int axiMmIntfIdx = 0;
            for (auto& kernel : kernels) {
                for (auto& intf : kernel.getInterfaces()) {
                    if (intf.getInterfaceType() == "axi4full") {
                        blockDesignFile << genQoS(axiMmIntfIdx + 4, bw);
                        axiMmIntfIdx++;
                    }
                }
            }
        }
        // utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__, "Segmented config: {}",
        // segmented);
        if (segmented) {
            int countAxi4FullInterfaces = 0;
            for (auto& kernel : kernels) {
                for (auto& intf : kernel.getInterfaces()) {
                    if (intf.getInterfaceType() == "axi4full") {
                        countAxi4FullInterfaces++;
                    }
                }
            }
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "Adding xbar for axi4full interfaces: {}", countAxi4FullInterfaces);
            blockDesignFile << addXbar(countAxi4FullInterfaces);
            blockDesignFile << connectXbarToNoC();
        }
        for (int i = 0; i < kernels.size(); i++) {
            Interface axi_intf;  // TODO: make this work for any number of interfaces....
            for (auto& el : kernels[i].getInterfaces()) {
                if (el.getInterfaceType() == "axi4full") {
                    blockDesignFile << connectInterface(kernels.at(i).getName(), el, axifull_idx++);
                }
            }
            for (auto& el : kernels[i].getInterfaces()) {
                if (el.getInterfaceType() == "axi4lite") {
                    uint64_t offset = std::pow(2, el.getAddrWidth());
                    MapEntry entry(kernels.at(i).getName(), BASE_ADDRESS + i * offset, offset);
                    entry.setRegisters(kernels.at(i).getRegisters());
                    systemMap.addEntry(entry);
                    blockDesignFile << assignSlaveAddress(kernels.at(i).getName(), i, el,
                                                          BASE_ADDRESS + i * offset);
                }
            }
            blockDesignFile << "\n";
        }
        blockDesignFile << assignClkWizAddr() << std::endl;
        // blockDesignFile << assignQdmaLogicGpioAddr() << std::endl;
        blockDesignFile << "assign_bd_address" << std::endl;

        if (segmented) {
            blockDesignFile << "set_property segmented_configuration true [current_project]\n";
            try {
                blockDesignFile << setSegmented() << std::endl;
            } catch (...){
                utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__, "Segmented not set");
            }
        }



        for (const auto &script : tclInjections.scriptsPreSynth) {
            blockDesignFile << generateSourceInstruction(script);
        }

        blockDesignFile << printFooter();
        blockDesignFile.close();

        // Inline and dirty.
        postBuildScriptFile << "proc run_post {} {\n"
            << "\topen_run impl_1\n"
            << "\treport_utilization -hierarchical -hierarchical_depth 3 -hierarchical_percentages -file build/report_utilization.txt\n"
            << "\treport_timing_summary -delay_type min_max -check_timing_verbose -max_paths 1 -input_pins -routable_nets -name timing_1 -file build/report_timing.txt\n\n";


        for (const auto &script : tclInjections.scriptsPostBuild) {
            postBuildScriptFile << generateSourceInstruction(script);
        }

        postBuildScriptFile << "}\n"
            << "run_post\n";


        systemMap.printToFile();
    } else if (platform == Platform::SIMULATOR) {
        std::string line;
        while (std::getline(inputBlockDesignFile, line)) {
            blockDesignFile << line << std::endl;
        }
        for (int i = 0; i < kernels.size(); i++) {
            auto kernel = kernels.at(i);
            blockDesignFile << createIpSim(i);
        }
        blockDesignFile << configNumberOfAXIFullSlavesSim();
        blockDesignFile << configNumberOfAXILiteSlavesSim();
        int axifull_idx = 0;
        int axilite_idx = 0;
        for (int i = 0; i < kernels.size(); i++) {
            Interface axi_intf;

            for (auto& el : kernels[i].getInterfaces()) {
                if (el.getInterfaceType() == "axi4full") {
                    blockDesignFile
                        << connectInterfaceSim(kernels.at(i).getName(), el, axifull_idx++);
                } else if (el.getInterfaceType() == "axi4lite") {
                    blockDesignFile
                        << connectInterfaceSim(kernels.at(i).getName(), el, axilite_idx++);
                } else if (el.getInterfaceType() == "axi4stream") {
                    blockDesignFile << connectAxisSim(kernels.at(i).getName());
                } else {
                    blockDesignFile << connectInterfaceSim(kernels.at(i).getName(), el, 0);
                }
            }
            for (auto& el : kernels[i].getInterfaces()) {
                if (el.getInterfaceType() == "axi4lite") {
                    uint64_t offset = std::pow(2, el.getAddrWidth());
                    MapEntry entry(kernels.at(i).getName(), BASE_ADDRESS + i * offset, offset);
                    entry.setRegisters(kernels.at(i).getRegisters());
                    systemMap.addEntry(entry);
                    blockDesignFile << assignSlaveAddressSim(kernels.at(i).getName(), i, el,
                                                             BASE_ADDRESS + i * offset);
                }
            }
            blockDesignFile << "\n";
        }
        // blockDesignFile << "connect_bd_intf_net [get_bd_intf_ports mem] [get_bd_intf_pins
        // mem_sc/M00_AXI]\n";
        blockDesignFile << "assign_bd_address -offset 0x4000000000 -range 128M [get_bd_addr_segs "
                           "/bram_ctrl/S_AXI/Mem0] -force\n";
        blockDesignFile << "save_bd_design\n";
        blockDesignFile << "validate_bd_design\n";
        blockDesignFile
            << "add_files -norecurse [make_wrapper -files [get_files \"${bd_name}.bd\"] -top]\n";
        blockDesignFile << "update_compile_order -fileset sources_1\n";
        blockDesignFile << "update_compile_order -fileset sim_1\n";
        blockDesignFile << "set_property -name {xsim.elaborate.xelab.more_options} -value {-dll} "
                           "-objects [get_filesets sim_1]\n";
        blockDesignFile << "set_property generate_scripts_only 1 [current_fileset -simset]\n";
        blockDesignFile << "launch_simulation -scripts_only\n";
        blockDesignFile << "close_project\n";
        blockDesignFile << "exit\n";

        blockDesignFile.close();
        systemMap.printToFile();
    } else if (platform == Platform::EMULATOR) {
        for (int i = 0; i < kernels.size(); i++) {
            for (auto& el : kernels[i].getInterfaces()) {
                if (el.getInterfaceType() == "axi4lite") {
                    uint64_t offset = std::pow(2, el.getAddrWidth());
                    MapEntry entry(kernels.at(i).getName(), BASE_ADDRESS + i * offset, offset);
                    entry.setRegisters(kernels.at(i).getRegisters());
                    systemMap.addEntry(entry);
                    // blockDesignFile << assignSlaveAddressSim(kernels.at(i).getName(), i, el,
                    // BASE_ADDRESS + i * offset);
                }
            }
        }

        systemMap.printToFile();
    }
}

// TODO Change it to accept input from config file. Add axi4full support. Add axistream support
std::string BdBuilder::connectInterface(std::string krnl_name, Interface intf, int idx) {
    if (intf.getInterfaceType() == "axi4lite") {
        if ((idx + 5) < 10) {
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "Connecting axi4lite interface: {} to axi xbar M0{} for kernel {}",
                               intf.getInterfaceName(), std::to_string(idx + 5), krnl_name);
            return "connect_bd_intf_net -intf_net pcie_slr0_mgmt_sc_M0" + std::to_string(idx + 5) +
                   "_AXI " + "[get_bd_intf_pins base_logic/pcie_slr0_mgmt_sc/M0" +
                   std::to_string(idx + 5) + "_AXI] [get_bd_intf_pins base_logic/" + krnl_name +
                   "/" + intf.getInterfaceName() + "]\n";
        } else {
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "Connecting axi4lite interface: {} to axi xbar M{} for kernel {}",
                               intf.getInterfaceName(), std::to_string(idx + 5), krnl_name);
            return "connect_bd_intf_net -intf_net pcie_slr0_mgmt_sc_M" + std::to_string(idx + 5) +
                   "_AXI " + "[get_bd_intf_pins base_logic/pcie_slr0_mgmt_sc/M" +
                   std::to_string(idx + 5) + "_AXI] [get_bd_intf_pins base_logic/" + krnl_name +
                   "/" + intf.getInterfaceName() + "]\n";
        }
    } else if (intf.getInterfaceType() == "clock") {
        utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                           "Connecting clock interface: {} to clk_wiz for kernel {}",
                           intf.getInterfaceName(), krnl_name);
        return "connect_bd_net -net clk_out1 [get_bd_pins base_logic/clk_wiz/clk_out1] "
               "[get_bd_pins base_logic/" +
               krnl_name + "/" + intf.getInterfaceName() + "]\n";
    } else if (intf.getInterfaceType() == "reset") {
        utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                           "Connecting reset interface: {} to sys_rst for kernel {}",
                           intf.getInterfaceName(), krnl_name);
        return "connect_bd_net -net sys_rst_peripheral_aresetn [get_bd_pins "
               "base_logic/sys_rst/peripheral_aresetn] [get_bd_pins base_logic/" +
               krnl_name + "/" + intf.getInterfaceName() + "]\n";
    } else if (intf.getInterfaceType() == "axi4full") {
        if (segmented) {
            if ((idx) < 10) {
                return "connect_bd_intf_net [get_bd_intf_pins base_logic/" + krnl_name + "/" +
                       intf.getInterfaceName() + "] [get_bd_intf_pins base_logic/noc_xbar/S0" +
                       std::to_string(idx) + "_AXI]\n";
            } else {
                return "connect_bd_intf_net [get_bd_intf_pins base_logic/" + krnl_name + "/" +
                       intf.getInterfaceName() + "] [get_bd_intf_pins base_logic/noc_xbar/S" +
                       std::to_string(idx) + "_AXI]\n";
            }
        } else {
            if ((idx + 4) < 10) {
                utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                                   "Connecting axi4full interface: {} to NoC S0{} for kernel {}",
                                   intf.getInterfaceName(), std::to_string(idx + 4), krnl_name);
                return "connect_bd_intf_net [get_bd_intf_pins base_logic/" + krnl_name + "/" +
                       intf.getInterfaceName() + "] [get_bd_intf_pins axi_noc_cips/S0" +
                       std::to_string(idx + 4) + "_AXI]\n";
            } else {
                utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                                   "Connecting axi4full interface: {} to NoC S{} for kernel {}",
                                   intf.getInterfaceName(), std::to_string(idx + 4), krnl_name);
                return "connect_bd_intf_net [get_bd_intf_pins base_logic/" + krnl_name + "/" +
                       intf.getInterfaceName() + "] [get_bd_intf_pins axi_noc_cips/S" +
                       std::to_string(idx + 4) + "_AXI]\n";
            }
        }
    }
    return std::string();
}

std::string BdBuilder::configNumberOfAXILiteSlaves() {
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                       "Configuring number of axi lite interfaces to: {}",
                       getNumberOfAxiLiteInterfaces() + 5);
    uint8_t no = getNumberOfAxiLiteInterfaces();
    return "set_property CONFIG.NUM_MI {" + std::to_string(no + 5) +
           "} [get_bd_cells base_logic/pcie_slr0_mgmt_sc] \n\
set_property CONFIG.NUM_CLKS {2} [get_bd_cells base_logic/pcie_slr0_mgmt_sc]\n";  // one for clk_wiz
}

std::string BdBuilder::configNumberOfAXIFullSlaves() {
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                       "Configuring number of axi full interfaces to: {}",
                       getNumberOfAxiMmInterfaces() + 4);
    uint8_t no = getNumberOfAxiMmInterfaces();
    if (segmented) {  // only one noc intf which connects to a xbar
        return "set_property CONFIG.NUM_SI {" + std::to_string(5) +
               "} [get_bd_cells axi_noc_cips]\n\
set_property CONFIG.NUM_CLKS {6} [get_bd_cells axi_noc_cips]\n\
connect_bd_net [get_bd_pins axi_noc_cips/aclk5] [get_bd_pins base_logic/clk_wiz/clk_out1]\n";
    } else {
        return "set_property CONFIG.NUM_SI {" + std::to_string(no + 4) +
               "} [get_bd_cells axi_noc_cips]\n\
set_property CONFIG.NUM_CLKS {6} [get_bd_cells axi_noc_cips]\n\
connect_bd_net [get_bd_pins axi_noc_cips/aclk5] [get_bd_pins base_logic/clk_wiz/clk_out1]\n";
    }
}

std::string BdBuilder::createIp(int idx) {
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__, "Creating IP instance: {}",
                       kernels.at(idx).getTopModelName());
    return "# set custom kernel: " + KERNEL_NAME + kernels.at(idx).getTopModelName() + "\n" +
           "set " + kernels.at(idx).getName() + " [ create_bd_cell -type ip -vlnv" + " " +
           KERNEL_NAME + kernels.at(idx).getTopModelName() + " base_logic/" +
           kernels.at(idx).getName() + " ]\n";
}

std::string BdBuilder::assignSlaveAddress(std::string krnl_name, int idx, Interface intf,
                                          uint64_t base_addr) {
    std::stringstream ss;
    uint64_t offset;
    offset = std::pow(2, intf.getAddrWidth());
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                       "Assigning address {x} to axi4lite interface: {} for kernel: {}", base_addr,
                       intf.getInterfaceName(), krnl_name);
    ss << std::hex << std::showbase << "assign_bd_address -offset " << base_addr << " -range "
       << std::hex << std::showbase << offset << NOC0_ADDR_STR << krnl_name << "/"
       << intf.getInterfaceName() << "/Reg] -force" << std::endl;

    ss << std::hex << std::showbase << "assign_bd_address -offset " << base_addr << " -range "
       << std::hex << std::showbase << offset << NOC1_ADDR_STR << krnl_name << "/"
       << intf.getInterfaceName() << "/Reg] -force" << std::endl;

    return ss.str();
}

std::string BdBuilder::genQoS(int slave_offset, int bw) {
    std::stringstream ss;
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                       "Generating QoS with bandwidth: {}", bw);

    if (slave_offset == 0) {
        ss << "set_property -dict [list CONFIG.CONNECTIONS {"
           << "HBM10_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "M02_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64} "
              "initial_boot {false}} "
           << "HBM15_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM10_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM5_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM15_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM5_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM1_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM1_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM6_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM12_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM0_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM6_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM14_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM12_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM0_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM8_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM8_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM14_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM3_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM3_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM4_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM4_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM9_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM2_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM11_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "M00_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64} "
              "initial_boot {false}} "
           << "HBM9_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM11_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM7_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM13_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM7_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM13_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM2_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "M00_AXI {read_bw {5} write_bw {5} read_avg_burst {64} write_avg_burst {64} "
              "initial_boot {false}}}] [get_bd_intf_pins /axi_noc_cips/";
    } else if (slave_offset == 1) {
        ss << "set_property -dict [list CONFIG.CONNECTIONS {"
           << "HBM10_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM10_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM5_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM15_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM0_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM15_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM1_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM5_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM1_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM6_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM14_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM0_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM8_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "M01_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64} "
              "initial_boot {false}} "
           << "HBM12_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM6_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM12_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM8_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM14_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM3_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM3_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM4_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM9_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM4_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM9_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM11_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM11_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM7_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM13_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM7_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "HBM2_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "M03_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64} "
              "initial_boot {false}} "
           << "HBM2_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}} "
           << "M00_AXI {read_bw {5} write_bw {5} read_avg_burst {64} write_avg_burst {64} "
              "initial_boot {false}} "
           << "HBM13_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4} initial_boot {false}}}] [get_bd_intf_pins "
              "/axi_noc_cips/";
    } else if (slave_offset == 2) {
        ss << "set_property -dict [list CONFIG.CONNECTIONS {M02_INI {read_bw {800} write_bw {800} "
              "read_avg_burst {64} write_avg_burst {64} initial_boot {true}}"
           << "M00_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64} "
              "initial_boot {true}}}] [get_bd_intf_pins /axi_noc_cips/";

    } else if (slave_offset == 3) {
        ss << "set_property -dict [list CONFIG.CONNECTIONS {M00_INI {read_bw {800} write_bw {800} "
              "read_avg_burst {64} write_avg_burst {64} initial_boot {true}}}] [get_bd_intf_pins "
              "/axi_noc_cips/";
    } else if (slave_offset % 2 == 0) {
        ss << "set_property -dict [list CONFIG.CONNECTIONS {"
           << "HBM10_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "M02_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64}} "
           << "HBM15_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM10_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM5_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM15_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM5_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM1_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM1_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM6_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM12_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM0_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM6_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM14_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM12_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM0_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM8_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM8_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM14_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM3_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM3_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM4_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM4_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM9_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM2_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM11_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "M00_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64}} "
           << "HBM9_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM11_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM7_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM13_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM7_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM13_PORT0 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM2_PORT2 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "M00_AXI {read_bw {5} write_bw {5} read_avg_burst {64} write_avg_burst {64}}}] "
              "[get_bd_intf_pins /axi_noc_cips/";
    } else {
        ss << "set_property -dict [list CONFIG.CONNECTIONS {"
           << "HBM10_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "M03_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64}} "
           << "HBM10_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM5_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM15_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM0_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM15_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM1_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM5_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM1_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM6_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM14_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM0_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM8_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM12_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM6_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM12_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM8_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM14_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM3_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM3_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM4_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM9_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM4_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM9_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "M01_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64}} "
           << "HBM11_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM11_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM7_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM13_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM7_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM2_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "HBM2_PORT1 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}} "
           << "M00_AXI {read_bw {5} write_bw {5} read_avg_burst {64} write_avg_burst {64}} "
           << "HBM13_PORT3 {read_bw {" << bw << "} write_bw {" << bw
           << "} read_avg_burst {4} write_avg_burst {4}}}] [get_bd_intf_pins /axi_noc_cips/";
    }

    if (slave_offset < 10) {
        ss << "S0" << slave_offset << "_AXI]" << std::endl;
    } else {
        ss << "S" << slave_offset << "_AXI]" << std::endl;
    }

    return ss.str();
}

uint16_t BdBuilder::calculateBw() {
    int numberOfAxiMMChannels = 2;  // 2 aximm accesses to HBM/DDR via NoC for QDMA
    if (segmented) {
        numberOfAxiMMChannels = 3;
    } else {
        for (auto& kernel : kernels) {
            for (auto& intf : kernel.getInterfaces()) {
                if (intf.getInterfaceType() == "axi4full") {
                    numberOfAxiMMChannels++;
                }
            }
        }
    }
    return MAX_ALLOCABLE_BW_HBM_PER_CHANNEL / numberOfAxiMMChannels - 1;  // avoid rounding errors
}

uint8_t BdBuilder::getNumberOfAxiMmInterfaces() {
    uint8_t numberOfInterfaces = 0;
    for (auto& kernel : kernels) {
        for (auto& intf : kernel.getInterfaces()) {
            if (intf.getInterfaceType() == "axi4full") {
                numberOfInterfaces++;
            }
        }
    }
    return numberOfInterfaces;
}

uint8_t BdBuilder::getNumberOfAxiLiteInterfaces() {
    uint8_t numberOfInterfaces = 0;
    for (auto& kernel : kernels) {
        for (auto& intf : kernel.getInterfaces()) {
            if (intf.getInterfaceType() == "axi4lite") {
                numberOfInterfaces++;
            }
        }
    }
    return numberOfInterfaces;
}

std::string BdBuilder::connectAxis(std::string krnl_name) {
    // connect_bd_intf_net -intf_net accumulate_0_axis_in [get_bd_intf_pins
    // base_logic/accumulate_0/axis_in] [get_bd_intf_pins base_logic/increment_0/axis_out]

    /*
    return "  connect_bd_intf_net -intf_net pcie_slr0_mgmt_sc_M0" + std::to_string(idx + 4)
         + "_AXI " + "[get_bd_intf_pins pcie_slr0_mgmt_sc/M0" + std::to_string(idx + 4) + "_AXI]
    [get_bd_intf_pins " + krnl_name + "/" + intf.getInterfaceName() + "]\n";
     */
    uint32_t c2hIdx = 0;
    for (auto el = streamConnections.begin(); el != streamConnections.end(); el++) {
        if ((el->src.kernelName == krnl_name || el->dst.kernelName == krnl_name) &&
            el->src.kernelName != "cips" && el->dst.kernelName != "cips") {
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "Connecting axi4stream {}.{} to {}.{}", el->src.kernelName,
                               el->src.interfaceName, el->dst.kernelName, el->dst.interfaceName);
            std::string result = "connect_bd_intf_net -intf_net " + el->src.kernelName + "_" +
                                 el->src.interfaceName + " [get_bd_intf_pins base_logic/" +
                                 el->src.kernelName + "/" + el->src.interfaceName +
                                 "] [get_bd_intf_pins base_logic/" + el->dst.kernelName + "/" +
                                 el->dst.interfaceName + "]\n";
            streamConnections.erase(el);
            return result;
        } else if (el->src.kernelName == "cips") {
            uint32_t qid;
            std::string srcInterface = el->src.interfaceName;
            std::regex re("qdma_(\\d)");
            std::smatch match;
            if (std::regex_search(srcInterface, match, re) && match.size() > 1) {
                qid = std::stoi(match.str(1));
            } else {
                throw std::runtime_error("Invalid QDMA interface name: " + el->src.interfaceName);
            }

            if (qid > 15) {
                throw std::runtime_error("Max qid is 15. Actual qid: " + std::to_string(qid));
            }

            std::string result;
            if (qid < 10) {
                result = "connect_bd_intf_net [get_bd_intf_pins qdma/axis_switch_0/M0" +
                         std::to_string(qid - 1) + "_AXIS] [get_bd_intf_pins base_logic/" +
                         el->dst.kernelName + "/" + el->dst.interfaceName + "]\n";
            } else {
                result = "connect_bd_intf_net [get_bd_intf_pins qdma/axis_switch_0/M" +
                         std::to_string(qid - 1) + "_AXIS] [get_bd_intf_pins base_logic/" +
                         el->dst.kernelName + "/" + el->dst.interfaceName + "]\n";
            }
            streamConnections.erase(el);
            return result;
        } else if (el->dst.kernelName == "cips") {
            throw std::runtime_error("QDMA Stream C2H connections not supported yet");
            // std::string result;
            // if(c2hIdx > 15) {
            //     throw std::runtime_error("Max c2hIdx is 15. Actual c2hIdx: " +
            //     std::to_string(c2hIdx));
            // }
            // if(c2hIdx < 10) {
            //     result = "connect_bd_intf_net [get_bd_intf_pins qdma/axis_switch_1/S0" +
            //     std::to_string(c2hIdx++)
            //     + "_AXIS] [get_bd_intf_pins base_logic/" + el->src.kernelName + "/" +
            //     el->src.interfaceName + "]\n";
            // } else {
            //     result = "connect_bd_intf_net [get_bd_intf_pins qdma/axis_switch_1/S" +
            //     std::to_string(c2hIdx++)
            //     + "_AXIS] [get_bd_intf_pins base_logic/" + el->src.kernelName + "/" +
            //     el->src.interfaceName + "]\n";
            // }
            // return result;
        }
    }
    return "\n";  // if no stream interfaces exist
}

std::string BdBuilder::configureUserClock() {
    double freqMHz = targetClockFreq / 1e6;
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                       "Configuring user clock with frequency: {} MHz", freqMHz);
    std::stringstream ss;
    ss << "set_property -dict [list \\\n"
       << "  CONFIG.CLKOUT_DRIVES {BUFG,BUFG,BUFG,BUFG,BUFG,BUFG,BUFG} \\\n"
       << "  CONFIG.CLKOUT_DYN_PS {None,None,None,None,None,None,None} \\\n"
       << "  CONFIG.CLKOUT_GROUPING {Auto,Auto,Auto,Auto,Auto,Auto,Auto} \\\n"
       << "  CONFIG.CLKOUT_MATCHED_ROUTING {false,false,false,false,false,false,false} \\\n"
       << "  CONFIG.CLKOUT_PORT {clk_out1,clk_out2,clk_out3,clk_out4,clk_out5,clk_out6,clk_out7} "
          "\\\n"
       << "  CONFIG.CLKOUT_REQUESTED_DUTY_CYCLE {50.000,50.000,50.000,50.000,50.000,50.000,50.000} "
          "\\\n"
       << "  CONFIG.CLKOUT_REQUESTED_OUT_FREQUENCY {" << freqMHz
       << ",100.000,100.000,100.000,100.000,100.000,100.000} \\\n"
       << "  CONFIG.CLKOUT_REQUESTED_PHASE {0.000,0.000,0.000,0.000,0.000,0.000,0.000} \\\n"
       << "  CONFIG.CLKOUT_USED {true,false,false,false,false,false,false} \\\n"
       << "  CONFIG.USE_DYN_RECONFIG {true} \\\n"
       << "] [get_bd_cells base_logic/clk_wiz]\n\n\n";
    return ss.str();
}
// connect_bd_intf_net [get_bd_intf_pins base_logic/clk_wizard_0/s_axi_lite] [get_bd_intf_pins
// base_logic/pcie_slr0_mgmt_sc/M04_AXI]

std::string BdBuilder::connectClkWiz() {
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                       "Connecting clock wizard axi4lite interface to axi4lite");
    return "connect_bd_intf_net -intf_net pcie_slr0_mgmt_sc_M04_AXI [get_bd_intf_pins base_logic/pcie_slr0_mgmt_sc/M04_AXI] [get_bd_intf_pins base_logic/clk_wiz/s_axi_lite]\n\
connect_bd_net -net clk_pl_1 [get_bd_pins base_logic/clk_pl] [get_bd_pins base_logic/clk_wiz/s_axi_aclk] [get_bd_pins base_logic/clk_wiz/clk_in1]\n\
connect_bd_net -net clk_out1 [get_bd_pins base_logic/clk_wiz/clk_out1] [get_bd_pins base_logic/pcie_slr0_mgmt_sc/aclk1] [get_bd_pins base_logic/sys_rst/slowest_sync_clk]\n\
connect_bd_net -net clk_wiz_locked [get_bd_pins base_logic/clk_wiz/locked] [get_bd_pins base_logic/sys_rst/dcm_locked]\n\
connect_bd_net -net resetn_pl_ic_1 [get_bd_pins resetn_pl_ic] [get_bd_pins base_logic/clk_wiz/s_axi_aresetn] [get_bd_pins base_logic/sys_rst/ext_reset_in]\n";
}

std::string BdBuilder::setupClkWiz() {
    std::stringstream ss;
    ss << "# Add clocking wizard\n"
       << "set clk_wiz [create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wizard:1.0 "
          "base_logic/clk_wiz]\n";
    return ss.str();
}
std::string BdBuilder::setupSysRst() {
    std::stringstream ss;
    ss << "# Add system reset\n"
       << "set sys_rst [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 "
          "base_logic/sys_rst]\n";
    return ss.str();
}

std::string BdBuilder::printFooter() {
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__, "Print TCL footer");
    std::stringstream ss;
    ss << "}\n";
    return ss.str();
}

std::string BdBuilder::setHBMConfig() {
    std::stringstream ss;
    ss << "set_property CONFIG.HBM_CHNL0_CONFIG {HBM_REORDER_EN FALSE HBM_MAINTAIN_COHERENCY TRUE "
          "HBM_Q_AGE_LIMIT 0x7F "
       << "HBM_CLOSE_PAGE_REORDER FALSE HBM_LOOKAHEAD_PCH TRUE HBM_COMMAND_PARITY FALSE "
          "HBM_DQ_WR_PARITY FALSE "
       << "HBM_DQ_RD_PARITY FALSE HBM_RD_DBI TRUE HBM_WR_DBI TRUE HBM_REFRESH_MODE "
          "SINGLE_BANK_REFRESH "
       << "HBM_PC0_PRE_DEFINED_ADDRESS_MAP USER_DEFINED_ADDRESS_MAP "
          "HBM_PC1_PRE_DEFINED_ADDRESS_MAP USER_DEFINED_ADDRESS_MAP "
       << "HBM_PC0_USER_DEFINED_ADDRESS_MAP 1BG-15RA-1SID-2BA-5CA-1BG "
          "HBM_PC1_USER_DEFINED_ADDRESS_MAP 1BG-15RA-1SID-2BA-5CA-1BG "
       << "HBM_PC0_ADDRESS_MAP "
          "BA3,RA14,RA13,RA12,RA11,RA10,RA9,RA8,RA7,RA6,RA5,RA4,RA3,RA2,RA1,RA0,SID,BA1,BA0,CA5,"
          "CA4,CA3,CA2,CA1,BA2,NC,NA,NA,NA,NA "
       << "HBM_PC1_ADDRESS_MAP "
          "BA3,RA14,RA13,RA12,RA11,RA10,RA9,RA8,RA7,RA6,RA5,RA4,RA3,RA2,RA1,RA0,SID,BA1,BA0,CA5,"
          "CA4,CA3,CA2,CA1,BA2,NC,NA,NA,NA,NA "
       << "HBM_PWR_DWN_IDLE_TIMEOUT_ENTRY FALSE HBM_SELF_REF_IDLE_TIMEOUT_ENTRY FALSE "
          "HBM_IDLE_TIME_TO_ENTER_PWR_DWN_MODE 0x0001000 "
       << "HBM_IDLE_TIME_TO_ENTER_SELF_REF_MODE 1X HBM_ECC_CORRECTION_EN FALSE "
          "HBM_WRITE_BACK_CORRECTED_DATA TRUE "
       << "HBM_ECC_SCRUBBING FALSE HBM_ECC_INITIALIZE_EN FALSE HBM_ECC_SCRUB_SIZE 1092 "
          "HBM_WRITE_DATA_MASK TRUE "
       << "HBM_REF_PERIOD_TEMP_COMP FALSE HBM_PARITY_LATENCY 3 HBM_PC0_PAGE_HIT 100.000 "
          "HBM_PC1_PAGE_HIT 100.000 "
       << "HBM_PC0_READ_RATE 25.000 HBM_PC1_READ_RATE 25.000 HBM_PC0_WRITE_RATE 25.000 "
          "HBM_PC1_WRITE_RATE 25.000 "
       << "HBM_PC0_PHY_ACTIVE ENABLED HBM_PC1_PHY_ACTIVE ENABLED HBM_PC0_SCRUB_START_ADDRESS "
          "0x0000000 "
       << "HBM_PC0_SCRUB_END_ADDRESS 0x3FFFBFF HBM_PC0_SCRUB_INTERVAL 24.000 "
          "HBM_PC1_SCRUB_START_ADDRESS 0x0000000 "
       << "HBM_PC1_SCRUB_END_ADDRESS 0x3FFFBFF HBM_PC1_SCRUB_INTERVAL 24.000} [get_bd_cells "
          "axi_noc_cips]\n";
    return ss.str();
}

std::string BdBuilder::assignClkWizAddr() {
    std::stringstream ss;
    ss << "assign_bd_address -offset 0x20100010000 -range 0x10000 -target_address_space "
          "[get_bd_addr_spaces cips/CPM_PCIE_NOC_0] [get_bd_addr_segs "
          "base_logic/clk_wiz/s_axi_lite/Reg] -force\n"
       << "assign_bd_address -offset 0x20100010000 -range 0x10000 -target_address_space "
          "[get_bd_addr_spaces cips/CPM_PCIE_NOC_1] [get_bd_addr_segs "
          "base_logic/clk_wiz/s_axi_lite/Reg] -force\n";
    return ss.str();
}

std::string BdBuilder::setSegmented() {
    char resolvedPath[PATH_MAX];
    if (realpath(NOC_SOLUTION.c_str(), resolvedPath) == nullptr) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                           "Failed to resolve path to {}", NOC_SOLUTION);
        throw std::runtime_error("Failed to resolve path to " + std::string(NOC_SOLUTION));
    }
    std::stringstream ss;
    ss << "set_property NOC_SOLUTION_FILE " << std::string(resolvedPath) << " [get_runs impl_1]\n";
    return ss.str();
}

std::string BdBuilder::addXbar(uint8_t numSlaves) {
    std::stringstream ss;
    ss << "create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 base_logic/noc_xbar\n"
       << "set_property CONFIG.NUM_SI {" << (int)numSlaves
       << "} [get_bd_cells base_logic/noc_xbar]\n"
       << "connect_bd_net [get_bd_pins base_logic/noc_xbar/aclk] [get_bd_pins "
          "base_logic/clk_wiz/clk_out1]\n"
       << "connect_bd_net [get_bd_pins base_logic/noc_xbar/aresetn] [get_bd_pins "
          "base_logic/sys_rst/peripheral_aresetn]\n";

    return ss.str();
}

std::string BdBuilder::connectXbarToNoC() {
    return "connect_bd_intf_net [get_bd_intf_pins base_logic/noc_xbar/M00_AXI] [get_bd_intf_pins "
           "axi_noc_cips/S04_AXI]\n";
}

std::string BdBuilder::setupQdmaStreaming() {
    std::stringstream ss;
    ss << "set_property -dict [list \\\n"
       << "    CONFIG.CPM_CONFIG { \\\n"
       << "        CPM_PCIE1_DMA_INTF {AXI_MM_and_AXI_Stream} \\\n"
       << "    } \\\n"
       << "] [get_bd_cells cips]\n\n\n";
    return ss.str();
}

std::string BdBuilder::addH2CAxisRouter() {
    std::stringstream ss;
    ss << "set axis_switch_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_switch:1.1 "
          "qdma/axis_switch_0 ]\n"
       << "set_property -dict [list \\\n"
       << "    CONFIG.M00_AXIS_BASETDEST {0x00000001} \\\n"
       << "    CONFIG.M00_AXIS_HIGHTDEST {0x00000001} \\\n"
       << "    CONFIG.M01_AXIS_BASETDEST {0x00000002} \\\n"
       << "    CONFIG.M01_AXIS_HIGHTDEST {0x00000002} \\\n"
       << "    CONFIG.M02_AXIS_BASETDEST {0x00000003} \\\n"
       << "    CONFIG.M02_AXIS_HIGHTDEST {0x00000003} \\\n"
       << "    CONFIG.M03_AXIS_BASETDEST {0x00000004} \\\n"
       << "    CONFIG.M03_AXIS_HIGHTDEST {0x00000004} \\\n"
       << "    CONFIG.M04_AXIS_BASETDEST {0x00000005} \\\n"
       << "    CONFIG.M04_AXIS_HIGHTDEST {0x00000005} \\\n"
       << "    CONFIG.M05_AXIS_BASETDEST {0x00000006} \\\n"
       << "    CONFIG.M05_AXIS_HIGHTDEST {0x00000006} \\\n"
       << "    CONFIG.M06_AXIS_BASETDEST {0x00000007} \\\n"
       << "    CONFIG.M06_AXIS_HIGHTDEST {0x00000007} \\\n"
       << "    CONFIG.M07_AXIS_BASETDEST {0x00000008} \\\n"
       << "    CONFIG.M07_AXIS_HIGHTDEST {0x00000008} \\\n"
       << "    CONFIG.M08_AXIS_BASETDEST {0x00000009} \\\n"
       << "    CONFIG.M08_AXIS_HIGHTDEST {0x00000009} \\\n"
       << "    CONFIG.M09_AXIS_BASETDEST {0x0000000a} \\\n"
       << "    CONFIG.M09_AXIS_HIGHTDEST {0x0000000a} \\\n"
       << "    CONFIG.M10_AXIS_BASETDEST {0x0000000b} \\\n"
       << "    CONFIG.M10_AXIS_HIGHTDEST {0x0000000b} \\\n"
       << "    CONFIG.M11_AXIS_BASETDEST {0x0000000c} \\\n"
       << "    CONFIG.M11_AXIS_HIGHTDEST {0x0000000c} \\\n"
       << "    CONFIG.M12_AXIS_BASETDEST {0x0000000d} \\\n"
       << "    CONFIG.M12_AXIS_HIGHTDEST {0x0000000d} \\\n"
       << "    CONFIG.M13_AXIS_BASETDEST {0x0000000e} \\\n"
       << "    CONFIG.M13_AXIS_HIGHTDEST {0x0000000e} \\\n"
       << "    CONFIG.M14_AXIS_BASETDEST {0x0000000f} \\\n"
       << "    CONFIG.M14_AXIS_HIGHTDEST {0x0000000f} \\\n"
       << "    CONFIG.M15_AXIS_BASETDEST {0x00000010} \\\n"
       << "    CONFIG.M15_AXIS_HIGHTDEST {0x00000010} \\\n"
       << "    CONFIG.NUM_MI {16} \\\n"
       << "    CONFIG.NUM_SI {1} \\\n"
       << "    CONFIG.TDATA_NUM_BYTES {64} \\\n"
       << "] $axis_switch_0\n\n\n";
    return ss.str();
}

std::string BdBuilder::addC2HAxisRouter() {
    std::stringstream ss;
    ss << "set axis_switch_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_switch:1.1 "
          "qdma/axis_switch_1 ]\n";
    ss << "set_property -dict [list \\\n"
       << "    CONFIG.NUM_SI {16} \\\n"
       << "    CONFIG.NUM_MI {1} \\\n"
       << "    CONFIG.TDATA_NUM_BYTES {64} \\\n"
       << "] $axis_switch_1\n\n\n";

    return ss.str();
}

std::string BdBuilder::connectQdmaH2CToRouter() {
    std::stringstream ss;
    ss << "connect_bd_net [get_bd_pins qdma/h2c_fifo/s_axis_tvalid] [get_bd_pins "
          "cips/dma1_m_axis_h2c_tvalid]\n"
       << "connect_bd_net [get_bd_pins qdma/h2c_fifo/s_axis_tready] [get_bd_pins "
          "cips/dma1_m_axis_h2c_tready]\n"
       << "connect_bd_net [get_bd_pins qdma/h2c_fifo/s_axis_tdata] [get_bd_pins "
          "cips/dma1_m_axis_h2c_tdata]\n"
       << "connect_bd_net [get_bd_pins cips/dma1_m_axis_h2c_qid] [get_bd_pins "
          "qdma/h2c_fifo/s_axis_tdest]\n"
       << "connect_bd_intf_net [get_bd_intf_pins qdma/h2c_fifo/M_AXIS] [get_bd_intf_pins "
          "qdma/axis_switch_0/S00_AXIS]\n"
       << "connect_bd_net [get_bd_pins base_logic/sys_rst/peripheral_aresetn] [get_bd_pins "
          "qdma/axis_switch_0/aresetn]\n"
       << "connect_bd_net [get_bd_pins base_logic/sys_rst/peripheral_aresetn] [get_bd_pins "
          "qdma/h2c_fifo/s_axis_aresetn]\n"
       << "connect_bd_net [get_bd_pins base_logic/clk_wiz/clk_out1] [get_bd_pins "
          "qdma/axis_switch_0/aclk]\n"
       << "connect_bd_net [get_bd_pins base_logic/clk_wiz/clk_out1] [get_bd_pins "
          "qdma/h2c_fifo/m_axis_aclk]\n"
       << "connect_bd_net [get_bd_pins cips/pl2_ref_clk] [get_bd_pins qdma/h2c_fifo/s_axis_aclk]\n";
    return ss.str();
}

std::string BdBuilder::connectQdmaC2HToRouter() {
    std::stringstream ss;
    ss << "connect_bd_net [get_bd_pins qdma/axis_switch_1/m_axis_tvalid] [get_bd_pins "
          "cips/dma1_s_axis_c2h_tvalid]\n"
       << "connect_bd_net [get_bd_pins qdma/axis_switch_1/m_axis_tready] [get_bd_pins "
          "cips/dma1_s_axis_c2h_tready]\n"
       << "connect_bd_net [get_bd_pins qdma/axis_switch_1/m_axis_tdata] [get_bd_pins "
          "cips/dma1_s_axis_c2h_tdata]\n"
       << "connect_bd_net [get_bd_pins base_logic/sys_rst/peripheral_aresetn] [get_bd_pins "
          "qdma/axis_switch_1/aresetn]\n"
       << "connect_bd_net [get_bd_pins base_logic/clk_wiz/clk_out1] [get_bd_pins "
          "qdma/axis_switch_1/aclk]\n";
    ss << "\n\n\n";
    return ss.str();
}

std::string BdBuilder::addQdmaLogic() {
    std::stringstream ss;
    ss << "create_bd_cell -type hier qdma\n";
    ss << addH2CAxisRouter();
    ss << addH2CFifo();
    // ss << addC2HAxisRouter();
    // ss << "set qdma_logic_gpio [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio:2.0
    // qdma/qdma_logic_gpio]\n"; ss << "set_property CONFIG.C_ALL_OUTPUTS {1} [get_bd_cells
    // qdma/qdma_logic_gpio]\n"; ss << "set qid_slice [create_bd_cell -type ip -vlnv
    // xilinx.com:ip:xlslice:1.0 qdma/qid_slice]\n\n"; ss << "set_property CONFIG.DIN_FROM {11}
    // [get_bd_cells qdma/qid_slice]\n"; ss << "set qlen_slice [create_bd_cell -type ip -vlnv
    // xilinx.com:ip:xlslice:1.0 qdma/qlen_slice]\n\n"; ss << "set_property -dict [list \\\n"
    //    << "    CONFIG.DIN_FROM {27} \\\n"
    //    << "     CONFIG.DIN_TO {12} \\\n"
    //    << "] [get_bd_cells qdma/qlen_slice]\n\n";
    // ss << "\n\n\n";
    return ss.str();
}

std::string BdBuilder::connectQdmaLogic() {
    std::stringstream ss;
    ss << connectQdmaH2CToRouter();
    // ss << connectQdmaC2HToRouter();
    // ss << "connect_bd_net [get_bd_pins qdma/qdma_logic_gpio/gpio_io_o] [get_bd_pins
    // qdma/qid_slice/Din]\n"; ss << "connect_bd_net [get_bd_pins qdma/qdma_logic_gpio/gpio_io_o]
    // [get_bd_pins qdma/qlen_slice/Din]\n"; ss << "connect_bd_net [get_bd_pins qdma/qid_slice/Dout]
    // [get_bd_pins cips/dma1_s_axis_c2h_ctrl_qid]\n"; ss << "connect_bd_net [get_bd_pins
    // qdma/qlen_slice/Dout] [get_bd_pins cips/dma1_s_axis_c2h_ctrl_len]\n"; ss << "connect_bd_net
    // [get_bd_pins base_logic/clk_wiz/clk_out1] [get_bd_pins qdma/qdma_logic_gpio/s_axi_aclk]\n";
    // ss << "connect_bd_net [get_bd_pins base_logic/sys_rst/peripheral_aresetn] [get_bd_pins
    // qdma/qdma_logic_gpio/s_axi_aresetn]\n"; ss << "connect_bd_intf_net [get_bd_intf_pins
    // base_logic/pcie_slr0_mgmt_sc/M05_AXI] [get_bd_intf_pins qdma/qdma_logic_gpio/S_AXI]\n";
    ss << "\n\n\n";
    return ss.str();
}

std::string BdBuilder::assignQdmaLogicGpioAddr() {
    std::stringstream ss;
    ss << "assign_bd_address -offset 0x20100020000 -range 0x00001000 -target_address_space "
          "[get_bd_addr_spaces cips/CPM_PCIE_NOC_0] [get_bd_addr_segs "
          "qdma/qdma_logic_gpio/S_AXI/Reg] -force\n"
       << "assign_bd_address -offset 0x20100020000 -range 0x00001000 -target_address_space "
          "[get_bd_addr_spaces cips/CPM_PCIE_NOC_1] [get_bd_addr_segs "
          "qdma/qdma_logic_gpio/S_AXI/Reg] -force\n";
    ss << "\n\n\n";
    return ss.str();
}

std::string BdBuilder::addH2CFifo() {
    std::stringstream ss;
    ss << "set h2c_fifo [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_data_fifo:2.0 "
          "qdma/h2c_fifo ]\n";
    ss << "set_property -dict [list \\\n"
       << "    CONFIG.FIFO_DEPTH {2048} \\\n"
       << "    CONFIG.IS_ACLK_ASYNC {1} \\\n"
       << "    CONFIG.TDATA_NUM_BYTES {64} \\\n"
       << "    CONFIG.TDEST_WIDTH {4}  \\\n"
       << "] [get_bd_cells qdma/h2c_fifo]\n";
    ss << "\n\n\n";

    return ss.str();
}

std::string BdBuilder::configNumberOfAXILiteSlavesSim() {
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                       "Configuring number of axi lite interfaces to: {}",
                       getNumberOfAxiLiteInterfaces());
    uint8_t no = getNumberOfAxiLiteInterfaces();
    return "set_property CONFIG.NUM_MI {" + std::to_string(no) + "} [get_bd_cells axi_sc] \n";
}

std::string BdBuilder::configNumberOfAXIFullSlavesSim() {
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                       "Configuring number of axi full interfaces to: {}",
                       getNumberOfAxiMmInterfaces());
    uint8_t no = getNumberOfAxiMmInterfaces();

    return "set_property CONFIG.NUM_SI {" + std::to_string(no + 1) + "} [get_bd_cells mem_sc] \n";
}

std::string BdBuilder::connectInterfaceSim(std::string krnl_name, Interface intf, int idx) {
    if (intf.getInterfaceType() == "axi4lite") {
        if ((idx) < 10) {
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "Connecting axi4lite interface: {} to axi xbar M0{} for kernel {}",
                               intf.getInterfaceName(), std::to_string(idx), krnl_name);
            return "connect_bd_intf_net [get_bd_intf_pins axi_sc/M0" + std::to_string(idx) +
                   "_AXI] [get_bd_intf_pins " + krnl_name + "/" + intf.getInterfaceName() + "]\n";
        } else {
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "Connecting axi4lite interface: {} to axi xbar M{} for kernel {}",
                               intf.getInterfaceName(), std::to_string(idx), krnl_name);
            return "connect_bd_intf_net [get_bd_intf_pins axi_sc/M" + std::to_string(idx) +
                   "_AXI] [get_bd_intf_pins " + krnl_name + "/" + intf.getInterfaceName() + "]\n";
        }
    } else if (intf.getInterfaceType() == "clock") {
        utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                           "Connecting clock interface: {} to clk_wiz for kernel {}",
                           intf.getInterfaceName(), krnl_name);
        return "connect_bd_net [get_bd_pins clk] [get_bd_pins " + krnl_name + "/" +
               intf.getInterfaceName() + "]\n";
    } else if (intf.getInterfaceType() == "reset") {
        utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                           "Connecting reset interface: {} to sys_rst for kernel {}",
                           intf.getInterfaceName(), krnl_name);
        return "connect_bd_net [get_bd_pins rst] [get_bd_pins " + krnl_name + "/" +
               intf.getInterfaceName() + "]\n";
    } else if (intf.getInterfaceType() == "axi4full") {
        if ((idx + 1) < 10) {
            return "connect_bd_intf_net [get_bd_intf_pins " + krnl_name + "/" +
                   intf.getInterfaceName() + "] [get_bd_intf_pins mem_sc/S0" +
                   std::to_string(idx + 1) + "_AXI]\n";
        } else {
            return "connect_bd_intf_net [get_bd_intf_pins " + krnl_name + "/" +
                   intf.getInterfaceName() + "] [get_bd_intf_pins mem_sc/S" +
                   std::to_string(idx + 1) + "_AXI]\n";
        }
    }
    return std::string();
}

std::string BdBuilder::createIpSim(int idx) {
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__, "Creating IP instance: {}",
                       kernels.at(idx).getTopModelName());
    return "# set custom kernel: " + KERNEL_NAME + kernels.at(idx).getTopModelName() + "\n" +
           "set " + kernels.at(idx).getName() + " [ create_bd_cell -type ip -vlnv" + " " +
           KERNEL_NAME + kernels.at(idx).getTopModelName() + " " + kernels.at(idx).getName() +
           " ]\n";
}

std::string BdBuilder::assignSlaveAddressSim(std::string krnl_name, int idx, Interface intf,
                                             uint64_t base_addr) {
    std::stringstream ss;
    uint64_t offset;
    offset = std::pow(2, intf.getAddrWidth());
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                       "Assigning address {x} to axi4lite interface: {} for kernel: {}", base_addr,
                       intf.getInterfaceName(), krnl_name);
    ss << std::hex << std::showbase << "assign_bd_address -offset " << base_addr << " -range "
       << std::hex << std::showbase << offset << " [get_bd_addr_segs " << krnl_name << "/"
       << intf.getInterfaceName() << "/Reg] -force" << std::endl;

    return ss.str();
}

std::string BdBuilder::connectAxisSim(std::string krnl_name) {
    uint32_t c2hIdx = 0;
    for (auto el = streamConnections.begin(); el != streamConnections.end(); el++) {
        if ((el->src.kernelName == krnl_name || el->dst.kernelName == krnl_name) &&
            el->src.kernelName != "cips" && el->dst.kernelName != "cips") {
            utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                               "Connecting axi4stream {}.{} to {}.{}", el->src.kernelName,
                               el->src.interfaceName, el->dst.kernelName, el->dst.interfaceName);
            std::string result = "connect_bd_intf_net -intf_net " + el->src.kernelName + "_" +
                                 el->src.interfaceName + " [get_bd_intf_pins " +
                                 el->src.kernelName + "/" + el->src.interfaceName +
                                 "] [get_bd_intf_pins " + el->dst.kernelName + "/" +
                                 el->dst.interfaceName + "]\n";
            streamConnections.erase(el);
            return result;
        } else if (el->src.kernelName == "cips") {
            throw std::runtime_error("QDMA Stream H2C connections not supported yet in simulator");
        } else if (el->dst.kernelName == "cips") {
            throw std::runtime_error("QDMA Stream C2H connections not supported yet in simulator");
        }
    }
    return "\n";  // if no stream interfaces exist
}

std::string BdBuilder::addRunPreHeader() {
    std::stringstream ss;
    ss << "proc run_pre { parentCell } {\n"
       << "\n"
       << "    variable script_folder\n"
       << "\n"
       << "    if { $parentCell eq \"\" } {\n"
       << "        set parentCell [get_bd_cells /]\n"
       << "    }\n"
       << "\n"
       << "    # Get object for parentCell\n"
       << "    set parentObj [get_bd_cells $parentCell]\n"
       << "    if { $parentObj == \"\" } {\n"
       << "        catch {common::send_gid_msg -ssname BD::TCL -id 2090 -severity \"ERROR\" "
          "\"Unable to find parent cell <$parentCell>!\"}\n"
       << "        return\n"
       << "    }\n"
       << "\n"
       << "    # Make sure parentObj is hier blk\n"
       << "    set parentType [get_property TYPE $parentObj]\n"
       << "    if { $parentType ne \"hier\" } {\n"
       << "        catch {common::send_gid_msg -ssname BD::TCL -id 2091 -severity \"ERROR\" "
          "\"Parent <$parentObj> has TYPE = <$parentType>. Expected to be <hier>.\"}\n"
       << "        return\n"
       << "    }\n"
       << "\n"
       << "    # Save current instance; Restore later\n"
       << "    set oldCurInst [current_bd_instance .]\n"
       << "\n"
       << "    # Set parent object as current\n"
       << "    current_bd_instance $parentObj\n"
       << "\n";

    return ss.str();
}

std::string BdBuilder::generateSourceInstruction(const std::string& path) const {
    std::stringstream ss;

    if (path.find("{") != std::string::npos
        || path.find("}") != std::string::npos) {
        throw std::runtime_error("Path to script to source cannot contian '{' or '}'");
    }

    ss << "\tsource {" << path << "}\n";

    return ss.str();
}

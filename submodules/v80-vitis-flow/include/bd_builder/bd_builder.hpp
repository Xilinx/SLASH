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

#pragma once

#include <limits.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "arg_parser.hpp"
#include "kernel.hpp"
#include "logger.hpp"
#include "system_map.hpp"

/**
 * @brief Class for building Vivado Block Design TCL scripts.
 *
 * This class generates TCL scripts that create and configure Vivado block designs
 * with hardware kernels and their interconnections. It supports both hardware and
 * simulation platforms, with appropriate settings for each.
 */
class BdBuilder {
    static bool hasAximmIntf;  ///< Flag indicating if any kernel has AXI-MM interface
    std::string KERNEL_NAME = "xilinx.com:hls:";     ///< Base kernel name prefix for IP cores
    std::string ALL_REGEX_CHAR = ":*";               ///< Regex characters for wildcard matching
    std::string BASE_BD_TCL_PATH = "../resources/";  ///< Path to base TCL scripts
    std::string INPUT_FILE_HW =
        "../resources/base_bd.tcl";  ///< Base TCL file for hardware platform
    std::string INPUT_FILE_SIM =
        "../resources/sim_prj.tcl";           ///< Base TCL file for simulation platform
    std::string PRE_OUTPUT_FILE = "run_pre.tcl";  ///< Output pre TCL file name
    std::string POST_OUTPUT_FILE = "run_post.tcl"; ///< Output post TCL file name
    std::string NOC_SOLUTION = "../resources/noc_sol_compute.ncr";  ///< NoC solution file path
    std::string NOC0_ADDR_STR =
        " -target_address_space [get_bd_addr_spaces cips/CPM_PCIE_NOC_0] [get_bd_addr_segs "
        "base_logic/";  ///< NoC 0 address space TCL string
    std::string NOC1_ADDR_STR =
        " -target_address_space [get_bd_addr_spaces cips/CPM_PCIE_NOC_1] [get_bd_addr_segs "
        "base_logic/";  ///< NoC 1 address space TCL string
    static constexpr uint64_t BASE_ADDRESS =
        0x20100000000;  ///< Base address for kernel memory mapping
    static constexpr uint64_t KERNEL_MEMORY_MAP_SIZE =
        0x00001000;  ///< Size of each kernel's memory map
    static constexpr uint16_t MAX_ALLOCABLE_BW_HBM_PER_CHANNEL =
        400;                      ///< Maximum allocable bandwidth for HBM per channel in MBps
    std::vector<Kernel> kernels;  ///< List of kernels to include in the design
    std::vector<Connection> streamConnections;  ///< List of streaming connections between kernels
    uint64_t targetClockFreq;                   ///< Target clock frequency in Hz
    SystemMap systemMap;                        ///< System memory map for the design
    bool segmented;                             ///< Flag indicating if design is segmented
    Platform platform;  ///< Target platform (hardware, simulation, emulation)
    TclInjections tclInjections; ///< Set of Tcl files to inject

    /**
     * @brief Generate source instruction.
     * @param path The path to source.
     * @return The source instruction.
     */
    std::string generateSourceInstruction(const std::string& path) const;

   public:
    /**
     * @brief Constructor for BdBuilder.
     * @param kernels Vector of kernel objects to include in the design.
     * @param connections Vector of connection objects defining inter-kernel connections.
     */
    BdBuilder(std::vector<Kernel> kernels, std::vector<Connection> connections);

    /**
     * @brief Extended constructor for BdBuilder with additional parameters.
     * @param kernels Vector of kernel objects to include in the design.
     * @param connections Vector of connection objects defining inter-kernel connections.
     * @param targetClockFreq Target clock frequency in Hz.
     * @param segmented Flag indicating if design is segmented.
     * @param platform Target platform (hardware, simulation, emulation).
     * @param tclInjections Set of Tcl files to inject.
     */
    BdBuilder(std::vector<Kernel> kernels, std::vector<Connection> connections,
              double targetClockFreq, bool segmented, Platform platform,
              TclInjections tclInjections);

    /**
     * @brief Builds the block design by generating TCL commands.
     *
     * This is the main function that orchestrates the creation of the entire block design.
     * It calls various helper functions to configure different aspects of the design.
     */
    void buildBlockDesign();

    /**
     * @brief Generates TCL commands to connect a kernel interface.
     * @param krnl_name Name of the kernel.
     * @param intf Interface to connect.
     * @param idx Index of the kernel.
     * @return String containing TCL commands for interface connection.
     */
    std::string connectInterface(std::string krnl_name, Interface intf, int idx);

    /**
     * @brief Generates TCL commands to configure the number of AXI-Lite slaves.
     * @return String containing TCL commands for AXI-Lite slave configuration.
     */
    std::string configNumberOfAXILiteSlaves();

    /**
     * @brief Generates TCL commands to create an IP for a kernel.
     * @param idx Index of the kernel.
     * @return String containing TCL commands for IP creation.
     */
    std::string createIp(int idx);

    /**
     * @brief Generates TCL commands for Quality of Service (QoS) settings.
     * @param slave_offset Offset for slave addressing.
     * @param bw Bandwidth allocation.
     * @return String containing TCL commands for QoS configuration.
     */
    std::string genQoS(int slave_offset, int bw);

    /**
     * @brief Generates TCL commands to assign an address to a slave interface.
     * @param krnl_name Name of the kernel.
     * @param idx Index of the kernel.
     * @param intf Interface to assign address to.
     * @param base_addr Base address for the interface.
     * @return String containing TCL commands for address assignment.
     */
    std::string assignSlaveAddress(std::string krnl_name, int idx, Interface intf,
                                   uint64_t base_addr);

    /**
     * @brief Generates TCL commands to configure the number of AXI-Full slaves.
     * @return String containing TCL commands for AXI-Full slave configuration.
     */
    std::string configNumberOfAXIFullSlaves();

    /**
     * @brief Generates TCL commands to connect AXI-Stream interfaces.
     * @param krnl_name Name of the kernel.
     * @return String containing TCL commands for AXI-Stream connections.
     */
    std::string connectAxis(std::string krnl_name);

    /**
     * @brief Calculates the required bandwidth for the design.
     * @return Required bandwidth in MBps.
     */
    uint16_t calculateBw();

    /**
     * @brief Gets the number of AXI-MM interfaces in the design.
     * @return Number of AXI-MM interfaces.
     */
    uint8_t getNumberOfAxiMmInterfaces();

    /**
     * @brief Gets the number of AXI-Lite interfaces in the design.
     * @return Number of AXI-Lite interfaces.
     */
    uint8_t getNumberOfAxiLiteInterfaces();

    /**
     * @brief Generates TCL commands to configure the user clock.
     * @return String containing TCL commands for user clock configuration.
     */
    std::string configureUserClock();

    /**
     * @brief Generates TCL commands to connect the clock wizard.
     * @return String containing TCL commands for clock wizard connections.
     */
    std::string connectClkWiz();

    /**
     * @brief Exports the system memory map to a file.
     */
    void exportSystemMap();

    /**
     * @brief Generates TCL commands to set up the clock wizard.
     * @return String containing TCL commands for clock wizard setup.
     */
    std::string setupClkWiz();

    /**
     * @brief Generates TCL commands to set up the system reset.
     * @return String containing TCL commands for system reset setup.
     */
    std::string setupSysRst();

    /**
     * @brief Generates TCL commands for the footer section of the script.
     * @return String containing TCL commands for the footer.
     */
    std::string printFooter();

    /**
     * @brief Generates TCL commands to configure HBM (High-Bandwidth Memory).
     * @return String containing TCL commands for HBM configuration.
     */
    std::string setHBMConfig();

    /**
     * @brief Generates TCL commands to assign address to the clock wizard.
     * @return String containing TCL commands for clock wizard address assignment.
     */
    std::string assignClkWizAddr();

    /**
     * @brief Generates TCL commands for segmented design configuration.
     * @return String containing TCL commands for segmented design setup.
     */
    std::string setSegmented();

    /**
     * @brief Generates TCL commands to add an AXI crossbar.
     * @param numSlaves Number of slave interfaces.
     * @return String containing TCL commands for crossbar creation.
     */
    std::string addXbar(uint8_t numSlaves);

    /**
     * @brief Generates TCL commands to connect the crossbar to NoC.
     * @return String containing TCL commands for crossbar-NoC connections.
     */
    std::string connectXbarToNoC();

    /**
     * @brief Generates TCL commands for QDMA streaming setup.
     * @return String containing TCL commands for QDMA streaming.
     */
    std::string setupQdmaStreaming();

    /**
     * @brief Generates TCL commands to add Host-to-Card AXI-Stream router.
     * @return String containing TCL commands for H2C router.
     */
    std::string addH2CAxisRouter();

    /**
     * @brief Generates TCL commands to add Card-to-Host AXI-Stream router.
     * @return String containing TCL commands for C2H router.
     */
    std::string addC2HAxisRouter();

    /**
     * @brief Generates TCL commands to connect QDMA H2C to router.
     * @return String containing TCL commands for QDMA H2C connections.
     */
    std::string connectQdmaH2CToRouter();

    /**
     * @brief Generates TCL commands to connect QDMA C2H to router.
     * @return String containing TCL commands for QDMA C2H connections.
     */
    std::string connectQdmaC2HToRouter();

    /**
     * @brief Generates TCL commands to add QDMA logic.
     * @return String containing TCL commands for QDMA logic addition.
     */
    std::string addQdmaLogic();

    /**
     * @brief Generates TCL commands to connect QDMA logic.
     * @return String containing TCL commands for QDMA logic connections.
     */
    std::string connectQdmaLogic();

    /**
     * @brief Generates TCL commands to assign GPIO address for QDMA logic.
     * @return String containing TCL commands for QDMA logic GPIO address.
     */
    std::string assignQdmaLogicGpioAddr();

    /**
     * @brief Generates TCL commands to add Host-to-Card FIFO.
     * @return String containing TCL commands for H2C FIFO creation.
     */
    std::string addH2CFifo();

    /**
     * @brief Generates TCL commands to configure AXI-Lite slaves for simulation platform.
     * @return String containing TCL commands for simulation AXI-Lite slaves.
     */
    std::string configNumberOfAXILiteSlavesSim();

    /**
     * @brief Generates TCL commands to configure AXI-Full slaves for simulation platform.
     * @return String containing TCL commands for simulation AXI-Full slaves.
     */
    std::string configNumberOfAXIFullSlavesSim();

    /**
     * @brief Generates TCL commands to connect interfaces in simulation platform.
     * @param krnl_name Name of the kernel.
     * @param intf Interface to connect.
     * @param idx Index of the kernel.
     * @return String containing TCL commands for simulation interface connections.
     */
    std::string connectInterfaceSim(std::string krnl_name, Interface intf, int idx);

    /**
     * @brief Generates TCL commands to create IPs for simulation platform.
     * @param idx Index of the kernel.
     * @return String containing TCL commands for simulation IP creation.
     */
    std::string createIpSim(int idx);

    /**
     * @brief Generates TCL commands to assign slave addresses for simulation platform.
     * @param krnl_name Name of the kernel.
     * @param idx Index of the kernel.
     * @param intf Interface to assign address to.
     * @param base_addr Base address for the interface.
     * @return String containing TCL commands for simulation address assignment.
     */
    std::string assignSlaveAddressSim(std::string krnl_name, int idx, Interface intf,
                                      uint64_t base_addr);

    /**
     * @brief Generates TCL commands to connect AXI-Stream in simulation platform.
     * @param krnl_name Name of the kernel.
     * @return String containing TCL commands for simulation AXI-Stream connections.
     */
    std::string connectAxisSim(std::string krnl_name);

    /**
     * @brief Generates TCL commands for the header of the run_pre.tcl script.
     * @return String containing TCL commands for script header.
     */
    std::string addRunPreHeader();
};
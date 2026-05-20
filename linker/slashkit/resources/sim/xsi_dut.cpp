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

#include "xsi_dut.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "sim_exec_log.hpp"

using namespace std;

XSI_DUT::XSI_DUT(const string& design_libname, const string& simkernel_libname,
                 const string& reset_name, bool reset_active_low, const string& clock_name,
                 float clock_period_ns, const string& wdbName, bool trace)
    : xsi(design_libname, simkernel_libname) {
    s_xsi_setup_info info;
    memset(&info, 0, sizeof(info));
    info.logFileName = NULL;
    info.wdbFileName = const_cast<char*>(wdbName.c_str());
    xsi.open(&info);
    SIM_EXEC_LOG(std::cout << "XSI opened" << std::endl);
    if (trace) {
        SIM_EXEC_LOG(std::cout << "Waveform enabled" << std::endl);
        xsi.trace_all();
    }
    for (int i = 0; i < xsi.get_num_ports(); i++) {
        string port_name(xsi.get_port_name(i));
        port_parameters p = {i, xsi.get_port_bits(i), xsi.port_is_input(i)};
        port_map[port_name] = p;
    }
    if (port_map.find(reset_name) == port_map.end())
        throw invalid_argument("Reset not found in ports list");
    if (port_map[reset_name].port_bits != 1) throw invalid_argument("Reset is not a scalar");
    if (!port_map[reset_name].is_input) throw invalid_argument("Reset is not an input port");
    rst = reset_name;
    rst_active_low = reset_active_low;
    if (port_map.find(clock_name) == port_map.end())
        throw invalid_argument("Clock not found in ports list");
    if (port_map[clock_name].port_bits != 1) throw invalid_argument("Clock is not a scalar");
    if (!port_map[clock_name].is_input) throw invalid_argument("Clock is not an input port");
    clk = clock_name;
    clk_half_period = (unsigned int)(clock_period_ns * pow(10, -9) / xsi.get_time_precision() / 2);
    if (clk_half_period == 0) throw invalid_argument("Calculated half period is zero");
    SIM_EXEC_LOG(std::cout << "Using " << rst << " as "
                           << (rst_active_low ? "active-low" : "active-high") << " reset"
                           << endl);
    SIM_EXEC_LOG(std::cout << "Using " << clk << " as clock with half-period of "
                           << clk_half_period << " simulation steps" << endl);
    cycle_count = 0;
}

void XSI_DUT::close() { xsi.close(); }

XSI_DUT::~XSI_DUT() { xsi.close(); }

void XSI_DUT::run_ncycles(unsigned int n) {
    for (int i = 0; i < n; i++) {
        write(clk, 0);
        xsi.run(clk_half_period);
        write(clk, 1);
        xsi.run(clk_half_period);
        cycle_count++;
    }
}

uint64_t XSI_DUT::get_cycle_count() { return cycle_count; }

void XSI_DUT::list_ports() {
    map<string, port_parameters>::iterator it = port_map.begin();
    while (it != port_map.end()) {
        SIM_EXEC_LOG(std::cout << it->first << " (ID: " << it->second.port_id << ", "
                               << it->second.port_bits << "b, "
                               << (it->second.is_input ? "I)" : "O)") << endl);
        it++;
    }
}

void XSI_DUT::reset_design() {
    map<string, port_parameters>::iterator it = port_map.begin();
    while (it != port_map.end()) {
        if (it->second.is_input) {
            write(it->first, 0);
        }
        it++;
    }
    write(rst, rst_active_low ? 0 : 1);
    run_ncycles(10);
    write(rst, rst_active_low ? 1 : 0);
    run_ncycles(10);
}

void XSI_DUT::rewind() { xsi.restart(); }

int XSI_DUT::num_ports() { return port_map.size(); }

void XSI_DUT::write(const std::string& port_name, unsigned int val) {
    if (!port_map[port_name].is_input) {
        throw invalid_argument("Write called on output port");
    }
    unsigned int nwords =
        (port_map[port_name].port_bits + 31) / 32;  // find how many 32-bit chunks we need
    vector<s_xsi_vlog_logicval> logic_val(nwords);
    logic_val.at(0) = (s_xsi_vlog_logicval){val, 0};
    for (int i = 1; i < nwords; i++) {
        logic_val.at(i) = (s_xsi_vlog_logicval){0, 0};  // only two-valued logic
    }
    xsi.put_value(port_map[port_name].port_id, logic_val.data());
}

unsigned int XSI_DUT::read(const std::string& port_name) {
    unsigned int nwords =
        (port_map[port_name].port_bits + 31) / 32;  // find how many 32-bit chunks we need
    if (nwords > 1) {
        throw invalid_argument("uint = read(string name) applies only to signals of 32b or less");
    }
    vector<s_xsi_vlog_logicval> logic_val(nwords);
    xsi.get_value(port_map[port_name].port_id, logic_val.data());
    return logic_val.at(0).aVal;
}

void XSI_DUT::set(const string& port_name) {
    if (port_map[port_name].port_bits != 1) {
        throw invalid_argument("set() applies only to scalars");
    }
    write(port_name, 1);
}

void XSI_DUT::clear(const string& port_name) {
    if (port_map[port_name].port_bits != 1) {
        throw invalid_argument("clear() applies only to scalars");
    }
    write(port_name, 0);
}

bool XSI_DUT::test(const string& port_name) {
    if (port_map[port_name].port_bits != 1) {
        throw invalid_argument("test() applies only to scalars");
    }
    unsigned int ret = read(port_name);
    return (ret == 1);
}

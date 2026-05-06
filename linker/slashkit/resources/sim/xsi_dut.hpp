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

#ifndef XSI_DUT_HPP
#define XSI_DUT_HPP

#include <cstdint>
#include <iostream>
#include <map>
#include <string>

#include "ap_int.h"
#include "xsi_loader.hpp"

typedef struct {
    int port_id;
    int port_bits;
    bool is_input;
} port_parameters;

class XSI_DUT {
   public:
    XSI_DUT(const std::string &design_libname, const std::string &simkernel_libname,
            const std::string &reset_name, bool reset_active_low, const std::string &clock_name,
            float clock_period_ns, const std::string &wdb_name, bool trace = true);
    ~XSI_DUT();
    void list_ports();
    int num_ports();
    void reset_design();
    void rewind();
    void run_ncycles(unsigned int n);
    template <unsigned int W>
    void write(const std::string &port_name, ap_uint<W> val);
    template <unsigned int W>
    ap_uint<W> read(const std::string &port_name);
    void write(const std::string &port_name, unsigned int val);
    unsigned int read(const std::string &port_name);
    void set(const std::string &port_name);
    void clear(const std::string &port_name);
    bool test(const std::string &port_name);
    uint64_t get_cycle_count();
    void close();

   private:
    // global instance of the XSI object
    Xsi::Loader xsi;
    // port map
    std::map<std::string, port_parameters> port_map;
    // names of clock and reset
    std::string clk;
    std::string rst;
    unsigned int clk_half_period;
    bool rst_active_low;
    uint64_t cycle_count;
};

template <unsigned int W>
void XSI_DUT::write(const std::string &port_name, ap_uint<W> val) {
    if (W != port_map[port_name].port_bits) {
        throw std::invalid_argument("Value bitwidth does not match port bitwidth");
    }
    constexpr int nwords = (W + 31) / 32;  // find how many 32-bit chunks we need
    s_xsi_vlog_logicval logic_val[nwords];
    for (int i = 0; i < nwords; i++) {
        logic_val[i] = (s_xsi_vlog_logicval){
            (XSI_UINT32)val(std::min((unsigned int)(32 * (i + 1) - 1), W - 1), 32 * i),
            0};  // only two-valued logic
    }
    xsi.put_value(port_map[port_name].port_id, logic_val);
}

template <unsigned int W>
ap_uint<W> XSI_DUT::read(const std::string &port_name) {
    if (W != port_map[port_name].port_bits) {
        throw std::invalid_argument("Return value bitwidth does not match port bitwidth");
    }
    constexpr int nwords = (W + 31) / 32;  // find how many 32-bit chunks we need
    s_xsi_vlog_logicval logic_val[nwords];
    ap_uint<W> ret;
    xsi.get_value(port_map[port_name].port_id, logic_val);
    for (int i = 0; i < nwords; i++) {
        ret(std::min((unsigned int)(32 * (i + 1) - 1), W - 1), 32 * i) =
            logic_val[i].aVal;  // only two-valued logic
    }
    return ret;
}

#endif  // XSI_DUT_HPP
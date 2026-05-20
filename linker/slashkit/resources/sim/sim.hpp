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

#ifndef SIM_HPP
#define SIM_HPP

#include <string>

enum axi_fsm_state {
    VALID_ADDR,
    CONTINUE_ADDR,
    READY_ADDR,
    CLEAR_ADDR,
    VALID_DATA,
    READY_DATA,
    UPDATE_DATA,
    CLEAR_DATA,
    VALID_ACK,
    READY_ACK,
    CLEAR_ACK
};

struct axilite {
    std::string basename = "";
    std::string araddr() const { return basename + "_araddr"; }
    std::string arvalid() const { return basename + "_arvalid"; }
    std::string arready() const { return basename + "_arready"; }
    std::string rready() const { return basename + "_rready"; }
    std::string rvalid() const { return basename + "_rvalid"; }
    std::string rdata() const { return basename + "_rdata"; }
    std::string awaddr() const { return basename + "_awaddr"; }
    std::string awvalid() const { return basename + "_awvalid"; }
    std::string awready() const { return basename + "_awready"; }
    std::string wready() const { return basename + "_wready"; }
    std::string wvalid() const { return basename + "_wvalid"; }
    std::string wdata() const { return basename + "_wdata"; }
    std::string wstrb() const { return basename + "_wstrb"; }
    std::string bready() const { return basename + "_bready"; }
    std::string bvalid() const { return basename + "_bvalid"; }
    std::string bresp() const { return basename + "_bresp"; }
    axilite(const std::string &name) : basename(name){};
};

struct axistream {
    std::string basename = "";
    std::string tdata() const { return basename + "_tdata"; }
    std::string tvalid() const { return basename + "_tvalid"; }
    std::string tlast() const { return basename + "_tlast"; }
    std::string tready() const { return basename + "_tready"; }
    std::string tdest() const { return basename + "_tdest"; }
    std::string tkeep() const { return basename + "_tkeep"; }
    axistream(const std::string &name) : basename(name){};
};

struct aximm {
    std::string basename = "";
    std::string araddr() const { return basename + "_araddr"; }
    std::string arburst() const { return basename + "_arburst"; }
    std::string arcache() const { return basename + "_arcache"; }
    std::string arid() const { return basename + "_arid"; }
    std::string arlen() const { return basename + "_arlen"; }
    std::string arlock() const { return basename + "_arlock"; }
    std::string arprot() const { return basename + "_arprot"; }
    std::string arqos() const { return basename + "_arqos"; }
    std::string arready() const { return basename + "_arready"; }
    std::string arsize() const { return basename + "_arsize"; }
    std::string aruser() const { return basename + "_aruser"; }
    std::string arvalid() const { return basename + "_arvalid"; }
    std::string awaddr() const { return basename + "_awaddr"; }
    std::string awburst() const { return basename + "_awburst"; }
    std::string awcache() const { return basename + "_awcache"; }
    std::string awid() const { return basename + "_awid"; }
    std::string awlen() const { return basename + "_awlen"; }
    std::string awlock() const { return basename + "_awlock"; }
    std::string awprot() const { return basename + "_awprot"; }
    std::string awqos() const { return basename + "_awqos"; }
    std::string awready() const { return basename + "_awready"; }
    std::string awsize() const { return basename + "_awsize"; }
    std::string awuser() const { return basename + "_awuser"; }
    std::string awvalid() const { return basename + "_awvalid"; }
    std::string bid() const { return basename + "_bid"; }
    std::string bready() const { return basename + "_bready"; }
    std::string bresp() const { return basename + "_bresp"; }
    std::string bvalid() const { return basename + "_bvalid"; }
    std::string rdata() const { return basename + "_rdata"; }
    std::string rid() const { return basename + "_rid"; }
    std::string rlast() const { return basename + "_rlast"; }
    std::string rready() const { return basename + "_rready"; }
    std::string rresp() const { return basename + "_rresp"; }
    std::string rvalid() const { return basename + "_rvalid"; }
    std::string wdata() const { return basename + "_wdata"; }
    std::string wlast() const { return basename + "_wlast"; }
    std::string wready() const { return basename + "_wready"; }
    std::string wstrb() const { return basename + "_wstrb"; }
    std::string wvalid() const { return basename + "_wvalid"; }
    aximm(const std::string &name) : basename(name){};
};

#endif  // SIM_HPP
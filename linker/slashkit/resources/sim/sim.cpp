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

#include "sim.hpp"

#include <json/json.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <zmq.hpp>

#include "sim_exec_log.hpp"
#include "xsi_dut.hpp"
#include "xsi_loader.hpp"

const axilite control("s_axi_ctrl");
const aximm mem("mem");
std::mutex mtx;

std::condition_variable cv_control_read;
std::condition_variable cv_control_write;
std::condition_variable cv_mem_read;
std::condition_variable cv_mem_write;

bool control_read_busy = false;
bool control_write_busy = false;
bool mem_read_busy = false;
bool mem_write_busy = false;

void control_read_fsm(XSI_DUT* dut, std::queue<ap_uint<64>>& addr, std::queue<uint32_t>& retVal) {
    static axi_fsm_state state = VALID_ADDR;
    dut->write("s_axi_ctrl_arburst", 1);  // set burst to incr
    switch (state) {
        case VALID_ADDR:
            if (!addr.empty()) {
                std::unique_lock<std::mutex> lock(mtx);
                control_read_busy = true;
                auto ad = addr.front();
                addr.pop();
                dut->write<64>(control.araddr(), ad);
                dut->set(control.arvalid());
                if (dut->test(control.arready())) {
                    state = CLEAR_ADDR;
                } else {
                    state = READY_ADDR;
                }
            }
            return;
        case READY_ADDR:
            if (dut->test(control.arready())) {
                state = CLEAR_ADDR;
            }
            return;
        case CLEAR_ADDR:
            dut->clear(control.arvalid());
            dut->set(control.rready());

            state = READY_DATA;
            return;
        case READY_DATA:
            if (dut->test(control.rvalid())) {
                retVal.push(dut->read<32>(control.rdata()));
                state = CLEAR_DATA;
            }
            return;
        case CLEAR_DATA:
            dut->clear(control.rready());
            state = VALID_ADDR;

            {
                std::unique_lock<std::mutex> lock(mtx);
                control_read_busy = false;
                cv_control_read.notify_all();
            }
            return;
    }
}

void control_write_fsm(XSI_DUT* dut, std::queue<ap_uint<64>>& addr, std::queue<uint32_t>& data) {
    dut->write("s_axi_ctrl_awburst", 1);  // set burst to incr
    dut->write("s_axi_ctrl_wlast", 1);    // set wlast to 1 so bvalid asserts at the end
    static axi_fsm_state state = VALID_ADDR;
    ap_uint<32> value;
    switch (state) {
        case VALID_ADDR:
            if (!addr.empty()) {
                std::unique_lock<std::mutex> lock(mtx);
                control_write_busy = true;
                ap_uint<64> popAddr = addr.front();
                addr.pop();
                dut->write<64>(control.awaddr(), popAddr);
                dut->set(control.awvalid());
                if (dut->test(control.awready())) {
                    state = CLEAR_ADDR;
                } else {
                    state = READY_ADDR;
                }
            }
            return;
        case READY_ADDR:
            if (dut->test(control.awready())) {
                state = CLEAR_ADDR;
            }
            return;
        case CLEAR_ADDR:
            dut->clear(control.awvalid());
            state = VALID_DATA;
            return;
        case VALID_DATA:
            value = data.front();
            data.pop();
            dut->write<32>(control.wdata(), value);
            dut->write(control.wstrb(), 0xF);
            dut->set(control.wvalid());
            if (dut->test(control.wready())) {
                state = CLEAR_DATA;
            } else {
                state = READY_DATA;
            }
            return;
        case READY_DATA:
            if (dut->test(control.wready())) {
                state = CLEAR_DATA;
            }
            return;
        case CLEAR_DATA:
            dut->clear(control.wvalid());
            state = VALID_ACK;
            return;
        case VALID_ACK:
            dut->set(control.bready());
            if (dut->test(control.bvalid())) {
                state = CLEAR_ACK;
            } else {
                state = READY_ACK;
            }
            return;
        case READY_ACK:
            if (dut->test(control.bvalid())) {
                state = CLEAR_ACK;
            }
            return;
        case CLEAR_ACK:
            dut->clear(control.bready());
            state = VALID_ADDR;
            {
                std::unique_lock<std::mutex> lock(mtx);
                control_write_busy = false;
                cv_control_write.notify_all();
            }
            return;
    }
}

void mem_read_fsm(XSI_DUT* dut, std::queue<ap_uint<64>>& addr, std::queue<uint32_t>& len,
                  std::queue<uint64_t>& ret) {
    static axi_fsm_state state = VALID_ADDR;
    static ap_uint<64> curr_addr = 0;
    static unsigned int curr_nbytes = 0;
    static ap_uint<8> nbeats = 0;
    unsigned int nbytes_to_4k_boundary, nbytes_this_transfer;

    switch (state) {
        case VALID_ADDR:
            if (!addr.empty()) {
                std::unique_lock<std::mutex> lock(mtx);
                mem_read_busy = true;
                curr_addr = addr.front();
                addr.pop();
                curr_nbytes = len.front();
                len.pop();
                nbytes_to_4k_boundary = (curr_addr / 2048 + 1) * 2048 - curr_addr;
                nbytes_this_transfer = std::min(curr_nbytes, nbytes_to_4k_boundary);
                nbeats = (nbytes_this_transfer + 7) / 8;  // 64b data, 8B per beat
                SIM_EXEC_LOG(std::cout << "Read start addr: " << curr_addr
                                       << " len: " << nbytes_this_transfer << " (" << nbeats
                                       << ")" << std::endl);
                dut->write(mem.arsize(), 3);          // set size to 8B
                dut->write(mem.arlen(), nbeats - 1);  // set len to nbeats - 1
                dut->write(mem.arburst(), 1);         // set burst to incr
                dut->write<64>(mem.araddr(), curr_addr);
                dut->set(mem.arvalid());
                if (dut->test(mem.arready())) {
                    state = CLEAR_ADDR;
                } else {
                    state = READY_ADDR;
                }
                curr_addr += nbytes_this_transfer;
                curr_nbytes -= nbytes_this_transfer;
            }
            return;

        case CONTINUE_ADDR:
            nbytes_to_4k_boundary = (curr_addr / 2048 + 1) * 2048 - curr_addr;
            nbytes_this_transfer = std::min(curr_nbytes, nbytes_to_4k_boundary);
            nbeats = (nbytes_this_transfer + 7) / 8;  // 64b data, 8B per beat
            SIM_EXEC_LOG(std::cout << "Read continue addr: " << curr_addr
                                   << " len: " << nbytes_this_transfer << " (" << nbeats << ")"
                                   << std::endl);
            dut->write(mem.arsize(), 3);          // set size to 8B
            dut->write(mem.arlen(), nbeats - 1);  // set len to nbeats - 1
            dut->write(mem.arburst(), 1);         // set burst to incr
            dut->write<64>(mem.araddr(), curr_addr);
            dut->set(mem.arvalid());
            if (dut->test(mem.arready())) {
                state = CLEAR_ADDR;
            } else {
                state = READY_ADDR;
            }
            curr_addr += nbytes_this_transfer;
            curr_nbytes -= nbytes_this_transfer;
            return;

        case READY_ADDR:
            if (dut->test(mem.arready())) {
                state = CLEAR_ADDR;
            }
            return;

        case CLEAR_ADDR:
            dut->clear(mem.arvalid());
            dut->set(mem.rready());
            state = READY_DATA;
            return;

        case READY_DATA:
            if (dut->test(mem.rvalid())) {
                nbeats--;
                ret.push(dut->read<64>(mem.rdata()));
                if (nbeats == 0) {
                    state = CLEAR_DATA;
                }
            }
            return;

        case CLEAR_DATA:
            dut->clear(mem.rready());
            if (curr_nbytes == 0) {
                state = VALID_ADDR;
                {
                    std::unique_lock<std::mutex> lock(mtx);
                    mem_read_busy = false;
                    cv_mem_read.notify_all();
                }
            } else {
                state = CONTINUE_ADDR;
            }
            return;
    }
}

void mem_write_fsm(XSI_DUT* dut, std::queue<ap_uint<64>>& addr, std::queue<uint32_t>& len,
                   std::queue<ap_uint<64>>& val) {
    static axi_fsm_state state = VALID_ADDR;
    static ap_uint<64> curr_addr = 0;
    static unsigned int curr_nbytes = 0;
    static ap_uint<8> nbeats = 0;
    unsigned int nbytes_to_4k_boundary, nbytes_this_transfer;
    switch (state) {
        case VALID_ADDR:
            if (!addr.empty()) {
                std::unique_lock<std::mutex> lock(mtx);
                mem_write_busy = true;
                curr_addr = addr.front();
                addr.pop();
                curr_nbytes = len.front();
                len.pop();
                nbytes_to_4k_boundary = (curr_addr / 2048 + 1) * 2048 - curr_addr;
                nbytes_this_transfer = std::min(nbytes_to_4k_boundary, curr_nbytes);
                nbeats = (nbytes_this_transfer + 7) / 8;  // number of 64B beats in transfer
                SIM_EXEC_LOG(std::cout << "Write start addr=" << curr_addr
                                       << " len=" << nbytes_this_transfer << " (" << nbeats
                                       << ")" << std::endl);
                dut->write(mem.awsize(), 3);  // 64B width
                dut->write(mem.awlen(), nbeats - 1);
                dut->write(mem.awburst(), 1);  // INCR
                dut->write<64>(mem.awaddr(), curr_addr);
                dut->set(mem.awvalid());
                if (dut->test(mem.awready())) {
                    state = CLEAR_ADDR;
                } else {
                    state = READY_ADDR;
                }
                curr_addr += nbytes_this_transfer;
                curr_nbytes -= nbytes_this_transfer;
            }
            return;
        case CONTINUE_ADDR:
            nbytes_to_4k_boundary = (curr_addr / 2048 + 1) * 2048 - curr_addr;
            nbytes_this_transfer = std::min(nbytes_to_4k_boundary, curr_nbytes);
            nbeats = (nbytes_this_transfer + 7) / 8;  // number of 64B beats in transfer
            SIM_EXEC_LOG(std::cout << "Write continue addr=" << curr_addr
                                   << " len=" << nbytes_this_transfer << " (" << nbeats << ")"
                                   << std::endl);
            dut->write(mem.awsize(), 3);  // 64B width
            dut->write(mem.awlen(), nbeats - 1);
            dut->write(mem.awburst(), 1);  // INCR
            dut->write<64>(mem.awaddr(), curr_addr);
            dut->set(mem.awvalid());
            if (dut->test(mem.awready())) {
                state = CLEAR_ADDR;
            } else {
                state = READY_ADDR;
            }
            curr_addr += nbytes_this_transfer;
            curr_nbytes -= nbytes_this_transfer;
            return;
        case READY_ADDR:
            if (dut->test(mem.awready())) {
                state = CLEAR_ADDR;
            }
            return;
        case CLEAR_ADDR:
            dut->clear(mem.awvalid());
            state = VALID_DATA;
            return;
        case VALID_DATA:
            if (!val.empty()) {
                dut->write<64>(mem.wdata(), val.front());
                val.pop();
                dut->write<8>(mem.wstrb(), 0xFF);
                dut->write(mem.wlast(), nbeats == 1);
                dut->set(mem.wvalid());
                if (dut->test(mem.wready())) {
                    nbeats--;
                    if (nbeats != 0) {
                        state = UPDATE_DATA;
                    } else {
                        state = CLEAR_DATA;
                    }
                } else {
                    state = READY_DATA;
                }
            }
            return;
        case READY_DATA:
            if (dut->test(mem.wready())) {
                nbeats--;
                if (nbeats != 0) {
                    state = UPDATE_DATA;
                } else {
                    state = CLEAR_DATA;
                }
            }
            return;
        case UPDATE_DATA:
            if (!val.empty()) {
                dut->write<64>(mem.wdata(), val.front());
                val.pop();
                dut->write<8>(mem.wstrb(), 0xFF);
                dut->write(mem.wlast(), nbeats == 1);
                if (dut->test(mem.wready())) {
                    nbeats--;
                    if (nbeats != 0) {
                        state = UPDATE_DATA;
                    } else {
                        state = CLEAR_DATA;
                    }
                } else {
                    state = READY_DATA;
                }
            }
            return;
        case CLEAR_DATA:
            dut->clear(mem.wvalid());
            state = VALID_ACK;
            return;
        case VALID_ACK:
            dut->set(mem.bready());
            if (dut->test(mem.bvalid())) {
                state = CLEAR_ACK;
            } else {
                state = READY_ACK;
            }
            return;
        case READY_ACK:
            if (dut->test(mem.bvalid())) {
                state = CLEAR_ACK;
            }
            return;
        case CLEAR_ACK:
            dut->clear(mem.bready());
            if (curr_nbytes == 0) {
                state = VALID_ADDR;
                {
                    std::unique_lock<std::mutex> lock(mtx);
                    mem_write_busy = false;
                    cv_mem_write.notify_all();
                }
            } else {
                state = CONTINUE_ADDR;
            }
            return;
    }
}

bool stop = false;
bool start = false;

void finish(int signum) { stop = true; }

std::queue<ap_uint<64>> axiReadAddr;
std::queue<uint32_t> axiReadVal;
std::queue<ap_uint<64>> axiWriteAddr;
std::queue<uint32_t> axiWriteData;

std::queue<ap_uint<64>> memReadAddr;
std::queue<uint32_t> memReadLen;
std::queue<uint64_t> memReadVal;

std::queue<ap_uint<64>> memWriteAddr;
std::queue<uint32_t> memWriteLen;
std::queue<ap_uint<64>> memWriteData;

template <typename T>
Json::Value createJsonValue(const T& var) {
    Json::Value value;
    value = *reinterpret_cast<const uint32_t*>(&var);
    return value;
}

void writeBuffer(ap_uint<64> addr, std::vector<uint8_t> data) {
    uint64_t len = data.size();
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv_mem_write.wait(lock, [] { return !mem_write_busy; });
        mem_write_busy = true;
        memWriteAddr.push(addr);
        memWriteLen.push(len);
    }

    ap_uint<64> temp = 0;

    {
        std::unique_lock<std::mutex> lock(mtx);
        for (std::size_t i = 0; i < data.size(); i++) {
            temp |= static_cast<uint64_t>(data[i]) << ((i % 8) * 8);
            if (i % 8 == 7 || i == data.size() - 1) {
                memWriteData.push(temp);
                temp = 0;
            }
        }
    }

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv_mem_write.wait(lock, [] { return !mem_write_busy; });
    }
}

Json::Value createJsonBuffer(const uint8_t* buffer, size_t size) {
    Json::Value value(Json::arrayValue);
    for (size_t i = 0; i < size; ++i) {
        value.append(buffer[i]);
    }
    return value;
}

void fetchBuffer(ap_uint<64> addr, uint64_t len, std::vector<uint8_t>& data) {
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv_mem_read.wait(lock, [] { return !mem_read_busy; });
        mem_read_busy = true;
        memReadAddr.push(addr);
        memReadLen.push(len);
    }
    SIM_EXEC_LOG(std::cout << std::hex << "Reading from address: " << addr << " length: " << len
                           << std::endl);
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv_mem_read.wait(lock, [] { return !mem_read_busy; });
    }
    while (data.size() < len && !memReadVal.empty()) {
        uint64_t temp = memReadVal.front();
        memReadVal.pop();
        SIM_EXEC_LOG(std::cout << std::showbase << std::hex << "Read value: " << temp << std::dec
                               << std::endl);
        for (int i = 0; i < 8 && data.size() < len; ++i) {
            data.push_back(static_cast<uint8_t>((temp >> (i * 8)) & 0xFF));
        }
    }
}

void fetchScalar(ap_uint<64> addr, uint32_t& data) {
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv_control_read.wait(lock, [] { return !control_read_busy; });
        control_read_busy = true;
        axiReadAddr.push(addr);
    }

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv_control_read.wait(lock, [] { return !control_read_busy; });
    }

    if (!axiReadVal.empty()) {
        data = (uint32_t)axiReadVal.front();
        axiReadVal.pop();
    }
}

void zmq_ctx_setup_and_run() {
    zmq::context_t context(1);
    zmq::socket_t socket(context, ZMQ_REP);
    socket.bind("tcp://*:5555");

    while (!stop) {
        zmq::message_t request;
        (void)socket.recv(request, zmq::recv_flags::none);
        std::string req_str(static_cast<char*>(request.data()), request.size());
        Json::Value root;
        Json::Reader reader;
        reader.parse(req_str, root);
        std::string command = root["command"].asString();

        if (command == "populate") {
            uint64_t addr = root["addr"].asUInt64();
            uint64_t bufferSize = root["size"].asUInt64();
            zmq::message_t data;
            (void)socket.recv(data, zmq::recv_flags::none);
            void* buffer = new uint8_t[bufferSize];
            std::memcpy(buffer, data.data(), bufferSize);
            std::vector<uint8_t> vec(static_cast<uint8_t*>(buffer),
                                     static_cast<uint8_t*>(buffer) + bufferSize);
            socket.send(zmq::message_t("OK", 2), zmq::send_flags::none);
            SIM_EXEC_LOG(std::cout << "Received data of size: " << std::hex << bufferSize
                                   << " at address: " << addr << std::endl);

            { writeBuffer(addr, vec); }
        } else if (command == "fetch") {
            std::string type = root["type"].asString();
            Json::Value response;
            if (type == "buffer") {
                uint64_t addr = root["addr"].asUInt64();
                uint64_t bufferSize = root["size"].asUInt64();  // sent as no of bytes
                SIM_EXEC_LOG(std::cout << "Fetching buffer of size: " << std::dec << bufferSize
                                       << " from address: " << std::hex << addr << std::endl);
                std::vector<uint8_t> vec;
                { fetchBuffer(addr, bufferSize, vec); }
                response = createJsonBuffer(vec.data(), vec.size());

            } else if (type == "scalar") {
                uint64_t addr = root["addr"].asUInt64();
                uint32_t val = 0;
                { fetchScalar(addr, val); }
                response = createJsonValue(val);

            } else {
                std::cerr << "Unknown fetch type" << std::endl;
            }
            std::string responseStr = Json::writeString(Json::StreamWriterBuilder(), response);
            socket.send(zmq::message_t(responseStr.c_str(), responseStr.size()),
                        zmq::send_flags::none);
        } else if (command == "exit") {
            stop = true;
            start = false;
            socket.send(zmq::message_t("OK", 2), zmq::send_flags::none);
        } else if (command == "reg") {
            uint64_t addr = root["addr"].asUInt64();
            uint32_t val = root["val"].asUInt();
            SIM_EXEC_LOG(std::cout << "Writing value: " << std::hex << "0x" << val
                                   << " to address: " << addr << std::endl);
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv_control_write.wait(lock, [] { return !control_write_busy; });
                control_write_busy = true;
                axiWriteAddr.push(addr);
                axiWriteData.push(val);
            }
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv_control_write.wait(lock, [] { return !control_write_busy; });
            }

            socket.send(zmq::message_t("OK", 2), zmq::send_flags::none);

        } else if (command == "start") {
            start = true;
            socket.send(zmq::message_t("OK", 2), zmq::send_flags::none);
        }
    }
}

int main() {
    std::string simengine_libname = "libxv_simulator_kernel.so";
    std::string design_libname = "xsim.dir/top_wrapper_behav/xsimk.so";
    SIM_EXEC_LOG(std::cout << "Sim Engine DLL: " << simengine_libname << std::endl);
    SIM_EXEC_LOG(std::cout << "Design DLL: " << design_libname << std::endl);
    XSI_DUT dut(design_libname, simengine_libname, "rst", true, "clk", 4, "test.wdb", true);

    SIM_EXEC_LOG(std::cout << "DUT initialized");
    SIM_EXEC_LOG(std::cout << "Initial cycle count: " << dut.get_cycle_count() << std::endl);
    dut.reset_design();
    SIM_EXEC_LOG(std::cout << "Cycle count after reset: " << dut.get_cycle_count() << std::endl);
    signal(SIGINT, finish);

    std::thread worker(zmq_ctx_setup_and_run);

    while (!stop) {
        control_read_fsm(&dut, axiReadAddr, axiReadVal);
        control_write_fsm(&dut, axiWriteAddr, axiWriteData);
        mem_read_fsm(&dut, memReadAddr, memReadLen, memReadVal);
        mem_write_fsm(&dut, memWriteAddr, memWriteLen, memWriteData);
        dut.run_ncycles((start) ? 1 : 0);
    }
    worker.join();

    dut.close();
}

# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
# 
#  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
#  and associated documentation files (the "Software"), to deal in the Software without restriction,
#  including without limitation the rights to use, copy, modify, merge, publish, distribute,
#  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
# 
#  The above copyright notice and this permission notice shall be included in all copies or
#  substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################

cmake_minimum_required(VERSION 3.10)
project(Simulation)

set(VIVADO_BIN_DIR "$ENV{XILINX_VIVADO}/bin")
set(XSI_INCLUDE_DIR "${VIVADO_BIN_DIR}/../data/xsim/include")
set(GCC_COMPILER "/usr/bin/g++")
set(OUT_EXE "vpp_sim")

if (NOT EXISTS $ENV{XILINX_HLS})
  message(FATAL_ERROR "Xilinx HLS not loaded!")
endif (NOT EXISTS $ENV{XILINX_HLS})

include_directories(${XSI_INCLUDE_DIR} $ENV{XILINX_HLS}/include/ /usr/include/jsoncpp/)

add_executable(${OUT_EXE} sim.cpp xsi_dut.cpp xsi_loader.cpp)

target_link_libraries(${OUT_EXE} dl rt jsoncpp zmq)
# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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

add_executable(
    v80-smi
    
    debug/bar_poke.cpp
    debug/clockwiz.cpp
    debug/mem_poke.cpp
    inspect.cpp
    list.cpp
    program.cpp
    reset.cpp
    validate.cpp
    smi.cpp
)

if(SMI_ENABLE_QDMA_DRIVER_BACKEND)
    target_sources(v80-smi PRIVATE qdma_driver_backend.cpp)

    # Off-the-shelf Xilinx QDMA driver netlink UAPI header (qdma_nl.h), used by
    # the --use-qdma-driver validate backend.
    set(QDMA_DRV_APPS_INCLUDE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../submodules/qdma_drv/QDMA/linux-kernel/apps/include")
    if(NOT EXISTS "${QDMA_DRV_APPS_INCLUDE}/qdma_nl.h")
      message(FATAL_ERROR
        "Missing ${QDMA_DRV_APPS_INCLUDE}/qdma_nl.h. "
        "Initialize submodules (git submodule update --init submodules/qdma_drv), "
        "or configure with -DSMI_ENABLE_QDMA_DRIVER_BACKEND=OFF.")
    endif()
endif()

target_compile_features(v80-smi PRIVATE cxx_std_20)

target_include_directories(
    v80-smi

    PRIVATE

    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/../generated # For version.hpp
)

if(SMI_ENABLE_QDMA_DRIVER_BACKEND)
    target_include_directories(v80-smi PRIVATE ${QDMA_DRV_APPS_INCLUDE})
    target_compile_definitions(v80-smi PRIVATE SMI_ENABLE_QDMA_DRIVER_BACKEND=1)
endif()

target_link_libraries(
    v80-smi

    PRIVATE

    vrt::vrt
    slash::slash
    CLI11::CLI11
)

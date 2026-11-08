// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT

#ifndef RDMA_TRAFFIC_HPP
#define RDMA_TRAFFIC_HPP

#include "ap_axi_sdata.h"
#include "ap_int.h"
#include "hls_stream.h"

// rdma.tx / rdma.rx: 1024 bits, two 64-byte units per beat
typedef ap_axiu<1024, 0, 0, 0> beat_t;

// The address pattern of docs/rdma-sideband.md: the 64-bit word at
// address a is (seed << 47) | a[46:0], seed 17 bits.

void rdma_gen(hls::stream<beat_t> &axis_out, ap_uint<32> seed, ap_uint<64> base,
              ap_uint<32> msg_bytes, ap_uint<64> total_bytes, ap_uint<32> epoch,
              ap_uint<64> &sent);

void rdma_check(hls::stream<beat_t> &axis_in, ap_uint<32> seed, ap_uint<64> base,
                ap_uint<32> epoch, ap_uint<64> &bytes, ap_uint<64> &errors,
                ap_uint<64> &first_error, ap_uint<32> &msgs);

#endif

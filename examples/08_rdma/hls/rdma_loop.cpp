// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ap_axi_sdata.h"
#include "ap_int.h"
#include "hls_stream.h"

// rdma.rx / rdma.tx: 1024 bits
typedef ap_axiu<1024, 0, 0, 0> pkt;

// What the RDMA service receives for its stream goes back out on it: an RDMA
// READ into Rdma::STREAM (readToStream) is what a later writeFromStream()
// sends.
void rdma_loop(hls::stream<pkt>   &axis_in,
               hls::stream<pkt>   &axis_out,
               ap_uint<32>        &beats)
{
#pragma HLS INTERFACE mode=axis      port=axis_in depth=16
#pragma HLS INTERFACE mode=axis      port=axis_out depth=16
#pragma HLS INTERFACE mode=s_axilite port=beats bundle=control
#pragma HLS INTERFACE ap_ctrl_none   port=return

    ap_uint<32> n = 0;
    beats = 0;

    while (1) {
#pragma HLS PIPELINE II=1
        axis_out.write(axis_in.read());
        n++;
        beats = n;             // readable via AXI-Lite
    }
}

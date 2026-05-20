// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ap_axi_sdata.h"
#include "ap_int.h"
#include "hls_stream.h"

#define DWIDTH 512
#define TDWIDTH 3

typedef ap_axiu<DWIDTH, 1, 1, TDWIDTH> pkt;

void traffic_producer(hls::stream<pkt>       &axis_out,
                      ap_uint<32>            flits,
                      ap_uint<TDWIDTH>       dest){

#pragma HLS INTERFACE mode=axis port=axis_out depth=16
#pragma HLS INTERFACE mode=s_axilite port=dest bundle=control
#pragma HLS INTERFACE mode=s_axilite port=flits bundle=control
#pragma HLS INTERFACE mode=s_axilite port=return bundle=control

    pkt axi_word;
generator:
    for(unsigned int i=0; i< flits; i++){
        #pragma HLS PIPELINE II=1
        for(unsigned int j=0; j<DWIDTH; j+=32){
        #pragma HLS UNROLL
            axi_word.data(j+31, j) = i;
        }
        axi_word.keep = -1;
        axi_word.dest = dest;
        axi_word.last = (i+1 == flits);
        axis_out.write(axi_word);
    }
}
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT

#include "rdma_traffic.hpp"

// Feeds rdma.tx: total_bytes of the pattern for the addresses base,
// base + 8, ... (where the data lands at the peer), tlast every msg_bytes,
// then nothing until the host writes a new epoch, which starts over. base
// and both sizes are multiples of 128 bytes (whole beats). One beat per
// call; the kernel is free-running, so each call sees the registers as they
// are.
void rdma_gen(hls::stream<beat_t> &axis_out, ap_uint<32> seed, ap_uint<64> base,
              ap_uint<32> msg_bytes, ap_uint<64> total_bytes, ap_uint<32> epoch,
              ap_uint<64> &sent)
{
#pragma HLS INTERFACE mode=axis      port=axis_out depth=16
#pragma HLS INTERFACE mode=s_axilite port=seed bundle=control
#pragma HLS INTERFACE mode=s_axilite port=base bundle=control
#pragma HLS INTERFACE mode=s_axilite port=msg_bytes bundle=control
#pragma HLS INTERFACE mode=s_axilite port=total_bytes bundle=control
#pragma HLS INTERFACE mode=s_axilite port=epoch bundle=control
#pragma HLS INTERFACE mode=s_axilite port=sent bundle=control
#pragma HLS INTERFACE ap_ctrl_none   port=return
#pragma HLS PIPELINE II=1

    static ap_uint<32> cur_epoch = 0;
    static ap_uint<64> pos = 0;             // bytes sent in this epoch
    static ap_uint<40> line = 0;            // peer address of the beat, bits 46:7
    static ap_uint<32> in_msg = 0;          // bytes of the current message

    if (epoch != cur_epoch) {
        cur_epoch = epoch;
        pos = 0;
        line = base.range(46, 7);
        in_msg = 0;
    } else if (pos < total_bytes) {
        beat_t b;
        for (int w = 0; w < 16; w++) {
#pragma HLS UNROLL
            b.data.range(64 * w + 63, 64 * w) =
                (ap_uint<64>(seed.range(16, 0)) << 47) | (ap_uint<64>(line) << 7) | (w * 8);
        }
        bool last = in_msg + 128 >= msg_bytes;
        b.keep = -1;
        b.strb = -1;
        b.last = last;
        axis_out.write(b);
        pos += 128;
        line++;
        in_msg = last ? ap_uint<32>(0) : ap_uint<32>(in_msg + 128);
    }
    sent = pos;
}

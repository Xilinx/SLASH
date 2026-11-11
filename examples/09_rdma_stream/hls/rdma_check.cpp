// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT

#include "rdma_traffic.hpp"

// Takes rdma.rx and checks it against the pattern for the addresses base,
// base + 8, ... (where the data came from at the peer), counting bytes,
// words that differ and messages (tlast). A new epoch from the host starts
// over at once. base is a multiple of 128 bytes and so is every message but
// the last bytes of the run, so each beat starts at a multiple of 128 at the
// peer. Free-running: one cycle per call, a beat taken when there is one; the
// stream is read without blocking, so that the pipeline never stalls with
// beats inside it and the counters are current as soon as the input stops.
void rdma_check(hls::stream<beat_t> &axis_in, ap_uint<32> seed, ap_uint<64> base,
                ap_uint<32> epoch, ap_uint<64> &bytes, ap_uint<64> &errors,
                ap_uint<64> &first_error, ap_uint<32> &msgs)
{
#pragma HLS INTERFACE mode=axis      port=axis_in depth=16
#pragma HLS INTERFACE mode=s_axilite port=seed bundle=control
#pragma HLS INTERFACE mode=s_axilite port=base bundle=control
#pragma HLS INTERFACE mode=s_axilite port=epoch bundle=control
#pragma HLS INTERFACE mode=s_axilite port=bytes bundle=control
#pragma HLS INTERFACE mode=s_axilite port=errors bundle=control
#pragma HLS INTERFACE mode=s_axilite port=first_error bundle=control
#pragma HLS INTERFACE mode=s_axilite port=msgs bundle=control
#pragma HLS INTERFACE ap_ctrl_none   port=return
#pragma HLS PIPELINE II=1

    static ap_uint<32> cur_epoch = 0;
    static ap_uint<64> pos = 0;             // bytes taken in this epoch
    static ap_uint<64> err = 0;
    static ap_uint<64> first = ~ap_uint<64>(0);
    static ap_uint<32> nmsg = 0;

    if (epoch != cur_epoch) {
        cur_epoch = epoch;
        pos = 0;
        err = 0;
        first = ~ap_uint<64>(0);
        nmsg = 0;
    }
    beat_t b;
    if (axis_in.read_nb(b)) {
        // peer address of the beat, bits 46:7
        ap_uint<40> line = (base + pos).range(46, 7);
        ap_uint<5> words = 0;               // whole 64-bit words in the beat
        ap_uint<5> bad = 0;
        ap_uint<5> first_bad = 16;
        for (int w = 15; w >= 0; w--) {
#pragma HLS UNROLL
            if (b.keep.range(8 * w + 7, 8 * w) == 0xFF) {
                words++;
                ap_uint<64> want =
                    (ap_uint<64>(seed.range(16, 0)) << 47) | (ap_uint<64>(line) << 7) | (w * 8);
                if (b.data.range(64 * w + 63, 64 * w) != want) {
                    bad++;
                    first_bad = w;
                }
            }
        }
        if (bad != 0 && err == 0)
            first = pos + 8 * first_bad;
        err += bad;
        pos += 8 * words;
        if (b.last)
            nmsg++;
    }
    bytes = pos;
    errors = err;
    first_error = first;
    msgs = nmsg;
}

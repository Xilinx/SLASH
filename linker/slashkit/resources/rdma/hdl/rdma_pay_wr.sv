// SPDX-License-Identifier: MIT
`timescale 1ns / 1ps

// Payload sink of rdma_service, service clock: takes the payload the stack
// receives (incoming WRITEs, READ responses), one packet per request, in
// request order, 1024 bits per beat from byte 0, and writes it
//   KIND_DDR     through the AXI DataMover (S2MM command, then the packet as
//                512-bit beats with tlast on the last one)
//   KIND_STREAM  to lane 0 or 1, one or two 512-bit beats per cycle; tkeep
//                gives the valid bytes and tlast ends a message (the last
//                packet of a WRITE or READ)
//   KIND_BAD     nowhere (an address outside the memory window)
module rdma_pay_wr (
    input  wire          clk,
    input  wire          resetn,

    // classified requests: [47:0] address, [63:48] length (1..65535 bytes),
    // [65:64] kind, [66] lane, [67] last packet of the message
    input  wire          req_valid,
    output wire          req_ready,
    input  wire [67:0]   req_data,

    // the packets
    input  wire          in_valid,
    output wire          in_ready,
    input  wire [1023:0] in_data,
    input  wire          in_last,

    // AXI DataMover S2MM, 64-bit addresses
    output wire          s2mm_cmd_tvalid,
    input  wire          s2mm_cmd_tready,
    output wire [103:0]  s2mm_cmd_tdata,
    output wire          s2mm_tvalid,
    input  wire          s2mm_tready,
    output wire [511:0]  s2mm_tdata,
    output wire [63:0]   s2mm_tkeep,
    output wire          s2mm_tlast,

    // the two lanes (rdma_fifo_w2r1 write sides)
    input  wire [1:0]    lane_room1,
    input  wire [1:0]    lane_room2,
    output wire [1:0]    lane_put1,
    output wire [1:0]    lane_put2,
    output wire [511:0]  lane_d0_data,
    output wire [63:0]   lane_d0_keep,
    output wire          lane_d0_last,
    output wire [511:0]  lane_d1_data,
    output wire [63:0]   lane_d1_keep,
    output wire          lane_d1_last,

    output reg           in_len_error       // pulse: packet tlast where not expected
);
    localparam [1:0] KIND_DDR = 2'd0, KIND_STREAM = 2'd1, KIND_BAD = 2'd2;

    reg         busy;
    reg  [1:0]  kind;
    reg         lane;
    reg         msg_last;
    reg  [10:0] rem;                        // 64-byte halves still to write
    reg  [6:0]  last_bytes;                 // valid bytes of the final half
    reg         half;                       // DDR: the high half of the beat is next

    // ---------------------------------------------------------------- requests
    wire [1:0]  req_kind = req_data[65:64];
    wire [15:0] req_len = req_data[63:48];
    wire [16:0] req_len_up = {1'b0, req_len} + 17'd63;

    assign s2mm_cmd_tvalid = !busy && req_valid && req_kind == KIND_DDR;
    assign req_ready = !busy && (req_kind != KIND_DDR || s2mm_cmd_tready);
    // [22:0] BTT, [23] INCR, [29:24] DSA, [30] EOF, [31] DRR, [95:32] address
    assign s2mm_cmd_tdata = {8'd0, 16'd0, req_data[47:0], 1'b0, 1'b1, 6'd0, 1'b1,
                             7'd0, req_len};

    // ---------------------------------------------------------------- data
    function automatic [63:0] keep_of(input [6:0] nbytes);
        keep_of = (nbytes >= 7'd64) ? {64{1'b1}} : ((64'd1 << nbytes) - 64'd1);
    endfunction
    wire [63:0] keep_a = (rem == 11'd1) ? keep_of(last_bytes) : {64{1'b1}};
    wire [63:0] keep_b = (rem == 11'd2) ? keep_of(last_bytes) : {64{1'b1}};
    wire        two = rem >= 11'd2;

    // DDR: one half per handshake, the beat goes when both (or its last) are out
    assign s2mm_tvalid = busy && kind == KIND_DDR && in_valid;
    assign s2mm_tdata = half ? in_data[1023:512] : in_data[511:0];
    assign s2mm_tkeep = keep_a;
    assign s2mm_tlast = rem == 11'd1;
    wire   ddr_step = s2mm_tvalid && s2mm_tready;
    wire   ddr_pop = ddr_step && (half || rem == 11'd1);

    // stream: the whole beat (one or two halves) per cycle
    wire   lane_room = two ? lane_room2[lane] : lane_room1[lane];
    wire   str_step = busy && kind == KIND_STREAM && in_valid && lane_room;
    assign lane_put1[0] = str_step && !lane && !two;
    assign lane_put2[0] = str_step && !lane &&  two;
    assign lane_put1[1] = str_step &&  lane && !two;
    assign lane_put2[1] = str_step &&  lane &&  two;
    assign lane_d0_data = in_data[511:0];
    assign lane_d0_keep = keep_a;
    assign lane_d0_last = msg_last && rem == 11'd1;
    assign lane_d1_data = in_data[1023:512];
    assign lane_d1_keep = keep_b;
    assign lane_d1_last = msg_last && rem == 11'd2;

    // bad: drop the beat
    wire   bad_step = busy && kind == KIND_BAD && in_valid;

    wire   pop = ddr_pop || str_step || bad_step;
    assign in_ready = busy && (kind == KIND_DDR ? (s2mm_tready && (half || rem == 11'd1)) :
                               kind == KIND_STREAM ? lane_room : 1'b1);

    wire [10:0] dec = ddr_step ? 11'd1 : (str_step || bad_step) ? (two ? 11'd2 : 11'd1) : 11'd0;
    wire [10:0] rem_next = rem - dec;

    always @(posedge clk) begin
        in_len_error <= 1'b0;
        if (!resetn) begin
            busy <= 1'b0;
            half <= 1'b0;
        end else begin
            if (req_valid && req_ready) begin
                busy <= 1'b1;
                kind <= req_kind;
                lane <= req_data[66];
                msg_last <= req_data[67];
                rem <= req_len_up[16:6];
                last_bytes <= (req_len[5:0] == 6'd0) ? 7'd64 : {1'b0, req_len[5:0]};
                half <= 1'b0;
            end
            if (ddr_step)
                half <= !half && rem != 11'd1;
            if (pop && in_last != (rem_next == 11'd0))
                in_len_error <= 1'b1;
            if (dec != 11'd0) begin
                rem <= rem_next;
                if (rem_next == 11'd0)
                    busy <= 1'b0;
            end
        end
    end
endmodule

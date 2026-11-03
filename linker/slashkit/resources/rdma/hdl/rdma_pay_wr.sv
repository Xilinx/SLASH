// SPDX-License-Identifier: MIT
`timescale 1ns / 1ps

// Payload sink of rdma_service, service clock: takes the payload the stack
// receives (incoming WRITEs, READ responses), one packet per request, in
// request order, 1024 bits per beat from byte 0, and writes it
//   KIND_DDR     through one of the two AXI DataMovers, alternately: the
//                S2MM command, then the packet into that DataMover's FIFO
//                (512-bit beats, tlast on the last one), so that both write
//                at once and the memory side keeps up with the line rate
//   KIND_STREAM  to lane 0 or 1, one or two 512-bit beats per cycle; tkeep
//                gives the valid bytes and tlast ends a message (the last
//                packet of a WRITE or READ)
//   KIND_BAD     nowhere (an address outside the memory window)
// Packets to different DataMovers may reach memory out of order; packets
// of one message never overlap.
module rdma_pay_wr #(
    parameter integer DDR_DEPTH = 256       // 512-bit beats per DataMover FIFO
) (
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

    // AXI DataMovers 0 and 1, S2MM, 64-bit addresses
    output wire [1:0]    s2mm_cmd_tvalid,
    input  wire [1:0]    s2mm_cmd_tready,
    output wire [103:0]  s2mm_cmd_tdata,
    output wire [1:0]    s2mm_tvalid,
    input  wire [1:0]    s2mm_tready,
    output wire [511:0]  s2mm0_tdata,
    output wire [63:0]   s2mm0_tkeep,
    output wire          s2mm0_tlast,
    output wire [511:0]  s2mm1_tdata,
    output wire [63:0]   s2mm1_tkeep,
    output wire          s2mm1_tlast,

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

    // a packet goes to one of four FIFOs: lane 0 / 1, DataMover 0 / 1
    reg         busy;
    reg  [1:0]  kind;
    reg  [1:0]  dst;                        // [1] DataMover, [0] which
    reg         msg_last;
    reg  [10:0] rem;                        // 64-byte halves still to write
    reg  [6:0]  last_bytes;                 // valid bytes of the final half
    reg         dm_next;                    // DataMover of the next DDR packet

    // ---------------------------------------------------------------- requests
    wire [1:0]  req_kind = req_data[65:64];
    wire [15:0] req_len = req_data[63:48];
    wire [16:0] req_len_up = {1'b0, req_len} + 17'd63;
    wire        req_ddr = req_kind == KIND_DDR;

    assign s2mm_cmd_tvalid[0] = !busy && req_valid && req_ddr && !dm_next;
    assign s2mm_cmd_tvalid[1] = !busy && req_valid && req_ddr &&  dm_next;
    assign req_ready = !busy && (!req_ddr || s2mm_cmd_tready[dm_next]);
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

    wire [1:0]  ddr_room1, ddr_room2;
    wire [3:0]  room1 = {ddr_room1, lane_room1};
    wire [3:0]  room2 = {ddr_room2, lane_room2};

    // the whole beat (one or two halves) per cycle, to the packet's FIFO
    wire   room = two ? room2[dst] : room1[dst];
    wire   step = busy && kind != KIND_BAD && in_valid && room;
    wire [3:0] put1, put2;
    for (genvar i = 0; i < 4; i++) begin : g_put
        assign put1[i] = step && dst == i && !two;
        assign put2[i] = step && dst == i &&  two;
    end
    assign lane_put1 = put1[1:0];
    assign lane_put2 = put2[1:0];
    assign lane_d0_data = in_data[511:0];
    assign lane_d0_keep = keep_a;
    assign lane_d0_last = msg_last && rem == 11'd1;
    assign lane_d1_data = in_data[1023:512];
    assign lane_d1_keep = keep_b;
    assign lane_d1_last = msg_last && rem == 11'd2;

    // bad: drop the beat
    wire   bad_step = busy && kind == KIND_BAD && in_valid;

    wire   pop = step || bad_step;
    assign in_ready = busy && (kind == KIND_BAD || room);

    wire [10:0] dec = pop ? (two ? 11'd2 : 11'd1) : 11'd0;
    wire [10:0] rem_next = rem - dec;

    always @(posedge clk) begin
        in_len_error <= 1'b0;
        if (!resetn) begin
            busy <= 1'b0;
            dm_next <= 1'b0;
        end else begin
            if (req_valid && req_ready) begin
                busy <= 1'b1;
                kind <= req_kind;
                dst <= req_ddr ? {1'b1, dm_next} : {1'b0, req_data[66]};
                if (req_ddr)
                    dm_next <= !dm_next;
                msg_last <= req_data[67];
                rem <= req_len_up[16:6];
                last_bytes <= (req_len[5:0] == 6'd0) ? 7'd64 : {1'b0, req_len[5:0]};
            end
            if (pop && in_last != (rem_next == 11'd0))
                in_len_error <= 1'b1;
            if (pop) begin
                rem <= rem_next;
                if (rem_next == 11'd0)
                    busy <= 1'b0;
            end
        end
    end

    // ---------------------------------------------------------------- DataMover FIFOs
    // tlast ends the packet: the S2MM command has EOF and the packet's BTT
    wire [511:0] dm_tdata [2];
    wire [63:0]  dm_tkeep [2];
    wire [1:0]   dm_tlast;
    for (genvar i = 0; i < 2; i++) begin : g_dm
        rdma_fifo_w2r1 #(.DEPTH(DDR_DEPTH)) inst_fifo (
            .clk(clk), .resetn(resetn),
            .room1(ddr_room1[i]), .room2(ddr_room2[i]), .put1(put1[2 + i]), .put2(put2[2 + i]),
            .d0_data(in_data[511:0]), .d0_keep(keep_a), .d0_last(rem == 11'd1),
            .d1_data(in_data[1023:512]), .d1_keep(keep_b), .d1_last(rem == 11'd2),
            .m_tvalid(s2mm_tvalid[i]), .m_tready(s2mm_tready[i]), .m_tdata(dm_tdata[i]),
            .m_tkeep(dm_tkeep[i]), .m_tlast(dm_tlast[i])
        );
    end
    assign s2mm0_tdata = dm_tdata[0];
    assign s2mm0_tkeep = dm_tkeep[0];
    assign s2mm0_tlast = dm_tlast[0];
    assign s2mm1_tdata = dm_tdata[1];
    assign s2mm1_tkeep = dm_tkeep[1];
    assign s2mm1_tlast = dm_tlast[1];
endmodule

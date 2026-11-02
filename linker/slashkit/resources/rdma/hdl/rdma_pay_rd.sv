// SPDX-License-Identifier: MIT
`timescale 1ns / 1ps

// Payload source of rdma_service, service clock: answers the stack's read
// requests (the payload it sends), in request order, with one packet of
// exactly the requested length, first byte in byte 0, 1024 bits per beat.
//
// A request comes from rdma_service already classified:
//   KIND_DDR     the AXI DataMover reads it (MM2S command here, its 512-bit
//                output packed two beats to one 1024-bit beat)
//   KIND_STREAM  the next ceil(len / 64) beats of lane 0 or 1 (tkeep and
//                tlast of the lane are ignored)
//   KIND_BAD     zeros (an address outside the memory window)
// DataMover commands are issued ahead of the data; the order FIFO keeps the
// requests whose data is still to come.
module rdma_pay_rd #(
    parameter integer ORDER_DEPTH = 32
) (
    input  wire          clk,
    input  wire          resetn,

    // classified requests: [47:0] address, [63:48] length (1..65535 bytes),
    // [65:64] kind, [66] lane
    input  wire          req_valid,
    output wire          req_ready,
    input  wire [66:0]   req_data,

    // AXI DataMover MM2S, 64-bit addresses
    output wire          mm2s_cmd_tvalid,
    input  wire          mm2s_cmd_tready,
    output wire [103:0]  mm2s_cmd_tdata,
    input  wire          mm2s_tvalid,
    output wire          mm2s_tready,
    input  wire [511:0]  mm2s_tdata,
    input  wire          mm2s_tlast,

    // the two lanes (rdma_fifo_w1r2 read sides)
    input  wire [1:0]    lane_avail1,
    input  wire [1:0]    lane_avail2,
    input  wire [511:0]  lane0_d0,
    input  wire [511:0]  lane0_d1,
    input  wire [511:0]  lane1_d0,
    input  wire [511:0]  lane1_d1,
    output wire [1:0]    lane_take1,
    output wire [1:0]    lane_take2,

    // payload packets to the stack (through the clock crossing)
    input  wire          out_room,          // a beat can be written
    output reg           out_valid,
    output reg  [1023:0] out_data,
    output reg  [127:0]  out_keep,
    output reg           out_last,

    output reg           mm2s_len_error     // pulse: DataMover tlast where not expected
);
    localparam [1:0] KIND_DDR = 2'd0, KIND_STREAM = 2'd1, KIND_BAD = 2'd2;

    // ---------------------------------------------------------------- dispatch
    reg         busy;
    wire        q_pop;
    wire        q_full, q_empty, q_rst_busy;
    wire [24:0] q_dout;                     // [15:0] length, [17:16] kind, [18] lane
    wire [1:0]  req_kind = req_data[65:64];

    wire   q_room = !q_full && !q_rst_busy;
    assign mm2s_cmd_tvalid = req_valid && q_room && req_kind == KIND_DDR;
    assign req_ready = q_room && (req_kind != KIND_DDR || mm2s_cmd_tready);
    wire   q_push = req_valid && req_ready;

    // DataMover command: [22:0] BTT, [23] INCR, [29:24] DSA, [30] EOF,
    // [31] DRR, [95:32] start address, [99:96] tag, [103:100] reserved
    assign mm2s_cmd_tdata = {8'd0, 16'd0, req_data[47:0], 1'b0, 1'b1, 6'd0, 1'b1,
                             7'd0, req_data[63:48]};

    xpm_fifo_sync #(
        .FIFO_MEMORY_TYPE("distributed"),
        .FIFO_WRITE_DEPTH(ORDER_DEPTH),
        .WRITE_DATA_WIDTH(25),
        .READ_DATA_WIDTH(25),
        .READ_MODE("fwft"),
        .FIFO_READ_LATENCY(0),
        .USE_ADV_FEATURES("0000"),
        .DOUT_RESET_VALUE("0")
    ) inst_order (
        .wr_clk(clk), .rst(!resetn),
        .wr_en(q_push), .din({6'd0, req_data[66:48]}), .full(q_full),
        .rd_en(q_pop), .dout(q_dout), .empty(q_empty),
        .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0),
        .overflow(), .prog_empty(), .prog_full(), .rd_data_count(), .wr_data_count(),
        .wr_ack(), .data_valid(), .almost_empty(), .almost_full(), .underflow(),
        .sbiterr(), .dbiterr(), .wr_rst_busy(q_rst_busy), .rd_rst_busy()
    );

    // ---------------------------------------------------------------- assemble
    // rem: 64-byte halves of the current packet still to take; last_bytes:
    // valid bytes of its final half; lo: a taken half waiting for its pair.
    reg  [1:0]  kind;
    reg         lane;
    reg  [10:0] rem;
    reg  [6:0]  last_bytes;
    reg         lo_valid;
    reg  [511:0] lo_data;
    reg  [63:0] lo_keep;

    wire [15:0] q_len = q_dout[15:0];
    wire [16:0] q_len_up = {1'b0, q_len} + 17'd63;
    wire [10:0] q_halves = q_len_up[16:6];
    wire [6:0]  q_last = (q_len[5:0] == 6'd0) ? 7'd64 : {1'b0, q_len[5:0]};
    assign      q_pop = !busy && !q_empty;

    // halves the source has this cycle, and what it hands over
    reg  [1:0]  have;
    reg  [511:0] src0, src1;
    always @* begin
        have = 2'd0;
        src0 = '0;
        src1 = '0;
        case (kind)
            KIND_DDR: begin
                have = mm2s_tvalid ? 2'd1 : 2'd0;
                src0 = mm2s_tdata;
            end
            KIND_STREAM: begin
                have = lane_avail2[lane] ? 2'd2 : lane_avail1[lane] ? 2'd1 : 2'd0;
                src0 = lane ? lane1_d0 : lane0_d0;
                src1 = lane ? lane1_d1 : lane0_d1;
            end
            default: have = 2'd2;
        endcase
    end

    // take what completes the output word (or starts it), if a beat can go out
    wire [1:0] want = lo_valid ? 2'd1 : (rem >= 11'd2 ? 2'd2 : 2'd1);
    wire [1:0] take = (!busy || !out_room) ? 2'd0 : (have < want ? have : want);

    function automatic [63:0] keep_of(input [6:0] nbytes);
        keep_of = (nbytes >= 7'd64) ? {64{1'b1}} : ((64'd1 << nbytes) - 64'd1);
    endfunction
    // keep of the half taken with rem_before halves still to go
    wire [63:0] keep_a = (rem == 11'd1) ? keep_of(last_bytes) : {64{1'b1}};
    wire [63:0] keep_b = (rem == 11'd2) ? keep_of(last_bytes) : {64{1'b1}};

    assign mm2s_tready = busy && kind == KIND_DDR && take != 2'd0;
    assign lane_take1[0] = busy && kind == KIND_STREAM && !lane && take == 2'd1;
    assign lane_take2[0] = busy && kind == KIND_STREAM && !lane && take == 2'd2;
    assign lane_take1[1] = busy && kind == KIND_STREAM &&  lane && take == 2'd1;
    assign lane_take2[1] = busy && kind == KIND_STREAM &&  lane && take == 2'd2;

    wire [10:0] rem_next = rem - take;

    always @(posedge clk) begin
        out_valid <= 1'b0;
        mm2s_len_error <= 1'b0;
        if (!resetn) begin
            busy <= 1'b0;
            lo_valid <= 1'b0;
        end else begin
            if (q_pop) begin
                busy <= 1'b1;
                kind <= q_dout[17:16];
                lane <= q_dout[18];
                rem <= q_halves;
                last_bytes <= q_last;
                lo_valid <= 1'b0;
            end
            if (take != 2'd0) begin
                if (kind == KIND_DDR && mm2s_tlast != (rem == 11'd1))
                    mm2s_len_error <= 1'b1;
                if (lo_valid) begin
                    // take == 1: the high half completes the word
                    out_valid <= 1'b1;
                    out_data <= {src0, lo_data};
                    out_keep <= {keep_a, lo_keep};
                    out_last <= rem_next == 11'd0;
                    lo_valid <= 1'b0;
                end else if (take == 2'd2) begin
                    out_valid <= 1'b1;
                    out_data <= {src1, src0};
                    out_keep <= {keep_b, keep_a};
                    out_last <= rem_next == 11'd0;
                end else if (rem == 11'd1) begin
                    // a single last half
                    out_valid <= 1'b1;
                    out_data <= {512'd0, src0};
                    out_keep <= {64'd0, keep_a};
                    out_last <= 1'b1;
                end else begin
                    lo_valid <= 1'b1;
                    lo_data <= src0;
                    lo_keep <= keep_a;
                end
                rem <= rem_next;
                if (rem_next == 11'd0)
                    busy <= 1'b0;
            end
        end
    end
endmodule

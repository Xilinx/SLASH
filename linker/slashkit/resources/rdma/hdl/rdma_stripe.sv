// SPDX-License-Identifier: MIT
`timescale 1ns / 1ps

// The service's stream (rdma.tx / rdma.rx) over its two 512-bit NoC lanes,
// one clock. The stream is a sequence of 64-byte units, dealt to the lanes
// in turn: unit k on lane (k mod 2), where k counts from the last unit that
// had tlast, so that both ends of the lanes fall back into step at every
// message end (the user region's rdma_stream_tx / rdma_stream_rx do the
// same on their side). Two lanes give the line rate that one cannot carry.

// Lanes -> payload source: up to two units out per cycle. tkeep is not kept
// (a request takes whole units and its length gives the valid bytes); tlast
// is, to know where the next unit comes from.
module rdma_stripe_rd #(
    parameter integer DEPTH = 256           // units per lane FIFO
) (
    input  wire          clk,
    input  wire          resetn,

    input  wire [1:0]    s_tvalid,          // lane 0, 1
    output wire [1:0]    s_tready,
    input  wire [511:0]  s0_tdata,
    input  wire          s0_tlast,
    input  wire [511:0]  s1_tdata,
    input  wire          s1_tlast,

    output wire          avail1,            // the next unit is here
    output wire          avail2,            // the next two units are here
    output wire [511:0]  d0,                // next unit
    output wire [511:0]  d1,                // the one after it
    input  wire          take1,
    input  wire          take2,

    output wire [1:0]    lane_take          // a unit left lane 0, 1 (counters)
);
    reg          rr;                        // lane of the next unit
    wire [1:0]   full, empty, rst_busy, rd_en;
    wire [512:0] dout [2];
    wire [512:0] din [2];
    assign din[0] = {s0_tlast, s0_tdata};
    assign din[1] = {s1_tlast, s1_tdata};

    wire last0 = dout[rr][512];
    assign avail1 = !empty[rr];
    // a unit with tlast ends the pair: the one after it is on lane 0
    assign avail2 = !empty[0] && !empty[1] && !last0;
    assign d0 = dout[rr][511:0];
    assign d1 = dout[!rr][511:0];
    assign rd_en[0] = take2 || (take1 && !rr);
    assign rd_en[1] = take2 || (take1 &&  rr);
    assign lane_take = rd_en;

    always @(posedge clk) begin
        if (!resetn)
            rr <= 1'b0;
        else if (take1)
            rr <= last0 ? 1'b0 : !rr;
        else if (take2)
            rr <= dout[!rr][512] ? 1'b0 : rr;
    end

    for (genvar i = 0; i < 2; i++) begin : g_lane
        assign s_tready[i] = !full[i] && !rst_busy[i];
        xpm_fifo_sync #(
            .FIFO_MEMORY_TYPE("block"),
            .FIFO_WRITE_DEPTH(DEPTH),
            .WRITE_DATA_WIDTH(513),
            .READ_DATA_WIDTH(513),
            .READ_MODE("fwft"),
            .FIFO_READ_LATENCY(0),
            .USE_ADV_FEATURES("0000"),
            .DOUT_RESET_VALUE("0")
        ) inst_fifo (
            .wr_clk(clk), .rst(!resetn),
            .wr_en(s_tvalid[i] && s_tready[i]), .din(din[i]), .full(full[i]),
            .rd_en(rd_en[i]), .dout(dout[i]), .empty(empty[i]),
            .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0),
            .overflow(), .prog_empty(), .prog_full(), .rd_data_count(), .wr_data_count(),
            .wr_ack(), .data_valid(), .almost_empty(), .almost_full(), .underflow(),
            .sbiterr(), .dbiterr(), .wr_rst_busy(rst_busy[i]), .rd_rst_busy()
        );
    end
endmodule

// Payload sink -> lanes: up to two units in per cycle, one per lane out.
module rdma_stripe_wr #(
    parameter integer DEPTH = 256           // units per lane FIFO
) (
    input  wire          clk,
    input  wire          resetn,

    output wire          room1,             // one unit can be written
    output wire          room2,             // two units can be written
    input  wire          put1,
    input  wire          put2,
    input  wire [511:0]  d0_data,           // first unit
    input  wire [63:0]   d0_keep,
    input  wire          d0_last,
    input  wire [511:0]  d1_data,           // second unit (put2)
    input  wire [63:0]   d1_keep,
    input  wire          d1_last,

    output wire [1:0]    m_tvalid,          // lane 0, 1
    input  wire [1:0]    m_tready,
    output wire [511:0]  m0_tdata,
    output wire [63:0]   m0_tkeep,
    output wire          m0_tlast,
    output wire [511:0]  m1_tdata,
    output wire [63:0]   m1_tkeep,
    output wire          m1_tlast,

    output wire [1:0]    lane_put           // a unit went to lane 0, 1 (counters)
);
    localparam integer W = 512 + 64 + 1;

    reg          rr;                        // lane of the next unit
    wire [1:0]   full, empty, rst_busy, wr_en;
    wire [W-1:0] din [2];
    wire [W-1:0] dout [2];

    assign room1 = !full[rr] && rst_busy == 2'b00;
    assign room2 = !full[0] && !full[1] && rst_busy == 2'b00;

    // the first unit goes to lane rr, the second to the other one
    assign din[0] = !rr ? {d0_last, d0_keep, d0_data} : {d1_last, d1_keep, d1_data};
    assign din[1] =  rr ? {d0_last, d0_keep, d0_data} : {d1_last, d1_keep, d1_data};
    assign wr_en[0] = put2 || (put1 && !rr);
    assign wr_en[1] = put2 || (put1 &&  rr);
    assign lane_put = wr_en;

    always @(posedge clk) begin
        if (!resetn)
            rr <= 1'b0;
        else if (put1)
            rr <= d0_last ? 1'b0 : !rr;
        else if (put2)
            rr <= d1_last ? 1'b0 : rr;
    end

    for (genvar i = 0; i < 2; i++) begin : g_lane
        assign m_tvalid[i] = !empty[i];
        xpm_fifo_sync #(
            .FIFO_MEMORY_TYPE("block"),
            .FIFO_WRITE_DEPTH(DEPTH),
            .WRITE_DATA_WIDTH(W),
            .READ_DATA_WIDTH(W),
            .READ_MODE("fwft"),
            .FIFO_READ_LATENCY(0),
            .USE_ADV_FEATURES("0000"),
            .DOUT_RESET_VALUE("0")
        ) inst_fifo (
            .wr_clk(clk), .rst(!resetn),
            .wr_en(wr_en[i]), .din(din[i]), .full(full[i]),
            .rd_en(m_tvalid[i] && m_tready[i]), .dout(dout[i]), .empty(empty[i]),
            .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0),
            .overflow(), .prog_empty(), .prog_full(), .rd_data_count(), .wr_data_count(),
            .wr_ack(), .data_valid(), .almost_empty(), .almost_full(), .underflow(),
            .sbiterr(), .dbiterr(), .wr_rst_busy(rst_busy[i]), .rd_rst_busy()
        );
    end
    assign {m0_tlast, m0_tkeep, m0_tdata} = dout[0];
    assign {m1_tlast, m1_tkeep, m1_tdata} = dout[1];
endmodule

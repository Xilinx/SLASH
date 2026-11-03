// SPDX-License-Identifier: MIT
`timescale 1ns / 1ps

// FIFOs between a 512-bit NoC stream lane and the 1024-bit payload paths of
// rdma_service, one clock. Each keeps its beats in two FIFOs, even and odd
// beats, so that the 1024-bit side moves one or two lane beats per cycle and
// a payload request may end on either half of a 1024-bit word.

// Lane -> payload: one beat in, up to two beats out per cycle. Only tdata is
// kept (and tlast, where WIDTH = 513 has room for it): a request takes whole
// beats and its length gives the valid bytes.
module rdma_fifo_w1r2 #(
    parameter integer DEPTH = 256,          // beats, both halves together
    parameter integer WIDTH = 512
) (
    input  wire          clk,
    input  wire          resetn,

    input  wire          s_tvalid,
    output wire          s_tready,
    input  wire [WIDTH-1:0] s_tdata,

    output wire          avail1,            // the next beat is here
    output wire          avail2,            // the next two beats are here
    output wire [WIDTH-1:0] d0,             // next beat
    output wire [WIDTH-1:0] d1,             // the one after it
    input  wire          take1,
    input  wire          take2
);
    reg        wr_phase, rd_phase;
    wire [1:0] full, empty;
    wire [WIDTH-1:0] dout [2];
    wire [1:0] wr_en, rd_en;

    wire [1:0] rst_busy;
    assign s_tready = !full[wr_phase] && rst_busy == 2'b00;
    assign wr_en[0] = s_tvalid && s_tready && !wr_phase;
    assign wr_en[1] = s_tvalid && s_tready &&  wr_phase;

    assign avail1 = !empty[rd_phase];
    assign avail2 = !empty[0] && !empty[1];
    assign d0 = dout[rd_phase];
    assign d1 = dout[!rd_phase];
    assign rd_en[0] = take2 || (take1 && !rd_phase);
    assign rd_en[1] = take2 || (take1 &&  rd_phase);

    always @(posedge clk) begin
        if (!resetn) begin
            wr_phase <= 1'b0;
            rd_phase <= 1'b0;
        end else begin
            if (s_tvalid && s_tready)
                wr_phase <= !wr_phase;
            if (take1)
                rd_phase <= !rd_phase;
        end
    end

    for (genvar i = 0; i < 2; i++) begin : g_half
        xpm_fifo_sync #(
            .FIFO_MEMORY_TYPE("block"),
            .FIFO_WRITE_DEPTH(DEPTH / 2),
            .WRITE_DATA_WIDTH(WIDTH),
            .READ_DATA_WIDTH(WIDTH),
            .READ_MODE("fwft"),
            .FIFO_READ_LATENCY(0),
            .USE_ADV_FEATURES("0000"),
            .DOUT_RESET_VALUE("0")
        ) inst_fifo (
            .wr_clk(clk), .rst(!resetn),
            .wr_en(wr_en[i]), .din(s_tdata), .full(full[i]),
            .rd_en(rd_en[i]), .dout(dout[i]), .empty(empty[i]),
            .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0),
            .overflow(), .prog_empty(), .prog_full(), .rd_data_count(), .wr_data_count(),
            .wr_ack(), .data_valid(), .almost_empty(), .almost_full(), .underflow(),
            .sbiterr(), .dbiterr(), .wr_rst_busy(rst_busy[i]), .rd_rst_busy()
        );
    end
endmodule

// Payload -> lane: up to two beats in per cycle, one beat out.
module rdma_fifo_w2r1 #(
    parameter integer DEPTH = 256
) (
    input  wire          clk,
    input  wire          resetn,

    output wire          room1,             // one beat can be written
    output wire          room2,             // two beats can be written
    input  wire          put1,
    input  wire          put2,
    input  wire [511:0]  d0_data,           // first beat
    input  wire [63:0]   d0_keep,
    input  wire          d0_last,
    input  wire [511:0]  d1_data,           // second beat (put2)
    input  wire [63:0]   d1_keep,
    input  wire          d1_last,

    output wire          m_tvalid,
    input  wire          m_tready,
    output wire [511:0]  m_tdata,
    output wire [63:0]   m_tkeep,
    output wire          m_tlast
);
    localparam integer W = 512 + 64 + 1;

    reg        wr_phase, rd_phase;
    wire [1:0] full, empty;
    wire [W-1:0] din [2];
    wire [W-1:0] dout [2];
    wire [1:0] wr_en, rd_en;

    wire [1:0] rst_busy;
    assign room1 = !full[wr_phase] && rst_busy == 2'b00;
    assign room2 = !full[0] && !full[1] && rst_busy == 2'b00;

    // the first beat goes to the FIFO at wr_phase, the second to the other
    assign din[0] = !wr_phase ? {d0_last, d0_keep, d0_data} : {d1_last, d1_keep, d1_data};
    assign din[1] =  wr_phase ? {d0_last, d0_keep, d0_data} : {d1_last, d1_keep, d1_data};
    assign wr_en[0] = put2 || (put1 && !wr_phase);
    assign wr_en[1] = put2 || (put1 &&  wr_phase);

    assign m_tvalid = !empty[rd_phase];
    assign {m_tlast, m_tkeep, m_tdata} = dout[rd_phase];
    assign rd_en[0] = m_tvalid && m_tready && !rd_phase;
    assign rd_en[1] = m_tvalid && m_tready &&  rd_phase;

    always @(posedge clk) begin
        if (!resetn) begin
            wr_phase <= 1'b0;
            rd_phase <= 1'b0;
        end else begin
            if (put1)
                wr_phase <= !wr_phase;
            if (m_tvalid && m_tready)
                rd_phase <= !rd_phase;
        end
    end

    for (genvar i = 0; i < 2; i++) begin : g_half
        xpm_fifo_sync #(
            .FIFO_MEMORY_TYPE("block"),
            .FIFO_WRITE_DEPTH(DEPTH / 2),
            .WRITE_DATA_WIDTH(W),
            .READ_DATA_WIDTH(W),
            .READ_MODE("fwft"),
            .FIFO_READ_LATENCY(0),
            .USE_ADV_FEATURES("0000"),
            .DOUT_RESET_VALUE("0")
        ) inst_fifo (
            .wr_clk(clk), .rst(!resetn),
            .wr_en(wr_en[i]), .din(din[i]), .full(full[i]),
            .rd_en(rd_en[i]), .dout(dout[i]), .empty(empty[i]),
            .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0),
            .overflow(), .prog_empty(), .prog_full(), .rd_data_count(), .wr_data_count(),
            .wr_ack(), .data_valid(), .almost_empty(), .almost_full(), .underflow(),
            .sbiterr(), .dbiterr(), .wr_rst_busy(rst_busy[i]), .rd_rst_busy()
        );
    end
endmodule

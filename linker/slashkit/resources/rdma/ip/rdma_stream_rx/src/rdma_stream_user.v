// SPDX-License-Identifier: MIT
`timescale 1ns / 1ps

// The user region's end of the RDMA service's stream, placed by the linker
// between a kernel's 1024-bit rdma.tx / rdma.rx stream and the service's two
// 512-bit NoC lanes, on user_clk. The stream is a sequence of 64-byte units,
// dealt to the lanes in turn: unit k on lane (k mod 2), where k counts from
// the last unit that had tlast (rdma_stripe in the service does the same),
// so both ends fall back into step at every message end, also after one of
// them was reset.

// rdma.tx: kernel -> lanes. A beat carries one unit in its low half, or two
// when any tkeep bit of the high half is set. tkeep is passed on but the
// service takes whole units; tlast marks a message end.
module rdma_stream_tx #(
    parameter integer DEPTH = 32            // units per lane FIFO
) (
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axis:m_axis0:m_axis1, ASSOCIATED_RESET aresetn" *)
    input  wire          aclk,
    input  wire          aresetn,

    input  wire          s_axis_tvalid,
    output wire          s_axis_tready,
    input  wire [1023:0] s_axis_tdata,
    input  wire [127:0]  s_axis_tkeep,
    input  wire          s_axis_tlast,

    output wire          m_axis0_tvalid,
    input  wire          m_axis0_tready,
    output wire [511:0]  m_axis0_tdata,
    output wire [63:0]   m_axis0_tkeep,
    output wire          m_axis0_tlast,
    output wire          m_axis1_tvalid,
    input  wire          m_axis1_tready,
    output wire [511:0]  m_axis1_tdata,
    output wire [63:0]   m_axis1_tkeep,
    output wire          m_axis1_tlast
);
    localparam integer W = 512 + 64 + 1;

    reg          rr;                        // lane of the next unit
    wire [1:0]   full, empty, rst_busy, wr_en, rd_en;
    wire [W-1:0] dout0, dout1;

    wire two = |s_axis_tkeep[127:64];
    wire [W-1:0] lo = {s_axis_tlast && !two, s_axis_tkeep[63:0], s_axis_tdata[511:0]};
    wire [W-1:0] hi = {s_axis_tlast, s_axis_tkeep[127:64], s_axis_tdata[1023:512]};

    // room for two units, whatever the beat holds
    assign s_axis_tready = full == 2'b00 && rst_busy == 2'b00;
    wire   take = s_axis_tvalid && s_axis_tready;
    assign wr_en[0] = take && (!rr || two);
    assign wr_en[1] = take && ( rr || two);
    wire [W-1:0] din0 = !rr ? lo : hi;
    wire [W-1:0] din1 =  rr ? lo : hi;

    always @(posedge aclk) begin
        if (!aresetn)
            rr <= 1'b0;
        else if (take)
            rr <= s_axis_tlast ? 1'b0 : two ? rr : !rr;
    end

    assign m_axis0_tvalid = !empty[0];
    assign m_axis1_tvalid = !empty[1];
    assign rd_en = {m_axis1_tvalid && m_axis1_tready, m_axis0_tvalid && m_axis0_tready};
    assign {m_axis0_tlast, m_axis0_tkeep, m_axis0_tdata} = dout0;
    assign {m_axis1_tlast, m_axis1_tkeep, m_axis1_tdata} = dout1;

    rdma_stream_fifo #(.DEPTH(DEPTH), .W(W)) inst_fifo0 (
        .clk(aclk), .resetn(aresetn), .wr_en(wr_en[0]), .din(din0), .full(full[0]),
        .rd_en(rd_en[0]), .dout(dout0), .empty(empty[0]), .rst_busy(rst_busy[0])
    );
    rdma_stream_fifo #(.DEPTH(DEPTH), .W(W)) inst_fifo1 (
        .clk(aclk), .resetn(aresetn), .wr_en(wr_en[1]), .din(din1), .full(full[1]),
        .rd_en(rd_en[1]), .dout(dout1), .empty(empty[1]), .rst_busy(rst_busy[1])
    );
endmodule

// rdma.rx: lanes -> kernel. Two units per beat, except where a unit has
// tlast: it ends its beat, in the low half when it is the first one (the
// high half's tkeep is then 0).
module rdma_stream_rx #(
    parameter integer DEPTH = 32            // units per lane FIFO
) (
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axis0:s_axis1:m_axis, ASSOCIATED_RESET aresetn" *)
    input  wire          aclk,
    input  wire          aresetn,

    input  wire          s_axis0_tvalid,
    output wire          s_axis0_tready,
    input  wire [511:0]  s_axis0_tdata,
    input  wire [63:0]   s_axis0_tkeep,
    input  wire          s_axis0_tlast,
    input  wire          s_axis1_tvalid,
    output wire          s_axis1_tready,
    input  wire [511:0]  s_axis1_tdata,
    input  wire [63:0]   s_axis1_tkeep,
    input  wire          s_axis1_tlast,

    output reg           m_axis_tvalid,
    input  wire          m_axis_tready,
    output reg  [1023:0] m_axis_tdata,
    output reg  [127:0]  m_axis_tkeep,
    output reg           m_axis_tlast
);
    localparam integer W = 512 + 64 + 1;

    reg          rr;                        // lane of the next unit
    wire [1:0]   full, empty, rst_busy, rd_en;
    wire [W-1:0] dout0, dout1;

    assign s_axis0_tready = !full[0] && !rst_busy[0];
    assign s_axis1_tready = !full[1] && !rst_busy[1];

    // the next unit and the one after it
    wire [W-1:0] u0 = rr ? dout1 : dout0;
    wire [W-1:0] u1 = rr ? dout0 : dout1;
    wire         v0 = rr ? !empty[1] : !empty[0];
    wire         v1 = rr ? !empty[0] : !empty[1];
    wire         last0 = u0[W-1];

    wire   pair = v0 && !last0 && v1;
    wire   single = v0 && last0;
    wire   fire = (!m_axis_tvalid || m_axis_tready) && (pair || single);
    assign rd_en[0] = fire && (!rr || pair);
    assign rd_en[1] = fire && ( rr || pair);

    always @(posedge aclk) begin
        if (!aresetn) begin
            rr <= 1'b0;
            m_axis_tvalid <= 1'b0;
        end else if (fire) begin
            m_axis_tvalid <= 1'b1;
            m_axis_tdata <= {pair ? u1[511:0] : 512'd0, u0[511:0]};
            m_axis_tkeep <= {pair ? u1[575:512] : 64'd0, u0[575:512]};
            m_axis_tlast <= pair ? u1[W-1] : 1'b1;
            rr <= (single || u1[W-1]) ? 1'b0 : rr;
        end else if (m_axis_tready) begin
            m_axis_tvalid <= 1'b0;
        end
    end

    rdma_stream_fifo #(.DEPTH(DEPTH), .W(W)) inst_fifo0 (
        .clk(aclk), .resetn(aresetn), .wr_en(s_axis0_tvalid && s_axis0_tready),
        .din({s_axis0_tlast, s_axis0_tkeep, s_axis0_tdata}), .full(full[0]),
        .rd_en(rd_en[0]), .dout(dout0), .empty(empty[0]), .rst_busy(rst_busy[0])
    );
    rdma_stream_fifo #(.DEPTH(DEPTH), .W(W)) inst_fifo1 (
        .clk(aclk), .resetn(aresetn), .wr_en(s_axis1_tvalid && s_axis1_tready),
        .din({s_axis1_tlast, s_axis1_tkeep, s_axis1_tdata}), .full(full[1]),
        .rd_en(rd_en[1]), .dout(dout1), .empty(empty[1]), .rst_busy(rst_busy[1])
    );
endmodule

// first-word-fall-through FIFO of the two blocks above
module rdma_stream_fifo #(
    parameter integer DEPTH = 32,
    parameter integer W = 577
) (
    input  wire         clk,
    input  wire         resetn,
    input  wire         wr_en,
    input  wire [W-1:0] din,
    output wire         full,
    input  wire         rd_en,
    output wire [W-1:0] dout,
    output wire         empty,
    output wire         rst_busy
);
    wire wr_busy, rd_busy;
    assign rst_busy = wr_busy || rd_busy;

    xpm_fifo_sync #(
        .FIFO_MEMORY_TYPE("auto"),
        .FIFO_WRITE_DEPTH(DEPTH),
        .WRITE_DATA_WIDTH(W),
        .READ_DATA_WIDTH(W),
        .READ_MODE("fwft"),
        .FIFO_READ_LATENCY(0),
        .USE_ADV_FEATURES("0000"),
        .DOUT_RESET_VALUE("0")
    ) inst_fifo (
        .wr_clk(clk), .rst(!resetn),
        .wr_en(wr_en), .din(din), .full(full),
        .rd_en(rd_en), .dout(dout), .empty(empty),
        .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0),
        .overflow(), .prog_empty(), .prog_full(), .rd_data_count(), .wr_data_count(),
        .wr_ack(), .data_valid(), .almost_empty(), .almost_full(), .underflow(),
        .sbiterr(), .dbiterr(), .wr_rst_busy(wr_busy), .rd_rst_busy(rd_busy)
    );
endmodule

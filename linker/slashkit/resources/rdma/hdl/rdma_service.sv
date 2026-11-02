// SPDX-License-Identifier: MIT
`timescale 1ns / 1ps

// RDMA service of the SLASH service layer: the RoCE v2 stack IP rdma_200
// (fpga-network-stack, created as rdma_200_ip) on one DCMAC port, with its
// payload in card memory or in user-region streams.
//
//   aclk (node clock, 230 MHz from the DCMAC clock wizard): the stack, its
//     Ethernet side (1024-bit, dcmac_rx_path / dcmac_tx_path), registers
//   sclk (service clock): the AXI DataMover to the memory window (SL2NOC)
//     and the port's NoC stream lanes to the user region
//
// Payload addresses (48-bit, the local address of a request or the address
// a peer reads or writes):
//   bit 47 = 0: card memory; must lie in the window
//               [MEM_BASE, MEM_BASE + 2^MEM_BITS) (VRT DDR buffers), else
//               reads return zeros and writes are dropped (counted)
//   bit 47 = 1: stream; bit 46 selects lane 0 or 1 (rdma.tx0/tx1 for data
//               the stack sends, rdma.rx0/rx1 for data it receives)
//
// Registers (window of 16 MiB): 0x00_0000 this block (below), 0x40_0000 the
// stack (rdma_200 register map). No register has a read side effect.
//   0x000 ID 0x52444D53 ("RDMS")       0x004 VERSION
//   0x008 BUILD                         0x00C ETH port index
//   0x010 / 0x014 MEM_BASE low / high   0x018 MEM_BITS
//   0x020 PHY_CTRL (RW)                 0x024 PHY_STATUS    0x028 RX_DROPS
//   0x100 RD_MEM     0x104 RD_STREAM    0x108 RD_BAD        payload requests
//   0x10C WR_MEM     0x110 WR_STREAM    0x114 WR_BAD          of each kind
//   0x118 MM2S_ERR   0x11C S2MM_ERR     DataMover status not OKAY
//   0x120 RD_LEN_ERR 0x124 WR_LEN_ERR   packet ends not where expected
//   0x128 LAST_STS   [7:0] last MM2S status, [15:8] last S2MM status
//   0x130 / 0x134 TX0 / TX1 beats taken from the lanes
//   0x138 / 0x13C RX0 / RX1 beats given to the lanes
module rdma_service #(
    parameter integer BUILD = 1,
    parameter integer ETH_INDEX = 0,
    parameter [47:0]  MEM_BASE = 48'h0600_0000_0000,
    parameter integer MEM_BITS = 35,                  // 32 GiB
    parameter integer LANE_DEPTH = 256                // beats per lane FIFO
) (
    input  wire            aclk,
    input  wire            aresetn,
    input  wire            sclk,
    input  wire            sresetn,

    // registers (aclk)
    input  wire [23:0]     s_axil_awaddr,
    input  wire            s_axil_awvalid,
    output wire            s_axil_awready,
    input  wire [31:0]     s_axil_wdata,
    input  wire [3:0]      s_axil_wstrb,
    input  wire            s_axil_wvalid,
    output wire            s_axil_wready,
    output wire [1:0]      s_axil_bresp,
    output wire            s_axil_bvalid,
    input  wire            s_axil_bready,
    input  wire [23:0]     s_axil_araddr,
    input  wire            s_axil_arvalid,
    output wire            s_axil_arready,
    output wire [31:0]     s_axil_rdata,
    output wire [1:0]      s_axil_rresp,
    output wire            s_axil_rvalid,
    input  wire            s_axil_rready,

    // Ethernet frames without FCS (aclk)
    input  wire [1023:0]   s_axis_rx_tdata,
    input  wire [127:0]    s_axis_rx_tkeep,
    input  wire            s_axis_rx_tlast,
    input  wire            s_axis_rx_tvalid,
    output wire            s_axis_rx_tready,
    output wire [1023:0]   m_axis_tx_tdata,
    output wire [127:0]    m_axis_tx_tkeep,
    output wire            m_axis_tx_tlast,
    output wire            m_axis_tx_tvalid,
    input  wire            m_axis_tx_tready,

    // DCMAC port control (aclk)
    input  wire [15:0]     rx_drop_count,
    output reg  [15:0]     phy_ctrl,
    input  wire [15:0]     phy_status,

    // AXI DataMover (sclk)
    output wire            m_axis_mm2s_cmd_tvalid,
    input  wire            m_axis_mm2s_cmd_tready,
    output wire [103:0]    m_axis_mm2s_cmd_tdata,
    input  wire            s_axis_mm2s_tvalid,
    output wire            s_axis_mm2s_tready,
    input  wire [511:0]    s_axis_mm2s_tdata,
    input  wire [63:0]     s_axis_mm2s_tkeep,
    input  wire            s_axis_mm2s_tlast,
    input  wire            s_axis_mm2s_sts_tvalid,
    output wire            s_axis_mm2s_sts_tready,
    input  wire [7:0]      s_axis_mm2s_sts_tdata,
    output wire            m_axis_s2mm_cmd_tvalid,
    input  wire            m_axis_s2mm_cmd_tready,
    output wire [103:0]    m_axis_s2mm_cmd_tdata,
    output wire            m_axis_s2mm_tvalid,
    input  wire            m_axis_s2mm_tready,
    output wire [511:0]    m_axis_s2mm_tdata,
    output wire [63:0]     m_axis_s2mm_tkeep,
    output wire            m_axis_s2mm_tlast,
    input  wire            s_axis_s2mm_sts_tvalid,
    output wire            s_axis_s2mm_sts_tready,
    input  wire [7:0]      s_axis_s2mm_sts_tdata,

    // NoC stream lanes (sclk): user region -> stack, stack -> user region
    input  wire            s_axis_tx0_tvalid,
    output wire            s_axis_tx0_tready,
    input  wire [511:0]    s_axis_tx0_tdata,
    input  wire [63:0]     s_axis_tx0_tkeep,
    input  wire            s_axis_tx0_tlast,
    input  wire            s_axis_tx1_tvalid,
    output wire            s_axis_tx1_tready,
    input  wire [511:0]    s_axis_tx1_tdata,
    input  wire [63:0]     s_axis_tx1_tkeep,
    input  wire            s_axis_tx1_tlast,
    output wire            m_axis_rx0_tvalid,
    input  wire            m_axis_rx0_tready,
    output wire [511:0]    m_axis_rx0_tdata,
    output wire [63:0]     m_axis_rx0_tkeep,
    output wire            m_axis_rx0_tlast,
    output wire            m_axis_rx1_tvalid,
    input  wire            m_axis_rx1_tready,
    output wire [511:0]    m_axis_rx1_tdata,
    output wire [63:0]     m_axis_rx1_tkeep,
    output wire            m_axis_rx1_tlast
);
    localparam [1:0] KIND_MEM = 2'd0, KIND_STREAM = 2'd1, KIND_BAD = 2'd2;
    localparam integer RD_DEPTH = 128;      // 1024-bit beats, 16 KiB

    // ================================================================ registers
    wire [23:0] r0_awaddr, r0_araddr;
    wire        r0_awvalid, r0_wvalid, r0_arvalid, r0_bready, r0_rready;
    wire [31:0] r0_wdata;
    wire [3:0]  r0_wstrb;
    reg         r0_awready, r0_wready, r0_bvalid, r0_arready, r0_rvalid;
    reg  [31:0] r0_rdata;

    wire [11:0] r1_awaddr, r1_araddr;
    wire        r1_awvalid, r1_awready, r1_wvalid, r1_wready, r1_bvalid, r1_bready;
    wire        r1_arvalid, r1_arready, r1_rvalid, r1_rready;
    wire [31:0] r1_wdata, r1_rdata;
    wire [3:0]  r1_wstrb;
    wire [1:0]  r1_bresp, r1_rresp;

    axil_split #(.REGION(4'd4)) inst_split (
        .clk(aclk), .resetn(aresetn),
        .s_awaddr(s_axil_awaddr), .s_awvalid(s_axil_awvalid), .s_awready(s_axil_awready),
        .s_wdata(s_axil_wdata), .s_wstrb(s_axil_wstrb), .s_wvalid(s_axil_wvalid), .s_wready(s_axil_wready),
        .s_bresp(s_axil_bresp), .s_bvalid(s_axil_bvalid), .s_bready(s_axil_bready),
        .s_araddr(s_axil_araddr), .s_arvalid(s_axil_arvalid), .s_arready(s_axil_arready),
        .s_rdata(s_axil_rdata), .s_rresp(s_axil_rresp), .s_rvalid(s_axil_rvalid), .s_rready(s_axil_rready),
        .m0_awaddr(r0_awaddr), .m0_awvalid(r0_awvalid), .m0_awready(r0_awready),
        .m0_wdata(r0_wdata), .m0_wstrb(r0_wstrb), .m0_wvalid(r0_wvalid), .m0_wready(r0_wready),
        .m0_bresp(2'b00), .m0_bvalid(r0_bvalid), .m0_bready(r0_bready),
        .m0_araddr(r0_araddr), .m0_arvalid(r0_arvalid), .m0_arready(r0_arready),
        .m0_rdata(r0_rdata), .m0_rresp(2'b00), .m0_rvalid(r0_rvalid), .m0_rready(r0_rready),
        .m1_awaddr(r1_awaddr), .m1_awvalid(r1_awvalid), .m1_awready(r1_awready),
        .m1_wdata(r1_wdata), .m1_wstrb(r1_wstrb), .m1_wvalid(r1_wvalid), .m1_wready(r1_wready),
        .m1_bresp(r1_bresp), .m1_bvalid(r1_bvalid), .m1_bready(r1_bready),
        .m1_araddr(r1_araddr), .m1_arvalid(r1_arvalid), .m1_arready(r1_arready),
        .m1_rdata(r1_rdata), .m1_rresp(r1_rresp), .m1_rvalid(r1_rvalid), .m1_rready(r1_rready)
    );

    // counters kept in aclk, and those crossing from sclk
    reg  [31:0] cnt_rd [3];                 // by kind
    reg  [31:0] cnt_wr [3];
    wire [31:0] s_cnt [8];                  // sclk counters, synchronized
    wire [15:0] last_sts;

    always @(posedge aclk) begin
        if (!aresetn) begin
            r0_awready <= 1'b0;
            r0_wready <= 1'b0;
            r0_bvalid <= 1'b0;
            r0_arready <= 1'b0;
            r0_rvalid <= 1'b0;
            phy_ctrl <= 16'd0;
        end else begin
            // one write at a time: address and data together
            r0_awready <= 1'b0;
            r0_wready <= 1'b0;
            if (r0_awvalid && r0_wvalid && !r0_bvalid && !r0_awready) begin
                r0_awready <= 1'b1;
                r0_wready <= 1'b1;
                r0_bvalid <= 1'b1;
                if (r0_awaddr[11:0] == 12'h020 && r0_wstrb[1:0] == 2'b11)
                    phy_ctrl <= r0_wdata[15:0];
            end
            if (r0_bvalid && r0_bready)
                r0_bvalid <= 1'b0;

            r0_arready <= 1'b0;
            if (r0_arvalid && !r0_rvalid && !r0_arready) begin
                r0_arready <= 1'b1;
                r0_rvalid <= 1'b1;
                case (r0_araddr[11:0])
                    12'h000: r0_rdata <= 32'h52444D53;
                    12'h004: r0_rdata <= 32'h0001_0000;
                    12'h008: r0_rdata <= BUILD;
                    12'h00C: r0_rdata <= ETH_INDEX;
                    12'h010: r0_rdata <= MEM_BASE[31:0];
                    12'h014: r0_rdata <= {16'd0, MEM_BASE[47:32]};
                    12'h018: r0_rdata <= MEM_BITS;
                    12'h020: r0_rdata <= {16'd0, phy_ctrl};
                    12'h024: r0_rdata <= {16'd0, phy_status};
                    12'h028: r0_rdata <= {16'd0, rx_drop_count};
                    12'h100: r0_rdata <= cnt_rd[KIND_MEM];
                    12'h104: r0_rdata <= cnt_rd[KIND_STREAM];
                    12'h108: r0_rdata <= cnt_rd[KIND_BAD];
                    12'h10C: r0_rdata <= cnt_wr[KIND_MEM];
                    12'h110: r0_rdata <= cnt_wr[KIND_STREAM];
                    12'h114: r0_rdata <= cnt_wr[KIND_BAD];
                    12'h118: r0_rdata <= s_cnt[0];
                    12'h11C: r0_rdata <= s_cnt[1];
                    12'h120: r0_rdata <= s_cnt[2];
                    12'h124: r0_rdata <= s_cnt[3];
                    12'h128: r0_rdata <= {16'd0, last_sts};
                    12'h130: r0_rdata <= s_cnt[4];
                    12'h134: r0_rdata <= s_cnt[5];
                    12'h138: r0_rdata <= s_cnt[6];
                    12'h13C: r0_rdata <= s_cnt[7];
                    default: r0_rdata <= 32'd0;
                endcase
            end
            if (r0_rvalid && r0_rready)
                r0_rvalid <= 1'b0;
        end
    end

    // ================================================================ stack
    wire          rd_req_valid, rd_req_ready, wr_req_valid, wr_req_ready;
    wire [127:0]  rd_req_word, wr_req_word;
    wire          rd_tvalid, rd_tready, rd_tlast;
    wire [1023:0] rd_tdata;
    wire [127:0]  rd_tkeep;
    wire          wr_tvalid, wr_tready, wr_tlast;
    wire [1023:0] wr_tdata;
    wire [127:0]  wr_tkeep;

    // The host posts requests and takes completions through the stack's
    // registers; its send queue and completion streams are not used here.
    rdma_200_ip inst_stack (
        .aclk(aclk), .aresetn(aresetn),
        .s_axil_awaddr(r1_awaddr), .s_axil_awprot(3'd0), .s_axil_awvalid(r1_awvalid), .s_axil_awready(r1_awready),
        .s_axil_wdata(r1_wdata), .s_axil_wstrb(r1_wstrb), .s_axil_wvalid(r1_wvalid), .s_axil_wready(r1_wready),
        .s_axil_bresp(r1_bresp), .s_axil_bvalid(r1_bvalid), .s_axil_bready(r1_bready),
        .s_axil_araddr(r1_araddr), .s_axil_arprot(3'd0), .s_axil_arvalid(r1_arvalid), .s_axil_arready(r1_arready),
        .s_axil_rdata(r1_rdata), .s_axil_rresp(r1_rresp), .s_axil_rvalid(r1_rvalid), .s_axil_rready(r1_rready),
        .s_axis_rx_tvalid(s_axis_rx_tvalid), .s_axis_rx_tready(s_axis_rx_tready), .s_axis_rx_tdata(s_axis_rx_tdata),
        .s_axis_rx_tkeep(s_axis_rx_tkeep), .s_axis_rx_tlast(s_axis_rx_tlast), .s_axis_rx_tuser(1'b0),
        .m_axis_tx_tvalid(m_axis_tx_tvalid), .m_axis_tx_tready(m_axis_tx_tready), .m_axis_tx_tdata(m_axis_tx_tdata),
        .m_axis_tx_tkeep(m_axis_tx_tkeep), .m_axis_tx_tlast(m_axis_tx_tlast),
        .s_axis_sq_tvalid(1'b0), .s_axis_sq_tready(), .s_axis_sq_tdata(192'd0),
        .m_axis_cq_tvalid(), .m_axis_cq_tdata(),
        .m_axis_rd_req_tvalid(rd_req_valid), .m_axis_rd_req_tready(rd_req_ready), .m_axis_rd_req_tdata(rd_req_word),
        .s_axis_rd_tvalid(rd_tvalid), .s_axis_rd_tready(rd_tready), .s_axis_rd_tdata(rd_tdata),
        .s_axis_rd_tkeep(rd_tkeep), .s_axis_rd_tlast(rd_tlast),
        .m_axis_wr_req_tvalid(wr_req_valid), .m_axis_wr_req_tready(wr_req_ready), .m_axis_wr_req_tdata(wr_req_word),
        .m_axis_wr_tvalid(wr_tvalid), .m_axis_wr_tready(wr_tready), .m_axis_wr_tdata(wr_tdata),
        .m_axis_wr_tkeep(wr_tkeep), .m_axis_wr_tlast(wr_tlast)
    );

    // ================================================================ classify
    // request word: [47:0] address, [95:64] length, [112] last packet
    function automatic [1:0] kind_of(input [47:0] addr);
        if (addr[47])
            kind_of = KIND_STREAM;
        else if (addr[47:MEM_BITS] == MEM_BASE[47:MEM_BITS])
            kind_of = KIND_MEM;
        else
            kind_of = KIND_BAD;
    endfunction

    wire [1:0]  rd_kind = kind_of(rd_req_word[47:0]);
    wire [1:0]  wr_kind = kind_of(wr_req_word[47:0]);
    wire [71:0] rd_cmd = {5'd0, rd_req_word[46], rd_kind, rd_req_word[79:64], rd_req_word[47:0]};
    wire [71:0] wr_cmd = {4'd0, wr_req_word[112], wr_req_word[46], wr_kind, wr_req_word[79:64],
                          wr_req_word[47:0]};

    always @(posedge aclk) begin
        if (!aresetn) begin
            for (int i = 0; i < 3; i++) begin
                cnt_rd[i] <= 32'd0;
                cnt_wr[i] <= 32'd0;
            end
        end else begin
            if (rd_req_valid && rd_req_ready)
                cnt_rd[rd_kind] <= cnt_rd[rd_kind] + 32'd1;
            if (wr_req_valid && wr_req_ready)
                cnt_wr[wr_kind] <= cnt_wr[wr_kind] + 32'd1;
        end
    end

    // ================================================================ clock crossings
    wire          s_rd_req_valid, s_rd_req_ready, s_wr_req_valid, s_wr_req_ready;
    wire [71:0]   s_rd_req, s_wr_req;
    wire          s_wr_valid, s_wr_ready, s_wr_last;
    wire [1023:0] s_wr_data;
    wire          s_rd_valid, s_rd_room;
    wire [1023:0] s_rd_data;
    wire [127:0]  s_rd_keep;
    wire          s_rd_last;
    wire          s_rd_full_n, s_rd_almost_full;

    rdma_cdc_axis #(.DATA_BITS(72), .DEPTH(16), .MEM("distributed")) inst_rd_req (
        .s_clk(aclk), .s_resetn(aresetn),
        .s_valid(rd_req_valid), .s_ready(rd_req_ready), .s_data(rd_cmd), .s_keep('1), .s_last(1'b1),
        .m_clk(sclk),
        .m_valid(s_rd_req_valid), .m_ready(s_rd_req_ready), .m_data(s_rd_req), .m_keep(), .m_last(),
        .s_almost_full()
    );
    rdma_cdc_axis #(.DATA_BITS(72), .DEPTH(16), .MEM("distributed")) inst_wr_req (
        .s_clk(aclk), .s_resetn(aresetn),
        .s_valid(wr_req_valid), .s_ready(wr_req_ready), .s_data(wr_cmd), .s_keep('1), .s_last(1'b1),
        .m_clk(sclk),
        .m_valid(s_wr_req_valid), .m_ready(s_wr_req_ready), .m_data(s_wr_req), .m_keep(), .m_last(),
        .s_almost_full()
    );
    rdma_cdc_axis #(.DATA_BITS(1024), .DEPTH(128), .MEM("block")) inst_wr_data (
        .s_clk(aclk), .s_resetn(aresetn),
        .s_valid(wr_tvalid), .s_ready(wr_tready), .s_data(wr_tdata), .s_keep(wr_tkeep), .s_last(wr_tlast),
        .m_clk(sclk),
        .m_valid(s_wr_valid), .m_ready(s_wr_ready), .m_data(s_wr_data), .m_keep(), .m_last(s_wr_last),
        .s_almost_full()
    );
    rdma_cdc_axis #(.DATA_BITS(1024), .DEPTH(RD_DEPTH), .MEM("block")) inst_rd_data (
        .s_clk(sclk), .s_resetn(sresetn),
        .s_valid(s_rd_valid), .s_ready(s_rd_full_n), .s_data(s_rd_data), .s_keep(s_rd_keep), .s_last(s_rd_last),
        .m_clk(aclk),
        .m_valid(rd_tvalid), .m_ready(rd_tready), .m_data(rd_tdata), .m_keep(rd_tkeep), .m_last(rd_tlast),
        .s_almost_full(s_rd_almost_full)
    );
    assign s_rd_room = !s_rd_almost_full;

    // ================================================================ lanes (sclk)
    wire [1:0]   tx_avail1, tx_avail2, tx_take1, tx_take2;
    wire [511:0] tx_d0 [2];
    wire [511:0] tx_d1 [2];
    wire [1:0]   rx_room1, rx_room2, rx_put1, rx_put2;
    wire [511:0] rx_d0_data, rx_d1_data;
    wire [63:0]  rx_d0_keep, rx_d1_keep;
    wire         rx_d0_last, rx_d1_last;

    rdma_fifo_w1r2 #(.DEPTH(LANE_DEPTH)) inst_tx0 (
        .clk(sclk), .resetn(sresetn),
        .s_tvalid(s_axis_tx0_tvalid), .s_tready(s_axis_tx0_tready), .s_tdata(s_axis_tx0_tdata),
        .avail1(tx_avail1[0]), .avail2(tx_avail2[0]), .d0(tx_d0[0]), .d1(tx_d1[0]),
        .take1(tx_take1[0]), .take2(tx_take2[0])
    );
    rdma_fifo_w1r2 #(.DEPTH(LANE_DEPTH)) inst_tx1 (
        .clk(sclk), .resetn(sresetn),
        .s_tvalid(s_axis_tx1_tvalid), .s_tready(s_axis_tx1_tready), .s_tdata(s_axis_tx1_tdata),
        .avail1(tx_avail1[1]), .avail2(tx_avail2[1]), .d0(tx_d0[1]), .d1(tx_d1[1]),
        .take1(tx_take1[1]), .take2(tx_take2[1])
    );
    rdma_fifo_w2r1 #(.DEPTH(LANE_DEPTH)) inst_rx0 (
        .clk(sclk), .resetn(sresetn),
        .room1(rx_room1[0]), .room2(rx_room2[0]), .put1(rx_put1[0]), .put2(rx_put2[0]),
        .d0_data(rx_d0_data), .d0_keep(rx_d0_keep), .d0_last(rx_d0_last),
        .d1_data(rx_d1_data), .d1_keep(rx_d1_keep), .d1_last(rx_d1_last),
        .m_tvalid(m_axis_rx0_tvalid), .m_tready(m_axis_rx0_tready), .m_tdata(m_axis_rx0_tdata),
        .m_tkeep(m_axis_rx0_tkeep), .m_tlast(m_axis_rx0_tlast)
    );
    rdma_fifo_w2r1 #(.DEPTH(LANE_DEPTH)) inst_rx1 (
        .clk(sclk), .resetn(sresetn),
        .room1(rx_room1[1]), .room2(rx_room2[1]), .put1(rx_put1[1]), .put2(rx_put2[1]),
        .d0_data(rx_d0_data), .d0_keep(rx_d0_keep), .d0_last(rx_d0_last),
        .d1_data(rx_d1_data), .d1_keep(rx_d1_keep), .d1_last(rx_d1_last),
        .m_tvalid(m_axis_rx1_tvalid), .m_tready(m_axis_rx1_tready), .m_tdata(m_axis_rx1_tdata),
        .m_tkeep(m_axis_rx1_tkeep), .m_tlast(m_axis_rx1_tlast)
    );

    // ================================================================ payload engines (sclk)
    wire rd_len_error, wr_len_error;

    rdma_pay_rd inst_rd (
        .clk(sclk), .resetn(sresetn),
        .req_valid(s_rd_req_valid), .req_ready(s_rd_req_ready), .req_data(s_rd_req[66:0]),
        .mm2s_cmd_tvalid(m_axis_mm2s_cmd_tvalid), .mm2s_cmd_tready(m_axis_mm2s_cmd_tready),
        .mm2s_cmd_tdata(m_axis_mm2s_cmd_tdata),
        .mm2s_tvalid(s_axis_mm2s_tvalid), .mm2s_tready(s_axis_mm2s_tready),
        .mm2s_tdata(s_axis_mm2s_tdata), .mm2s_tlast(s_axis_mm2s_tlast),
        .lane_avail1(tx_avail1), .lane_avail2(tx_avail2),
        .lane0_d0(tx_d0[0]), .lane0_d1(tx_d1[0]), .lane1_d0(tx_d0[1]), .lane1_d1(tx_d1[1]),
        .lane_take1(tx_take1), .lane_take2(tx_take2),
        .out_room(s_rd_room), .out_valid(s_rd_valid), .out_data(s_rd_data),
        .out_keep(s_rd_keep), .out_last(s_rd_last),
        .mm2s_len_error(rd_len_error)
    );

    rdma_pay_wr inst_wr (
        .clk(sclk), .resetn(sresetn),
        .req_valid(s_wr_req_valid), .req_ready(s_wr_req_ready), .req_data(s_wr_req[67:0]),
        .in_valid(s_wr_valid), .in_ready(s_wr_ready), .in_data(s_wr_data), .in_last(s_wr_last),
        .s2mm_cmd_tvalid(m_axis_s2mm_cmd_tvalid), .s2mm_cmd_tready(m_axis_s2mm_cmd_tready),
        .s2mm_cmd_tdata(m_axis_s2mm_cmd_tdata),
        .s2mm_tvalid(m_axis_s2mm_tvalid), .s2mm_tready(m_axis_s2mm_tready),
        .s2mm_tdata(m_axis_s2mm_tdata), .s2mm_tkeep(m_axis_s2mm_tkeep), .s2mm_tlast(m_axis_s2mm_tlast),
        .lane_room1(rx_room1), .lane_room2(rx_room2), .lane_put1(rx_put1), .lane_put2(rx_put2),
        .lane_d0_data(rx_d0_data), .lane_d0_keep(rx_d0_keep), .lane_d0_last(rx_d0_last),
        .lane_d1_data(rx_d1_data), .lane_d1_keep(rx_d1_keep), .lane_d1_last(rx_d1_last),
        .in_len_error(wr_len_error)
    );

    // ================================================================ sclk status
    assign s_axis_mm2s_sts_tready = 1'b1;
    assign s_axis_s2mm_sts_tready = 1'b1;

    reg  [31:0] scnt [8];
    reg  [15:0] s_last_sts;
    // DataMover status: [3:0] tag, [4] internal, [5] decode, [6] slave error, [7] OKAY
    wire mm2s_err = s_axis_mm2s_sts_tvalid && (!s_axis_mm2s_sts_tdata[7] || |s_axis_mm2s_sts_tdata[6:4]);
    wire s2mm_err = s_axis_s2mm_sts_tvalid && (!s_axis_s2mm_sts_tdata[7] || |s_axis_s2mm_sts_tdata[6:4]);
    wire [7:0] s_event = {
        rx_put1[1] ? 1'b1 : rx_put2[1],
        rx_put1[0] ? 1'b1 : rx_put2[0],
        tx_take1[1] | tx_take2[1],
        tx_take1[0] | tx_take2[0],
        wr_len_error, rd_len_error, s2mm_err, mm2s_err
    };
    // beats moved per cycle on the lanes (1 or 2), events (1)
    function automatic [31:0] inc_of(input integer i);
        case (i)
            4: inc_of = tx_take2[0] ? 32'd2 : 32'd1;
            5: inc_of = tx_take2[1] ? 32'd2 : 32'd1;
            6: inc_of = rx_put2[0] ? 32'd2 : 32'd1;
            7: inc_of = rx_put2[1] ? 32'd2 : 32'd1;
            default: inc_of = 32'd1;
        endcase
    endfunction

    always @(posedge sclk) begin
        if (!sresetn) begin
            for (int i = 0; i < 8; i++)
                scnt[i] <= 32'd0;
            s_last_sts <= 16'd0;
        end else begin
            for (int i = 0; i < 8; i++)
                if (s_event[i])
                    scnt[i] <= scnt[i] + inc_of(i);
            if (s_axis_mm2s_sts_tvalid)
                s_last_sts[7:0] <= s_axis_mm2s_sts_tdata;
            if (s_axis_s2mm_sts_tvalid)
                s_last_sts[15:8] <= s_axis_s2mm_sts_tdata;
        end
    end

    for (genvar i = 0; i < 8; i++) begin : g_cnt_cdc
        xpm_cdc_gray #(
            .DEST_SYNC_FF(3), .INIT_SYNC_FF(0), .REG_OUTPUT(1), .SIM_ASSERT_CHK(0),
            .SIM_LOSSLESS_GRAY_CHK(0), .WIDTH(32)
        ) inst_cdc (
            .src_clk(sclk), .src_in_bin(scnt[i]), .dest_clk(aclk), .dest_out_bin(s_cnt[i])
        );
    end
    // quasi-static: read for diagnosis only
    xpm_cdc_array_single #(
        .DEST_SYNC_FF(3), .INIT_SYNC_FF(0), .SIM_ASSERT_CHK(0), .SRC_INPUT_REG(1), .WIDTH(16)
    ) inst_sts_cdc (
        .src_clk(sclk), .src_in(s_last_sts), .dest_clk(aclk), .dest_out(last_sts)
    );
endmodule

// An AXI4-Stream asynchronous FIFO (first word fall through).
module rdma_cdc_axis #(
    parameter integer DATA_BITS = 1024,
    parameter integer DEPTH = 128,
    parameter         MEM = "block"
) (
    input  wire                   s_clk,
    input  wire                   s_resetn,
    input  wire                   s_valid,
    output wire                   s_ready,
    input  wire [DATA_BITS-1:0]   s_data,
    input  wire [DATA_BITS/8-1:0] s_keep,
    input  wire                   s_last,
    output wire                   s_almost_full,  // room for fewer than 8 more beats
    input  wire                   m_clk,
    output wire                   m_valid,
    input  wire                   m_ready,
    output wire [DATA_BITS-1:0]   m_data,
    output wire [DATA_BITS/8-1:0] m_keep,
    output wire                   m_last
);
    xpm_fifo_axis #(
        .CLOCKING_MODE("independent_clock"),
        .FIFO_DEPTH(DEPTH),
        .FIFO_MEMORY_TYPE(MEM),
        .PACKET_FIFO("false"),
        .TDATA_WIDTH(DATA_BITS),
        .TDEST_WIDTH(1), .TID_WIDTH(1), .TUSER_WIDTH(1),
        .CDC_SYNC_STAGES(3),
        .ECC_MODE("no_ecc"),
        .RELATED_CLOCKS(0),
        .USE_ADV_FEATURES("0002"),
        .PROG_FULL_THRESH(DEPTH - 8),
        .SIM_ASSERT_CHK(0)
    ) inst_fifo (
        .s_aclk(s_clk), .m_aclk(m_clk), .s_aresetn(s_resetn),
        .s_axis_tvalid(s_valid), .s_axis_tready(s_ready), .s_axis_tdata(s_data),
        .s_axis_tstrb(s_keep), .s_axis_tkeep(s_keep), .s_axis_tlast(s_last),
        .s_axis_tid(1'b0), .s_axis_tdest(1'b0), .s_axis_tuser(1'b0),
        .m_axis_tvalid(m_valid), .m_axis_tready(m_ready), .m_axis_tdata(m_data),
        .m_axis_tstrb(), .m_axis_tkeep(m_keep), .m_axis_tlast(m_last),
        .m_axis_tid(), .m_axis_tdest(), .m_axis_tuser(),
        .prog_full_axis(s_almost_full), .prog_empty_axis(), .almost_full_axis(), .almost_empty_axis(),
        .wr_data_count_axis(), .rd_data_count_axis(),
        .injectsbiterr_axis(1'b0), .injectdbiterr_axis(1'b0), .sbiterr_axis(), .dbiterr_axis()
    );
endmodule

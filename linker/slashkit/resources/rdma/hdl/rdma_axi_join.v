// SPDX-License-Identifier: MIT
`timescale 1ns / 1ps

// One AXI4 port from a read-only and a write-only master: an AXI DataMover's
// M_AXI_MM2S (AR, R) and M_AXI_S2MM (AW, W, B) onto one SL2NOC port. The
// channels of the two sides are disjoint, so this is wiring only: no
// arbitration, no buffering. (A SmartConnect in this place stalled the write
// path on the V80 after a few packets at line rate.)
module rdma_axi_join (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axi_rd:s_axi_wr:m_axi" *)
    input  wire          aclk,

    // read side (MM2S)
    input  wire [3:0]    s_axi_rd_arid,
    input  wire [63:0]   s_axi_rd_araddr,
    input  wire [7:0]    s_axi_rd_arlen,
    input  wire [2:0]    s_axi_rd_arsize,
    input  wire [1:0]    s_axi_rd_arburst,
    input  wire [2:0]    s_axi_rd_arprot,
    input  wire [3:0]    s_axi_rd_arcache,
    input  wire          s_axi_rd_arvalid,
    output wire          s_axi_rd_arready,
    output wire [511:0]  s_axi_rd_rdata,
    output wire [1:0]    s_axi_rd_rresp,
    output wire          s_axi_rd_rlast,
    output wire          s_axi_rd_rvalid,
    input  wire          s_axi_rd_rready,

    // write side (S2MM)
    input  wire [3:0]    s_axi_wr_awid,
    input  wire [63:0]   s_axi_wr_awaddr,
    input  wire [7:0]    s_axi_wr_awlen,
    input  wire [2:0]    s_axi_wr_awsize,
    input  wire [1:0]    s_axi_wr_awburst,
    input  wire [2:0]    s_axi_wr_awprot,
    input  wire [3:0]    s_axi_wr_awcache,
    input  wire          s_axi_wr_awvalid,
    output wire          s_axi_wr_awready,
    input  wire [511:0]  s_axi_wr_wdata,
    input  wire [63:0]   s_axi_wr_wstrb,
    input  wire          s_axi_wr_wlast,
    input  wire          s_axi_wr_wvalid,
    output wire          s_axi_wr_wready,
    output wire [1:0]    s_axi_wr_bresp,
    output wire          s_axi_wr_bvalid,
    input  wire          s_axi_wr_bready,

    // to the SL2NOC port
    output wire [3:0]    m_axi_awid,
    output wire [63:0]   m_axi_awaddr,
    output wire [7:0]    m_axi_awlen,
    output wire [2:0]    m_axi_awsize,
    output wire [1:0]    m_axi_awburst,
    output wire [2:0]    m_axi_awprot,
    output wire [3:0]    m_axi_awcache,
    output wire          m_axi_awvalid,
    input  wire          m_axi_awready,
    output wire [511:0]  m_axi_wdata,
    output wire [63:0]   m_axi_wstrb,
    output wire          m_axi_wlast,
    output wire          m_axi_wvalid,
    input  wire          m_axi_wready,
    input  wire [3:0]    m_axi_bid,
    input  wire [1:0]    m_axi_bresp,
    input  wire          m_axi_bvalid,
    output wire          m_axi_bready,
    output wire [3:0]    m_axi_arid,
    output wire [63:0]   m_axi_araddr,
    output wire [7:0]    m_axi_arlen,
    output wire [2:0]    m_axi_arsize,
    output wire [1:0]    m_axi_arburst,
    output wire [2:0]    m_axi_arprot,
    output wire [3:0]    m_axi_arcache,
    output wire          m_axi_arvalid,
    input  wire          m_axi_arready,
    input  wire [3:0]    m_axi_rid,
    input  wire [511:0]  m_axi_rdata,
    input  wire [1:0]    m_axi_rresp,
    input  wire          m_axi_rlast,
    input  wire          m_axi_rvalid,
    output wire          m_axi_rready
);
    assign m_axi_awid = s_axi_wr_awid;
    assign m_axi_awaddr = s_axi_wr_awaddr;
    assign m_axi_awlen = s_axi_wr_awlen;
    assign m_axi_awsize = s_axi_wr_awsize;
    assign m_axi_awburst = s_axi_wr_awburst;
    assign m_axi_awprot = s_axi_wr_awprot;
    assign m_axi_awcache = s_axi_wr_awcache;
    assign m_axi_awvalid = s_axi_wr_awvalid;
    assign s_axi_wr_awready = m_axi_awready;
    assign m_axi_wdata = s_axi_wr_wdata;
    assign m_axi_wstrb = s_axi_wr_wstrb;
    assign m_axi_wlast = s_axi_wr_wlast;
    assign m_axi_wvalid = s_axi_wr_wvalid;
    assign s_axi_wr_wready = m_axi_wready;
    assign s_axi_wr_bresp = m_axi_bresp;
    assign s_axi_wr_bvalid = m_axi_bvalid;
    assign m_axi_bready = s_axi_wr_bready;

    assign m_axi_arid = s_axi_rd_arid;
    assign m_axi_araddr = s_axi_rd_araddr;
    assign m_axi_arlen = s_axi_rd_arlen;
    assign m_axi_arsize = s_axi_rd_arsize;
    assign m_axi_arburst = s_axi_rd_arburst;
    assign m_axi_arprot = s_axi_rd_arprot;
    assign m_axi_arcache = s_axi_rd_arcache;
    assign m_axi_arvalid = s_axi_rd_arvalid;
    assign s_axi_rd_arready = m_axi_arready;
    assign s_axi_rd_rdata = m_axi_rdata;
    assign s_axi_rd_rresp = m_axi_rresp;
    assign s_axi_rd_rlast = m_axi_rlast;
    assign s_axi_rd_rvalid = m_axi_rvalid;
    assign m_axi_rready = s_axi_rd_rready;
endmodule

/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

module axi4_full_passthrough #(
  parameter integer AXI_ID_WIDTH     = 6,
  parameter integer AXI_ADDR_WIDTH   = 64,
  parameter integer AXI_DATA_WIDTH   = 256,
  parameter integer AXI_AWUSER_WIDTH = 1,
  parameter integer AXI_WUSER_WIDTH  = 1,
  parameter integer AXI_BUSER_WIDTH  = 1,
  parameter integer AXI_ARUSER_WIDTH = 1,
  parameter integer AXI_RUSER_WIDTH  = 1
)(
  input  wire                        aclk,
  input  wire                        aresetn,

  // -----------------------
  // Slave AXI4-Full (S_AXI)
  // -----------------------
  // Write address channel
  input  wire [AXI_ID_WIDTH-1:0]      s_axi_awid,
  input  wire [AXI_ADDR_WIDTH-1:0]    s_axi_awaddr,
  input  wire [7:0]                   s_axi_awlen,
  input  wire [2:0]                   s_axi_awsize,
  input  wire [1:0]                   s_axi_awburst,
  input  wire                         s_axi_awlock,
  input  wire [3:0]                   s_axi_awcache,
  input  wire [2:0]                   s_axi_awprot,
  input  wire [3:0]                   s_axi_awqos,
  input  wire [3:0]                   s_axi_awregion,
  input  wire [AXI_AWUSER_WIDTH-1:0]  s_axi_awuser,
  input  wire                         s_axi_awvalid,
  output wire                         s_axi_awready,

  // Write data channel
  input  wire [AXI_DATA_WIDTH-1:0]    s_axi_wdata,
  input  wire [(AXI_DATA_WIDTH/8)-1:0] s_axi_wstrb,
  input  wire                         s_axi_wlast,
  input  wire [AXI_WUSER_WIDTH-1:0]   s_axi_wuser,
  input  wire                         s_axi_wvalid,
  output wire                         s_axi_wready,

  // Write response channel
  output wire [AXI_ID_WIDTH-1:0]      s_axi_bid,
  output wire [1:0]                   s_axi_bresp,
  output wire [AXI_BUSER_WIDTH-1:0]   s_axi_buser,
  output wire                         s_axi_bvalid,
  input  wire                         s_axi_bready,

  // Read address channel
  input  wire [AXI_ID_WIDTH-1:0]      s_axi_arid,
  input  wire [AXI_ADDR_WIDTH-1:0]    s_axi_araddr,
  input  wire [7:0]                   s_axi_arlen,
  input  wire [2:0]                   s_axi_arsize,
  input  wire [1:0]                   s_axi_arburst,
  input  wire                         s_axi_arlock,
  input  wire [3:0]                   s_axi_arcache,
  input  wire [2:0]                   s_axi_arprot,
  input  wire [3:0]                   s_axi_arqos,
  input  wire [3:0]                   s_axi_arregion,
  input  wire [AXI_ARUSER_WIDTH-1:0]  s_axi_aruser,
  input  wire                         s_axi_arvalid,
  output wire                         s_axi_arready,

  // Read data channel
  output wire [AXI_ID_WIDTH-1:0]      s_axi_rid,
  output wire [AXI_DATA_WIDTH-1:0]    s_axi_rdata,
  output wire [1:0]                   s_axi_rresp,
  output wire                         s_axi_rlast,
  output wire [AXI_RUSER_WIDTH-1:0]   s_axi_ruser,
  output wire                         s_axi_rvalid,
  input  wire                         s_axi_rready,

  // ------------------------
  // Master AXI4-Full (M_AXI)
  // ------------------------
  // Write address channel
  output wire [AXI_ID_WIDTH-1:0]      m_axi_awid,
  output wire [AXI_ADDR_WIDTH-1:0]    m_axi_awaddr,
  output wire [7:0]                   m_axi_awlen,
  output wire [2:0]                   m_axi_awsize,
  output wire [1:0]                   m_axi_awburst,
  output wire                         m_axi_awlock,
  output wire [3:0]                   m_axi_awcache,
  output wire [2:0]                   m_axi_awprot,
  output wire [3:0]                   m_axi_awqos,
  output wire [3:0]                   m_axi_awregion,
  output wire [AXI_AWUSER_WIDTH-1:0]  m_axi_awuser,
  output wire                         m_axi_awvalid,
  input  wire                         m_axi_awready,

  // Write data channel
  output wire [AXI_DATA_WIDTH-1:0]    m_axi_wdata,
  output wire [(AXI_DATA_WIDTH/8)-1:0] m_axi_wstrb,
  output wire                         m_axi_wlast,
  output wire [AXI_WUSER_WIDTH-1:0]   m_axi_wuser,
  output wire                         m_axi_wvalid,
  input  wire                         m_axi_wready,

  // Write response channel
  input  wire [AXI_ID_WIDTH-1:0]      m_axi_bid,
  input  wire [1:0]                   m_axi_bresp,
  input  wire [AXI_BUSER_WIDTH-1:0]   m_axi_buser,
  input  wire                         m_axi_bvalid,
  output wire                         m_axi_bready,

  // Read address channel
  output wire [AXI_ID_WIDTH-1:0]      m_axi_arid,
  output wire [AXI_ADDR_WIDTH-1:0]    m_axi_araddr,
  output wire [7:0]                   m_axi_arlen,
  output wire [2:0]                   m_axi_arsize,
  output wire [1:0]                   m_axi_arburst,
  output wire                         m_axi_arlock,
  output wire [3:0]                   m_axi_arcache,
  output wire [2:0]                   m_axi_arprot,
  output wire [3:0]                   m_axi_arqos,
  output wire [3:0]                   m_axi_arregion,
  output wire [AXI_ARUSER_WIDTH-1:0]  m_axi_aruser,
  output wire                         m_axi_arvalid,
  input  wire                         m_axi_arready,

  // Read data channel
  input  wire [AXI_ID_WIDTH-1:0]      m_axi_rid,
  input  wire [AXI_DATA_WIDTH-1:0]    m_axi_rdata,
  input  wire [1:0]                   m_axi_rresp,
  input  wire                         m_axi_rlast,
  input  wire [AXI_RUSER_WIDTH-1:0]   m_axi_ruser,
  input  wire                         m_axi_rvalid,
  output wire                         m_axi_rready
);

  // aclk/aresetn are intentionally unused (wire-through)
  wire _unused = aclk ^ aresetn;

  // -------------------------
  // AW channel passthrough
  // -------------------------
  assign m_axi_awid     = s_axi_awid;
  assign m_axi_awaddr   = s_axi_awaddr;
  assign m_axi_awlen    = s_axi_awlen;
  assign m_axi_awsize   = s_axi_awsize;
  assign m_axi_awburst  = s_axi_awburst;
  assign m_axi_awlock   = s_axi_awlock;
  assign m_axi_awcache  = s_axi_awcache;
  assign m_axi_awprot   = s_axi_awprot;
  assign m_axi_awqos    = s_axi_awqos;
  assign m_axi_awregion = s_axi_awregion;
  assign m_axi_awuser   = s_axi_awuser;
  assign m_axi_awvalid  = s_axi_awvalid;
  assign s_axi_awready  = m_axi_awready;

  // -------------------------
  // W channel passthrough
  // -------------------------
  assign m_axi_wdata   = s_axi_wdata;
  assign m_axi_wstrb   = s_axi_wstrb;
  assign m_axi_wlast   = s_axi_wlast;
  assign m_axi_wuser   = s_axi_wuser;
  assign m_axi_wvalid  = s_axi_wvalid;
  assign s_axi_wready  = m_axi_wready;

  // -------------------------
  // B channel passthrough
  // -------------------------
  assign s_axi_bid     = m_axi_bid;
  assign s_axi_bresp   = m_axi_bresp;
  assign s_axi_buser   = m_axi_buser;
  assign s_axi_bvalid  = m_axi_bvalid;
  assign m_axi_bready  = s_axi_bready;

  // -------------------------
  // AR channel passthrough
  // -------------------------
  assign m_axi_arid     = s_axi_arid;
  assign m_axi_araddr   = s_axi_araddr;
  assign m_axi_arlen    = s_axi_arlen;
  assign m_axi_arsize   = s_axi_arsize;
  assign m_axi_arburst  = s_axi_arburst;
  assign m_axi_arlock   = s_axi_arlock;
  assign m_axi_arcache  = s_axi_arcache;
  assign m_axi_arprot   = s_axi_arprot;
  assign m_axi_arqos    = s_axi_arqos;
  assign m_axi_arregion = s_axi_arregion;
  assign m_axi_aruser   = s_axi_aruser;
  assign m_axi_arvalid  = s_axi_arvalid;
  assign s_axi_arready  = m_axi_arready;

  // -------------------------
  // R channel passthrough
  // -------------------------
  assign s_axi_rid     = m_axi_rid;
  assign s_axi_rdata   = m_axi_rdata;
  assign s_axi_rresp   = m_axi_rresp;
  assign s_axi_rlast   = m_axi_rlast;
  assign s_axi_ruser   = m_axi_ruser;
  assign s_axi_rvalid  = m_axi_rvalid;
  assign m_axi_rready  = s_axi_rready;

endmodule

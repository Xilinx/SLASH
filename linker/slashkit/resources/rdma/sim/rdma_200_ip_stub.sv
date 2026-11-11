// SPDX-License-Identifier: MIT
// Port-compatible stand-in for rdma_200_ip (compile checks and unit tests of
// the service around it; the real IP comes from fpga-network-stack).
module rdma_200_ip (
    input  wire aclk, input wire aresetn,
    input  wire [11:0] s_axil_awaddr, input wire [2:0] s_axil_awprot, input wire s_axil_awvalid, output wire s_axil_awready,
    input  wire [31:0] s_axil_wdata, input wire [3:0] s_axil_wstrb, input wire s_axil_wvalid, output wire s_axil_wready,
    output wire [1:0] s_axil_bresp, output wire s_axil_bvalid, input wire s_axil_bready,
    input  wire [11:0] s_axil_araddr, input wire [2:0] s_axil_arprot, input wire s_axil_arvalid, output wire s_axil_arready,
    output wire [31:0] s_axil_rdata, output wire [1:0] s_axil_rresp, output wire s_axil_rvalid, input wire s_axil_rready,
    input  wire s_axis_rx_tvalid, output wire s_axis_rx_tready, input wire [1023:0] s_axis_rx_tdata,
    input  wire [127:0] s_axis_rx_tkeep, input wire s_axis_rx_tlast, input wire s_axis_rx_tuser,
    output wire m_axis_tx_tvalid, input wire m_axis_tx_tready, output wire [1023:0] m_axis_tx_tdata,
    output wire [127:0] m_axis_tx_tkeep, output wire m_axis_tx_tlast,
    input  wire s_axis_sq_tvalid, output wire s_axis_sq_tready, input wire [191:0] s_axis_sq_tdata,
    output wire m_axis_cq_tvalid, output wire [31:0] m_axis_cq_tdata,
    output wire m_axis_rd_req_tvalid, input wire m_axis_rd_req_tready, output wire [127:0] m_axis_rd_req_tdata,
    input  wire s_axis_rd_tvalid, output wire s_axis_rd_tready, input wire [1023:0] s_axis_rd_tdata,
    input  wire [127:0] s_axis_rd_tkeep, input wire s_axis_rd_tlast,
    output wire m_axis_wr_req_tvalid, input wire m_axis_wr_req_tready, output wire [127:0] m_axis_wr_req_tdata,
    output wire m_axis_wr_tvalid, input wire m_axis_wr_tready, output wire [1023:0] m_axis_wr_tdata,
    output wire [127:0] m_axis_wr_tkeep, output wire m_axis_wr_tlast
);
endmodule

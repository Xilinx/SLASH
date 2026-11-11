`timescale 1ns / 1ps

// Host control and status of one DCMAC's GT quad and reset sequencer
// (Versal-DCMAC dcmac_reset_ctrl), between the RoCE node registers (PHY_CTRL /
// PHY_STATUS, node clock) and the DCMAC hierarchy (sys_clk, the GT free-running
// clock).
//
// ctrl (node clock, quasi-static):
//   [2:0] GT loopback, all four channels (0 normal, 1 near-end PCS,
//         2 near-end PMA, 4 far-end PMA, 6 far-end PCS)
//   [3]   RX CDR hold, all channels
//   [4]   GT RX datapath reset
//   [5]   GT RX PLL and datapath reset
//   [6]   GT TX datapath reset
//   [7]   hold the DCMAC/GT reset sequencer in reset; on release it reruns from
//         the GT reset (as after configuration)
// status (node clock):
//   [0] GT TX reset done   [1] GT RX reset done   [2] GT power good
//   [3] DCMAC clock wizard locked
//   [4] DCMAC TX core reset   [5] DCMAC RX core reset
//   [6] DCMAC RX serdes reset, port 0   [7] AXIS out of reset
//   [8] sequencer held (ctrl[7] as seen in sys_clk)
module dcmac_phy_ctl (
    input  wire        sys_clk,
    input  wire        user_clk,
    input  wire        aresetn,             // hierarchy reset (asynchronous)

    input  wire [15:0] ctrl,                // user_clk
    output wire [15:0] status,              // user_clk

    // sys_clk
    output reg  [2:0]  gt_loopback,
    output reg         gt_rxcdrhold,
    output reg         gt_rst_rx_datapath,
    output reg         gt_rst_rx_pll_datapath,
    output reg         gt_rst_tx_datapath,
    output wire        fsm_resetn,          // to dcmac_reset_ctrl async_resetn

    input  wire        gt_rst_tx_done,
    input  wire        gt_rst_rx_done,
    input  wire        gtpowergood,
    input  wire        user_locked,
    input  wire        tx_core_reset,
    input  wire        rx_core_reset,
    input  wire [5:0]  rx_serdes_reset,
    input  wire        axis_resetn
);
    wire [7:0] ctrl_s;
    xpm_cdc_array_single #(
        .DEST_SYNC_FF(4), .INIT_SYNC_FF(0), .SIM_ASSERT_CHK(0), .SRC_INPUT_REG(1), .WIDTH(8)
    ) inst_ctrl_cdc (
        .src_clk(user_clk), .src_in(ctrl[7:0]), .dest_clk(sys_clk), .dest_out(ctrl_s)
    );

    reg hold = 1'b0;
    always @(posedge sys_clk) begin
        gt_loopback <= ctrl_s[2:0];
        gt_rxcdrhold <= ctrl_s[3];
        gt_rst_rx_datapath <= ctrl_s[4];
        gt_rst_rx_pll_datapath <= ctrl_s[5];
        gt_rst_tx_datapath <= ctrl_s[6];
        hold <= ctrl_s[7];
    end
    // dcmac_reset_ctrl synchronizes its reset to sys_clk itself
    assign fsm_resetn = aresetn & ~hold;

    wire [8:0] st_s;
    xpm_cdc_array_single #(
        .DEST_SYNC_FF(4), .INIT_SYNC_FF(0), .SIM_ASSERT_CHK(0), .SRC_INPUT_REG(0), .WIDTH(9)
    ) inst_status_cdc (
        .src_clk(sys_clk),
        .src_in({hold, axis_resetn, rx_serdes_reset[0], rx_core_reset, tx_core_reset,
                 user_locked, gtpowergood, gt_rst_rx_done, gt_rst_tx_done}),
        .dest_clk(user_clk), .dest_out(st_s)
    );
    assign status = {7'd0, st_s};
endmodule

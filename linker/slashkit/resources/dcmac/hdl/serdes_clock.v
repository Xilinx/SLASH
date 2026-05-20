// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT

`timescale 1ns / 1ps

module clock_to_serdes (
	input  usrclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:gt_usrclk:1.0 GT_USRCLK.RX_ALT_SERDES_CLK CLK" *) (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME GT_USRCLK.RX_ALT_SERDES_CLK, CLK_DOMAIN dcmac_200g_exdes_support_rx_alt_serdes_clk, FREQ_HZ 156250000, PARENT_ID undef, PHASE 0.0" *)
	output wire	[5:0] serdes_clk
	);

    assign serdes_clk = {1'b0, 1'b0, 1'b0, 1'b0, usrclk, usrclk};

endmodule
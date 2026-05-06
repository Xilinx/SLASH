// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT

`timescale 1ns / 1ps

module clock_to_clock_bus (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *)
	input  clk,
	output wire	[5:0] clockbus
	);

    assign clockbus = {6{clk}};

endmodule
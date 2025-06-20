// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT

`timescale 1ps/1ps

module dcmac_syncer_reset #(
    parameter RESET_PIPE_LEN = 3
)
(
    input  wire clk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 clk_wizard_lock,resetn_async RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire clk_wizard_lock,
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire resetn_async,
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    output wire resetn
);

    (* ASYNC_REG = "TRUE" *) reg  [RESET_PIPE_LEN-1:0] reset_pipe_retime;
    reg  reset_pipe_out = 1'b0;
    assign resetn_async_inv = resetn_async & clk_wizard_lock;

    always @(posedge clk or negedge resetn_async_inv) begin
        if (resetn_async_inv == 1'b0) begin
            reset_pipe_retime <= {RESET_PIPE_LEN{1'b0}};
            reset_pipe_out    <= 1'b0;
        end
        else begin
            reset_pipe_retime <= {reset_pipe_retime[RESET_PIPE_LEN-2:0], 1'b1};
            reset_pipe_out    <= reset_pipe_retime[RESET_PIPE_LEN-1];
        end
    end

    assign resetn = reset_pipe_out;

endmodule
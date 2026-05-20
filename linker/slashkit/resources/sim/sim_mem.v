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
 
`timescale 1ns / 1ps

module sim_mem
#(
    parameter MEM_WIDTH = 64,
    parameter MEM_DEPTH_LOG = 26,
    parameter READ_LATENCY = 50
)(

(* X_INTERFACE_PARAMETER = "MODE Slave, MASTER_TYPE BRAM_CTRL, MEM_ECC NONE, READ_WRITE_MODE READ_WRITE" *)
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_A CLK" *)
    input clk_a,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_A RST" *)
    input rst_a,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_A EN" *)
    input en_a,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_A ADDR" *)
    input [MEM_DEPTH_LOG-1:0] addr_a,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_A WE" *)
    input [MEM_WIDTH/8-1:0] we_a,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_A DIN" *)
    input [MEM_WIDTH-1:0] din_a,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_A DOUT" *)
    output [MEM_WIDTH-1:0] dout_a,

(* X_INTERFACE_PARAMETER = "MODE Slave, MASTER_TYPE BRAM_CTRL, MEM_ECC NONE, READ_WRITE_MODE READ_WRITE" *)
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_B CLK" *)
    input clk_b,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_B RST" *)
    input rst_b,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_B EN" *)
    input en_b,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_B ADDR" *)
    input [MEM_DEPTH_LOG-1:0] addr_b,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_B WE" *)
    input [MEM_WIDTH/8-1:0] we_b,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_B DIN" *)
    input [MEM_WIDTH-1:0] din_b,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_B DOUT" *)
    output [MEM_WIDTH-1:0] dout_b
);


reg [MEM_WIDTH-1:0] mem[2**MEM_DEPTH_LOG-1:0];


genvar byte_idx;
generate for(byte_idx=0; byte_idx<MEM_WIDTH/8; byte_idx=byte_idx+1) begin: byte_write
    always @(posedge clk_a)
        if(en_a)
            if(we_a[byte_idx])
                mem[addr_a][8*(byte_idx+1)-1:8*byte_idx] <= din_a[8*(byte_idx+1)-1:8*byte_idx];
    always @(posedge clk_b)
        if(en_b)
            if(we_b[byte_idx])
                mem[addr_b][8*(byte_idx+1)-1:8*byte_idx] <= din_b[8*(byte_idx+1)-1:8*byte_idx];
end
endgenerate

reg [MEM_WIDTH-1:0] delayline_a[READ_LATENCY-1:0];
reg [MEM_WIDTH-1:0] delayline_b[READ_LATENCY-1:0];

always @(posedge clk_a)
    if(rst_a)     delayline_a[0] <= 0; 
    else if(en_a) delayline_a[0] <= mem[addr_a];

always @(posedge clk_b)
    if(rst_b)     delayline_b[0] <= 0; 
    else if(en_b) delayline_b[0] <= mem[addr_b];

genvar i;
generate for(i=1; i<READ_LATENCY; i=i+1) begin: read_delay
    always @(posedge clk_a)
        if(rst_a) delayline_a[i] <= 0; 
        else      delayline_a[i] <= delayline_a[i-1];
    always @(posedge clk_b)
        if(rst_b) delayline_b[i] <= 0; 
        else      delayline_b[i] <= delayline_b[i-1];
end
endgenerate

assign dout_a = delayline_a[READ_LATENCY-1];
assign dout_b = delayline_b[READ_LATENCY-1];

endmodule

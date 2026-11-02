`timescale 1ns / 1ps

// AXI4-Lite 1:2 split of a node window: addresses whose bits 23:20 equal
// REGION go to m1 (bits 11:0 only), all others to m0. One write and one read
// at a time, so responses come back in order.
module axil_split #(
    parameter [3:0] REGION = 4'd4
) (
    input  wire        clk,
    input  wire        resetn,

    input  wire [23:0] s_awaddr,
    input  wire        s_awvalid,
    output reg         s_awready,
    input  wire [31:0] s_wdata,
    input  wire [3:0]  s_wstrb,
    input  wire        s_wvalid,
    output reg         s_wready,
    output reg  [1:0]  s_bresp,
    output reg         s_bvalid,
    input  wire        s_bready,
    input  wire [23:0] s_araddr,
    input  wire        s_arvalid,
    output reg         s_arready,
    output reg  [31:0] s_rdata,
    output reg  [1:0]  s_rresp,
    output reg         s_rvalid,
    input  wire        s_rready,

    output reg  [23:0] m0_awaddr,
    output reg         m0_awvalid,
    input  wire        m0_awready,
    output reg  [31:0] m0_wdata,
    output reg  [3:0]  m0_wstrb,
    output reg         m0_wvalid,
    input  wire        m0_wready,
    input  wire [1:0]  m0_bresp,
    input  wire        m0_bvalid,
    output wire        m0_bready,
    output reg  [23:0] m0_araddr,
    output reg         m0_arvalid,
    input  wire        m0_arready,
    input  wire [31:0] m0_rdata,
    input  wire [1:0]  m0_rresp,
    input  wire        m0_rvalid,
    output wire        m0_rready,

    output wire [11:0] m1_awaddr,
    output reg         m1_awvalid,
    input  wire        m1_awready,
    output wire [31:0] m1_wdata,
    output wire [3:0]  m1_wstrb,
    output reg         m1_wvalid,
    input  wire        m1_wready,
    input  wire [1:0]  m1_bresp,
    input  wire        m1_bvalid,
    output wire        m1_bready,
    output wire [11:0] m1_araddr,
    output reg         m1_arvalid,
    input  wire        m1_arready,
    input  wire [31:0] m1_rdata,
    input  wire [1:0]  m1_rresp,
    input  wire        m1_rvalid,
    output wire        m1_rready
);
    // ---------------------------------------------------------------- write
    reg w_busy, w_sel;                      // a write is out, to m1

    assign m1_awaddr = m0_awaddr[11:0];
    assign m1_wdata = m0_wdata;
    assign m1_wstrb = m0_wstrb;
    assign m0_bready = w_busy && !w_sel && !s_bvalid;
    assign m1_bready = w_busy && w_sel && !s_bvalid;

    always @(posedge clk) begin
        if (!resetn) begin
            w_busy <= 1'b0;
            s_awready <= 1'b0;
            s_wready <= 1'b0;
            s_bvalid <= 1'b0;
            m0_awvalid <= 1'b0;
            m0_wvalid <= 1'b0;
            m1_awvalid <= 1'b0;
            m1_wvalid <= 1'b0;
        end else begin
            s_awready <= 1'b0;
            s_wready <= 1'b0;
            if (s_bvalid && s_bready)
                s_bvalid <= 1'b0;
            if (m0_awvalid && m0_awready) m0_awvalid <= 1'b0;
            if (m0_wvalid && m0_wready) m0_wvalid <= 1'b0;
            if (m1_awvalid && m1_awready) m1_awvalid <= 1'b0;
            if (m1_wvalid && m1_wready) m1_wvalid <= 1'b0;
            if (!w_busy) begin
                if (s_awvalid && s_wvalid && !s_awready) begin
                    s_awready <= 1'b1;
                    s_wready <= 1'b1;
                    w_busy <= 1'b1;
                    w_sel <= s_awaddr[23:20] == REGION;
                    m0_awaddr <= s_awaddr;
                    m0_wdata <= s_wdata;
                    m0_wstrb <= s_wstrb;
                    if (s_awaddr[23:20] == REGION) begin
                        m1_awvalid <= 1'b1;
                        m1_wvalid <= 1'b1;
                    end else begin
                        m0_awvalid <= 1'b1;
                        m0_wvalid <= 1'b1;
                    end
                end
            end else if (!s_bvalid && (w_sel ? m1_bvalid : m0_bvalid)) begin
                s_bvalid <= 1'b1;
                s_bresp <= w_sel ? m1_bresp : m0_bresp;
                w_busy <= 1'b0;
            end
        end
    end

    // ---------------------------------------------------------------- read
    reg r_busy, r_sel;

    assign m1_araddr = m0_araddr[11:0];
    assign m0_rready = r_busy && !r_sel && !s_rvalid;
    assign m1_rready = r_busy && r_sel && !s_rvalid;

    always @(posedge clk) begin
        if (!resetn) begin
            r_busy <= 1'b0;
            s_arready <= 1'b0;
            s_rvalid <= 1'b0;
            m0_arvalid <= 1'b0;
            m1_arvalid <= 1'b0;
        end else begin
            s_arready <= 1'b0;
            if (s_rvalid && s_rready)
                s_rvalid <= 1'b0;
            if (m0_arvalid && m0_arready) m0_arvalid <= 1'b0;
            if (m1_arvalid && m1_arready) m1_arvalid <= 1'b0;
            if (!r_busy) begin
                if (s_arvalid && !s_arready) begin
                    s_arready <= 1'b1;
                    r_busy <= 1'b1;
                    r_sel <= s_araddr[23:20] == REGION;
                    m0_araddr <= s_araddr;
                    if (s_araddr[23:20] == REGION)
                        m1_arvalid <= 1'b1;
                    else
                        m0_arvalid <= 1'b1;
                end
            end else if (!s_rvalid && (r_sel ? m1_rvalid : m0_rvalid)) begin
                s_rvalid <= 1'b1;
                s_rdata <= r_sel ? m1_rdata : m0_rdata;
                s_rresp <= r_sel ? m1_rresp : m0_rresp;
                r_busy <= 1'b0;
            end
        end
    end
endmodule

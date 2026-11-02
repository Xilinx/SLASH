`timescale 1ns / 1ps

// DCMAC TX path for the 200G RoCE node: replaces the Versal-DCMAC datapath_tx
// (which takes 512 bits in the user clock).
//
//   aclk (node clock, 230 MHz): 1024-bit AXIS from the network stack
//     -> pad frames shorter than 60 bytes (the DCMAC appends the FCS but
//        does not pad; ARP replies are 42 bytes)
//     -> packet FIFO: a frame leaves only once it is complete, and then
//        back to back at 250 Gb/s, faster than the DCMAC side drains it, so
//        the DCMAC never sees a frame underrun
//     -> dcmac_tx_cdc: to dclk (391 MHz), every beat split into two
//        512-bit beats
//     -> 512-bit AXIS to axis_to_dcmac_seg (4 segments of 128 bits)
//
// 512 bits at 391 MHz carry 200.2 Gb/s, just above the 200G line. The packet
// FIFO stays in aclk: its read-side logic in packet mode is too deep for
// 391 MHz.
module dcmac_tx_path #(
    parameter integer FIFO_DEPTH = 128          // beats of 128 bytes, a few 4 KiB frames
) (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axis, ASSOCIATED_RESET aresetn" *)
    input  wire            aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire            aresetn,

    input  wire [1023:0]   s_axis_tdata,
    input  wire [127:0]    s_axis_tkeep,
    input  wire            s_axis_tlast,
    input  wire            s_axis_tvalid,
    output wire            s_axis_tready,

    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 dclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF m_axis, ASSOCIATED_RESET dresetn" *)
    input  wire            dclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 dresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire            dresetn,

    output wire [511:0]    m_axis_tdata,
    output wire [63:0]     m_axis_tkeep,
    output wire            m_axis_tlast,
    output wire            m_axis_tvalid,
    input  wire            m_axis_tready
);
    localparam integer MIN_BYTES = 60;

    // ---- aclk: padding, one register stage ----
    reg  [1023:0] p_tdata;
    reg  [127:0]  p_tkeep;
    reg           p_tlast, p_tvalid;
    wire          p_tready;
    reg           first;                        // next beat starts a frame

    assign s_axis_tready = !p_tvalid || p_tready;

    integer i;
    always @(posedge aclk) begin
        if (!aresetn) begin
            p_tvalid <= 1'b0;
            first <= 1'b1;
        end else begin
            if (s_axis_tready) begin
                p_tvalid <= s_axis_tvalid;
                p_tdata <= s_axis_tdata;
                p_tkeep <= s_axis_tkeep;
                p_tlast <= s_axis_tlast;
                // a frame shorter than 60 bytes is a single beat
                if (s_axis_tvalid && s_axis_tlast && first) begin
                    for (i = 0; i < MIN_BYTES; i = i + 1) begin
                        if (!s_axis_tkeep[i]) begin
                            p_tkeep[i] <= 1'b1;
                            p_tdata[i*8 +: 8] <= 8'h00;
                        end
                    end
                end
            end
            if (s_axis_tvalid && s_axis_tready)
                first <= s_axis_tlast;
        end
    end

    // ---- packet FIFO (aclk), then aclk -> dclk ----
    wire [1023:0] f_tdata;
    wire [127:0]  f_tkeep;
    wire          f_tlast, f_tvalid;
    wire          f_tready;

    xpm_fifo_axis #(
        .CLOCKING_MODE("common_clock"),
        .FIFO_DEPTH(FIFO_DEPTH),
        .FIFO_MEMORY_TYPE("auto"),
        .PACKET_FIFO("true"),
        .TDATA_WIDTH(1024),
        .TDEST_WIDTH(1),
        .TID_WIDTH(1),
        .TUSER_WIDTH(1),
        .CDC_SYNC_STAGES(3),
        .ECC_MODE("no_ecc"),
        .RELATED_CLOCKS(0),
        .USE_ADV_FEATURES("0000")
    ) inst_pkt (
        .s_aclk(aclk),
        .s_aresetn(aresetn),
        .s_axis_tvalid(p_tvalid),
        .s_axis_tready(p_tready),
        .s_axis_tdata(p_tdata),
        .s_axis_tkeep(p_tkeep),
        .s_axis_tstrb(p_tkeep),
        .s_axis_tlast(p_tlast),
        .s_axis_tdest(1'b0),
        .s_axis_tid(1'b0),
        .s_axis_tuser(1'b0),
        .m_aclk(aclk),
        .m_axis_tvalid(f_tvalid),
        .m_axis_tready(f_tready),
        .m_axis_tdata(f_tdata),
        .m_axis_tkeep(f_tkeep),
        .m_axis_tstrb(),
        .m_axis_tlast(f_tlast),
        .m_axis_tdest(),
        .m_axis_tid(),
        .m_axis_tuser(),
        .almost_empty_axis(),
        .almost_full_axis(),
        .dbiterr_axis(),
        .prog_empty_axis(),
        .prog_full_axis(),
        .rd_data_count_axis(),
        .sbiterr_axis(),
        .wr_data_count_axis(),
        .injectdbiterr_axis(1'b0),
        .injectsbiterr_axis(1'b0)
    );

    // ---- aclk -> dclk, 1024 -> 512, low half first ----
    wire [511:0] w_tdata;
    wire [63:0]  w_tkeep;
    wire         w_tlast, w_tvalid;
    wire         w_tready;

    dcmac_tx_cdc inst_cdc (
        .aclk(aclk),
        .aresetn(aresetn),
        .s_tdata(f_tdata),
        .s_tkeep(f_tkeep),
        .s_tlast(f_tlast),
        .s_tvalid(f_tvalid),
        .s_tready(f_tready),
        .dclk(dclk),
        .m_tdata(w_tdata),
        .m_tkeep(w_tkeep),
        .m_tlast(w_tlast),
        .m_tvalid(w_tvalid),
        .m_tready(w_tready)
    );

    // full register slice in front of the DCMAC (tx_ready is combinational there)
    axis_skid #(
        .DATA_BITS(512 + 64 + 1),
        .SLICES(16)
    ) inst_skid (
        .clk(dclk),
        .resetn(dresetn),
        .s_valid(w_tvalid),
        .s_ready(w_tready),
        .s_data({w_tlast, w_tkeep, w_tdata}),
        .m_valid(m_axis_tvalid),
        .m_ready(m_axis_tready),
        .m_data({m_axis_tlast, m_axis_tkeep, m_axis_tdata})
    );
endmodule

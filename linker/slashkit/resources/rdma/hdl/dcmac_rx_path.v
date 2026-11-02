`timescale 1ns / 1ps

// DCMAC RX path for the 200G RoCE node: replaces the Versal-DCMAC datapath_rx
// (which converts the 1024-bit RX stream to 512 bits and drops tuser).
//
//   dclk (391 MHz): seg_to_unseg 1024-bit AXIS, tuser = error, no backpressure
//     -> rx_fcs_filter: store-and-forward, drops frames with tuser on the
//        last beat (bad FCS) and frames that do not fit (overflow)
//     -> asynchronous FIFO
//   aclk (node clock, 230 MHz): 1024-bit AXIS to the network stack
//
// The drop counters are reported in aclk as the network stack expects them:
// rx_drop_count[11:0] bad FCS, [15:12] overflow (both wrap).
module dcmac_rx_path #(
    parameter integer FILTER_ADDR_BITS = 9      // 512 beats of 128 bytes
) (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 dclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axis, ASSOCIATED_RESET dresetn" *)
    input  wire            dclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 dresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire            dresetn,

    input  wire [1023:0]   s_axis_tdata,
    input  wire [127:0]    s_axis_tkeep,
    input  wire            s_axis_tlast,
    input  wire            s_axis_tuser,
    input  wire            s_axis_tvalid,
    output wire            s_axis_tready,

    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF m_axis, ASSOCIATED_RESET aresetn" *)
    input  wire            aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire            aresetn,

    output wire [1023:0]   m_axis_tdata,
    output wire [127:0]    m_axis_tkeep,
    output wire            m_axis_tlast,
    output wire            m_axis_tvalid,
    input  wire            m_axis_tready,

    output wire [15:0]     rx_drop_count
);
    // the converter drops what we do not take; the filter takes every beat
    assign s_axis_tready = 1'b1;

    wire [1023:0] f_tdata;
    wire [127:0]  f_tkeep;
    wire          f_tlast, f_tvalid, f_tready;
    wire [31:0]   bad_fcs_frames, overflow_frames;

    rx_fcs_filter #(
        .DATA_BITS(1024),
        .ADDR_BITS(FILTER_ADDR_BITS)
    ) inst_filter (
        .aclk(dclk),
        .aresetn(dresetn),
        .s_tvalid(s_axis_tvalid),
        .s_tdata(s_axis_tdata),
        .s_tkeep(s_axis_tkeep),
        .s_tlast(s_axis_tlast),
        .s_tuser(s_axis_tuser),
        .m_tvalid(f_tvalid),
        .m_tready(f_tready),
        .m_tdata(f_tdata),
        .m_tkeep(f_tkeep),
        .m_tlast(f_tlast),
        .bad_fcs_frames(bad_fcs_frames),
        .overflow_frames(overflow_frames)
    );

    xpm_fifo_axis #(
        .CLOCKING_MODE("independent_clock"),
        .FIFO_DEPTH(32),
        .FIFO_MEMORY_TYPE("auto"),
        .PACKET_FIFO("false"),
        .TDATA_WIDTH(1024),
        .TDEST_WIDTH(1),
        .TID_WIDTH(1),
        .TUSER_WIDTH(1),
        .CDC_SYNC_STAGES(3),
        .ECC_MODE("no_ecc"),
        .RELATED_CLOCKS(0),
        .USE_ADV_FEATURES("0000")
    ) inst_cdc (
        .s_aclk(dclk),
        .s_aresetn(dresetn),
        .s_axis_tvalid(f_tvalid),
        .s_axis_tready(f_tready),
        .s_axis_tdata(f_tdata),
        .s_axis_tkeep(f_tkeep),
        .s_axis_tstrb(f_tkeep),
        .s_axis_tlast(f_tlast),
        .s_axis_tdest(1'b0),
        .s_axis_tid(1'b0),
        .s_axis_tuser(1'b0),
        .m_aclk(aclk),
        .m_axis_tvalid(m_axis_tvalid),
        .m_axis_tready(m_axis_tready),
        .m_axis_tdata(m_axis_tdata),
        .m_axis_tkeep(m_axis_tkeep),
        .m_axis_tstrb(),
        .m_axis_tlast(m_axis_tlast),
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

    // each count moves by at most one per dclk cycle
    xpm_cdc_gray #(
        .DEST_SYNC_FF(3),
        .WIDTH(12)
    ) inst_cdc_bad_fcs (
        .src_clk(dclk),
        .src_in_bin(bad_fcs_frames[11:0]),
        .dest_clk(aclk),
        .dest_out_bin(rx_drop_count[11:0])
    );

    xpm_cdc_gray #(
        .DEST_SYNC_FF(3),
        .WIDTH(4)
    ) inst_cdc_overflow (
        .src_clk(dclk),
        .src_in_bin(overflow_frames[3:0]),
        .dest_clk(aclk),
        .dest_out_bin(rx_drop_count[15:12])
    );
endmodule

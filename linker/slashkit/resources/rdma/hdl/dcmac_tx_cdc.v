`timescale 1ns / 1ps

// Clock crossing of the DCMAC TX path: 1024-bit AXIS beats written in aclk
// (node clock, 230 MHz) leave as 512-bit halves in dclk (391 MHz), low half
// first; a last beat whose upper 64 bytes are empty leaves as its low half only.
//
// A 1281-bit FIFO with one read pointer (xpm_fifo_axis) does not close at
// 391 MHz: its read address and output enable fan out to every bit. Here each
// entry is kept as two halves in LUT RAM, sliced into SLICES pieces per half,
// and every slice has its own copy of the read and write address. The output
// registers load every cycle without an enable: a half becomes valid one
// cycle after its read address settles. The two halves have separate read
// pointers, so consecutive halves leave back to back; only a frame that ends
// in a low half costs one idle cycle. Pointers cross through xpm_cdc_gray, the
// read side is reset from aresetn (xpm_cdc_async_rst); dclk has no reset of
// its own here, so the two sides cannot lose step.
//
// The LUT RAM is written in aclk and read into dclk registers: those paths are
// constrained in the implementation hook (roce_opt_post.tcl.in); data is only
// used once its write pointer has crossed (3 dclk cycles later).
module dcmac_tx_cdc #(
    parameter integer ADDR_BITS = 5,            // 32 entries
    parameter integer SLICES = 8                // per half
) (
    input  wire            aclk,
    input  wire            aresetn,
    input  wire [1023:0]   s_tdata,
    input  wire [127:0]    s_tkeep,
    input  wire            s_tlast,
    input  wire            s_tvalid,
    output wire            s_tready,

    input  wire            dclk,
    output wire [511:0]    m_tdata,
    output wire [63:0]     m_tkeep,
    output wire            m_tlast,
    output wire            m_tvalid,
    input  wire            m_tready
);
    localparam integer HALF = 512 + 64 + 1;     // data, keep, last
    localparam integer SW = (HALF + SLICES - 1) / SLICES;
    localparam integer HW = SW * SLICES;        // padded half
    localparam integer DEPTH = 1 << ADDR_BITS;
    localparam integer A = ADDR_BITS;

    // ---- aclk: write ----
    wire lo_last_in = s_tlast && (s_tkeep[127:64] == 64'd0);
    wire [HW-1:0] in_lo = {{(HW-HALF){1'b0}}, lo_last_in, s_tkeep[63:0], s_tdata[511:0]};
    wire [HW-1:0] in_hi = {{(HW-HALF){1'b0}}, s_tlast, s_tkeep[127:64], s_tdata[1023:512]};

    reg  [A:0]   wbin;
    wire [A:0]   rbin_a;                        // read pointer (hi half) in aclk
    wire         full = (wbin[A] != rbin_a[A]) && (wbin[A-1:0] == rbin_a[A-1:0]);
    reg          wr_ok;                         // out of reset
    assign s_tready = wr_ok && !full;
    wire         we = s_tvalid && s_tready;

    always @(posedge aclk) begin
        if (!aresetn) begin
            wbin <= {(A+1){1'b0}};
            wr_ok <= 1'b0;
        end else begin
            wr_ok <= 1'b1;
            if (we)
                wbin <= wbin + 1'b1;
        end
    end

    // ---- dclk: read ----
    wire         rd_rstn;
    wire [A:0]   wbin_d;                        // write pointer in dclk
    reg  [A:0]   rlo, rhi;                      // next low / high half to leave
    reg          vlo, vhi;                      // dlo / dhi hold the half at rlo / rhi
    reg          hi;                            // the high half leaves next
    wire [HW-1:0] dlo, dhi;

    wire lo_last = dlo[HALF-1];
    wire adv_lo = !hi && vlo && m_tready;
    wire adv_hi = (hi && vhi && m_tready) || (adv_lo && lo_last);

    always @(posedge dclk) begin
        if (!rd_rstn) begin
            rlo <= {(A+1){1'b0}};
            rhi <= {(A+1){1'b0}};
            vlo <= 1'b0;
            vhi <= 1'b0;
            hi <= 1'b0;
        end else begin
            if (adv_lo) rlo <= rlo + 1'b1;
            if (adv_hi) rhi <= rhi + 1'b1;
            vlo <= (rlo != wbin_d) && !adv_lo;
            vhi <= (rhi != wbin_d) && !adv_hi;
            if (hi)
                hi <= !adv_hi;
            else
                hi <= adv_lo && !lo_last;
        end
    end

    // ---- memory slices, each with its own address copies ----
    genvar k;
    generate
        for (k = 0; k < 2 * SLICES; k = k + 1) begin : g_slice
            localparam integer H = k / SLICES;  // 0: low half, 1: high half
            localparam integer B = (k % SLICES) * SW;
            (* ram_style = "distributed" *) reg [SW-1:0] mem [0:DEPTH-1];
            (* dont_touch = "true" *) reg [A-1:0] wa;
            (* dont_touch = "true" *) reg [A-1:0] ra;
            (* dont_touch = "true" *) reg         sel;   // copy of hi for this slice's mux
            reg [SW-1:0] q;

            always @(posedge aclk)
                if (we)
                    mem[wa] <= H ? in_hi[B +: SW] : in_lo[B +: SW];
            always @(posedge aclk) begin
                if (!aresetn)
                    wa <= {A{1'b0}};
                else if (we)
                    wa <= wa + 1'b1;
            end
            always @(posedge dclk) begin
                if (!rd_rstn) begin
                    ra <= {A{1'b0}};
                    sel <= 1'b0;
                end else begin
                    if (H ? adv_hi : adv_lo) ra <= ra + 1'b1;
                    sel <= hi ? !adv_hi : (adv_lo && !lo_last);
                end
                q <= mem[ra];
            end
            if (H) begin : g_hi
                assign dhi[B +: SW] = q;
            end else begin : g_lo
                assign dlo[B +: SW] = q;
            end
        end
    endgenerate

    // output mux, each slice selected by its own copy of hi
    wire [HW-1:0] out;
    generate
        for (k = 0; k < SLICES; k = k + 1) begin : g_out
            assign out[k*SW +: SW] = g_slice[SLICES + k].sel ? dhi[k*SW +: SW] : dlo[k*SW +: SW];
        end
    endgenerate
    assign m_tdata = out[511:0];
    assign m_tkeep = out[575:512];
    assign m_tlast = out[576];
    assign m_tvalid = hi ? vhi : vlo;

    // ---- pointer and reset crossings ----
    xpm_cdc_gray #(
        .DEST_SYNC_FF(3),
        .INIT_SYNC_FF(0),
        .REG_OUTPUT(1),
        .SIM_ASSERT_CHK(0),
        .SIM_LOSSLESS_GRAY_CHK(0),
        .WIDTH(A + 1)
    ) inst_wptr (
        .src_clk(aclk),
        .src_in_bin(wbin),
        .dest_clk(dclk),
        .dest_out_bin(wbin_d)
    );

    xpm_cdc_gray #(
        .DEST_SYNC_FF(3),
        .INIT_SYNC_FF(0),
        .REG_OUTPUT(1),
        .SIM_ASSERT_CHK(0),
        .SIM_LOSSLESS_GRAY_CHK(0),
        .WIDTH(A + 1)
    ) inst_rptr (
        .src_clk(dclk),
        .src_in_bin(rhi),
        .dest_clk(aclk),
        .dest_out_bin(rbin_a)
    );

    xpm_cdc_async_rst #(
        .DEST_SYNC_FF(4),
        .INIT_SYNC_FF(0),
        .RST_ACTIVE_HIGH(0)
    ) inst_rst (
        .src_arst(aresetn),
        .dest_clk(dclk),
        .dest_arst(rd_rstn)
    );
endmodule

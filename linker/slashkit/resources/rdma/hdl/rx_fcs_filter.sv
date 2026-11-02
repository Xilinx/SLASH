`timescale 1ns / 1ps

// Store-and-forward filter behind the CMAC RX interface. The CMAC passes every
// frame on and only flags a bad FCS with tuser on the last beat, and the RoCE
// stack checks no ICRC (DISABLE_CRC_CHECK), so without this filter a bit error
// on the fibre reaches the stack as a corrupted frame. Each frame is written
// into a buffer and released only after its last beat: a frame with a bad FCS
// is rolled back and never seen downstream.
//
// The CMAC RX interface has no tready. A frame that does not fit into the
// buffer because the stack backpressures is dropped whole as well, instead of
// losing beats in the middle of it.
module rx_fcs_filter #(
    parameter integer DATA_BITS = 512,
    parameter integer ADDR_BITS = 9            // 512 beats of 64 bytes
) (
    input  wire                     aclk,
    input  wire                     aresetn,

    // from the CMAC, no backpressure
    input  wire                     s_tvalid,
    input  wire [DATA_BITS-1:0]     s_tdata,
    input  wire [DATA_BITS/8-1:0]   s_tkeep,
    input  wire                     s_tlast,
    input  wire                     s_tuser,   // bad FCS, valid with tlast

    output wire                     m_tvalid,
    input  wire                     m_tready,
    output wire [DATA_BITS-1:0]     m_tdata,
    output wire [DATA_BITS/8-1:0]   m_tkeep,
    output wire                     m_tlast,

    output reg  [31:0]              bad_fcs_frames,
    output reg  [31:0]              overflow_frames
);
    localparam integer W = DATA_BITS + DATA_BITS / 8 + 1;
    localparam integer DEPTH = 1 << ADDR_BITS;
    localparam integer SKID = 4;

    (* ram_style = "block" *) reg [W-1:0] mem [0:DEPTH-1];

    // write side: wr_ptr runs ahead of commit_ptr while a frame comes in
    reg [ADDR_BITS:0] wr_ptr, commit_ptr, rd_ptr;
    reg frame_bad;                              // a beat of this frame did not fit

    wire [ADDR_BITS:0] used = wr_ptr - rd_ptr;
    wire fits = used != DEPTH[ADDR_BITS:0];
    wire write = s_tvalid && fits && !frame_bad;
    wire lost = s_tvalid && (!fits || frame_bad);

    always @(posedge aclk) begin
        if (write)
            mem[wr_ptr[ADDR_BITS-1:0]] <= {s_tlast, s_tkeep, s_tdata};
    end

    always @(posedge aclk) begin
        if (!aresetn) begin
            wr_ptr <= 0;
            commit_ptr <= 0;
            frame_bad <= 1'b0;
            bad_fcs_frames <= 0;
            overflow_frames <= 0;
        end else if (s_tvalid) begin
            if (s_tlast) begin
                frame_bad <= 1'b0;
                if (lost) begin
                    wr_ptr <= commit_ptr;
                    overflow_frames <= overflow_frames + 1;
                end else if (s_tuser) begin
                    wr_ptr <= commit_ptr;
                    bad_fcs_frames <= bad_fcs_frames + 1;
                end else begin
                    wr_ptr <= wr_ptr + 1;
                    commit_ptr <= wr_ptr + 1;
                end
            end else begin
                if (write)
                    wr_ptr <= wr_ptr + 1;
                if (lost)
                    frame_bad <= 1'b1;
            end
        end
    end

    // read side: two-cycle block RAM read into a small output queue; a read
    // is issued only when the queue has room for everything in flight
    reg [1:0] rd_pipe;                          // reads in flight, per stage
    reg [W-1:0] rdata_1, rdata_2;
    reg [W-1:0] skid [0:SKID-1];
    reg [2:0] skid_head, skid_tail;             // one extra bit: full vs empty
    wire [2:0] skid_count = skid_tail - skid_head;
    wire [2:0] in_flight = skid_count + rd_pipe[0] + rd_pipe[1];
    wire issue = (rd_ptr != commit_ptr) && (in_flight < SKID);
    wire pop = m_tvalid && m_tready;

    always @(posedge aclk) begin
        rdata_1 <= mem[rd_ptr[ADDR_BITS-1:0]];
        rdata_2 <= rdata_1;
        if (rd_pipe[1])
            skid[skid_tail[1:0]] <= rdata_2;
    end

    always @(posedge aclk) begin
        if (!aresetn) begin
            rd_ptr <= 0;
            rd_pipe <= 2'b00;
            skid_head <= 0;
            skid_tail <= 0;
        end else begin
            if (issue)
                rd_ptr <= rd_ptr + 1;
            rd_pipe <= {rd_pipe[0], issue};
            if (rd_pipe[1])
                skid_tail <= skid_tail + 1;
            if (pop)
                skid_head <= skid_head + 1;
        end
    end

    wire [W-1:0] head = skid[skid_head[1:0]];
    assign m_tvalid = skid_count != 0;
    assign m_tdata = head[DATA_BITS-1:0];
    assign m_tkeep = head[DATA_BITS +: DATA_BITS / 8];
    assign m_tlast = head[W-1];
endmodule

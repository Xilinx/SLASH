// SPDX-License-Identifier: MIT
`timescale 1ns / 1ps

// Functional test of rdma_service's payload paths and registers. The stack
// is replaced by a test model of rdma_200_ip whose payload request ports the
// test drives; the AXI DataMover by a behavioural model over a byte memory;
// the NoC lanes by a source and a sink. Every byte is checked.
//
//   +N=<requests per direction> (default 400); seed: SEED=<n> run_tb_rdma_service.sh
//
// PASS / FAIL on the last line.

// ---------------------------------------------------------------- stack model
module rdma_200_ip (
    input  wire aclk, input wire aresetn,
    input  wire [11:0] s_axil_awaddr, input wire [2:0] s_axil_awprot, input wire s_axil_awvalid, output wire s_axil_awready,
    input  wire [31:0] s_axil_wdata, input wire [3:0] s_axil_wstrb, input wire s_axil_wvalid, output wire s_axil_wready,
    output wire [1:0] s_axil_bresp, output wire s_axil_bvalid, input wire s_axil_bready,
    input  wire [11:0] s_axil_araddr, input wire [2:0] s_axil_arprot, input wire s_axil_arvalid, output wire s_axil_arready,
    output wire [31:0] s_axil_rdata, output wire [1:0] s_axil_rresp, output wire s_axil_rvalid, input wire s_axil_rready,
    input  wire s_axis_rx_tvalid, output wire s_axis_rx_tready, input wire [1023:0] s_axis_rx_tdata,
    input  wire [127:0] s_axis_rx_tkeep, input wire s_axis_rx_tlast, input wire s_axis_rx_tuser,
    output wire m_axis_tx_tvalid, input wire m_axis_tx_tready, output wire [1023:0] m_axis_tx_tdata,
    output wire [127:0] m_axis_tx_tkeep, output wire m_axis_tx_tlast,
    input  wire s_axis_sq_tvalid, output wire s_axis_sq_tready, input wire [191:0] s_axis_sq_tdata,
    output wire m_axis_cq_tvalid, output wire [31:0] m_axis_cq_tdata,
    output logic m_axis_rd_req_tvalid, input wire m_axis_rd_req_tready, output logic [127:0] m_axis_rd_req_tdata,
    input  wire s_axis_rd_tvalid, output logic s_axis_rd_tready, input wire [1023:0] s_axis_rd_tdata,
    input  wire [127:0] s_axis_rd_tkeep, input wire s_axis_rd_tlast,
    output logic m_axis_wr_req_tvalid, input wire m_axis_wr_req_tready, output logic [127:0] m_axis_wr_req_tdata,
    output logic m_axis_wr_tvalid, input wire m_axis_wr_tready, output logic [1023:0] m_axis_wr_tdata,
    output logic [127:0] m_axis_wr_tkeep, output logic m_axis_wr_tlast
);
    // registers: ID at 0x000, everything else reads 0
    logic aw_ok, r_ok;
    assign s_axil_awready = aw_ok;
    assign s_axil_wready = aw_ok;
    assign s_axil_bresp = 2'b00;
    assign s_axil_arready = r_ok;
    assign s_axil_rresp = 2'b00;
    logic bv, rv;
    logic [31:0] rd;
    assign s_axil_bvalid = bv;
    assign s_axil_rvalid = rv;
    assign s_axil_rdata = rd;
    always @(posedge aclk) begin
        aw_ok <= 0; r_ok <= 0;
        if (!aresetn) begin bv <= 0; rv <= 0; end
        else begin
            if (s_axil_awvalid && s_axil_wvalid && !bv && !aw_ok) begin aw_ok <= 1; bv <= 1; end
            if (bv && s_axil_bready) bv <= 0;
            if (s_axil_arvalid && !rv && !r_ok) begin
                r_ok <= 1; rv <= 1; rd <= (s_axil_araddr == 12'h000) ? 32'h52444D41 : 32'd0;
            end
            if (rv && s_axil_rready) rv <= 0;
        end
    end
    assign s_axis_rx_tready = 1'b1;
    assign m_axis_tx_tvalid = 1'b0;
    assign m_axis_tx_tdata = '0;
    assign m_axis_tx_tkeep = '0;
    assign m_axis_tx_tlast = 1'b0;
    assign s_axis_sq_tready = 1'b0;
    assign m_axis_cq_tvalid = 1'b0;
    assign m_axis_cq_tdata = '0;

    initial begin
        m_axis_rd_req_tvalid = 0;
        m_axis_wr_req_tvalid = 0;
        m_axis_wr_tvalid = 0;
        s_axis_rd_tready = 0;
    end

    task automatic rd_req(input [47:0] addr, input [31:0] len);
        @(negedge aclk);
        m_axis_rd_req_tdata = {32'd0, len, 16'd0, addr};
        m_axis_rd_req_tvalid = 1;
        do @(posedge aclk); while (!m_axis_rd_req_tready);
        #0.1 m_axis_rd_req_tvalid = 0;
    endtask

    task automatic wr_req(input [47:0] addr, input [31:0] len, input bit last);
        @(negedge aclk);
        m_axis_wr_req_tdata = {15'd0, last, 16'd0, len, 16'd0, addr};
        m_axis_wr_req_tvalid = 1;
        do @(posedge aclk); while (!m_axis_wr_req_tready);
        #0.1 m_axis_wr_req_tvalid = 0;
    endtask

    // one packet of len bytes on m_axis_wr, random gaps
    task automatic wr_packet(input byte data[], input int gap_pct);
        int n = data.size();
        int beats = (n + 127) / 128;
        @(negedge aclk);
        for (int b = 0; b < beats; b++) begin
            while ($urandom_range(99) < gap_pct) begin @(posedge aclk); #0.1; end
            for (int i = 0; i < 128; i++) begin
                m_axis_wr_tdata[8*i +: 8] = (b*128 + i < n) ? data[b*128 + i] : 8'hxx;
                m_axis_wr_tkeep[i] = b*128 + i < n;
            end
            m_axis_wr_tlast = b == beats - 1;
            m_axis_wr_tvalid = 1;
            do @(posedge aclk); while (!m_axis_wr_tready);
            #0.1 m_axis_wr_tvalid = 0;
        end
    endtask

    // one packet from s_axis_rd: returns its bytes (up to tlast), random stalls
    task automatic rd_packet(output byte data[$], output int keep_errors, input int stall_pct);
        bit last = 0;
        data = {};
        keep_errors = 0;
        while (!last) begin
            s_axis_rd_tready = $urandom_range(99) >= stall_pct;
            @(posedge aclk);
            if (s_axis_rd_tvalid && s_axis_rd_tready) begin
                bit seen_gap = 0;
                for (int i = 0; i < 128; i++) begin
                    if (s_axis_rd_tkeep[i]) begin
                        if (seen_gap) keep_errors++;
                        data.push_back(s_axis_rd_tdata[8*i +: 8]);
                    end else seen_gap = 1;
                end
                last = s_axis_rd_tlast;
            end
            #0.1;
        end
        s_axis_rd_tready = 0;
    endtask
endmodule

// ---------------------------------------------------------------- test
module tb_rdma_service;
    localparam [47:0] MEM_BASE = 48'h0600_0000_0000;

    logic aclk = 0, sclk = 0, aresetn = 0, sresetn = 0;
    always #2.174 aclk = !aclk;             // 230 MHz
    always #1.667 sclk = !sclk;             // 300 MHz

    int N = 400;
    int errors = 0;

    // registers
    logic [23:0] awaddr, araddr;
    logic awvalid = 0, wvalid = 0, bready = 1, arvalid = 0, rready = 1;
    logic [31:0] wdata;
    wire awready, wready, bvalid, arready, rvalid;
    wire [1:0] bresp, rresp;
    wire [31:0] rdata;

    // DataMovers 0 and 1
    wire [1:0] mm2s_cmd_tvalid, s2mm_cmd_tvalid, s2mm_tvalid, s2mm_tlast, mm2s_tready;
    logic [1:0] mm2s_cmd_tready = 0, s2mm_cmd_tready = 0, s2mm_tready = 0;
    wire [103:0] mm2s_cmd_tdata [2];
    wire [103:0] s2mm_cmd_tdata [2];
    logic [1:0] mm2s_tvalid = 0, mm2s_tlast = 0;
    logic [511:0] mm2s_tdata [2];
    logic [63:0] mm2s_tkeep [2];
    wire [511:0] s2mm_tdata [2];
    wire [63:0] s2mm_tkeep [2];
    logic [1:0] mm2s_sts_tvalid = 0, s2mm_sts_tvalid = 0;
    logic [7:0] mm2s_sts_tdata [2];
    logic [7:0] s2mm_sts_tdata [2];
    int dm_cmds [2][2] = '{'{0, 0}, '{0, 0}};   // [MM2S / S2MM][DataMover]
    logic [15:0] rx_drop_count = 16'h0005;

    // lanes
    logic [1:0] tx_tvalid = 0;
    wire [1:0] tx_tready;
    logic [511:0] tx_tdata [2];
    wire [1:0] rx_tvalid, rx_tlast;
    logic [1:0] rx_tready = 0;
    wire [511:0] rx_tdata [2];
    wire [63:0] rx_tkeep [2];

    rdma_service #(.BUILD(7), .ETH_INDEX(2)) dut (
        .aclk(aclk), .aresetn(aresetn), .sclk(sclk), .sresetn(sresetn),
        .s_axil_awaddr(awaddr), .s_axil_awvalid(awvalid), .s_axil_awready(awready),
        .s_axil_wdata(wdata), .s_axil_wstrb(4'hF), .s_axil_wvalid(wvalid), .s_axil_wready(wready),
        .s_axil_bresp(bresp), .s_axil_bvalid(bvalid), .s_axil_bready(bready),
        .s_axil_araddr(araddr), .s_axil_arvalid(arvalid), .s_axil_arready(arready),
        .s_axil_rdata(rdata), .s_axil_rresp(rresp), .s_axil_rvalid(rvalid), .s_axil_rready(rready),
        .s_axis_rx_tdata('0), .s_axis_rx_tkeep('0), .s_axis_rx_tlast(1'b0), .s_axis_rx_tvalid(1'b0),
        .s_axis_rx_tready(),
        .m_axis_tx_tdata(), .m_axis_tx_tkeep(), .m_axis_tx_tlast(), .m_axis_tx_tvalid(),
        .m_axis_tx_tready(1'b1),
        .rx_drop_count(rx_drop_count), .phy_ctrl(), .phy_status(16'h0007),
        .m_axis_mm2s0_cmd_tvalid(mm2s_cmd_tvalid[0]), .m_axis_mm2s0_cmd_tready(mm2s_cmd_tready[0]),
        .m_axis_mm2s0_cmd_tdata(mm2s_cmd_tdata[0]),
        .s_axis_mm2s0_tvalid(mm2s_tvalid[0]), .s_axis_mm2s0_tready(mm2s_tready[0]),
        .s_axis_mm2s0_tdata(mm2s_tdata[0]), .s_axis_mm2s0_tkeep(mm2s_tkeep[0]),
        .s_axis_mm2s0_tlast(mm2s_tlast[0]),
        .s_axis_mm2s0_sts_tvalid(mm2s_sts_tvalid[0]), .s_axis_mm2s0_sts_tready(),
        .s_axis_mm2s0_sts_tdata(mm2s_sts_tdata[0]),
        .m_axis_s2mm0_cmd_tvalid(s2mm_cmd_tvalid[0]), .m_axis_s2mm0_cmd_tready(s2mm_cmd_tready[0]),
        .m_axis_s2mm0_cmd_tdata(s2mm_cmd_tdata[0]),
        .m_axis_s2mm0_tvalid(s2mm_tvalid[0]), .m_axis_s2mm0_tready(s2mm_tready[0]),
        .m_axis_s2mm0_tdata(s2mm_tdata[0]), .m_axis_s2mm0_tkeep(s2mm_tkeep[0]),
        .m_axis_s2mm0_tlast(s2mm_tlast[0]),
        .s_axis_s2mm0_sts_tvalid(s2mm_sts_tvalid[0]), .s_axis_s2mm0_sts_tready(),
        .s_axis_s2mm0_sts_tdata(s2mm_sts_tdata[0]),
        .m_axis_mm2s1_cmd_tvalid(mm2s_cmd_tvalid[1]), .m_axis_mm2s1_cmd_tready(mm2s_cmd_tready[1]),
        .m_axis_mm2s1_cmd_tdata(mm2s_cmd_tdata[1]),
        .s_axis_mm2s1_tvalid(mm2s_tvalid[1]), .s_axis_mm2s1_tready(mm2s_tready[1]),
        .s_axis_mm2s1_tdata(mm2s_tdata[1]), .s_axis_mm2s1_tkeep(mm2s_tkeep[1]),
        .s_axis_mm2s1_tlast(mm2s_tlast[1]),
        .s_axis_mm2s1_sts_tvalid(mm2s_sts_tvalid[1]), .s_axis_mm2s1_sts_tready(),
        .s_axis_mm2s1_sts_tdata(mm2s_sts_tdata[1]),
        .m_axis_s2mm1_cmd_tvalid(s2mm_cmd_tvalid[1]), .m_axis_s2mm1_cmd_tready(s2mm_cmd_tready[1]),
        .m_axis_s2mm1_cmd_tdata(s2mm_cmd_tdata[1]),
        .m_axis_s2mm1_tvalid(s2mm_tvalid[1]), .m_axis_s2mm1_tready(s2mm_tready[1]),
        .m_axis_s2mm1_tdata(s2mm_tdata[1]), .m_axis_s2mm1_tkeep(s2mm_tkeep[1]),
        .m_axis_s2mm1_tlast(s2mm_tlast[1]),
        .s_axis_s2mm1_sts_tvalid(s2mm_sts_tvalid[1]), .s_axis_s2mm1_sts_tready(),
        .s_axis_s2mm1_sts_tdata(s2mm_sts_tdata[1]),
        .mm2s_addr_posted(2'b00), .mm2s_rd_done(2'b00), .s2mm_addr_posted(2'b00), .s2mm_wr_done(2'b00),
        .s_axis_tx0_tvalid(tx_tvalid[0]), .s_axis_tx0_tready(tx_tready[0]), .s_axis_tx0_tdata(tx_tdata[0]),
        .s_axis_tx0_tkeep('1), .s_axis_tx0_tlast(1'b0),
        .s_axis_tx1_tvalid(tx_tvalid[1]), .s_axis_tx1_tready(tx_tready[1]), .s_axis_tx1_tdata(tx_tdata[1]),
        .s_axis_tx1_tkeep('1), .s_axis_tx1_tlast(1'b0),
        .m_axis_rx0_tvalid(rx_tvalid[0]), .m_axis_rx0_tready(rx_tready[0]), .m_axis_rx0_tdata(rx_tdata[0]),
        .m_axis_rx0_tkeep(rx_tkeep[0]), .m_axis_rx0_tlast(rx_tlast[0]),
        .m_axis_rx1_tvalid(rx_tvalid[1]), .m_axis_rx1_tready(rx_tready[1]), .m_axis_rx1_tdata(rx_tdata[1]),
        .m_axis_rx1_tkeep(rx_tkeep[1]), .m_axis_rx1_tlast(rx_tlast[1])
    );

    // ---------------------------------------------------------------- memory
    byte mem [longint];
    function automatic byte mem_init(longint a);
        return byte'((a * 32'h9E3779B1) >> 13);
    endfunction
    function automatic byte mem_rd(longint a);
        return mem.exists(a) ? mem[a] : mem_init(a);
    endfunction

    // ---------------------------------------------------------------- DataMover models
    // MM2S: BTT bytes from the address, packed from byte 0, then OKAY. Each
    // DataMover has its own pace, so the two run out of step.
    for (genvar d = 0; d < 2; d++) begin : g_dm
        initial begin
            forever begin
                longint a; int btt, beats;
                mm2s_cmd_tready[d] = 1;
                do @(posedge sclk); while (!mm2s_cmd_tvalid[d]);
                btt = mm2s_cmd_tdata[d][22:0];
                a = mm2s_cmd_tdata[d][95:32];
                dm_cmds[0][d]++;
                if (!mm2s_cmd_tdata[d][23] || !mm2s_cmd_tdata[d][30]) begin
                    $display("ERROR: MM2S%0d command not INCR+EOF: %h", d, mm2s_cmd_tdata[d]); errors++;
                end
                #0.1 mm2s_cmd_tready[d] = 0;
                beats = (btt + 63) / 64;
                for (int b = 0; b < beats; b++) begin
                    while ($urandom_range(99) < (d ? 40 : 10)) begin @(posedge sclk); #0.1; end
                    for (int i = 0; i < 64; i++) begin
                        mm2s_tdata[d][8*i +: 8] = (b*64 + i < btt) ? mem_rd(a + b*64 + i) : 8'h00;
                        mm2s_tkeep[d][i] = b*64 + i < btt;
                    end
                    mm2s_tlast[d] = b == beats - 1;
                    mm2s_tvalid[d] = 1;
                    do @(posedge sclk); while (!mm2s_tready[d]);
                    #0.1 mm2s_tvalid[d] = 0;
                end
                mm2s_sts_tdata[d] = 8'h80;
                mm2s_sts_tvalid[d] = 1;
                @(posedge sclk);
                #0.1 mm2s_sts_tvalid[d] = 0;
            end
        end
        // S2MM: BTT bytes to the address; tlast must end exactly there
        initial begin
            forever begin
                longint a; int btt, got;
                bit last;
                s2mm_cmd_tready[d] = 1;
                do @(posedge sclk); while (!s2mm_cmd_tvalid[d]);
                btt = s2mm_cmd_tdata[d][22:0];
                a = s2mm_cmd_tdata[d][95:32];
                dm_cmds[1][d]++;
                #0.1 s2mm_cmd_tready[d] = 0;
                got = 0;
                last = 0;
                while (!last) begin
                    s2mm_tready[d] = $urandom_range(99) >= (d ? 10 : 40);
                    @(posedge sclk);
                    if (s2mm_tvalid[d] && s2mm_tready[d]) begin
                        for (int i = 0; i < 64; i++)
                            if (s2mm_tkeep[d][i]) begin
                                mem[a + got] = s2mm_tdata[d][8*i +: 8];
                                got++;
                            end
                        last = s2mm_tlast[d];
                    end
                    #0.1;
                end
                s2mm_tready[d] = 0;
                if (got != btt) begin
                    $display("ERROR: S2MM%0d got %0d bytes for BTT %0d", d, got, btt); errors++;
                end
                s2mm_sts_tdata[d] = 8'h80;
                s2mm_sts_tvalid[d] = 1;
                @(posedge sclk);
                #0.1 s2mm_sts_tvalid[d] = 0;
            end
        end
    end

    // ---------------------------------------------------------------- lanes
    // source: beat k of lane l carries bytes lane_byte(l, 64k + i)
    function automatic byte lane_byte(int l, longint i);
        return byte'(((i + 1) * (l ? 32'h85EBCA6B : 32'hC2B2AE35)) >> 11);
    endfunction
    longint tx_sent [2] = '{0, 0};          // beats
    for (genvar l = 0; l < 2; l++) begin : g_src
        initial begin
            wait (sresetn);
            forever begin
                while ($urandom_range(99) < 30) begin @(posedge sclk); #0.1; end
                for (int i = 0; i < 64; i++)
                    tx_tdata[l][8*i +: 8] = lane_byte(l, tx_sent[l]*64 + i);
                tx_tvalid[l] = 1;
                do @(posedge sclk); while (tx_tready[l] !== 1'b1);
                tx_sent[l]++;
                #0.1 tx_tvalid[l] = 0;
            end
        end
    end
    // sink: collect beats as {data, keep, last}
    typedef struct { logic [511:0] d; logic [63:0] k; logic l; } beat_t;
    beat_t rx_q [2][$];
    for (genvar l = 0; l < 2; l++) begin : g_sink
        initial begin
            forever begin
                rx_tready[l] = $urandom_range(99) >= 30;
                @(posedge sclk);
                if (rx_tvalid[l] && rx_tready[l])
                    rx_q[l].push_back('{rx_tdata[l], rx_tkeep[l], rx_tlast[l]});
                #0.1;
            end
        end
    end

    // ---------------------------------------------------------------- registers
    task automatic reg_rd(input [23:0] a, output [31:0] d);
        @(negedge aclk);
        araddr = a; arvalid = 1;
        do @(posedge aclk); while (!arready);
        #0.1 arvalid = 0;
        while (!rvalid) @(posedge aclk);
        d = rdata;
        @(posedge aclk);
    endtask
    task automatic reg_wr(input [23:0] a, input [31:0] d);
        @(negedge aclk);
        awaddr = a; wdata = d; awvalid = 1; wvalid = 1;
        do @(posedge aclk); while (!awready);
        #0.1 awvalid = 0; wvalid = 0;
        while (!bvalid) @(posedge aclk);
        @(posedge aclk);
    endtask
    task automatic expect_reg(input [23:0] a, input [31:0] want, input string what);
        logic [31:0] d;
        reg_rd(a, d);
        if (d !== want) begin
            $display("ERROR: %s (0x%06h) = 0x%08h, expected 0x%08h", what, a, d, want); errors++;
        end
    endtask

    // ---------------------------------------------------------------- requests
    typedef enum {K_MEM, K_STREAM, K_BAD} kind_t;
    function automatic kind_t pick();
        int r = $urandom_range(99);
        return r < 50 ? K_MEM : r < 90 ? K_STREAM : K_BAD;
    endfunction
    function automatic int pick_len();
        int r = $urandom_range(99);
        if (r < 20) return 4096;
        if (r < 30) return 4 * $urandom_range(1, 16);
        return 4 * $urandom_range(1, 1024);
    endfunction
    function automatic [47:0] mem_addr();
        return MEM_BASE + 48'(4 * $urandom_range(0, 32'h0FFF_FFFF));
    endfunction

    int n_rd [3] = '{0, 0, 0};
    int n_wr [3] = '{0, 0, 0};
    longint rx_taken [2] = '{0, 0};         // lane beats consumed by READ checks

    // read side: issue requests, check each packet against what it must carry
    typedef struct { kind_t k; logic [47:0] a; int len; int lane; longint first_beat; } rq_t;
    rq_t rd_q [$];
    longint tx_used [2] = '{0, 0};          // lane beats already promised to requests

    task automatic do_reads();
        for (int r = 0; r < N; r++) begin
            rq_t q;
            q.k = pick();
            q.len = pick_len();
            q.lane = $urandom_range(1);
            case (q.k)
                K_MEM:    q.a = mem_addr();
                K_STREAM: q.a = {1'b1, q.lane[0], 46'd0};
                default:  q.a = 48'h0000_0001_0000;
            endcase
            if (q.k == K_STREAM) begin
                q.first_beat = tx_used[q.lane];
                tx_used[q.lane] += (q.len + 63) / 64;
            end
            rd_q.push_back(q);
            n_rd[q.k]++;
            dut.inst_stack.rd_req(q.a, q.len);
        end
    endtask

    task automatic check_reads();
        for (int r = 0; r < N; r++) begin
            byte got[$];
            int kerr;
            rq_t q;
            dut.inst_stack.rd_packet(got, kerr, 25);
            wait (rd_q.size() > 0);
            q = rd_q.pop_front();
            if (got.size() != q.len || kerr) begin
                $display("ERROR: read %0d (%s): %0d bytes (want %0d), %0d keep gaps",
                         r, q.k.name(), got.size(), q.len, kerr);
                errors++;
                continue;
            end
            for (int i = 0; i < q.len; i++) begin
                byte want;
                case (q.k)
                    K_MEM:    want = mem_rd(q.a + i);
                    K_STREAM: want = lane_byte(q.lane, q.first_beat*64 + i);
                    default:  want = 0;
                endcase
                if (got[i] !== want) begin
                    $display("ERROR: read %0d (%s, len %0d) byte %0d: %02h, expected %02h",
                             r, q.k.name(), q.len, i, got[i], want);
                    errors++;
                    break;
                end
            end
        end
    endtask

    // write side: mem writes are checked in memory, stream writes on the lanes
    typedef struct { int len; bit last; byte data[]; } wpk_t;
    wpk_t lane_expect [2][$];
    typedef struct { logic [47:0] a; byte data[]; } mwr_t;
    mwr_t mem_expect [$];

    task automatic do_writes();
        for (int r = 0; r < N; r++) begin
            kind_t k = pick();
            int len = pick_len();
            int lane = $urandom_range(1);
            bit last = $urandom_range(3) != 0;
            logic [47:0] a;
            byte data[];
            data = new[len];
            foreach (data[i]) data[i] = byte'($urandom);
            case (k)
                K_MEM:    a = mem_addr();
                K_STREAM: a = {1'b1, lane[0], 46'd0};
                default:  a = 48'h0000_0002_0000;
            endcase
            if (k == K_STREAM) lane_expect[lane].push_back('{len, last, data});
            if (k == K_MEM) mem_expect.push_back('{a, data});
            n_wr[k]++;
            fork
                dut.inst_stack.wr_req(a, len, last);
                dut.inst_stack.wr_packet(data, 20);
            join
        end
    endtask

    task automatic check_lane_writes(int l);
        while (lane_expect[l].size() > 0) begin
            wpk_t p = lane_expect[l].pop_front();
            int beats = (p.len + 63) / 64;
            for (int b = 0; b < beats; b++) begin
                beat_t x;
                int nb = (b == beats - 1) ? p.len - 64*b : 64;
                logic [63:0] k = (nb == 64) ? '1 : ((64'd1 << nb) - 1);
                wait (rx_q[l].size() > 0);
                x = rx_q[l].pop_front();
                if (x.k !== k || x.l !== (p.last && b == beats - 1)) begin
                    $display("ERROR: lane %0d beat %0d of %0d: keep %h last %0d", l, b, beats, x.k, x.l);
                    errors++;
                end
                for (int i = 0; i < nb; i++)
                    if (x.d[8*i +: 8] !== p.data[64*b + i]) begin
                        $display("ERROR: lane %0d packet byte %0d differs", l, 64*b + i);
                        errors++;
                        break;
                    end
            end
        end
    endtask

    // ---------------------------------------------------------------- main
    initial begin

        logic [31:0] d;
        if ($value$plusargs("N=%d", N)) ;
        repeat (20) @(posedge aclk);
        aresetn = 1;
        sresetn = 1;
        repeat (50) @(posedge aclk);

        expect_reg(24'h000000, 32'h52444D53, "ID");
        expect_reg(24'h000008, 32'd7, "BUILD");
        expect_reg(24'h00000C, 32'd2, "ETH");
        expect_reg(24'h000010, 32'h0000_0000, "MEM_BASE lo");
        expect_reg(24'h000014, 32'h0000_0600, "MEM_BASE hi");
        expect_reg(24'h000024, 32'h0000_0007, "PHY_STATUS");
        expect_reg(24'h000028, 32'h0000_0005, "RX_DROPS");
        expect_reg(24'h00001C, 32'd2, "MEM_PORTS");
        // the port's counters wrap: 5000 bad-FCS and 37 no-room drops
        for (int i = 0; i < 5000; i++) begin
            @(negedge aclk);
            rx_drop_count[11:0] = rx_drop_count[11:0] + 1;
            if (i % 3 == 0 && i < 111) rx_drop_count[15:12] = rx_drop_count[15:12] + 1;
        end
        repeat (4) @(posedge aclk);
        expect_reg(24'h00002C, 32'd5000, "RX_FCS_DROPS");
        expect_reg(24'h000030, 32'd37, "RX_ROOM_DROPS");
        reg_wr(24'h000020, 32'h0000_1234);
        expect_reg(24'h000020, 32'h0000_1234, "PHY_CTRL");
        expect_reg(24'h400000, 32'h52444D41, "stack ID through the split");

        fork
            do_reads();
            check_reads();
            do_writes();
        join
        $display("reads: %0d memory, %0d stream, %0d outside; writes: %0d / %0d / %0d",
                 n_rd[K_MEM], n_rd[K_STREAM], n_rd[K_BAD], n_wr[K_MEM], n_wr[K_STREAM], n_wr[K_BAD]);
        fork
            check_lane_writes(0);
            check_lane_writes(1);
        join
        // memory writes: wait for the S2MM model to finish, then compare
        repeat (2000) @(posedge sclk);
        foreach (mem_expect[w]) begin
            // later writes to the same bytes win: check only bytes no later write covers
            for (int i = 0; i < mem_expect[w].data.size(); i++) begin
                longint a = mem_expect[w].a + i;
                bit later = 0;
                for (int v = w + 1; v < mem_expect.size(); v++)
                    if (a >= mem_expect[v].a && a < mem_expect[v].a + mem_expect[v].data.size())
                        later = 1;
                if (!later && mem_rd(a) !== mem_expect[w].data[i]) begin
                    $display("ERROR: memory write %0d byte %0d differs", w, i);
                    errors++;
                    break;
                end
            end
        end
        repeat (100) @(posedge aclk);
        expect_reg(24'h000100, n_rd[K_MEM], "RD_MEM");
        expect_reg(24'h000104, n_rd[K_STREAM], "RD_STREAM");
        expect_reg(24'h000108, n_rd[K_BAD], "RD_BAD");
        expect_reg(24'h00010C, n_wr[K_MEM], "WR_MEM");
        expect_reg(24'h000110, n_wr[K_STREAM], "WR_STREAM");
        expect_reg(24'h000114, n_wr[K_BAD], "WR_BAD");
        expect_reg(24'h000118, 0, "MM2S_ERR");
        expect_reg(24'h00011C, 0, "S2MM_ERR");
        expect_reg(24'h000120, 0, "RD_LEN_ERR");
        expect_reg(24'h000124, 0, "WR_LEN_ERR");
        reg_rd(24'h000130, d);
        if (d != tx_used[0]) begin $display("ERROR: TX0 beats %0d, expected %0d", d, tx_used[0]); errors++; end
        reg_rd(24'h000134, d);
        if (d != tx_used[1]) begin $display("ERROR: TX1 beats %0d, expected %0d", d, tx_used[1]); errors++; end

        $display("DataMover commands: MM2S %0d / %0d, S2MM %0d / %0d",
                 dm_cmds[0][0], dm_cmds[0][1], dm_cmds[1][0], dm_cmds[1][1]);
        for (int d = 0; d < 2; d++)
            if (dm_cmds[0][d] + 1 < n_rd[K_MEM] / 2 || dm_cmds[1][d] + 1 < n_wr[K_MEM] / 2) begin
                $display("ERROR: DataMover %0d is not used for every second packet", d); errors++;
            end
        // diagnosis counters: commands and status per DataMover
        for (int d = 0; d < 2; d++) begin
            expect_reg(24'h000140 + 4 * d, dm_cmds[1][d], $sformatf("S2MM_CMD%0d", d));
            expect_reg(24'h000160 + 4 * d, dm_cmds[1][d], $sformatf("S2MM_STS%0d", d));
            expect_reg(24'h000168 + 4 * d, dm_cmds[0][d], $sformatf("MM2S_CMD%0d", d));
            expect_reg(24'h000188 + 4 * d, dm_cmds[0][d], $sformatf("MM2S_STS%0d", d));
        end
        expect_reg(24'h000034, 32'd3, "MEM_MODE");

        // MEM_MODE 1 / 2: one DataMover takes every memory packet
        for (int m = 1; m <= 2; m++) begin
            automatic int prev [2] = '{dm_cmds[1][0], dm_cmds[1][1]};
            automatic byte data [] = new [4096];
            reg_wr(24'h000034, m);
            repeat (20) @(posedge sclk);
            for (int k = 0; k < 6; k++) begin
                foreach (data[i]) data[i] = $urandom;
                fork
                    dut.inst_stack.wr_req(48'h0600_0010_0000 + k * 4096, 4096, 1);
                    dut.inst_stack.wr_packet(data, 0);
                join
            end
            repeat (2000) @(posedge sclk);
            if (dm_cmds[1][m - 1] != prev[m - 1] + 6 || dm_cmds[1][2 - m] != prev[2 - m]) begin
                $display("ERROR: MEM_MODE %0d: S2MM commands %0d / %0d, before %0d / %0d", m,
                         dm_cmds[1][0], dm_cmds[1][1], prev[0], prev[1]);
                errors++;
            end
        end
        reg_wr(24'h000034, 3);

        if (errors == 0) $display("PASS");
        else $display("FAIL: %0d errors", errors);
        $finish;
    end


    initial begin
        #50ms;
        $display("rd: busy %0d kind %0d src %0d rem %0d have %0d out_room %0d q_empty %0d",
                 dut.inst_rd.busy, dut.inst_rd.kind, dut.inst_rd.src, dut.inst_rd.rem, dut.inst_rd.have,
                 dut.inst_rd.out_room, dut.inst_rd.q_empty);
        $display("wr: busy %0d kind %0d dst %0d rem %0d room %0d in_valid %0d req_valid %0d",
                 dut.inst_wr.busy, dut.inst_wr.kind, dut.inst_wr.dst, dut.inst_wr.rem, dut.inst_wr.room,
                 dut.inst_wr.in_valid, dut.inst_wr.req_valid);
        $display("DataMover commands: MM2S %0d / %0d, S2MM %0d / %0d; reads issued %0d",
                 dm_cmds[0][0], dm_cmds[0][1], dm_cmds[1][0], dm_cmds[1][1], n_rd[0] + n_rd[1] + n_rd[2]);
        $display("FAIL: timeout");
        $finish;
    end
endmodule

`timescale 1ns / 1ps

// Two-entry register slice: registers data, valid and ready, full throughput.
//
// The data registers load on m_ready, which comes straight from the consumer.
// At 577 bits and 391 MHz one enable net cannot reach them all in time, so the
// valid flags are kept in SLICES identical copies (KEEP stops synthesis merging
// them) and each copy only enables its own slice of the data.
module axis_skid #(
    parameter integer DATA_BITS = 64,
    parameter integer SLICES = 1
) (
    input  wire                 clk,
    input  wire                 resetn,

    input  wire                 s_valid,
    output wire                 s_ready,
    input  wire [DATA_BITS-1:0] s_data,

    output wire                 m_valid,
    input  wire                 m_ready,
    output wire [DATA_BITS-1:0] m_data
);
    localparam integer W = (DATA_BITS + SLICES - 1) / SLICES;

    reg [DATA_BITS-1:0] main_data, skid_data;
    (* keep = "true" *) reg [SLICES-1:0] main_valid = {SLICES{1'b0}};
    (* keep = "true" *) reg [SLICES-1:0] skid_valid = {SLICES{1'b0}};

    assign s_ready = !skid_valid[0];
    assign m_valid = main_valid[0];
    assign m_data = main_data;

    genvar k;
    generate
        for (k = 0; k < SLICES; k = k + 1) begin : g_slice
            localparam integer LO = k * W;
            localparam integer HI = (LO + W > DATA_BITS) ? DATA_BITS - 1 : LO + W - 1;

            always @(posedge clk) begin
                if (!resetn) begin
                    main_valid[k] <= 1'b0;
                    skid_valid[k] <= 1'b0;
                end else if (!main_valid[k] || m_ready) begin
                    // the output register is free: fill it, from the skid entry first
                    main_valid[k] <= skid_valid[k] || s_valid;
                    skid_valid[k] <= 1'b0;
                end else if (s_valid && !skid_valid[k]) begin
                    // output stalled: park the incoming beat
                    skid_valid[k] <= 1'b1;
                end
            end

            if (LO <= HI) begin : g_data
                always @(posedge clk) begin
                    if (!main_valid[k] || m_ready)
                        main_data[HI:LO] <= skid_valid[k] ? skid_data[HI:LO] : s_data[HI:LO];
                    else if (s_valid && !skid_valid[k])
                        skid_data[HI:LO] <= s_data[HI:LO];
                end
            end
        end
    endgenerate
endmodule

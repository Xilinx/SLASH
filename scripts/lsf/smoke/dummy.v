// Throwaway design for the LSF plumbing test: big enough that synthesis takes
// a couple of minutes and shows progress, small enough not to hog the queue.
module dummy #(
    parameter integer LANES = 64,
    parameter integer WIDTH = 32
) (
    input  wire                     clk,
    input  wire                     rst,
    input  wire [WIDTH-1:0]         a,
    input  wire [WIDTH-1:0]         b,
    output reg  [2*WIDTH-1:0]       acc
);
    reg [2*WIDTH-1:0] lane [0:LANES-1];
    reg [WIDTH-1:0]   pipe_a [0:LANES-1];
    reg [WIDTH-1:0]   pipe_b [0:LANES-1];

    integer i;
    always @(posedge clk) begin
        if (rst) begin
            for (i = 0; i < LANES; i = i + 1) begin
                lane[i]   <= {(2*WIDTH){1'b0}};
                pipe_a[i] <= {WIDTH{1'b0}};
                pipe_b[i] <= {WIDTH{1'b0}};
            end
            acc <= {(2*WIDTH){1'b0}};
        end else begin
            pipe_a[0] <= a;
            pipe_b[0] <= b;
            for (i = 1; i < LANES; i = i + 1) begin
                pipe_a[i] <= pipe_a[i-1] + i[WIDTH-1:0];
                pipe_b[i] <= pipe_b[i-1] ^ {i[WIDTH-2:0], 1'b1};
            end
            for (i = 0; i < LANES; i = i + 1) begin
                lane[i] <= lane[i] + (pipe_a[i] * pipe_b[i]);
            end
            acc <= lane[LANES-1] + lane[0];
        end
    end
endmodule

`default_nettype none

module cc20_qr (
    input  wire        i_clk,
    input  wire        i_rst_n,

    input  wire [31:0] i_a,
    input  wire [31:0] i_b,
    input  wire [31:0] i_c,
    input  wire [31:0] i_d,

    output reg  [31:0] o_a,
    output reg  [31:0] o_b,
    output reg  [31:0] o_c,
    output reg  [31:0] o_d
);

    reg  [31:0] r_a, r_b, r_c, r_d;

    wire [31:0] w_a1, w_d1_xor, w_d1;
    wire [31:0] w_c2, w_b2_xor, w_b2;
    wire [31:0] w_a3, w_d3_xor, w_d3;
    wire [31:0] w_c4, w_b4_xor, w_b4;

    assign w_a1     = i_a + i_b;
    assign w_d1_xor = i_d ^ w_a1;
    assign w_d1     = {w_d1_xor[15:0], w_d1_xor[31:16]}; // ROTL 16

    assign w_c2     = i_c + w_d1;
    assign w_b2_xor = i_b ^ w_c2;
    assign w_b2     = {w_b2_xor[19:0], w_b2_xor[31:20]}; // ROTL 12

    assign w_a3     = r_a + r_b;
    assign w_d3_xor = r_d ^ w_a3;
    assign w_d3     = {w_d3_xor[23:0], w_d3_xor[31:24]}; // ROTL 8

    assign w_c4     = r_c + w_d3;
    assign w_b4_xor = r_b ^ w_c4;
    assign w_b4     = {w_b4_xor[24:0], w_b4_xor[31:25]}; // ROTL 7

    always @(posedge i_clk) begin
        if (i_rst_n) begin
            // Stage 1
            r_a <= w_a1;
            r_b <= w_b2;
            r_c <= w_c2;
            r_d <= w_d1;
            // Stage 2
            o_a <= w_a3;
            o_b <= w_b4;
            o_c <= w_c4;
            o_d <= w_d3;
        end
    end

endmodule

`default_nettype wire

`default_nettype none

module cc20_round (
    input  wire         i_clk,
    input  wire         i_rst_n,

    input  wire [511:0] i_state,
    input  wire         i_valid,
    input  wire         i_diagonal,

    output wire [511:0] o_state,
    output wire         o_valid
);

    reg  [1:0] r_valid_p;
    reg  [1:0] r_diagonal_p;

    wire [31:0] w_st_in  [0:15];
    reg  [31:0] w_st_out [0:15];
    reg  [31:0] w_qr_in     [0:15];
    wire [31:0] w_qr_out    [0:15];

    genvar i;
    generate
        for (i = 0; i < 16; i = i + 1) begin : g_state_words
            assign w_st_in[i]            = i_state[i * 32 +: 32];
            assign o_state[i * 32 +: 32] = w_st_out[i];
        end
    endgenerate

    // Rows contain {a, b, c, d} for QR 0..3.
    always @(*) begin
        if (!i_diagonal) begin
            {w_qr_in[ 0], w_qr_in[ 1], w_qr_in[ 2], w_qr_in[ 3]} ={w_st_in[ 0], w_st_in[ 4], w_st_in[ 8], w_st_in[12]};
            {w_qr_in[ 4], w_qr_in[ 5], w_qr_in[ 6], w_qr_in[ 7]} ={w_st_in[ 1], w_st_in[ 5], w_st_in[ 9], w_st_in[13]};
            {w_qr_in[ 8], w_qr_in[ 9], w_qr_in[10], w_qr_in[11]} ={w_st_in[ 2], w_st_in[ 6], w_st_in[10], w_st_in[14]};
            {w_qr_in[12], w_qr_in[13], w_qr_in[14], w_qr_in[15]} ={w_st_in[ 3], w_st_in[ 7], w_st_in[11], w_st_in[15]};
        end
        else begin
            {w_qr_in[ 0], w_qr_in[ 1], w_qr_in[ 2], w_qr_in[ 3]} ={w_st_in[ 0], w_st_in[ 5], w_st_in[10], w_st_in[15]};
            {w_qr_in[ 4], w_qr_in[ 5], w_qr_in[ 6], w_qr_in[ 7]} ={w_st_in[ 1], w_st_in[ 6], w_st_in[11], w_st_in[12]};
            {w_qr_in[ 8], w_qr_in[ 9], w_qr_in[10], w_qr_in[11]} ={w_st_in[ 2], w_st_in[ 7], w_st_in[ 8], w_st_in[13]};
            {w_qr_in[12], w_qr_in[13], w_qr_in[14], w_qr_in[15]} ={w_st_in[ 3], w_st_in[ 4], w_st_in[ 9], w_st_in[14]};
        end
    end

    generate
        for (i = 0; i < 4; i = i + 1) begin : g_qr
            cc20_qr u_qr (
                .i_clk   (i_clk),
                .i_rst_n (i_rst_n),

                .i_a     (w_qr_in[i * 4]),
                .i_b     (w_qr_in[i * 4 + 1]),
                .i_c     (w_qr_in[i * 4 + 2]),
                .i_d     (w_qr_in[i * 4 + 3]),

                .o_a     (w_qr_out[i * 4]),
                .o_b     (w_qr_out[i * 4 + 1]),
                .o_c     (w_qr_out[i * 4 + 2]),
                .o_d     (w_qr_out[i * 4 + 3])
            );
        end
    endgenerate

    // Restore the matrix using the pipelined mode.
    always @(*) begin
        if (!r_diagonal_p[1]) begin
            {w_st_out[ 0], w_st_out[ 1], w_st_out[ 2], w_st_out[ 3]} ={w_qr_out[ 0], w_qr_out[ 4], w_qr_out[ 8], w_qr_out[12]};
            {w_st_out[ 4], w_st_out[ 5], w_st_out[ 6], w_st_out[ 7]} ={w_qr_out[ 1], w_qr_out[ 5], w_qr_out[ 9], w_qr_out[13]};
            {w_st_out[ 8], w_st_out[ 9], w_st_out[10], w_st_out[11]} ={w_qr_out[ 2], w_qr_out[ 6], w_qr_out[10], w_qr_out[14]};
            {w_st_out[12], w_st_out[13], w_st_out[14], w_st_out[15]} ={w_qr_out[ 3], w_qr_out[ 7], w_qr_out[11], w_qr_out[15]};
        end
        else begin
            {w_st_out[ 0], w_st_out[ 1], w_st_out[ 2], w_st_out[ 3]} ={w_qr_out[ 0], w_qr_out[ 4], w_qr_out[ 8], w_qr_out[12]};
            {w_st_out[ 4], w_st_out[ 5], w_st_out[ 6], w_st_out[ 7]} ={w_qr_out[13], w_qr_out[ 1], w_qr_out[ 5], w_qr_out[ 9]};
            {w_st_out[ 8], w_st_out[ 9], w_st_out[10], w_st_out[11]} ={w_qr_out[10], w_qr_out[14], w_qr_out[ 2], w_qr_out[ 6]};
            {w_st_out[12], w_st_out[13], w_st_out[14], w_st_out[15]} ={w_qr_out[ 7], w_qr_out[11], w_qr_out[15], w_qr_out[ 3]};
        end
    end

    // Pipeline valid and mode through both QR stages.
    always @(posedge i_clk) begin
        if (!i_rst_n) begin
            r_valid_p    <= 2'd0;
            r_diagonal_p <= 2'd0;
        end
        else begin
            r_valid_p    <= {r_valid_p[0], i_valid};
            r_diagonal_p <= {r_diagonal_p[0], i_diagonal};
        end
    end

    assign o_valid = r_valid_p[1];

endmodule

`default_nettype wire

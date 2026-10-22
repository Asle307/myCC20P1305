`default_nettype none

module cc20_core (
    input  wire         i_clk,
    input  wire         i_rst_n,

    input  wire [255:0] i_key,
    input  wire [ 95:0] i_nonce,
    input  wire [ 31:0] i_counter,
    input  wire         i_request_valid,
    output wire         o_request_ready,

    output wire [511:0] o_keystream,
    output wire         o_result_valid,
    input  wire         i_result_ready
);

    localparam [1:0] ST_IDLE         = 2'd0;
    localparam [1:0] ST_ROUND        = 2'd1;
    localparam [1:0] ST_FEED_FORWARD = 2'd2;
    localparam [1:0] ST_HOLD         = 2'd3;

    localparam [127:0] LP_CONSTANT   = 128'h6b206574_79622d32_3320646e_61707865;
    localparam [  4:0] LP_LAST_ROUND = 5'd19;

    reg  [1:0] r_state;
    reg  [4:0] r_round_index;

    reg  [ 31:0] r_counter;
    reg  [511:0] r_working_state;

    reg          w_round_in_valid;
    reg          w_round_diagonal;
    wire [511:0] w_initial_state;
    wire [511:0] w_original_for_sum;
    reg  [511:0] w_round_input_state;
    wire [511:0] w_round_state;
    wire         w_round_out_valid;
    wire [511:0] w_feed_forward_sum;

    genvar i;

    assign o_request_ready = (r_state == ST_IDLE);
    assign o_result_valid  = (r_state == ST_HOLD);
    assign o_keystream     = r_working_state;

    // Low to high: constants, key, counter, nonce.
    assign w_initial_state = {i_nonce, i_counter, i_key, LP_CONSTANT};

    // Top holds key/nonce; capture the counter per request.
    assign w_original_for_sum = {i_nonce, r_counter, i_key, LP_CONSTANT};

    // Launch round 0 on request; feed later rounds from the QR result.
    // Zero invalid inputs to avoid another wide mux source.
    always @(*) begin
        w_round_input_state = 512'd0;
        w_round_diagonal    = r_round_index[0];
        w_round_in_valid    = 1'b0;

        if ((r_state == ST_IDLE) && i_request_valid && o_request_ready) begin
            w_round_in_valid    = 1'b1;
            w_round_input_state = w_initial_state;
            w_round_diagonal    = 1'b0;
        end
        else if ((r_state == ST_ROUND) && w_round_out_valid) begin
            // Gate valid only; invalid state data is ignored.
            w_round_in_valid    = (r_round_index != LP_LAST_ROUND);
            w_round_input_state = w_round_state;
            w_round_diagonal    = ~r_round_index[0];
        end
    end

    // Feed-forward: independent additions modulo 2^32.
    // The sum is sampled only in ST_FEED_FORWARD.
    generate
        for (i = 0; i < 16; i = i + 1) begin : g_feed_forward
            assign w_feed_forward_sum[i * 32 +: 32] = r_working_state[i * 32 +: 32] + w_original_for_sum[i * 32 +: 32];
        end
    endgenerate

    cc20_round u_cc20_round (
        .i_clk      (i_clk),
        .i_rst_n    (i_rst_n),

        .i_state    (w_round_input_state),
        .i_valid    (w_round_in_valid),
        .i_diagonal (w_round_diagonal),

        .o_state    (w_round_state),
        .o_valid    (w_round_out_valid)
    );

    always @(posedge i_clk) begin
        if (!i_rst_n) begin
            r_state       <= ST_IDLE;
            r_round_index <= 5'd0;
        end
        else begin
            case (r_state)
                ST_IDLE: begin
                    if (i_request_valid && o_request_ready) begin
                        r_counter        <= i_counter;
                        r_round_index    <= 5'd0;
                        r_state          <= ST_ROUND;
                    end
                end

                ST_ROUND: begin
                    if (w_round_out_valid) begin
                        if (r_round_index == LP_LAST_ROUND) begin
                            // Capture the final round for feed-forward.
                            r_working_state <= w_round_state;
                            r_state         <= ST_FEED_FORWARD;
                        end
                        else begin
                            r_round_index <= r_round_index + 1'b1;
                        end
                    end
                end

                ST_FEED_FORWARD: begin
                    // 20 two-cycle rounds + feed-forward = 41 clocks.
                    // Hold the result in the working bank.
                    r_working_state <= w_feed_forward_sum;
                    r_state         <= ST_HOLD;
                end

                ST_HOLD: begin
                    if (i_result_ready) begin
                        r_state <= ST_IDLE;
                    end
                end

                default: begin
                    r_state <= ST_IDLE;
                end
            endcase
        end
    end

endmodule

`default_nettype wire

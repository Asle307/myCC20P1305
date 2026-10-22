`default_nettype none

module p1305_core (
    input  wire         i_clk,
    input  wire         i_rst_n,

    input  wire [255:0] i_otk,
    input  wire         i_start,

    input  wire [127:0] i_block,
    input  wire         i_block_valid,
    output wire         o_block_ready,
    input  wire         i_last,

    output wire [127:0] o_tag,
    output wire         o_tag_valid,
    input  wire         i_tag_ready
);

    localparam [127:0] LP_R_MASK = 128'h0ffffffc_0ffffffc_0ffffffc_0fffffff;

    localparam [3:0] ST_IDLE    = 4'd0;
    localparam [3:0] ST_R2_WAIT = 4'd1;

    localparam [3:0] ST_ACCEPT_FIRST = 4'd2;
    localparam [3:0] ST_WAIT_SECOND  = 4'd3;

    localparam [3:0] ST_SUM          = 4'd4;
    localparam [3:0] ST_MUL_START    = 4'd5;
    localparam [3:0] ST_WAIT_ARITH   = 4'd6;
    localparam [3:0] ST_PAIR_CORRECT = 4'd7;

    localparam [3:0] ST_TAG_MAKE = 4'd8;
    localparam [3:0] ST_TAG_HOLD = 4'd9;

    reg  [3:0] r_state;

    // Key and accumulator.
    reg  [129:0] r_r;
    reg  [127:0] r_s;
    reg  [129:0] r_r2;
    reg  [129:0] r_h;

    // Hold encoded blocks until launch.
    reg  [129:0] r_n0;
    reg  [129:0] r_n1;
    reg          r_pair_mode;
    reg          r_pair_last;

    // Reuse for h+n0 and the pair residue sum.
    reg  [130:0] r_sum;

    wire [129:0] w_r_clamped;
    wire [129:0] w_block_n;
    wire [130:0] w_h_plus_block;
    wire [130:0] w_pair_residue_sum;
    wire [130:0] w_sum_plus_5;
    wire [129:0] w_sum_reduced;
    wire [127:0] w_tag_sum;

    wire w_block_transfer;
    wire w_tag_transfer;
    wire w_arith_result_transfer;

    // Lane 1 is active only for pairs.
    reg  [129:0] w_operand_a [0:1];
    reg  [129:0] w_operand_b [0:1];

    reg          w_arith_start;
    wire         w_arith_result_valid;
    wire         w_arith_result_ready;
    wire [129:0] w_residue [0:1];

    // OTK: low half is clamped r; high half is s.
    assign w_r_clamped = {2'b00, (i_otk[127:0] & LP_R_MASK)};

    // AEAD supplies complete 16-byte blocks, including padding and lengths.
    // Poly1305 appends its mandatory one bit at bit 128: n = block + 2^128.
    assign w_block_n = {2'b01, i_block};

    // Pair: h' = (h+n0)*r^2 + n1*r (mod p). Single: h' = (h+n0)*r (mod p).
    assign w_h_plus_block = {1'b0, r_h} + {1'b0, r_n0};

    assign w_pair_residue_sum = {1'b0, w_residue[0]} + {1'b0, w_residue[1]};

    // sum < 2p: at most one subtraction, p = 2^130-5.
    // sum[130] or carry from low130(sum)+5 selects sum-p.
    assign w_sum_plus_5  = {1'b0, r_sum[129:0]} + 131'd5;
    assign w_sum_reduced = (r_sum[130] || w_sum_plus_5[130]) ?
                          w_sum_plus_5[129:0] : r_sum[129:0];

    // Tag addition is modulo 2^128.
    assign w_tag_sum = r_h[127:0] + r_s;

    // Reuse h to hold the final tag.
    assign o_tag = r_h[127:0];

    assign o_block_ready = (r_state == ST_ACCEPT_FIRST) ||
                           (r_state == ST_WAIT_SECOND);
    assign o_tag_valid = (r_state == ST_TAG_HOLD);

    assign w_arith_result_ready = (r_state == ST_R2_WAIT) ||
                                 (r_state == ST_WAIT_ARITH);

    assign w_block_transfer        = i_block_valid && o_block_ready;
    assign w_tag_transfer          = o_tag_valid && i_tag_ready;
    assign w_arith_result_transfer = w_arith_result_valid && w_arith_result_ready;

    // Operands are sampled on START; key setup uses lane 0 for r*r.
    always @(*) begin
        w_arith_start = 1'b0;

        w_operand_a[0] = w_sum_reduced;
        w_operand_b[0] = r_pair_mode ? r_r2 : r_r;

        w_operand_a[1] = r_n1;
        w_operand_b[1] = r_r;

        case (r_state)
            ST_IDLE: begin
                if (i_start) begin
                    w_arith_start  = 1'b1;
                    w_operand_a[0] = w_r_clamped;
                    w_operand_b[0] = w_r_clamped;
                end
            end

            ST_MUL_START: begin
                w_arith_start = 1'b1;
            end

            default: begin
            end
        endcase
    end

    always @(posedge i_clk) begin
        if (!i_rst_n) begin
            r_state <= ST_IDLE;
        end
        else begin
            case (r_state)
                ST_IDLE: begin
                    if (i_start) begin
                        r_r         <= w_r_clamped;
                        r_s         <= i_otk[255:128];
                        r_h         <= 130'd0;

                        r_pair_mode <= 1'b0;
                        r_state     <= ST_R2_WAIT;
                    end
                end

                ST_R2_WAIT: begin
                    if (w_arith_result_transfer) begin
                        r_r2    <= w_residue[0];
                        r_state <= ST_ACCEPT_FIRST;
                    end
                end

                ST_ACCEPT_FIRST: begin
                    if (w_block_transfer) begin
                        r_n0        <= w_block_n;
                        r_pair_mode <= 1'b0;
                        if (i_last) begin
                            r_state <= ST_SUM;
                        end
                        else begin
                            r_state <= ST_WAIT_SECOND;
                        end
                    end
                end

                ST_WAIT_SECOND: begin
                    if (w_block_transfer) begin
                        r_n1        <= w_block_n;
                        r_pair_mode <= 1'b1;
                        r_pair_last <= i_last;
                        r_state     <= ST_SUM;
                    end
                end

                ST_SUM: begin
                    r_sum   <= w_h_plus_block;
                    r_state <= ST_MUL_START;
                end

                ST_MUL_START: begin
                    r_state <= ST_WAIT_ARITH;
                end

                ST_WAIT_ARITH: begin
                    if (w_arith_result_transfer) begin
                        if (r_pair_mode) begin
                            // Register the pair sum before correction.
                            r_sum   <= w_pair_residue_sum;
                            r_state <= ST_PAIR_CORRECT;
                        end
                        else begin
                            // Singleton ends the message.
                            r_h     <= w_residue[0];
                            r_state <= ST_TAG_MAKE;
                        end
                    end
                end

                ST_PAIR_CORRECT: begin
                    r_h <= w_sum_reduced;
                    if (r_pair_last) begin
                        r_state <= ST_TAG_MAKE;
                    end
                    else begin
                        r_state <= ST_ACCEPT_FIRST;
                    end
                end

                ST_TAG_MAKE: begin
                    r_h     <= {2'b00, w_tag_sum};
                    r_state <= ST_TAG_HOLD;
                end

                ST_TAG_HOLD: begin
                    if (w_tag_transfer) begin
                        r_state <= ST_IDLE;
                    end
                end

                default: begin
                    r_state <= ST_IDLE;
                end
            endcase
        end
    end

    p1305_pair u_arithmetic (
        .i_clk          (i_clk),
        .i_rst_n        (i_rst_n),

        .i_operand_a0   (w_operand_a[0]),
        .i_operand_b0   (w_operand_b[0]),
        .i_operand_a1   (w_operand_a[1]),
        .i_operand_b1   (w_operand_b[1]),
        .i_start        (w_arith_start),
        .i_pair_mode    ((r_state == ST_MUL_START) && r_pair_mode),

        .o_residue_0    (w_residue[0]),
        .o_residue_1    (w_residue[1]),
        .o_result_valid (w_arith_result_valid),
        .i_result_ready (w_arith_result_ready)
    );

endmodule

`default_nettype wire

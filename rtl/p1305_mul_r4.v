`default_nettype none

module p1305_mul_r4 (
    input  wire         i_clk,
    input  wire         i_rst_n,

    input  wire [129:0] i_operand_a,
    input  wire [129:0] i_operand_b,
    input  wire         i_start,

    output wire [259:0] o_product,
    output reg          o_done
);

    // 65 radix-4 digits, indexed 0..64.
    localparam [6:0] LP_LAST_ITERATION = 7'd64;

    localparam [1:0] ST_IDLE      = 2'd0;
    localparam [1:0] ST_SETUP     = 2'd1;
    localparam [1:0] ST_ITER      = 2'd2;
    localparam [1:0] ST_WAIT_ITER = 2'd3;

    reg  [1:0] r_state;
    reg  [6:0] r_iteration;

    // Hold A; consume B two bits per iteration.
    reg  [129:0] r_operand_a;
    reg  [129:0] r_operand_b_shift;

    // Cache 3A in SETUP; 2A is wiring.
    reg  [131:0] r_partial_3a;

    // Clear the product bank on START.
    reg  [129:0] r_product_high;
    reg  [129:0] r_product_low;

    wire [131:0] w_partial_1a;
    wire [131:0] w_partial_2a;
    wire [131:0] w_partial_3a;

    reg  [131:0] w_digit_product;
    wire [131:0] w_product_sum;

    wire         w_iteration_last;

    assign w_partial_1a = {2'b00, r_operand_a};
    assign w_partial_2a = w_partial_1a << 1;
    assign w_partial_3a = w_partial_1a + w_partial_2a;

    // Select 0, A, 2A or 3A.
    always @(*) begin
        w_digit_product = 132'd0;

        case (r_operand_b_shift[1:0])
            2'd1: w_digit_product = w_partial_1a;
            2'd2: w_digit_product = w_partial_2a;
            2'd3: w_digit_product = r_partial_3a;
            default: begin
            end
        endcase
    end

    // high + digit*A < 4*2^130: 132 bits suffice.
    assign w_product_sum    = {2'b00, r_product_high} + w_digit_product;
    assign w_iteration_last = (r_iteration == LP_LAST_ITERATION);

    // Hold the completed product until START.
    assign o_product = {r_product_high, r_product_low};

    // Reset aborts control; START initializes data.
    always @(posedge i_clk) begin
        if (!i_rst_n) begin
            r_state     <= ST_IDLE;
            r_iteration <= 7'd0;
            o_done      <= 1'b0;
        end
        else begin
            o_done <= 1'b0;

            case (r_state)
                ST_IDLE: begin
                    if (i_start) begin
                        r_operand_a       <= i_operand_a;
                        r_operand_b_shift <= i_operand_b;

                        r_product_high    <= 130'd0;
                        r_product_low     <= 130'd0;

                        r_iteration       <= 7'd0;
                        r_state           <= ST_SETUP;
                    end
                end

                ST_SETUP: begin
                    r_partial_3a <= w_partial_3a;
                    r_state      <= ST_WAIT_ITER;
                end

                ST_WAIT_ITER: begin
                    // SETUP + wait + 65 iterations = 67 clocks.
                    r_state <= ST_ITER;
                end

                ST_ITER: begin
                    // Shift {sum, low} right by two; sum[1:0] enters low.
                    r_product_high    <= w_product_sum[131:2];
                    r_product_low     <= {w_product_sum[1:0], r_product_low[129:2]};

                    r_operand_b_shift <= r_operand_b_shift >> 2;

                    if (w_iteration_last) begin
                        o_done  <= 1'b1;
                        r_state <= ST_IDLE;
                    end
                    else begin
                        r_iteration <= r_iteration + 1'b1;
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

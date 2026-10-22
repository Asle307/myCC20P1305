`default_nettype none

module p1305_pair (
    input  wire         i_clk,
    input  wire         i_rst_n,

    input  wire [129:0] i_operand_a0,
    input  wire [129:0] i_operand_b0,
    input  wire [129:0] i_operand_a1,
    input  wire [129:0] i_operand_b1,
    input  wire         i_start,
    input  wire         i_pair_mode,

    output wire [129:0] o_residue_0,
    output wire [129:0] o_residue_1,
    output wire         o_result_valid,
    input  wire         i_result_ready
);

    localparam integer LP_LANES = 2;

    localparam [1:0] ST_IDLE        = 2'd0;
    localparam [1:0] ST_WAIT_MUL    = 2'd1;
    localparam [1:0] ST_WAIT_REDUCE = 2'd2;

    reg  [1:0] r_state;
    reg        r_pair_mode;

    // Each lane owns its operands, product and residue.
    wire [129:0] w_operand_a [0:LP_LANES-1];
    wire [129:0] w_operand_b [0:LP_LANES-1];
    wire [259:0] w_product   [0:LP_LANES-1];
    wire [129:0] w_residue   [0:LP_LANES-1];

    wire [1:0] w_mul_start;
    wire [1:0] w_mul_done;
    wire [1:0] w_reduce_start;
    wire [1:0] w_reduce_valid;
    wire [1:0] w_reduce_ready;

    wire w_request;
    wire w_mul_complete;
    wire w_reduce_request;
    wire w_reduce_complete;
    wire w_result_transfer;

    genvar i;

    assign w_operand_a[0] = i_operand_a0;
    assign w_operand_b[0] = i_operand_b0;

    assign w_operand_a[1] = i_operand_a1;
    assign w_operand_b[1] = i_operand_b1;

    assign w_request = (r_state == ST_IDLE) && i_start;

    // Single: lane 0 only. Pair: both lanes transfer together.
    assign w_mul_complete    = w_mul_done[0] && (!r_pair_mode || w_mul_done[1]);
    assign w_reduce_complete = w_reduce_valid[0] && (!r_pair_mode || w_reduce_valid[1]);

    assign w_reduce_request  = (r_state == ST_WAIT_MUL) && w_mul_complete;
    assign o_result_valid    = (r_state == ST_WAIT_REDUCE) && w_reduce_complete;
    assign w_result_transfer = o_result_valid && i_result_ready;

    assign o_residue_0 = w_residue[0];
    assign o_residue_1 = w_residue[1];

    always @(posedge i_clk) begin
        if (!i_rst_n) begin
            r_state     <= ST_IDLE;
            r_pair_mode <= 1'b0;
        end
        else begin
            case (r_state)
                ST_IDLE: begin
                    if (i_start) begin
                        r_pair_mode <= i_pair_mode;
                        r_state     <= ST_WAIT_MUL;
                    end
                end

                ST_WAIT_MUL: begin
                    if (w_reduce_request) begin
                        r_state <= ST_WAIT_REDUCE;
                    end
                end

                ST_WAIT_REDUCE: begin
                    if (w_result_transfer) begin
                        r_state <= ST_IDLE;
                    end
                end

                default: begin
                    r_state <= ST_IDLE;
                end
            endcase
        end
    end

    generate
        for (i = 0; i < LP_LANES; i = i + 1) begin : g_lane
            // Use incoming mode on START, captured mode afterward.
            assign w_mul_start[i]    = w_request && ((i == 0) || i_pair_mode);
            assign w_reduce_start[i] = w_reduce_request && ((i == 0) || r_pair_mode);
            assign w_reduce_ready[i] = w_result_transfer && ((i == 0) || r_pair_mode);

            p1305_mul_r4 u_mul (
                .i_clk       (i_clk),
                .i_rst_n     (i_rst_n),

                .i_operand_a (w_operand_a[i]),
                .i_operand_b (w_operand_b[i]),
                .i_start     (w_mul_start[i]),

                .o_product   (w_product[i]),
                .o_done      (w_mul_done[i])
            );

            p1305_reduce u_reduce (
                .i_clk     (i_clk),
                .i_rst_n   (i_rst_n),

                .i_product (w_product[i]),
                .i_start   (w_reduce_start[i]),

                .o_residue (w_residue[i]),
                .o_valid   (w_reduce_valid[i]),
                .i_ready   (w_reduce_ready[i])
            );
        end
    endgenerate

endmodule

`default_nettype wire

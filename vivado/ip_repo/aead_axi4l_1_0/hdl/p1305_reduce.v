`default_nettype none

module p1305_reduce (
    input  wire         i_clk,
    input  wire         i_rst_n,

    input  wire [259:0] i_product,
    input  wire         i_start,

    output wire [129:0] o_residue,
    output wire         o_valid,
    input  wire         i_ready
);

    localparam [1:0] ST_IDLE   = 2'd0;
    localparam [1:0] ST_FINISH = 2'd1;
    localparam [1:0] ST_HOLD   = 2'd3;

    reg  [1:0] r_state;

    // Reuse the bank for fold 1, then the held residue.
    reg  [132:0] r_fold;

    wire [129:0] w_product_low;
    wire [129:0] w_product_high;

    wire [132:0] w_fold_1;

    wire [129:0] w_fold_2_low;
    wire [130:0] w_corrected_sum;
    wire [129:0] w_residue;

    wire [5:0]   w_fold_2_increment;
    wire [5:0]   w_correction_increment;

    assign w_product_low  = i_product[129:0];
    assign w_product_high = i_product[259:130];

    // Fold 1: low + 5*high < 6*2^130; use 133 bits.
    // 2^130 = 5 (mod p); 5*high = (high << 2) + high.
    assign w_fold_1 = {3'b000, w_product_low} +
                     {1'b0, w_product_high, 2'b00} +
                     {3'b000, w_product_high};

    // Fold 2 increment: 5*high <= 25.
    assign w_fold_2_increment = {1'b0, r_fold[132:130], 2'b00} +
                               {3'b000, r_fold[132:130]};
    assign w_fold_2_low       = r_fold[129:0] + {124'b0, w_fold_2_increment};

    // Parallel candidates: low+k and low+(k+5).
    // Only the six-bit increment precedes the wide adder.
    assign w_correction_increment = w_fold_2_increment + 6'd5;
    assign w_corrected_sum        = {1'b0, r_fold[129:0]} +
                                    {125'b0, w_correction_increment};

    // fold_2 < 2^130+25. Carry from fold_2+5 selects fold_2-p.
    // That carry also handles overflow discarded by w_fold_2_low.
    assign w_residue = w_corrected_sum[130] ? w_corrected_sum[129:0] :
                                            w_fold_2_low;

    assign o_residue = r_fold[129:0];
    assign o_valid   = (r_state == ST_HOLD);

    // Reset aborts control; data is retained.
    always @(posedge i_clk) begin
        if (!i_rst_n) begin
            r_state <= ST_IDLE;
        end
        else begin
            case (r_state)
                ST_IDLE: begin
                    // Capture fold 1 on START.
                    if (i_start) begin
                        r_fold  <= w_fold_1;
                        r_state <= ST_FINISH;
                    end
                end

                ST_FINISH: begin
                    // Capture the residue and raise valid.
                    r_fold  <= {3'b000, w_residue};
                    r_state <= ST_HOLD;
                end

                ST_HOLD: begin
                    // Hold data/valid until ready.
                    if (i_ready) begin
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

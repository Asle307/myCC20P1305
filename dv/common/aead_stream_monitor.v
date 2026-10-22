`timescale 1ns / 1ps

module aead_stream_monitor #(
    parameter integer LP_WIDTH = 1
) (
    input wire                  i_clk,
    input wire                  i_rst_n,

    input wire [LP_WIDTH-1:0]    i_data,
    input wire                  i_valid,
    input wire                  i_ready
);
    reg [LP_WIDTH-1:0] r_held;
    reg                r_stalled = 1'b0;

    always @(posedge i_clk) begin
        if (!i_rst_n) begin
            r_stalled <= 1'b0;
        end
        else begin
            if ((i_valid !== 1'b0 && i_valid !== 1'b1) ||
                (i_valid && ((^i_data) === 1'bx ||
                             (i_ready !== 1'b0 && i_ready !== 1'b1))) ||
                (r_stalled && (i_valid !== 1'b1 || i_data !== r_held))) begin
                $display("FAIL %m: unknown transfer or unstable VALID/data under backpressure");
                $finish;
            end
            r_held    <= i_data;
            r_stalled <= i_valid && !i_ready;
        end
    end
endmodule

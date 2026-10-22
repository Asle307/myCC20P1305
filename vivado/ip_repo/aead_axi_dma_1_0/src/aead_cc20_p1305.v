`default_nettype none

module aead_cc20_p1305 (
    input  wire         i_clk,
    input  wire         i_rst_n,

    input  wire [255:0] i_key,
    input  wire [ 95:0] i_nonce,
    input  wire         i_start,
    input  wire         i_mode,
    output wire         o_start_ready,

    input  wire [ 31:0] i_data,
    input  wire [  2:0] i_data_byte_count,
    input  wire         i_is_aad,
    input  wire         i_data_valid,
    output wire         o_data_ready,
    input  wire         i_data_last,

    output wire [ 31:0] o_data,
    output wire [  2:0] o_data_byte_count,
    output wire         o_is_tag,
    output wire         o_data_last,
    output wire         o_data_valid,
    input  wire         i_data_ready,

    output reg          o_done,
    output reg          o_auth_ok
);

    localparam [3:0] ST_IDLE           = 4'd0;
    localparam [3:0] ST_OTK_REQUEST    = 4'd1;
    localparam [3:0] ST_OTK_WAIT       = 4'd2;
    localparam [3:0] ST_AAD            = 4'd3;
    localparam [3:0] ST_PAYLOAD        = 4'd4;
    localparam [3:0] ST_CC_REQUEST     = 4'd5;
    localparam [3:0] ST_CC_WAIT        = 4'd6;
    localparam [3:0] ST_PAYLOAD_XOR    = 4'd7;
    localparam [3:0] ST_PAYLOAD_MAC    = 4'd8;
    localparam [3:0] ST_PAYLOAD_OUTPUT = 4'd9;
    localparam [3:0] ST_LENGTH         = 4'd10;
    localparam [3:0] ST_TAG_OUTPUT     = 4'd11;
    localparam [3:0] ST_TAG_INPUT      = 4'd12;

    // Session context is captured once on START.
    reg  [255:0] r_key;
    reg  [ 95:0] r_nonce;
    reg  [  3:0] r_state;
    reg          r_mode;

    // One assembled input block; byte count also supplies the word position.
    reg  [127:0] r_input_data;
    reg  [  4:0] r_input_byte_count;
    reg          r_input_valid;
    reg          r_input_last;
    wire         w_input_block_ready;
    wire         w_input_word_transfer;
    integer      byte_index;

    // MAC consumes ciphertext before decrypt overwrites this payload bank.
    reg  [127:0] r_payload_data;
    wire [127:0] w_payload_masked_xor;
    reg  [  4:0] r_payload_byte_count;
    reg          r_payload_last;
    wire         w_payload_capture;
    wire         w_payload_xor;

    // One held CC20 result supplies four consecutive 16-byte payload blocks.
    wire [511:0] w_keystream;
    wire [127:0] w_keystream_slice;
    reg  [ 31:0] r_payload_counter;
    wire [ 31:0] w_cc_counter;
    wire [  3:0] w_cipher_state;
    reg  [  1:0] r_keystream_index;
    reg          r_keystream_available;
    wire         w_payload_release;
    wire         w_cc_request_valid;
    wire         w_cc_request_ready;
    wire         w_cc_result_valid;
    wire         w_cc_result_ready;

    // Original byte lengths, excluding padding and the received tag.
    wire [127:0] w_length_block;
    reg  [ 63:0] r_aad_length;
    reg  [ 37:0] r_payload_length;

    reg  [127:0] w_poly_block;
    wire [127:0] w_poly_tag;
    reg          w_poly_block_valid;
    wire         w_poly_block_ready;
    wire         w_poly_start;
    wire         w_poly_tag_valid;
    wire         w_poly_tag_ready;

    wire [127:0] w_output_block;
    wire [  4:0] w_output_count;
    wire [  4:0] w_output_remaining;
    reg  [  1:0] r_output_word;
    wire         w_output_final;
    wire         w_output_word_transfer;

    wire w_start_transfer;
    wire w_input_block_transfer;
    wire w_output_block_transfer;
    wire w_aad_transfer;
    wire w_poly_transfer;
    wire w_finish;

    function [127:0] f_mask_bytes;
        input [127:0] i_value;
        input [  4:0] i_count;
        integer i;
        begin
            f_mask_bytes = 128'd0;
            for (i = 0; i < 16; i = i + 1) begin
                if (i < i_count) begin
                    f_mask_bytes[i * 8 +: 8] = i_value[i * 8 +: 8];
                end
            end
        end
    endfunction

    assign w_payload_masked_xor = f_mask_bytes(r_payload_data ^ w_keystream_slice,
                                             r_payload_byte_count);
    assign w_length_block      = {26'd0, r_payload_length, r_aad_length};
    assign w_payload_release   = (r_keystream_index == 2'd3) || r_payload_last;

    // A retained 64-byte CC20 block serves four 16-byte payload blocks.
    assign w_keystream_slice = w_keystream[r_keystream_index * 128 +: 128];
    assign w_cipher_state    = r_keystream_available ? ST_PAYLOAD_XOR : ST_CC_REQUEST;

    assign o_start_ready    = (r_state == ST_IDLE) && i_rst_n;
    assign w_start_transfer = i_start && o_start_ready;

    // A type mismatch stalls without accepting a word for a later section.
    assign o_data_ready = i_rst_n && !r_input_valid &&
                          (((r_state == ST_AAD) && i_is_aad) ||
                           ((r_state == ST_PAYLOAD) && !i_is_aad) ||
                           ((r_state == ST_TAG_INPUT) && !i_is_aad && w_poly_tag_valid));
    assign w_input_word_transfer = i_data_valid && o_data_ready;

    // The byte count is also the insertion offset: every nonfinal word
    // contains four bytes. Clearing the bank supplies all RFC padding zeros.
    always @(posedge i_clk) begin
        if (!i_rst_n || w_start_transfer || w_input_block_transfer) begin
            r_input_data       <= 128'd0;
            r_input_byte_count <= 5'd0;
            r_input_valid      <= 1'b0;
            r_input_last       <= 1'b0;
        end
        else if (w_input_word_transfer) begin
            for (byte_index = 0; byte_index < 4; byte_index = byte_index + 1) begin
                if (byte_index < i_data_byte_count) begin
                    r_input_data[r_input_byte_count[3:2] * 32 + byte_index * 8 +: 8]
                        <= i_data[byte_index * 8 +: 8];
                end
            end
            r_input_byte_count <= r_input_byte_count + {2'b00, i_data_byte_count};
            r_input_last       <= i_data_last;
            r_input_valid      <= (r_input_byte_count == 5'd12) || i_data_last;
        end
    end

    // Section type was checked at word acceptance; FSM identifies the owner
    // of the completed block. The assembler already masked unused bytes.
    assign w_input_block_ready    = ((r_state == ST_AAD) && w_poly_block_ready) ||
                                    (r_state == ST_PAYLOAD) ||
                                    ((r_state == ST_TAG_INPUT) && w_poly_tag_valid);
    assign w_input_block_transfer = r_input_valid && w_input_block_ready;
    assign w_aad_transfer         = (r_state == ST_AAD) && w_input_block_transfer;
    assign w_payload_capture      = (r_state == ST_PAYLOAD) && w_input_block_transfer;
    assign w_payload_xor          = (r_state == ST_PAYLOAD_XOR);

    assign w_cc_request_valid = (r_state == ST_OTK_REQUEST) || (r_state == ST_CC_REQUEST);
    assign w_cc_counter       = (r_state == ST_OTK_REQUEST) ? 32'd0 : r_payload_counter;
    assign w_cc_result_ready  = (r_state == ST_OTK_WAIT) ||
                                (w_payload_xor && w_payload_release);

    // OTK transfer starts MAC and releases CC20.
    assign w_poly_start = (r_state == ST_OTK_WAIT) && w_cc_result_valid;

    always @(*) begin
        w_poly_block       = 128'd0;
        w_poly_block_valid = 1'b0;

        case (r_state)
            ST_AAD: begin
                w_poly_block       = r_input_data;
                w_poly_block_valid = r_input_valid && (r_input_byte_count != 5'd0);
            end
            ST_PAYLOAD_MAC: begin
                w_poly_block       = r_payload_data;
                w_poly_block_valid = 1'b1;
            end
            ST_LENGTH: begin
                w_poly_block       = w_length_block;
                w_poly_block_valid = 1'b1;
            end
            default: begin
            end
        endcase
    end

    assign w_poly_transfer = w_poly_block_valid && w_poly_block_ready;

    // Serialize each held payload/tag block into little-endian CPU words.
    // The FSM advances only after the final word has actually transferred.
    assign o_is_tag                = (r_state == ST_TAG_OUTPUT);
    assign w_output_block          = o_is_tag ? w_poly_tag : r_payload_data;
    assign w_output_count          = o_is_tag ? 5'd16 : r_payload_byte_count;
    assign w_output_remaining      = w_output_count - {1'b0, r_output_word, 2'b00};
    assign w_output_final          = (w_output_remaining <= 5'd4);
    assign o_data_byte_count       = w_output_final ? w_output_remaining[2:0] : 3'd4;
    assign o_data_last             = (o_is_tag || r_payload_last) && w_output_final;
    assign o_data_valid            = (r_state == ST_PAYLOAD_OUTPUT) ||
                                     (o_is_tag && w_poly_tag_valid);
    assign w_output_word_transfer  = o_data_valid && i_data_ready;
    assign w_output_block_transfer = w_output_word_transfer && w_output_final;
    assign w_poly_tag_ready        = (o_is_tag && w_output_block_transfer) ||
                                     ((r_state == ST_TAG_INPUT) && w_input_block_transfer);

    assign o_data = w_output_block[r_output_word * 32 +: 32];

    always @(posedge i_clk) begin
        if (!i_rst_n || w_start_transfer) begin
            r_output_word <= 2'd0;
        end
        else if (w_output_word_transfer) begin
            r_output_word <= w_output_final ? 2'd0 : r_output_word + 1'b1;
        end
    end

    assign w_finish = ((r_state == ST_TAG_OUTPUT) && w_output_block_transfer) ||
                      ((r_state == ST_TAG_INPUT) && w_input_block_transfer);

    // AAD/payload/tag phases advance only on complete-block handshakes.
    always @(posedge i_clk) begin
        if (!i_rst_n) begin
            r_state               <= ST_IDLE;
            r_keystream_available <= 1'b0;
            r_payload_counter     <= 32'd1;
            r_keystream_index     <= 2'd0;
            r_aad_length          <= 64'd0;
            r_payload_length      <= 38'd0;
            o_done                <= 1'b0;
            o_auth_ok             <= 1'b0;
        end
        else begin
            o_done <= w_finish;
            if (w_finish && !r_mode) begin
                o_auth_ok <= (r_input_data == w_poly_tag);
            end

            case (r_state)
                ST_IDLE: begin
                    if (w_start_transfer) begin
                        r_key                 <= i_key;
                        r_nonce               <= i_nonce;
                        r_mode                <= i_mode;
                        r_keystream_available <= 1'b0;
                        r_payload_counter     <= 32'd1;
                        r_keystream_index     <= 2'd0;
                        r_aad_length          <= 64'd0;
                        r_payload_length      <= 38'd0;
                        o_auth_ok             <= 1'b0;
                        r_state               <= ST_OTK_REQUEST;
                    end
                end
                ST_OTK_REQUEST, ST_CC_REQUEST: begin
                    if (w_cc_request_ready) begin
                        r_state <= (r_state == ST_OTK_REQUEST) ? ST_OTK_WAIT : ST_CC_WAIT;
                    end
                end
                ST_OTK_WAIT: begin
                    if (w_cc_result_valid) begin
                        r_state <= ST_AAD;
                    end
                end
                ST_AAD: begin
                    if (w_aad_transfer) begin
                        if (r_input_byte_count == 5'd16) begin
                            r_aad_length[63:4] <= r_aad_length[63:4] + 1'b1;
                        end
                        else begin
                            r_aad_length[3:0] <= r_input_byte_count[3:0];
                        end
                        if (r_input_last) begin
                            r_state <= ST_PAYLOAD;
                        end
                    end
                end
                ST_PAYLOAD: begin
                    if (w_payload_capture) begin
                        r_payload_data       <= r_input_data;
                        r_payload_byte_count <= r_input_byte_count;
                        r_payload_last       <= r_input_last;
                        if (r_input_byte_count == 5'd16) begin
                            r_payload_length[37:4] <= r_payload_length[37:4] + 1'b1;
                        end
                        else begin
                            r_payload_length[3:0] <= r_input_byte_count[3:0];
                        end
                        if (r_input_byte_count == 5'd0) begin
                            r_state <= ST_LENGTH;
                        end
                        else begin
                            r_state <= r_mode ? w_cipher_state : ST_PAYLOAD_MAC;
                        end
                    end
                end
                ST_CC_WAIT: begin
                    if (w_cc_result_valid) begin
                        r_keystream_available <= 1'b1;
                        r_state               <= ST_PAYLOAD_XOR;
                    end
                end
                ST_PAYLOAD_XOR: begin
                    r_payload_data    <= w_payload_masked_xor;
                    r_keystream_index <= r_keystream_index + 1'b1;
                    if (r_keystream_index == 2'd3) begin
                        r_payload_counter <= r_payload_counter + 1'b1;
                    end
                    if (w_payload_release) begin
                        r_keystream_available <= 1'b0;
                    end
                    r_state <= r_mode ? ST_PAYLOAD_MAC : ST_PAYLOAD_OUTPUT;
                end
                ST_PAYLOAD_MAC: begin
                    if (w_poly_transfer) begin
                        r_state <= r_mode ? ST_PAYLOAD_OUTPUT : w_cipher_state;
                    end
                end
                ST_PAYLOAD_OUTPUT: begin
                    if (w_output_block_transfer) begin
                        r_state <= r_payload_last ? ST_LENGTH : ST_PAYLOAD;
                    end
                end
                ST_LENGTH: begin
                    if (w_poly_transfer) begin
                        r_state <= r_mode ? ST_TAG_OUTPUT : ST_TAG_INPUT;
                    end
                end
                ST_TAG_OUTPUT, ST_TAG_INPUT: begin
                    if (w_finish) begin
                        r_state <= ST_IDLE;
                    end
                end
                default: begin
                    r_state <= ST_IDLE;
                end
            endcase
        end
    end

    cc20_core u_cc20_core (
        .i_clk           (i_clk),
        .i_rst_n         (i_rst_n),

        .i_key           (r_key),
        .i_nonce         (r_nonce),
        .i_counter       (w_cc_counter),
        .i_request_valid (w_cc_request_valid),
        .o_request_ready (w_cc_request_ready),

        .o_keystream     (w_keystream),
        .o_result_valid  (w_cc_result_valid),
        .i_result_ready  (w_cc_result_ready)
    );

    p1305_core u_p1305_core (
        .i_clk         (i_clk),
        .i_rst_n       (i_rst_n),

        .i_otk         (w_keystream[255:0]),
        .i_start       (w_poly_start),

        .i_block       (w_poly_block),
        .i_block_valid (w_poly_block_valid),
        .o_block_ready (w_poly_block_ready),
        .i_last        (r_state == ST_LENGTH),

        .o_tag         (w_poly_tag),
        .o_tag_valid   (w_poly_tag_valid),
        .i_tag_ready   (w_poly_tag_ready)
    );

endmodule

`default_nettype wire

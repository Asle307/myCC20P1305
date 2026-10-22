`default_nettype none

// DMA transport: pad4(AAD) || pad4(payload) || received tag (decrypt).
// Every AXIS beat has KEEP=1111. Length registers remove transport padding;
// empty native sections are injected locally, without zero-length DMA packets.
module aead_axis (
    input  wire         i_clk,
    input  wire         i_rst_n,

    input  wire [255:0] i_key,
    input  wire [ 95:0] i_nonce,
    input  wire [ 31:0] i_aad_bytes,
    input  wire [ 31:0] i_payload_bytes,
    input  wire         i_start,
    input  wire         i_mode,
    output wire         o_start_ready,
    output wire         o_done,
    output wire         o_auth_ok,
    output reg          o_error,

    input  wire [31:0]  s_axis_tdata,
    input  wire [ 3:0]  s_axis_tkeep,
    input  wire         s_axis_tvalid,
    input  wire         s_axis_tlast,
    output wire         s_axis_tready,

    output wire [31:0]  m_axis_tdata,
    output wire [ 3:0]  m_axis_tkeep,
    output wire         m_axis_tvalid,
    output wire         m_axis_tlast,
    input  wire         m_axis_tready
);
    localparam [2:0] ST_IDLE    = 3'd0;
    localparam [2:0] ST_AAD     = 3'd1;
    localparam [2:0] ST_PAYLOAD = 3'd2;
    localparam [2:0] ST_TAG     = 3'd3;
    localparam [2:0] ST_WAIT    = 3'd4;

    reg  [31:0] r_aad_left, r_payload_left;
    reg  [ 2:0] r_state;
    reg  [ 1:0] r_tag_word;
    reg          r_mode;
    wire [31:0] w_left, w_core_input;
    wire [ 2:0] w_count, w_core_count;
    wire         w_section, w_empty, w_last, w_packet_last, w_bad;
    wire         w_core_ready, w_core_valid, w_core_tag, w_core_last, w_start_ready;
    wire         w_input_valid, w_input_transfer, w_axis_transfer;

    assign o_start_ready = (r_state == ST_IDLE) && w_start_ready && !o_error;
    assign w_section     = (r_state == ST_AAD) || (r_state == ST_PAYLOAD) || (r_state == ST_TAG);
    assign w_left        = (r_state == ST_AAD) ? r_aad_left : r_payload_left;
    assign w_empty       = (r_state != ST_TAG) && (w_left == 0);
    assign w_last        = (r_state == ST_TAG) ? (r_tag_word == 3) : (w_left <= 4);
    assign w_count       = (r_state == ST_TAG || w_left >= 4) ? 3'd4 : w_left[2:0];
    assign w_packet_last = (r_state == ST_TAG) ? w_last :
                          (r_mode && w_last && ((r_state == ST_PAYLOAD) || r_payload_left == 0));
    assign w_bad         = (s_axis_tkeep != 4'hf) || (s_axis_tlast != w_packet_last);
    assign s_axis_tready = w_section && !w_empty && w_core_ready && !o_error;
    assign w_axis_transfer = s_axis_tvalid && s_axis_tready;
    assign w_input_valid = w_section && !o_error && (w_empty || (s_axis_tvalid && !w_bad));
    assign w_input_transfer = w_input_valid && w_core_ready;
    assign w_core_input  = w_empty ? 32'd0 : s_axis_tdata;

    // Payload transport padding is already zeroed by the native core. Only
    // the final packet beat asserts TLAST; encrypt payload-LAST is internal.
    assign m_axis_tkeep  = 4'hf;
    assign m_axis_tvalid = w_core_valid;
    assign m_axis_tlast  = w_core_last && (!r_mode || w_core_tag);

    always @(posedge i_clk) begin
        if (!i_rst_n) begin
            r_state        <= ST_IDLE;
            r_aad_left     <= 32'd0;
            r_payload_left <= 32'd0;
            r_tag_word     <= 2'd0;
            r_mode         <= 1'b0;
            o_error        <= 1'b0;
        end
        else begin
            if (i_start && o_start_ready) begin
                r_state        <= ST_AAD;
                r_aad_left     <= i_aad_bytes;
                r_payload_left <= i_payload_bytes;
                r_tag_word     <= 2'd0;
                r_mode         <= i_mode;
            end
            if (w_axis_transfer && w_bad) begin
                // Stop input on malformed framing. Software must reset both
                // DMA channels and this wrapper; discard tentative plaintext.
                o_error <= 1'b1;
            end
            if (w_input_transfer) begin
                case (r_state)
                    ST_AAD: begin
                        r_aad_left <= w_last ? 32'd0 : r_aad_left - 4;
                        if (w_last) begin
                            r_state <= ST_PAYLOAD;
                        end
                    end
                    ST_PAYLOAD: begin
                        r_payload_left <= w_last ? 32'd0 : r_payload_left - 4;
                        if (w_last) begin
                            r_state <= r_mode ? ST_WAIT : ST_TAG;
                        end
                    end
                    ST_TAG: begin
                        r_tag_word <= r_tag_word + 1'b1;
                        if (w_last) begin
                            r_state <= ST_WAIT;
                        end
                    end
                    default: begin
                    end
                endcase
            end
            if (o_done) begin
                r_state <= ST_IDLE;
            end
        end
    end

    aead_cc20_p1305 u_aead (
        .i_clk             (i_clk),
        .i_rst_n           (i_rst_n),

        .i_key             (i_key),
        .i_nonce           (i_nonce),
        .i_start           (i_start && o_start_ready),
        .i_mode            (i_mode),
        .o_start_ready     (w_start_ready),

        .i_data            (w_core_input),
        .i_data_byte_count (w_count),
        .i_is_aad          (r_state == ST_AAD),
        .i_data_valid      (w_input_valid),
        .o_data_ready      (w_core_ready),
        .i_data_last       (w_last),

        .o_data            (m_axis_tdata),
        .o_data_byte_count (w_core_count),
        .o_is_tag          (w_core_tag),
        .o_data_last       (w_core_last),
        .o_data_valid      (w_core_valid),
        .i_data_ready      (m_axis_tready),

        .o_done            (o_done),
        .o_auth_ok         (o_auth_ok)
    );
endmodule

`default_nettype wire

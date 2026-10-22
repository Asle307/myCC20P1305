`timescale 1ns / 1ps

module aead_axi4l_slave_lite_v1_0_S00_AXI #(
    parameter integer C_S_AXI_DATA_WIDTH = 32,
    parameter integer C_S_AXI_ADDR_WIDTH = 6
) (
    input  wire                               S_AXI_ACLK,
    input  wire                               S_AXI_ARESETN,

    input  wire [C_S_AXI_ADDR_WIDTH-1:0]       S_AXI_AWADDR,
    input  wire [2:0]                          S_AXI_AWPROT,
    input  wire                               S_AXI_AWVALID,
    output wire                               S_AXI_AWREADY,

    input  wire [C_S_AXI_DATA_WIDTH-1:0]       S_AXI_WDATA,
    input  wire [(C_S_AXI_DATA_WIDTH/8)-1:0]   S_AXI_WSTRB,
    input  wire                               S_AXI_WVALID,
    output wire                               S_AXI_WREADY,

    output wire [1:0]                         S_AXI_BRESP,
    output wire                               S_AXI_BVALID,
    input  wire                               S_AXI_BREADY,

    input  wire [C_S_AXI_ADDR_WIDTH-1:0]       S_AXI_ARADDR,
    input  wire [2:0]                          S_AXI_ARPROT,
    input  wire                               S_AXI_ARVALID,
    output wire                               S_AXI_ARREADY,

    output wire [C_S_AXI_DATA_WIDTH-1:0]       S_AXI_RDATA,
    output wire [1:0]                         S_AXI_RRESP,
    output wire                               S_AXI_RVALID,
    input  wire                               S_AXI_RREADY
);
    // Register offsets (16 x 32-bit words, 64 bytes total):
    // 00 CONTROL: START[0], MODE_ENCRYPT[1], CLEAR_DONE[2], SOFT_RESET[3], CLEAR_ERROR[4].
    // 04 STATUS: START_READY[0], BUSY[1], DONE[2], AUTH_OK[3], INPUT_FREE[4],
    //            OUTPUT_VALID[5], ERROR[6], START_PENDING[7].
    // 08..24 KEY0..KEY7; 28..30 NONCE0..NONCE2. Word 0 is the low 32 bits.
    // 34 INPUT_DATA: staging register; PUSH copies it into the held input slot.
    // 38 write STREAM_CTRL: COUNT[2:0], IS_AAD[8], LAST[9], PUSH[16], POP[17].
    // 38 read OUTPUT_META: COUNT[2:0], IS_TAG[8], LAST[9], VALID[16].
    // 3c OUTPUT_DATA: reading does not pop. Write POP only after reading data/meta.
    // Commands are write-one pulses, masked by WSTRB. Configuration writes are
    // rejected while BUSY. Invalid accesses return SLVERR; ERROR is sticky.

    localparam [1:0] LP_OKAY   = 2'b00;
    localparam [1:0] LP_SLVERR = 2'b10;

    // AXI holds AW and W independently: either channel may arrive first.
    // One outstanding write, with no new AW/W accepted until B is consumed.
    reg [C_S_AXI_ADDR_WIDTH-1:0] r_awaddr;
    reg [31:0] r_wdata;
    reg [ 3:0] r_wstrb;
    reg        r_aw_pending, r_w_pending;
    reg [ 1:0] r_bresp;
    reg        r_bvalid;
    reg [31:0] r_rdata;
    reg [ 1:0] r_rresp;
    reg        r_rvalid;
    wire       w_write_commit;
    wire [3:0] w_write_index;
    wire [31:0] w_command;
    reg         w_write_error;
    reg [31:0]  w_read_data;
    wire        w_read_error;

    // Software-visible configuration and data staging. Written only here;
    // no register is driven by both an AXI process and a core process.
    reg [31:0] r_config [0:10];
    reg [31:0] r_input_staging;
    reg        r_mode;
    reg        r_busy, r_done, r_auth_ok, r_error;
    reg        r_start, r_soft_reset;
    wire       w_configuration_free, w_start_ready, w_input_free;

    // Input slot keeps all core signals stable until VALID && READY.
    reg [31:0] r_input_data;
    reg [ 2:0] r_input_count;
    reg        r_input_aad, r_input_last, r_input_valid;
    wire       w_input_transfer;

    // Output slot belongs to software until explicit POP. No read side effects.
    reg [31:0] r_output_data;
    reg [ 2:0] r_output_count;
    reg        r_output_tag, r_output_last, r_output_valid;
    wire       w_output_transfer;

    wire [255:0] w_key;
    wire [ 95:0] w_nonce;
    wire [ 31:0] w_core_data;
    wire [  2:0] w_core_count;
    wire         w_core_rst_n, w_core_start_ready, w_core_input_ready;
    wire         w_core_tag, w_core_last, w_core_valid, w_core_ready;
    wire         w_core_done, w_core_auth_ok;
    integer      register_index;

    function [31:0] f_write_bytes;
        input [31:0] old_value;
        input [31:0] new_value;
        input [ 3:0] strobes;
        integer byte_number;
        begin
            f_write_bytes = old_value;
            for (byte_number = 0; byte_number < 4; byte_number = byte_number + 1) begin
                if (strobes[byte_number]) begin
                    f_write_bytes[byte_number * 8 +: 8] = new_value[byte_number * 8 +: 8];
                end
            end
        end
    endfunction

    assign S_AXI_AWREADY = S_AXI_ARESETN && !r_aw_pending && !r_bvalid;
    assign S_AXI_WREADY  = S_AXI_ARESETN && !r_w_pending && !r_bvalid;
    assign S_AXI_BRESP   = r_bresp;
    assign S_AXI_BVALID  = r_bvalid;
    assign S_AXI_ARREADY = S_AXI_ARESETN && !r_rvalid;
    assign S_AXI_RDATA   = r_rdata;
    assign S_AXI_RRESP   = r_rresp;
    assign S_AXI_RVALID  = r_rvalid;

    assign w_write_commit = r_aw_pending && r_w_pending && !r_bvalid;
    assign w_write_index  = r_awaddr[5:2];
    assign w_command      = f_write_bytes(32'd0, r_wdata, r_wstrb);
    assign w_read_error   = (S_AXI_ARADDR[1:0] != 2'b00);

    assign w_core_rst_n         = S_AXI_ARESETN && !r_soft_reset;
    assign w_configuration_free = !r_busy && !r_start && !r_soft_reset;
    assign w_start_ready        = w_configuration_free && !r_input_valid &&
                                  !r_output_valid && w_core_start_ready;
    assign w_input_free         = r_busy && !r_start && !r_soft_reset && !r_input_valid;
    assign w_input_transfer     = r_input_valid && w_core_input_ready;
    assign w_core_ready         = !r_output_valid && r_busy && !r_soft_reset;
    assign w_output_transfer    = w_core_valid && w_core_ready;

    assign w_key = {r_config[7], r_config[6], r_config[5], r_config[4],
                    r_config[3], r_config[2], r_config[1], r_config[0]};
    assign w_nonce = {r_config[10], r_config[9], r_config[8]};

    // Reject a command without partially applying it. Failed PUSH never
    // overwrites a pending word; failed POP never discards a future word.
    always @(*) begin
        w_write_error = (r_awaddr[1:0] != 2'b00);
        if (|r_wstrb) begin
            case (w_write_index)
                4'h0: begin
                    if (!w_command[3]) begin
                        if (w_command[0] && !w_start_ready) begin
                            w_write_error = 1'b1;
                        end
                        if (r_wstrb[0] && (w_command[1] != r_mode) && !w_configuration_free) begin
                            w_write_error = 1'b1;
                        end
                    end
                end
                4'h1, 4'hf: w_write_error = 1'b1;
                4'h2, 4'h3, 4'h4, 4'h5, 4'h6, 4'h7,
                4'h8, 4'h9, 4'ha, 4'hb, 4'hc: begin
                    if (!w_configuration_free) begin
                        w_write_error = 1'b1;
                    end
                end
                4'he: begin
                    if ((w_command[16] && (!w_input_free || w_command[2:0] > 4 ||
                         (!w_command[9] && w_command[2:0] != 4))) ||
                        (w_command[17] && !r_output_valid)) begin
                        w_write_error = 1'b1;
                    end
                end
                default: begin
                end
            endcase
        end
    end

    // Snapshot at AR handshake: changing STATUS cannot change an outstanding
    // RDATA response, even if software holds RREADY low for many clocks.
    always @(*) begin
        w_read_data = 32'd0;
        case (S_AXI_ARADDR[5:2])
            4'h0: w_read_data = {30'd0, r_mode, 1'b0};
            4'h1: w_read_data = {24'd0, r_start, r_error, r_output_valid,
                                 w_input_free, r_auth_ok, r_done, r_busy, w_start_ready};
            4'h2, 4'h3, 4'h4, 4'h5, 4'h6, 4'h7,
            4'h8, 4'h9, 4'ha, 4'hb, 4'hc: w_read_data = r_config[S_AXI_ARADDR[5:2] - 2];
            4'hd: w_read_data = r_input_staging;
            4'he: begin
                if (r_output_valid) begin
                    w_read_data = {15'd0, 1'b1, 6'd0, r_output_last, r_output_tag, 5'd0, r_output_count};
                end
            end
            4'hf: w_read_data = r_output_valid ? r_output_data : 32'd0;
            default: begin
            end
        endcase
    end

    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN) begin
            r_awaddr     <= 0;
            r_wdata      <= 0;
            r_wstrb      <= 0;
            r_aw_pending <= 1'b0;
            r_w_pending  <= 1'b0;
            r_bvalid     <= 1'b0;
            r_bresp      <= LP_OKAY;
            r_rvalid     <= 1'b0;
            r_rdata      <= 0;
            r_rresp      <= LP_OKAY;
        end
        else begin
            if (S_AXI_AWVALID && S_AXI_AWREADY) begin
                r_awaddr     <= S_AXI_AWADDR;
                r_aw_pending <= 1'b1;
            end
            if (S_AXI_WVALID && S_AXI_WREADY) begin
                r_wdata     <= S_AXI_WDATA;
                r_wstrb     <= S_AXI_WSTRB;
                r_w_pending <= 1'b1;
            end
            if (w_write_commit) begin
                r_aw_pending <= 1'b0;
                r_w_pending  <= 1'b0;
                r_bvalid     <= 1'b1;
                r_bresp      <= w_write_error ? LP_SLVERR : LP_OKAY;
            end
            else if (r_bvalid && S_AXI_BREADY) begin
                r_bvalid <= 1'b0;
            end
            if (S_AXI_ARVALID && S_AXI_ARREADY) begin
                r_rdata  <= w_read_error ? 32'd0 : w_read_data;
                r_rresp  <= w_read_error ? LP_SLVERR : LP_OKAY;
                r_rvalid <= 1'b1;
            end
            else if (r_rvalid && S_AXI_RREADY) begin
                r_rvalid <= 1'b0;
            end
        end
    end

    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN) begin
            for (register_index = 0; register_index < 11; register_index = register_index + 1) begin
                r_config[register_index] <= 32'd0;
            end
            r_input_staging <= 0;
            r_mode          <= 1'b0;
            r_busy          <= 1'b0;
            r_done          <= 1'b0;
            r_auth_ok       <= 1'b0;
            r_error         <= 1'b0;
            r_start         <= 1'b0;
            r_soft_reset    <= 1'b0;
            r_input_data    <= 0;
            r_input_count   <= 0;
            r_input_aad     <= 1'b0;
            r_input_last    <= 1'b0;
            r_input_valid   <= 1'b0;
            r_output_data   <= 0;
            r_output_count  <= 0;
            r_output_tag    <= 1'b0;
            r_output_last   <= 1'b0;
            r_output_valid  <= 1'b0;
        end
        else begin
            r_soft_reset <= 1'b0;
            if (r_start && w_core_start_ready) begin
                r_start <= 1'b0;
            end
            if (w_input_transfer) begin
                r_input_valid <= 1'b0;
            end
            if (w_output_transfer) begin
                r_output_data  <= w_core_data;
                r_output_count <= w_core_count;
                r_output_tag   <= w_core_tag;
                r_output_last  <= w_core_last;
                r_output_valid <= 1'b1;
            end
            if (w_core_done && !r_soft_reset) begin
                r_busy    <= 1'b0;
                r_done    <= 1'b1;
                r_auth_ok <= w_core_auth_ok;
            end
            if (w_write_commit && !w_write_error && |r_wstrb) begin
                case (w_write_index)
                    4'h0: begin
                        if (r_wstrb[0]) begin
                            r_mode <= w_command[1];
                        end
                        if (w_command[2]) begin
                            r_done <= 1'b0;
                        end
                        if (w_command[4]) begin
                            r_error <= 1'b0;
                        end
                        if (w_command[0]) begin
                            r_start   <= 1'b1;
                            r_busy    <= 1'b1;
                            r_done    <= 1'b0;
                            r_auth_ok <= 1'b0;
                        end
                        // Synchronous abort resets the core and both stream slots.
                        // Preserve software key/nonce, mode and INPUT_DATA staging.
                        if (w_command[3]) begin
                            r_soft_reset   <= 1'b1;
                            r_start        <= 1'b0;
                            r_busy         <= 1'b0;
                            r_done         <= 1'b0;
                            r_auth_ok      <= 1'b0;
                            r_error        <= 1'b0;
                            r_input_valid  <= 1'b0;
                            r_output_valid <= 1'b0;
                        end
                    end
                    4'h2, 4'h3, 4'h4, 4'h5, 4'h6, 4'h7,
                    4'h8, 4'h9, 4'ha, 4'hb, 4'hc: begin
                        r_config[w_write_index - 2] <= f_write_bytes(r_config[w_write_index - 2], r_wdata, r_wstrb);
                    end
                    4'hd: r_input_staging <= f_write_bytes(r_input_staging, r_wdata, r_wstrb);
                    4'he: begin
                        if (w_command[16]) begin
                            r_input_data  <= r_input_staging;
                            r_input_count <= w_command[2:0];
                            r_input_aad   <= w_command[8];
                            r_input_last  <= w_command[9];
                            r_input_valid <= 1'b1;
                        end
                        if (w_command[17]) begin
                            r_output_valid <= 1'b0;
                        end
                    end
                    default: begin
                    end
                endcase
            end
            // Error reporting wins over clear; no accepted command can disappear.
            if ((w_write_commit && w_write_error) ||
                (S_AXI_ARVALID && S_AXI_ARREADY && w_read_error)) begin
                r_error <= 1'b1;
            end
        end
    end

    // Same architecture as the VJU_SoC AXI-Lite + compute boundary, with the
    // native AEAD stream handshake owned by this register/slot adapter.
    aead_cc20_p1305 u_aead (
        .i_clk             (S_AXI_ACLK),
        .i_rst_n           (w_core_rst_n),

        .i_key             (w_key),
        .i_nonce           (w_nonce),
        .i_start           (r_start),
        .i_mode            (r_mode),
        .o_start_ready     (w_core_start_ready),

        .i_data            (r_input_data),
        .i_data_byte_count (r_input_count),
        .i_is_aad          (r_input_aad),
        .i_data_valid      (r_input_valid),
        .o_data_ready      (w_core_input_ready),
        .i_data_last       (r_input_last),

        .o_data            (w_core_data),
        .o_data_byte_count (w_core_count),
        .o_is_tag          (w_core_tag),
        .o_data_last       (w_core_last),
        .o_data_valid      (w_core_valid),
        .i_data_ready      (w_core_ready),

        .o_done            (w_core_done),
        .o_auth_ok         (w_core_auth_ok)
    );

endmodule

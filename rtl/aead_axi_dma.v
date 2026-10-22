`default_nettype none

// AXI-Lite configuration with independent DMA streams; one shared clock.
module aead_axi_dma (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 CLK CLK",
       X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF S_AXI:S_AXIS:M_AXIS, ASSOCIATED_RESET i_rst_n" *)
    input  wire         i_clk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 RST RST",
       X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire         i_rst_n,

    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WDATA",
       X_INTERFACE_PARAMETER = "XIL_INTERFACENAME S_AXI, PROTOCOL AXI4LITE, DATA_WIDTH 32, ADDR_WIDTH 6, NUM_READ_OUTSTANDING 1, NUM_WRITE_OUTSTANDING 1" *)
    input  wire [31:0]  s_axi_wdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWADDR" *)
    input  wire [ 5:0]  s_axi_awaddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WSTRB" *)
    input  wire [ 3:0]  s_axi_wstrb,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWPROT" *)
    input  wire [ 2:0]  s_axi_awprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWVALID" *)
    input  wire         s_axi_awvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWREADY" *)
    output wire         s_axi_awready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WVALID" *)
    input  wire         s_axi_wvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WREADY" *)
    output wire         s_axi_wready,

    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BRESP" *)
    output reg  [ 1:0]  s_axi_bresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BVALID" *)
    output reg          s_axi_bvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BREADY" *)
    input  wire         s_axi_bready,

    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RDATA" *)
    output reg  [31:0]  s_axi_rdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARADDR" *)
    input  wire [ 5:0]  s_axi_araddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARPROT" *)
    input  wire [ 2:0]  s_axi_arprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RRESP" *)
    output reg  [ 1:0]  s_axi_rresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARVALID" *)
    input  wire         s_axi_arvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARREADY" *)
    output wire         s_axi_arready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RVALID" *)
    output reg          s_axi_rvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RREADY" *)
    input  wire         s_axi_rready,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS TDATA",
       X_INTERFACE_PARAMETER = "XIL_INTERFACENAME S_AXIS, TDATA_NUM_BYTES 4, HAS_TKEEP 1, HAS_TLAST 1" *)
    input  wire [31:0]  s_axis_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS TKEEP" *)
    input  wire [ 3:0]  s_axis_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS TVALID" *)
    input  wire         s_axis_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS TLAST" *)
    input  wire         s_axis_tlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS TREADY" *)
    output wire         s_axis_tready,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS TDATA",
       X_INTERFACE_PARAMETER = "XIL_INTERFACENAME M_AXIS, TDATA_NUM_BYTES 4, HAS_TKEEP 1, HAS_TLAST 1" *)
    output wire [31:0]  m_axis_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS TKEEP" *)
    output wire [ 3:0]  m_axis_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS TVALID" *)
    output wire         m_axis_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS TLAST" *)
    output wire         m_axis_tlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS TREADY" *)
    input  wire         m_axis_tready
);
    // 00 CONTROL: START[0], ENCRYPT[1], CLEAR_DONE[2], RESET[3], CLEAR_ERROR[4].
    // CONTROL read signature 414532xx identifies the incompatible DMA map.
    // 04 STATUS: READY[0], BUSY[1], DONE[2], AUTH_OK[3], ERROR[6], OVERFLOW[7].
    // 08..24 KEY; 28..30 NONCE; 34 AAD_BYTES; 38 PAYLOAD_BYTES; 3c CYCLES (RO).
    reg [31:0] r_config [0:12];
    reg [31:0] r_wdata, r_cycles, r_count;
    reg [ 5:0] r_awaddr;
    reg [ 3:0] r_wstrb;
    reg         r_aw_pending, r_w_pending;
    reg         r_mode, r_start, r_reset, r_busy, r_done, r_auth_ok;
    reg         r_error, r_overflow, r_counting;
    wire [31:0] w_command;
    reg  [31:0] w_read_data;
    wire [ 3:0] w_write_index;
    wire         w_commit, w_free, w_ready, w_axis_ready, w_axis_done, w_axis_auth, w_axis_error;
    reg          w_write_error;
    integer      index;

    function [31:0] f_bytes;
        input [31:0] old_value, new_value;
        input [3:0] strobes;
        integer byte_index;
        begin
            f_bytes = old_value;
            for (byte_index = 0; byte_index < 4; byte_index = byte_index + 1) begin
                if (strobes[byte_index]) begin
                    f_bytes[byte_index * 8 +: 8] = new_value[byte_index * 8 +: 8];
                end
            end
        end
    endfunction

    assign s_axi_awready = i_rst_n && !r_aw_pending && !s_axi_bvalid;
    assign s_axi_wready  = i_rst_n && !r_w_pending && !s_axi_bvalid;
    assign s_axi_arready = i_rst_n && !s_axi_rvalid;
    assign w_commit     = r_aw_pending && r_w_pending && !s_axi_bvalid;
    assign w_write_index = r_awaddr[5:2];
    assign w_command    = f_bytes(32'd0, r_wdata, r_wstrb);
    assign w_free       = !r_busy && !r_start && !r_reset;
    assign w_ready      = w_free && w_axis_ready && !r_error;

    always @(*) begin
        w_write_error = (r_awaddr[1:0] != 0);
        if (|r_wstrb) begin
            case (w_write_index)
                0: begin
                    if (!w_command[3]) begin
                        if ((w_command[0] && !w_ready) ||
                            (r_wstrb[0] && w_command[1] != r_mode && !w_free)) begin
                            w_write_error = 1'b1;
                        end
                    end
                end
                1, 15: w_write_error = 1'b1;
                default: begin
                    if (!w_free) begin
                        w_write_error = 1'b1;
                    end
                end
            endcase
        end
    end

    always @(*) begin
        w_read_data = 32'd0;
        case (s_axi_araddr[5:2])
            0: w_read_data = 32'h41453200 | {30'd0, r_mode, 1'b0};
            1: w_read_data = {24'd0, r_overflow, r_error, 2'd0, r_auth_ok, r_done, r_busy, w_ready};
            15: w_read_data = r_cycles;
            default: w_read_data = r_config[s_axi_araddr[5:2] - 2];
        endcase
    end

    // AW and W can arrive in either order. Responses remain fixed until ready.
    always @(posedge i_clk) begin
        if (!i_rst_n) begin
            r_awaddr     <= 0;
            r_wdata      <= 0;
            r_wstrb      <= 0;
            r_aw_pending <= 1'b0;
            r_w_pending  <= 1'b0;
            s_axi_bresp  <= 0;
            s_axi_bvalid <= 1'b0;
            s_axi_rdata  <= 0;
            s_axi_rresp  <= 0;
            s_axi_rvalid <= 1'b0;
        end
        else begin
            if (s_axi_awvalid && s_axi_awready) begin
                r_awaddr     <= s_axi_awaddr;
                r_aw_pending <= 1'b1;
            end
            if (s_axi_wvalid && s_axi_wready) begin
                r_wdata     <= s_axi_wdata;
                r_wstrb     <= s_axi_wstrb;
                r_w_pending <= 1'b1;
            end
            if (w_commit) begin
                r_aw_pending <= 1'b0;
                r_w_pending  <= 1'b0;
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= w_write_error ? 2'b10 : 2'b00;
            end
            else if (s_axi_bvalid && s_axi_bready) begin
                s_axi_bvalid <= 1'b0;
            end
            if (s_axi_arvalid && s_axi_arready) begin
                s_axi_rdata  <= (s_axi_araddr[1:0] == 0) ? w_read_data : 32'd0;
                s_axi_rresp  <= (s_axi_araddr[1:0] == 0) ? 2'b00 : 2'b10;
                s_axi_rvalid <= 1'b1;
            end
            else if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end

    always @(posedge i_clk) begin
        if (!i_rst_n) begin
            for (index = 0; index < 13; index = index + 1) begin
                r_config[index] <= 0;
            end
            r_mode     <= 1'b0;
            r_start    <= 1'b0;
            r_reset    <= 1'b0;
            r_busy     <= 1'b0;
            r_done     <= 1'b0;
            r_auth_ok  <= 1'b0;
            r_error    <= 1'b0;
            r_overflow <= 1'b0;
            r_counting <= 1'b0;
            r_count    <= 0;
            r_cycles   <= 0;
        end
        else begin
            r_reset <= 1'b0;
            if (r_start && w_axis_ready) begin
                r_start    <= 1'b0;
                r_counting <= 1'b1;
                r_count    <= 0;
            end
            else if (r_counting) begin
                if (w_axis_done) begin
                    // Exclude the registered-DONE observer edge.
                    r_cycles   <= r_count;
                    r_counting <= 1'b0;
                end
                else if (&r_count) begin
                    r_overflow <= 1'b1;
                end
                else begin
                    r_count <= r_count + 1'b1;
                end
            end
            if (w_axis_done && !r_reset) begin
                r_busy    <= 1'b0;
                r_done    <= 1'b1;
                r_auth_ok <= w_axis_auth;
            end
            if (w_commit && !w_write_error && |r_wstrb) begin
                if (w_write_index == 0) begin
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
                        r_start    <= 1'b1;
                        r_busy     <= 1'b1;
                        r_done     <= 1'b0;
                        r_auth_ok  <= 1'b0;
                        r_cycles   <= 0;
                        r_overflow <= 1'b0;
                    end
                    if (w_command[3]) begin
                        r_reset    <= 1'b1;
                        r_start    <= 1'b0;
                        r_busy     <= 1'b0;
                        r_done     <= 1'b0;
                        r_auth_ok  <= 1'b0;
                        r_error    <= 1'b0;
                        r_overflow <= 1'b0;
                        r_counting <= 1'b0;
                        r_count    <= 0;
                        r_cycles   <= 0;
                    end
                end
                else begin
                    r_config[w_write_index - 2] <= f_bytes(r_config[w_write_index - 2], r_wdata, r_wstrb);
                end
            end
            if ((w_axis_error && !r_reset && !(w_commit && !w_write_error &&
                 w_write_index == 0 && w_command[3])) || (w_commit && w_write_error) ||
                (s_axi_arvalid && s_axi_arready && s_axi_araddr[1:0] != 0)) begin
                r_error <= 1'b1;
            end
        end
    end

    aead_axis u_stream (
        .i_clk           (i_clk),
        .i_rst_n         (i_rst_n && !r_reset),

        .i_key           ({r_config[7], r_config[6], r_config[5], r_config[4],
                           r_config[3], r_config[2], r_config[1], r_config[0]}),
        .i_nonce         ({r_config[10], r_config[9], r_config[8]}),
        .i_aad_bytes     (r_config[11]),
        .i_payload_bytes (r_config[12]),
        .i_start         (r_start),
        .i_mode          (r_mode),
        .o_start_ready   (w_axis_ready),
        .o_done          (w_axis_done),
        .o_auth_ok       (w_axis_auth),
        .o_error         (w_axis_error),

        .s_axis_tdata    (s_axis_tdata),
        .s_axis_tkeep    (s_axis_tkeep),
        .s_axis_tvalid   (s_axis_tvalid),
        .s_axis_tlast    (s_axis_tlast),
        .s_axis_tready   (s_axis_tready),

        .m_axis_tdata    (m_axis_tdata),
        .m_axis_tkeep    (m_axis_tkeep),
        .m_axis_tvalid   (m_axis_tvalid),
        .m_axis_tlast    (m_axis_tlast),
        .m_axis_tready   (m_axis_tready)
    );
endmodule

`default_nettype wire

`timescale 1ns / 1ps

// AXI-Lite control and AXI-Stream packets against independent native fixtures.
module tb_aead_axi_dma;
    reg i_clk = 1'b0, i_rst_n = 1'b0;
    reg [31:0] r_wdata = 0;
    reg [ 5:0] r_awaddr = 0, r_araddr = 0;
    reg [ 3:0] r_wstrb = 0;
    reg r_awvalid = 0, r_wvalid = 0, r_bready = 0, r_arvalid = 0, r_rready = 0;
    wire [31:0] w_rdata;
    wire [ 1:0] w_bresp, w_rresp;
    wire w_awready, w_wready, w_bvalid, w_arready, w_rvalid;
    integer r_cycle = 0, r_deadline = 0, r_cases = 0, r_rejected = 0;
    integer r_input_word, r_output_word, r_index, r_order = 0;
    integer r_stream_words, r_sent_words, r_perf_start, r_perf_cycles, r_cc_start, r_cc_requests;
    reg [31:0] s_axis_tdata = 0;
    reg [3:0] s_axis_tkeep = 15;
    reg s_axis_tvalid = 0, s_axis_tlast = 0;
    wire s_axis_tready;
    wire [31:0] m_axis_tdata;
    wire [3:0] m_axis_tkeep;
    wire m_axis_tvalid, m_axis_tlast;
    reg m_axis_tready = 0;
    reg r_active = 0, r_unstalled = 0, r_cc_pending = 0;
    reg [4095:0] r_catalog_path;

    reg [31:0] r_status, r_value, r_meta;
    reg [31:0] r_reference_data, r_reference_control;
    reg r_bheld = 0, r_rheld = 0;
    reg [33:0] r_read_held;
    reg [1:0] r_write_held;

    always #2.5 i_clk = ~i_clk;

    always @(*) begin
        r_reference_data = 0;
        r_reference_control = 0;
        if (r_active && r_output_word < u_case.r_expected_words) begin
            r_reference_data = u_case.r_expected[r_output_word];
            r_reference_control = u_case.r_expected_control[r_output_word];
        end
    end

    aead_case_reader #(.FILE_NAME ("suite_cases.txt")) u_case ();

    aead_axi_dma u_dut (
        .i_clk            (i_clk),
        .i_rst_n          (i_rst_n),

        .s_axi_wdata      (r_wdata),
        .s_axi_awaddr     (r_awaddr),
        .s_axi_wstrb      (r_wstrb),
        .s_axi_awprot     (3'd0),
        .s_axi_awvalid    (r_awvalid),
        .s_axi_awready    (w_awready),
        .s_axi_wvalid     (r_wvalid),
        .s_axi_wready     (w_wready),

        .s_axi_bresp      (w_bresp),
        .s_axi_bvalid     (w_bvalid),
        .s_axi_bready     (r_bready),

        .s_axi_rdata      (w_rdata),
        .s_axi_araddr     (r_araddr),
        .s_axi_arprot     (3'd0),
        .s_axi_rresp      (w_rresp),
        .s_axi_arvalid    (r_arvalid),
        .s_axi_arready    (w_arready),
        .s_axi_rvalid     (w_rvalid),
        .s_axi_rready     (r_rready),

        .s_axis_tdata     (s_axis_tdata),
        .s_axis_tkeep     (s_axis_tkeep),
        .s_axis_tvalid    (s_axis_tvalid),
        .s_axis_tlast     (s_axis_tlast),
        .s_axis_tready    (s_axis_tready),

        .m_axis_tdata     (m_axis_tdata),
        .m_axis_tkeep     (m_axis_tkeep),
        .m_axis_tvalid    (m_axis_tvalid),
        .m_axis_tlast     (m_axis_tlast),
        .m_axis_tready    (m_axis_tready)
    );

    aead_stream_monitor #(.LP_WIDTH (37)) u_input_monitor (
        .i_clk            (i_clk),
        .i_rst_n          (i_rst_n),
        .i_data           ({s_axis_tdata, s_axis_tkeep, s_axis_tlast}),
        .i_valid          (s_axis_tvalid),
        .i_ready          (s_axis_tready)
    );
    aead_stream_monitor #(.LP_WIDTH (37)) u_output_monitor (
        .i_clk            (i_clk),
        .i_rst_n          (i_rst_n),
        .i_data           ({m_axis_tdata, m_axis_tkeep, m_axis_tlast}),
        .i_valid          (m_axis_tvalid),
        .i_ready          (m_axis_tready)
    );

    task f_check;
        input condition;
        input [1023:0] message;
        begin
            if (condition !== 1'b1) begin
                $display("FAIL AXIS DMA adapter case=%0d cycle=%0d: %0s", r_cases, r_cycle, message);
                $finish;
            end
        end
    endtask

    always @(posedge i_clk) begin
        r_cycle = r_cycle + 1;
        if (i_rst_n) begin
            f_check(r_deadline == 0 || r_cycle < r_deadline, "Timeout");
            if (r_bheld) begin
                f_check(w_bvalid && w_bresp === r_write_held, "B response changed under backpressure");
                f_check(!w_awready && !w_wready, "Accepted new write while B outstanding");
            end
            if (r_rheld) begin
                f_check(w_rvalid && {w_rresp, w_rdata} === r_read_held, "R response changed under backpressure");
                f_check(!w_arready, "Accepted new read while R outstanding");
            end
            r_bheld = w_bvalid && !r_bready;
            r_rheld = w_rvalid && !r_rready;
            r_write_held = w_bresp;
            r_read_held = {w_rresp, w_rdata};
        end
        else begin
            r_bheld = 0;
            r_rheld = 0;
        end
    end

    // order: -1 W before AW, +1 AW before W, 0 simultaneous.
    task automatic f_write;
        input [5:0] address;
        input [31:0] data;
        input [3:0] strobes;
        input integer order;
        input [1:0] response;
        begin
            fork
                begin
                    if (order < 0) begin
                        repeat (4) @(negedge i_clk);
                    end
                    @(negedge i_clk);
                    r_awaddr = address;
                    r_awvalid = 1;
                    @(posedge i_clk);
                    while (!w_awready) begin
                        @(posedge i_clk);
                    end
                    @(negedge i_clk);
                    r_awvalid = 0;
                end
                begin
                    if (order > 0) begin
                        repeat (4) @(negedge i_clk);
                    end
                    @(negedge i_clk);
                    r_wdata = data;
                    r_wstrb = strobes;
                    r_wvalid = 1;
                    @(posedge i_clk);
                    while (!w_wready) begin
                        @(posedge i_clk);
                    end
                    @(negedge i_clk);
                    r_wvalid = 0;
                end
            join
            wait (w_bvalid);
            f_check(w_bresp === response, "Unexpected BRESP");
            repeat (3) @(negedge i_clk);
            r_bready = 1;
            @(negedge i_clk);
            r_bready = 0;
        end
    endtask

    task automatic f_read;
        input [5:0] address;
        input integer hold_cycles;
        input [1:0] response;
        output [31:0] data;
        begin
            @(negedge i_clk);
            r_araddr = address;
            r_arvalid = 1;
            @(posedge i_clk);
            while (!w_arready) begin
                @(posedge i_clk);
            end
            @(negedge i_clk);
            r_arvalid = 0;
            wait (w_rvalid);
            data = w_rdata;
            f_check(w_rresp === response, "Unexpected RRESP");
            repeat (hold_cycles) @(negedge i_clk);
            r_rready = 1;
            @(negedge i_clk);
            r_rready = 0;
        end
    endtask

    task f_reset;
        begin
            @(negedge i_clk);
            i_rst_n = 0;
            r_awvalid = 0;
            r_wvalid = 0;
            r_bready = 0;
            r_arvalid = 0;
            r_rready = 0;
            repeat (4) @(negedge i_clk);
            i_rst_n = 1;
            repeat (3) @(negedge i_clk);
        end
    endtask

    always @(negedge i_clk) begin
        m_axis_tready = i_rst_n && (r_unstalled || r_cycle % 7 >= 3);
    end

    always @(posedge i_clk) begin
        if (!i_rst_n) begin
            r_cc_pending = 0;
        end
        else if (r_active) begin
            if (u_dut.r_start && u_dut.w_axis_ready) begin
                r_perf_start = $time;
            end
            if (u_dut.w_axis_done) begin
                r_perf_cycles = ($time - r_perf_start) / 5 - 1;
            end
            if (u_dut.u_stream.u_aead.w_cc_request_valid && u_dut.u_stream.u_aead.w_cc_request_ready) begin
                f_check(!r_cc_pending && u_dut.u_stream.u_aead.w_cc_counter == r_cc_requests,
                        "CC20 counter order/reuse");
                r_cc_pending = 1;
                r_cc_start = r_cycle;
                r_cc_requests = r_cc_requests + 1;
            end
            if (r_cc_pending && u_dut.u_stream.u_aead.w_cc_result_valid) begin
                f_check(r_cycle - r_cc_start == 42, "CC20 41-clock latency");
                r_cc_pending = 0;
            end
            if (m_axis_tvalid && m_axis_tready) begin
                f_check(r_output_word < u_case.r_expected_words, "Extra AXIS output");
                f_check(m_axis_tdata === u_case.r_expected[r_output_word], "AXIS payload/tag/padding mismatch");
                f_check(m_axis_tkeep == 15 && m_axis_tlast == (r_output_word == u_case.r_expected_words - 1),
                        "AXIS KEEP/packet LAST mismatch");
                r_output_word = r_output_word + 1;
            end
        end
    end

    task f_configure;
        begin
            for (r_index = 0; r_index < 8; r_index = r_index + 1) begin
                f_write(8 + 4 * r_index, u_case.r_key[r_index * 32 +: 32], 15, (r_index % 3) - 1, 0);
            end
            for (r_index = 0; r_index < 3; r_index = r_index + 1) begin
                f_write(40 + 4 * r_index, u_case.r_nonce[r_index * 32 +: 32], 15, 0, 0);
            end
            f_write(52, u_case.r_aad_bytes, 15, 0, 0);
            f_write(56, u_case.r_payload_bytes, 15, 0, 0);
        end
    endtask

    task f_feed;
        integer byte_index;
        reg [31:0] control;
        begin
            r_sent_words = 0;
            for (r_input_word = 0; r_input_word < u_case.r_input_words; r_input_word = r_input_word + 1) begin
                control = u_case.r_input_control[r_input_word];
                if (control[2:0] != 0) begin
                    if (!r_unstalled) begin
                        repeat (r_input_word % 3) @(negedge i_clk);
                        @(negedge i_clk);
                    end
                    s_axis_tdata = u_case.r_input[r_input_word];
                    for (byte_index = control[2:0]; byte_index < 4; byte_index = byte_index + 1) begin
                        s_axis_tdata[byte_index * 8 +: 8] = 8'hff;
                    end
                    s_axis_tkeep = 15;
                    s_axis_tlast = (r_sent_words == r_stream_words - 1);
                    s_axis_tvalid = 1;
                    @(posedge i_clk);
                    while (!s_axis_tready) begin
                        @(posedge i_clk);
                    end
                    @(negedge i_clk);
                    s_axis_tvalid = 0;
                    r_sent_words = r_sent_words + 1;
                end
            end
        end
    endtask

    task f_run_case;
        begin
            r_deadline = r_cycle + 10000 + 200 * (u_case.r_aad_bytes + u_case.r_payload_bytes);
            f_configure();
            r_output_word = 0;
            r_cc_requests = 0;
            r_perf_cycles = 0;
            r_active = 1;
            r_stream_words = (u_case.r_aad_bytes + 3) / 4 + (u_case.r_payload_bytes + 3) / 4 +
                             (u_case.r_encrypt ? 0 : 4);
            f_write(0, 1 | (u_case.r_encrypt << 1), 15, 0, 0);
            fork
                f_feed();
                begin
                    r_status = 0;
                    while (!r_status[2]) begin
                        f_read(4, 3, 0, r_status);
                        f_check(!r_status[6] && !r_status[7], "DMA wrapper ERROR/overflow");
                    end
                end
            join
            f_check(r_output_word == u_case.r_expected_words && r_sent_words == r_stream_words,
                    "Incomplete DMA packet");
            f_check(r_status[0] && !r_status[1] && r_status[3] == u_case.r_auth_ok,
                    "DMA DONE/AUTH/READY mismatch");
            f_check(r_cc_requests == 1 + (u_case.r_payload_bytes + 63) / 64, "CC20 request count");
            f_read(60, 12, 0, r_value);
            f_check(r_value == r_perf_cycles && r_value > 0, "Hardware cycle snapshot off by one");
            if (r_unstalled) begin
                $display("PERF AXIS %0s: aad=%0d payload=%0d cycles=%0d Mbit_s=%0.3f",
                         u_case.r_encrypt ? "encrypt" : "decrypt", u_case.r_aad_bytes,
                         u_case.r_payload_bytes, r_value, 1600.0 * u_case.r_payload_bytes / r_value);
            end
            f_read(60, 8, 0, r_meta);
            f_check(r_meta == r_value, "Cycle snapshot not held");
            r_active = 0;
            if (!u_case.r_encrypt && !u_case.r_auth_ok) begin
                r_rejected = r_rejected + 1;
            end
            r_cases = r_cases + 1;
        end
    endtask

    // Malformed DMA input must be rejected, then explicit abort must recover.
    task f_bad_packet;
        input [3:0] keep;
        input last;
        input integer payload_bytes;
        begin
            r_deadline = r_cycle + 10000;
            f_write(52, 0, 15, 0, 0);
            f_write(56, payload_bytes, 15, 0, 0);
            f_write(0, 3, 15, 0, 0);
            @(negedge i_clk);
            s_axis_tdata = 0;
            s_axis_tkeep = keep;
            s_axis_tlast = last;
            s_axis_tvalid = 1;
            @(posedge i_clk);
            while (!s_axis_tready) begin
                @(posedge i_clk);
            end
            @(negedge i_clk);
            s_axis_tvalid = 0;
            f_read(4, 3, 0, r_value);
            f_check(r_value[6], "Malformed packet not rejected");
            f_write(0, 10, 15, 0, 0);
            f_read(4, 3, 0, r_value);
            f_check(r_value == 1, "Soft reset did not recover stream error");
        end
    endtask

    initial begin
        r_deadline = 100000;
        f_reset();
        f_read(0, 8, 0, r_value);
        f_check(r_value == 32'h41453200, "DMA map signature mismatch");
        f_write(8, 32'h11223344, 15, -1, 0);
        f_write(8, 32'haabbccdd, 5, 1, 0);
        f_read(8, 8, 0, r_value);
        f_check(r_value == 32'h11bb33dd, "WSTRB/AW/W ordering");
        f_write(8, 0, 0, 0, 0);
        f_read(8, 3, 0, r_value);
        f_check(r_value == 32'h11bb33dd, "Zero WSTRB changed register");
        f_write(60, 0, 15, 0, 2);
        f_read(5, 4, 2, r_value);
        f_write(0, 16, 15, 0, 0);
        f_read(4, 3, 0, r_value);
        f_check(r_value == 1, "CLEAR_ERROR failed");
        // A session waiting for payload must reject reconfiguration/START.
        f_write(52, 0, 15, 0, 0);
        f_write(56, 4, 15, 0, 0);
        f_write(0, 3, 15, 0, 0);
        f_write(52, 123, 15, 0, 2);
        f_write(0, 18, 15, 0, 0);
        f_write(0, 3, 15, 0, 2);
        f_write(0, 10, 15, 0, 0);
        f_read(4, 3, 0, r_value);
        f_check(r_value == 1, "Busy command abort/recovery");
        f_bad_packet(7, 1, 4);
        f_bad_packet(15, 0, 4);
        f_bad_packet(15, 1, 8);
        u_case.f_open();
        if ($value$plusargs("AXIS_CATALOG=%s", r_catalog_path)) begin
            $fclose(u_case.r_catalog);
            u_case.r_catalog = $fopen(r_catalog_path, "r");
            f_check(u_case.r_catalog != 0, "Cannot open AXIS_CATALOG");
        end
        r_unstalled = $test$plusargs("AEAD_UNSTALLED");
        u_case.f_next();
        while (u_case.r_available) begin
            f_run_case();
            u_case.f_next();
        end
        f_check(r_cases > 0, "Empty AXIS catalog");
        $display("PASS AXIS DMA adapter: %0d cases, %0d auth rejects; padded packets, stalls, counters, framing errors, reset recovery, AXI-Lite ordering/WSTRB/responses", r_cases, r_rejected);
        $fclose(u_case.r_catalog);
        $finish;
    end
endmodule

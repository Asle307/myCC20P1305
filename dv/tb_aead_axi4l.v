`timescale 1ns / 1ps

// AXI-Lite bus tests plus the complete generated suite through the MMIO adapter.
module tb_aead_axi4l;
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
    integer r_reset_word;
    reg [31:0] r_status, r_value, r_meta;
    reg [31:0] r_reference_data, r_reference_control;
    reg r_bheld = 0, r_rheld = 0;
    reg [33:0] r_read_held;
    reg [1:0] r_write_held;

    always #2.5 i_clk = ~i_clk;

    aead_case_reader #(.FILE_NAME ("suite_cases.txt")) u_case ();

    aead_axi4l u_dut (
        .s00_axi_aclk    (i_clk),
        .s00_axi_aresetn (i_rst_n),

        .s00_axi_wdata   (r_wdata),
        .s00_axi_awaddr  (r_awaddr),
        .s00_axi_wstrb   (r_wstrb),
        .s00_axi_awprot  (3'd0),
        .s00_axi_awvalid (r_awvalid),
        .s00_axi_awready (w_awready),
        .s00_axi_wvalid  (r_wvalid),
        .s00_axi_wready  (w_wready),
        .s00_axi_bresp   (w_bresp),
        .s00_axi_bvalid  (w_bvalid),
        .s00_axi_bready  (r_bready),

        .s00_axi_rdata   (w_rdata),
        .s00_axi_araddr  (r_araddr),
        .s00_axi_arprot  (3'd0),
        .s00_axi_arvalid (r_arvalid),
        .s00_axi_arready (w_arready),
        .s00_axi_rresp   (w_rresp),
        .s00_axi_rvalid  (w_rvalid),
        .s00_axi_rready  (r_rready)
    );

    task f_check;
        input condition;
        input [1023:0] message;
        begin
            if (condition !== 1'b1) begin
                $display("FAIL AXI-Lite case=%0d cycle=%0d: %0s", r_cases, r_cycle, message);
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

    task f_run_case;
        begin
            r_deadline = r_cycle + 10000 + 200 * (u_case.r_aad_bytes + u_case.r_payload_bytes);
            for (r_index = 0; r_index < 8; r_index = r_index + 1) begin
                f_write(8 + 4 * r_index, u_case.r_key[r_index * 32 +: 32], 4'hf, (r_index % 3) - 1, 0);
            end
            for (r_index = 0; r_index < 3; r_index = r_index + 1) begin
                f_write(40 + 4 * r_index, u_case.r_nonce[r_index * 32 +: 32], 4'hf, 0, 0);
            end
            f_write(0, 1 | (u_case.r_encrypt << 1), 4'hf, 0, 0);
            r_input_word = 0;
            r_output_word = 0;
            r_status = 0;
            while (!r_status[2] || r_output_word < u_case.r_expected_words) begin
                f_read(4, 2, 0, r_status);
                f_check(!r_status[6], "Unexpected sticky error during AEAD");
                if (r_status[5]) begin
                    f_read(56, 3, 0, r_meta);
                    f_read(60, 3, 0, r_value);
                    f_check(r_output_word < u_case.r_expected_words, "Extra output word");
                    r_reference_data = u_case.r_expected[r_output_word];
                    r_reference_control = u_case.r_expected_control[r_output_word];
                    f_check(r_value === r_reference_data, "Payload/tag mismatch");
                    f_check(r_meta === (32'h10000 | r_reference_control), "Output metadata mismatch");
                    f_write(56, 32'h20000, 4'hf, 0, 0);
                    r_output_word = r_output_word + 1;
                end
                if (r_status[4] && r_input_word < u_case.r_input_words) begin
                    f_write(52, u_case.r_input[r_input_word], 4'hf, 0, 0);
                    f_write(56, 32'h10000 | u_case.r_input_control[r_input_word], 4'hf,
                            (r_input_word % 3) - 1, 0);
                    r_input_word = r_input_word + 1;
                end
            end
            f_read(4, 10, 0, r_status);
            f_check(!r_status[1] && r_status[2] && r_status[3] == u_case.r_auth_ok &&
                    r_status[0] && !r_status[5] && r_input_word == u_case.r_input_words,
                    "DONE/auth/ready or word accounting mismatch");
            if (!u_case.r_encrypt && !u_case.r_auth_ok) begin
                r_rejected = r_rejected + 1;
            end
            r_cases = r_cases + 1;
        end
    endtask

    initial begin
        r_deadline = 100000;
        f_reset();
        // WSTRB, AW/W ordering, read-only and unaligned access behavior.
        f_write(8, 32'h11223344, 4'hf, -1, 0);
        f_write(8, 32'haabbccdd, 4'b0101, 1, 0);
        f_read(8, 8, 0, r_value);
        f_check(r_value == 32'h11bb33dd, "WSTRB byte merge failed");
        f_write(8, 0, 0, 0, 0);
        f_read(8, 3, 0, r_value);
        f_check(r_value == 32'h11bb33dd, "Zero WSTRB changed register");
        f_write(4, 32'hffffffff, 4'hf, 0, 2);
        f_write(9, 0, 4'hf, 0, 2);
        f_read(5, 4, 2, r_value);
        f_write(56, 32'h20000, 4'hf, 0, 2);
        f_write(56, 32'h10304, 4'hf, 0, 2);
        f_write(0, 16, 4'hf, 0, 0);
        f_read(4, 4, 0, r_value);
        f_check(!r_value[6], "CLEAR_ERROR failed");

        // A status read must remain fixed while writes start and abort a core session.
        fork
            begin
                f_read(4, 100, 0, r_value);
            end
            begin
                repeat (5) @(negedge i_clk);
                f_write(0, 3, 4'hf, 0, 0);
                f_write(0, 3, 4'hf, 0, 2);
                f_write(8, 0, 4'hf, 0, 2);
                // Queue an incorrect section while core expects AAD: slot must hold.
                f_write(52, 32'h01234567, 4'hf, 0, 0);
                f_write(56, 32'h10204, 4'hf, 0, 0);
                f_write(56, 32'h10204, 4'hf, 0, 2);
                f_write(0, 10, 4'hf, 0, 0);
            end
        join
        f_read(4, 5, 0, r_value);
        f_check(r_value == 1, "SOFT_RESET did not clear session slots/status");
        f_read(8, 3, 0, r_value);
        f_check(r_value == 32'h11bb33dd, "SOFT_RESET lost key configuration");

        // External reset must discard an AW-only transaction without committing it.
        @(negedge i_clk);
        r_awaddr = 8;
        r_awvalid = 1;
        @(posedge i_clk);
        f_check(w_awready, "AW-only setup failed");
        @(negedge i_clk);
        r_awvalid = 0;
        f_reset();
        f_write(8, 32'h55aa55aa, 4'hf, -1, 0);
        f_read(8, 3, 0, r_value);
        f_check(r_value == 32'h55aa55aa, "Reset left stale AW pending");

        u_case.f_open();
        u_case.f_next();
        // Abort with a full output slot, then verify a complete replay below.
        for (r_index = 0; r_index < 8; r_index = r_index + 1) begin
            f_write(8 + 4 * r_index, u_case.r_key[r_index * 32 +: 32], 4'hf, 0, 0);
        end
        for (r_index = 0; r_index < 3; r_index = r_index + 1) begin
            f_write(40 + 4 * r_index, u_case.r_nonce[r_index * 32 +: 32], 4'hf, 0, 0);
        end
        f_write(0, 3, 4'hf, 0, 0);
        for (r_reset_word = 0; r_reset_word < (u_case.r_aad_bytes + 3) / 4 + 4;
             r_reset_word = r_reset_word + 1) begin
            f_read(4, 2, 0, r_status);
            while (!r_status[4]) begin
                f_read(4, 2, 0, r_status);
            end
            f_write(52, u_case.r_input[r_reset_word], 4'hf, 0, 0);
            f_write(56, 32'h10000 | u_case.r_input_control[r_reset_word], 4'hf, 0, 0);
        end
        f_read(4, 2, 0, r_status);
        while (!r_status[5]) begin
            f_read(4, 2, 0, r_status);
        end
        f_read(60, 40, 0, r_value);
        f_read(60, 40, 0, r_meta);
        f_check(r_value === r_meta, "Output slot changed before POP");
        f_write(0, 10, 4'hf, 0, 0);
        f_read(4, 5, 0, r_status);
        f_check(r_status == 1, "Abort did not clear full output slot");
        while (u_case.r_available) begin
            f_run_case();
            u_case.f_next();
        end
        f_check(r_cases > 0 && r_rejected > 0, "No AEAD/authentication-failure coverage");
        $display("PASS AXI-Lite: %0d AEAD cases, %0d auth rejects; AW/W order, WSTRB, B/R stalls, slots, resets, errors", r_cases, r_rejected);
        $fclose(u_case.r_catalog);
        $finish;
    end
endmodule

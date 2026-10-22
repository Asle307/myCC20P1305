`timescale 1ns / 1ps

// Native word-stream driver and scoreboard. Runs one catalog case at a time.
module aead_runner #(
    parameter FILE_NAME = "suite_cases.txt",
    parameter LP_REGRESSION = 1
);
    reg          i_clk = 1'b0;
    reg          i_rst_n = 1'b0;

    reg  [255:0] i_key = 256'd0;
    reg  [ 95:0] i_nonce = 96'd0;
    reg          i_start = 1'b0;
    reg          i_mode = 1'b0;
    wire         o_start_ready;

    reg  [31:0]  i_data = 32'd0;
    reg  [ 2:0]  i_data_byte_count = 3'd0;
    reg          i_is_aad = 1'b0;
    reg          i_data_valid = 1'b0;
    wire         o_data_ready;
    reg          i_data_last = 1'b0;

    wire [31:0]  o_data;
    wire [ 2:0]  o_data_byte_count;
    wire         o_is_tag, o_data_last, o_data_valid;
    reg          i_data_ready = 1'b0;

    wire         o_done, o_auth_ok;

    reg [63:0] r_cycle = 0, r_deadline = 0, r_cc_cycle = 0;
    reg        r_active = 1'b0, r_done_seen = 1'b0, r_previous_done = 1'b0;
    reg        r_unstalled = 1'b0, r_cc_pending = 1'b0;
    reg        r_force_unstalled = 1'b0;
    reg [63:0] r_start_cycle, r_session_cycles;
    reg [63:0] r_input_starved, r_output_stalled;
    reg        r_session_started, r_session_finished;
    real       r_start_ns, r_session_ns;
    integer r_case = 0, r_cases = 0, r_completed = 0, r_rejected = 0, r_resets = 0;
    integer r_output_word, r_input_word, r_cc_requests;
    integer r_aad_received, r_payload_received, r_tag_received;
    integer r_phase = -1, r_phase_index;
    integer r_stream_fd = 0, r_payload_fd = 0, r_tag_fd = 0;
    reg [31:0] w_reference_data, w_reference_control;
    wire w_sample_valid = r_active && o_data_valid && r_output_word < u_case.r_expected_words;
    wire w_sample_match = w_sample_valid && o_data === w_reference_data;
    wire [127:0] w_reference_tag = u_case.r_tag;
    wire w_expected_auth = u_case.r_auth_ok;
    reg [159:0] w_phase_name;

    aead_case_reader #(.FILE_NAME (FILE_NAME)) u_case ();

    always #2.5 i_clk = ~i_clk;

    always @(*) begin
        w_reference_data = 32'd0;
        w_reference_control = 32'd0;
        if (r_active && r_output_word < u_case.r_expected_words) begin
            w_reference_data = u_case.r_expected[r_output_word];
            w_reference_control = u_case.r_expected_control[r_output_word];
        end
        w_phase_name = "RESET";
        if (i_rst_n) begin
            case (u_dut.r_state)
                0: w_phase_name = "IDLE";
                1, 2: w_phase_name = "OTK";
                3: w_phase_name = "AAD";
                4: w_phase_name = "PAYLOAD";
                5, 6: w_phase_name = "KEYSTREAM";
                7, 8: w_phase_name = "XOR_MAC";
                9: w_phase_name = "PAYLOAD_OUTPUT";
                10: w_phase_name = "LENGTH";
                11: w_phase_name = "TAG_OUTPUT";
                12: w_phase_name = "TAG_INPUT";
                default: w_phase_name = "UNKNOWN";
            endcase
        end
    end

    aead_cc20_p1305 u_dut (
        .i_clk             (i_clk),
        .i_rst_n           (i_rst_n),

        .i_key             (i_key),
        .i_nonce           (i_nonce),
        .i_start           (i_start),
        .i_mode            (i_mode),
        .o_start_ready     (o_start_ready),

        .i_data            (i_data),
        .i_data_byte_count (i_data_byte_count),
        .i_is_aad          (i_is_aad),
        .i_data_valid      (i_data_valid),
        .o_data_ready      (o_data_ready),
        .i_data_last       (i_data_last),

        .o_data            (o_data),
        .o_data_byte_count (o_data_byte_count),
        .o_is_tag          (o_is_tag),
        .o_data_last       (o_data_last),
        .o_data_valid      (o_data_valid),
        .i_data_ready      (i_data_ready),

        .o_done            (o_done),
        .o_auth_ok         (o_auth_ok)
    );

    aead_stream_monitor #(.LP_WIDTH (37)) u_input_monitor (
        .i_clk   (i_clk),
        .i_rst_n (i_rst_n),

        .i_data  ({i_data, i_data_byte_count, i_is_aad, i_data_last}),
        .i_valid (i_data_valid),
        .i_ready (o_data_ready)
    );
    aead_stream_monitor #(.LP_WIDTH (37)) u_output_monitor (
        .i_clk   (i_clk),
        .i_rst_n (i_rst_n),

        .i_data  ({o_data, o_data_byte_count, o_is_tag, o_data_last}),
        .i_valid (o_data_valid),
        .i_ready (i_data_ready)
    );
    aead_stream_monitor #(.LP_WIDTH (353)) u_configuration_monitor (
        .i_clk   (i_clk),
        .i_rst_n (i_rst_n),

        .i_data  ({i_key, i_nonce, i_mode}),
        .i_valid (i_start),
        .i_ready (o_start_ready)
    );

    task f_check;
        input condition;
        input [1023:0] message;
        begin
            if (condition !== 1'b1) begin
                $display("FAIL case=%0d cycle=%0d %0s: %0s", r_case, r_cycle, u_case.r_directory, message);
                $finish;
            end
        end
    endtask

    always @(negedge i_clk) begin
        i_data_ready = i_rst_n && (r_unstalled || r_cycle % 7 >= 3) &&
                       !(r_phase == 1 && !o_is_tag) && !(r_phase == 2 && o_is_tag);
    end

    always @(posedge i_clk) begin
        r_cycle = r_cycle + 1;
        if (!i_rst_n) begin
            r_previous_done = 1'b0;
            r_cc_pending = 1'b0;
        end
        else begin
            f_check(!(o_done && r_previous_done), "DONE lasts more than one clock");
            r_previous_done = o_done;
            f_check(r_active || !o_data_valid, "Output outside a session");
            f_check(r_deadline == 0 || r_cycle < r_deadline, "Session timeout");
            if (r_active) begin
                if (i_start && o_start_ready) begin
                    f_check(!r_session_started, "Repeated session START");
                    r_session_started = 1'b1;
                    r_start_cycle = r_cycle;
                    r_start_ns = $realtime;
                end
                if (r_session_started && !r_session_finished) begin
                    if (o_data_ready && !i_data_valid) begin
                        r_input_starved = r_input_starved + 1;
                    end
                    if (o_data_valid && !i_data_ready) begin
                        r_output_stalled = r_output_stalled + 1;
                    end
                    // w_finish is sampled on the edge that registers DONE.
                    // Observing o_done below takes one more rising edge;
                    // exclude that observer cycle from the measured latency.
                    if (u_dut.w_finish) begin
                        r_session_finished = 1'b1;
                        r_session_cycles = r_cycle - r_start_cycle;
                        r_session_ns = $realtime - r_start_ns;
                    end
                end
                if (u_dut.w_cc_request_valid && u_dut.w_cc_request_ready) begin
                    f_check(u_dut.w_cc_counter == r_cc_requests && !r_cc_pending, "CC20 counter order/reuse");
                    r_cc_requests = r_cc_requests + 1;
                    r_cc_cycle = r_cycle;
                    r_cc_pending = 1'b1;
                end
                if (r_cc_pending && u_dut.w_cc_result_valid) begin
                    // Sampled before NBA: result rises after clock 41, visible on clock 42.
                    f_check(r_cycle - r_cc_cycle == 42, "CC20 41-clock result-valid latency");
                    r_cc_pending = 1'b0;
                end
                if (i_data_valid && o_data_ready) begin
                    if (i_is_aad) begin
                        r_aad_received = r_aad_received + i_data_byte_count;
                    end
                    else if (r_input_word >= u_case.r_input_words - 4 && !u_case.r_encrypt) begin
                        r_tag_received = r_tag_received + 1;
                    end
                    else begin
                        r_payload_received = r_payload_received + i_data_byte_count;
                    end
                end
                if (o_data_valid && i_data_ready) begin
                    f_check(w_sample_valid && w_sample_match, "Output data/order/padding mismatch");
                    f_check({o_data_last, o_is_tag, o_data_byte_count} ==
                            {w_reference_control[9], w_reference_control[8], w_reference_control[2:0]},
                            "Output count/type/LAST mismatch");
                    if (r_stream_fd != 0) begin
                        $fdisplay(r_stream_fd, "%08x", o_data);
                        if (o_is_tag) begin
                            $fdisplay(r_tag_fd, "%08x", o_data);
                        end
                        else begin
                            $fdisplay(r_payload_fd, "%08x", o_data);
                        end
                    end
                    r_output_word = r_output_word + 1;
                end
                if (o_done) begin
                    f_check(!r_done_seen && o_start_ready, "Repeated DONE or START not ready");
                    f_check(r_session_finished && r_session_cycles > 0 &&
                            r_session_ns == 5.0 * r_session_cycles, "Session cycle/time accounting mismatch");
                    f_check(r_output_word == u_case.r_expected_words &&
                            r_aad_received == u_case.r_aad_bytes &&
                            r_payload_received == u_case.r_payload_bytes &&
                            r_tag_received == (u_case.r_encrypt ? 0 : 4), "Incomplete session at DONE");
                    f_check(r_cc_requests == 1 + (u_case.r_payload_bytes + 63) / 64, "CC20 request count");
                    f_check(o_auth_ok === u_case.r_auth_ok, "AUTH_OK mismatch");
                    if (r_unstalled) begin
                        f_check(r_input_starved == 0 && r_output_stalled == 0,
                                "Unstalled measurement contains external stalls");
                        $display("PERF AEAD %0s: aad_bytes=%0d payload_bytes=%0d cycles=%0d latency_ns=%0.1f payload_Mbit_s=%0.3f input_starved=%0d output_stalled=%0d",
                                 u_case.r_encrypt ? "encrypt" : "decrypt", u_case.r_aad_bytes,
                                 u_case.r_payload_bytes, r_session_cycles, r_session_ns,
                                 8000.0 * u_case.r_payload_bytes / r_session_ns,
                                 r_input_starved, r_output_stalled);
                    end
                    r_done_seen = 1'b1;
                end
            end
        end
    end

    task automatic f_feed;
        input integer limit;
        reg [31:0] control;
        integer byte_index;
        begin
            for (r_input_word = 0; r_input_word < limit; r_input_word = r_input_word + 1) begin
                if (!r_unstalled) begin
                    repeat (r_input_word % 3) @(negedge i_clk);
                    @(negedge i_clk);
                end
                control = u_case.r_input_control[r_input_word];
                i_data = u_case.r_input[r_input_word];
                i_data_byte_count = control[2:0];
                i_is_aad = control[8];
                i_data_last = control[9];
                // Non-data byte lanes must be ignored by the DUT.
                for (byte_index = control[2:0]; byte_index < 4; byte_index = byte_index + 1) begin
                    i_data[byte_index * 8 +: 8] = 8'hff;
                end
                i_data_valid = 1'b1;
                @(posedge i_clk);
                while (!o_data_ready) begin
                    @(posedge i_clk);
                end
                @(negedge i_clk);
                i_data_valid = 1'b0;
            end
        end
    endtask

    task automatic f_run;
        input integer phase;
        input unstalled;
        reg [4095:0] filename;
        integer limit, status_fd;
        begin
            @(negedge i_clk);
            r_phase = phase;
            r_unstalled = unstalled;
            r_output_word = 0;
            r_input_word = 0;
            r_cc_requests = 0;
            r_aad_received = 0;
            r_payload_received = 0;
            r_tag_received = 0;
            r_done_seen = 1'b0;
            r_session_started = 1'b0;
            r_session_finished = 1'b0;
            r_start_cycle = 0;
            r_session_cycles = 0;
            r_input_starved = 0;
            r_output_stalled = 0;
            r_start_ns = 0.0;
            r_session_ns = 0.0;
            r_active = 1'b1;
            r_deadline = r_cycle + 3000 + 80 * (u_case.r_aad_bytes + u_case.r_payload_bytes);
            if (phase < 0) begin
                $sformat(filename, "%0s/actual_stream.hex", u_case.r_directory);
                r_stream_fd = $fopen(filename, "w");
                $sformat(filename, "%0s/actual_output.hex", u_case.r_directory);
                r_payload_fd = $fopen(filename, "w");
                $sformat(filename, "%0s/actual_tag_words.hex", u_case.r_directory);
                r_tag_fd = $fopen(filename, "w");
                f_check(r_stream_fd != 0 && r_payload_fd != 0 && r_tag_fd != 0, "Cannot open DUT capture files");
            end
            i_key = u_case.r_key;
            i_nonce = u_case.r_nonce;
            i_mode = u_case.r_encrypt;
            i_start = 1'b1;
            @(posedge i_clk);
            while (!o_start_ready) begin
                @(posedge i_clk);
            end
            @(negedge i_clk);
            i_start = 1'b0;
            i_key = ~i_key;
            i_nonce = ~i_nonce;
            i_mode = !i_mode;
            f_check(!o_auth_ok, "START did not clear authentication status");
            limit = u_case.r_input_words;
            if (phase == 3) begin
                limit = ((u_case.r_aad_bytes == 0) ? 1 : (u_case.r_aad_bytes + 3) / 4) + 1;
            end
            else if (phase == 4) begin
                limit = limit - 2;
            end
            fork : session_threads
                begin
                    f_feed(limit);
                end
                begin
                    if (phase < 0) begin
                        wait (r_done_seen);
                    end
                    else begin
                        case (phase)
                            0: wait (u_dut.u_cc20_core.r_round_index == 2);
                            1: wait (o_data_valid && !o_is_tag);
                            2: wait (o_data_valid && o_is_tag);
                            3: wait (r_payload_received == 4 && u_dut.r_input_byte_count == 4);
                            4: wait (r_tag_received == 2);
                        endcase
                        repeat (3) @(negedge i_clk);
                        i_rst_n = 1'b0;
                        i_data_valid = 1'b0;
                        repeat (3) @(negedge i_clk);
                        f_check(!o_data_valid && !o_done && !o_auth_ok, "Reset did not abort/clear session");
                        i_rst_n = 1'b1;
                        r_resets = r_resets + 1;
                        disable session_threads;
                    end
                end
            join
            @(negedge i_clk);
            r_active = 1'b0;
            r_deadline = 0;
            if (phase < 0) begin
                $fclose(r_stream_fd);
                $fclose(r_payload_fd);
                $fclose(r_tag_fd);
                r_stream_fd = 0;
                r_payload_fd = 0;
                r_tag_fd = 0;
                $sformat(filename, "%0s/actual_status.hex", u_case.r_directory);
                status_fd = $fopen(filename, "w");
                f_check(status_fd != 0, "Cannot open status capture");
                $fdisplay(status_fd, "%08x", o_auth_ok);
                $fclose(status_fd);
                r_completed = r_completed + 1;
                if (!u_case.r_encrypt && !u_case.r_auth_ok) begin
                    r_rejected = r_rejected + 1;
                end
                repeat (3) begin
                    @(negedge i_clk);
                    f_check(!o_done && o_start_ready && o_auth_ok === u_case.r_auth_ok, "Completion status not held");
                end
            end
            r_phase = -1;
            repeat (2) @(negedge i_clk);
        end
    endtask

    initial begin
        r_force_unstalled = $test$plusargs("AEAD_UNSTALLED");
        u_case.f_open();
        repeat (4) @(negedge i_clk);
        i_rst_n = 1'b1;
        u_case.f_next();
        while (u_case.r_available) begin
            r_case = r_cases;
            f_run(-1, r_force_unstalled);
            r_cases = r_cases + 1;
            u_case.f_next();
        end
        f_check(r_cases > 0, "Empty catalog");
        if (LP_REGRESSION) begin
            u_case.f_rewind();
            u_case.f_next();
            while (u_case.r_available && (!u_case.r_encrypt || u_case.r_payload_bytes < 16)) begin
                u_case.f_next();
            end
            f_check(u_case.r_available, "Regression requires nonempty encryption for reset tests");
            for (r_phase_index = 0; r_phase_index < 4; r_phase_index = r_phase_index + 1) begin
                f_run(r_phase_index, 1'b0);
                f_run(-1, 1'b0);
            end
            f_run(-1, 1'b1);
            u_case.f_rewind();
            u_case.f_next();
            while (u_case.r_available && u_case.r_encrypt) begin
                u_case.f_next();
            end
            f_check(u_case.r_available, "Regression requires decryption for reset tests");
            f_run(4, 1'b0);
            f_run(-1, 1'b0);
            f_run(-1, 1'b1);
        end
        f_check(r_completed == r_cases + (LP_REGRESSION ? 7 : 0) &&
                r_resets == (LP_REGRESSION ? 5 : 0), "Operation/reset accounting mismatch");
        $display("PASS AEAD %0s: %0d cases, %0d completed, %0d auth rejects, %0d reset recoveries",
                 FILE_NAME, r_cases, r_completed, r_rejected, r_resets);
        $fclose(u_case.r_catalog);
        $finish;
    end
endmodule

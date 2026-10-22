`timescale 1ns / 1ps

// One case in memory at a time. The catalog contains absolute directory paths.
module aead_case_reader #(
    parameter FILE_NAME = "suite_cases.txt",
    parameter integer LP_MAX_WORDS = 32774
);
    reg [255:0] r_key;
    reg [127:0] r_tag;
    reg [ 95:0] r_nonce;
    reg [ 31:0] r_aad_bytes, r_payload_bytes;
    reg         r_encrypt, r_auth_ok, r_available;
    reg [2047:0] r_directory;
    reg [31:0] r_input [0:LP_MAX_WORDS-1];
    reg [31:0] r_input_control [0:LP_MAX_WORDS-1];
    reg [31:0] r_expected [0:LP_MAX_WORDS-1];
    reg [31:0] r_expected_control [0:LP_MAX_WORDS-1];
    integer r_input_words, r_expected_words;
    integer r_catalog;

    task f_fail;
        input [1023:0] message;
        begin
            $display("FAIL fixture %0s / %0s: %0s", FILE_NAME, r_directory, message);
            $finish;
        end
    endtask

    function integer f_nibble;
        input integer character;
        begin
            if (character >= 48 && character <= 57) begin
                f_nibble = character - 48;
            end
            else if (character >= 65 && character <= 70) begin
                f_nibble = character - 55;
            end
            else if (character >= 97 && character <= 102) begin
                f_nibble = character - 87;
            end
            else begin
                f_nibble = -1;
            end
        end
    endfunction

    task f_word;
        input integer fd;
        input integer digits;
        output [255:0] value;
        integer character, count, nibble;
        begin
            value = 256'd0;
            count = 0;
            character = $fgetc(fd);
            while (character != -1 && character != 10 && character != 13) begin
                nibble = f_nibble(character);
                if (nibble < 0 || count >= digits) begin
                    f_fail("Invalid hex, X/Z, or overwide word");
                end
                value = (value << 4) | nibble[3:0];
                count = count + 1;
                character = $fgetc(fd);
            end
            if (count != digits) begin
                f_fail("Missing or short hexadecimal word");
            end
            if (character == 13) begin
                character = $fgetc(fd);
                if (character != 10 && character != -1) begin
                    f_fail("Invalid line ending");
                end
            end
        end
    endtask

    task f_end;
        input integer fd;
        integer character;
        begin
            character = $fgetc(fd);
            while (character == 10 || character == 13) begin
                character = $fgetc(fd);
            end
            if (character != -1) begin
                f_fail("Extra fixture data");
            end
            $fclose(fd);
        end
    endtask

    task f_open;
        begin
            r_available = 1'b0;
            r_directory = 0;
            r_catalog = $fopen(FILE_NAME, "r");
            if (r_catalog == 0) begin
                f_fail("Cannot open catalog; run script/gen_vectors.py generate");
            end
        end
    endtask

    task f_rewind;
        integer status;
        begin
            status = $fseek(r_catalog, 0, 0);
            if (status != 0) begin
                f_fail("Cannot rewind catalog");
            end
        end
    endtask

    task f_next;
        reg [4095:0] filename;
        reg [255:0] value;
        integer character, count, fd, section, index, words;
        begin
            r_directory = 0;
            count = 0;
            character = $fgetc(r_catalog);
            while (character == 10 || character == 13) begin
                character = $fgetc(r_catalog);
            end
            while (character != -1 && character != 10 && character != 13) begin
                if (count >= 255 || character < 32 || character > 126) begin
                    f_fail("Invalid or overlong directory path");
                end
                r_directory = (r_directory << 8) | character[7:0];
                count = count + 1;
                character = $fgetc(r_catalog);
            end
            r_available = count != 0;
            if (r_available) begin
                $sformat(filename, "%0s/rtl_config.hex", r_directory);
                fd = $fopen(filename, "r");
                if (fd == 0) begin
                    f_fail("Missing rtl_config.hex");
                end
                f_word(fd, 64, value); r_key = value;
                f_word(fd, 24, value); r_nonce = value;
                f_word(fd, 32, value); r_tag = value;
                f_word(fd, 8, value);
                if (value > 1) begin
                    f_fail("Invalid encrypt flag");
                end
                r_encrypt = value[0];
                f_word(fd, 8, value);
                if (value > 1 || (r_encrypt && value != 0)) begin
                    f_fail("Invalid authentication flag");
                end
                r_auth_ok = value[0];
                f_word(fd, 8, value); r_aad_bytes = value;
                f_word(fd, 8, value); r_payload_bytes = value;
                f_word(fd, 8, value);
                if (value < 2 || value > LP_MAX_WORDS) begin
                    f_fail("Invalid input word count");
                end
                r_input_words = value;
                f_word(fd, 8, value);
                if (value > LP_MAX_WORDS) begin
                    f_fail("Invalid output word count");
                end
                r_expected_words = value;
                f_end(fd);
                if (r_aad_bytes > 65536 || r_payload_bytes > 65536 ||
                    r_input_words != ((r_aad_bytes == 0) ? 1 : (r_aad_bytes + 3) / 4) +
                                     ((r_payload_bytes == 0) ? 1 : (r_payload_bytes + 3) / 4) +
                                     (r_encrypt ? 0 : 4) ||
                    r_expected_words != (r_payload_bytes + 3) / 4 + (r_encrypt ? 4 : 0)) begin
                    f_fail("Length/beat count mismatch");
                end
                for (section = 0; section < 4; section = section + 1) begin
                    case (section)
                        0: $sformat(filename, "%0s/rtl_input.hex", r_directory);
                        1: $sformat(filename, "%0s/rtl_input_control.hex", r_directory);
                        2: $sformat(filename, "%0s/rtl_expected.hex", r_directory);
                        3: $sformat(filename, "%0s/rtl_expected_control.hex", r_directory);
                    endcase
                    fd = $fopen(filename, "r");
                    if (fd == 0) begin
                        f_fail("Missing adapter word file");
                    end
                    words = (section < 2) ? r_input_words : r_expected_words;
                    for (index = 0; index < words; index = index + 1) begin
                        f_word(fd, 8, value);
                        case (section)
                            0: r_input[index] = value;
                            1: r_input_control[index] = value;
                            2: r_expected[index] = value;
                            3: r_expected_control[index] = value;
                        endcase
                        if (section == 1 || section == 3) begin
                            if ((value & 32'hfffffcf8) != 0 || value[2:0] > 4 ||
                                (!value[9] && value[2:0] != 4) ||
                                (section == 3 && value[2:0] == 0)) begin
                                f_fail("Invalid adapter control");
                            end
                        end
                    end
                    f_end(fd);
                end
            end
        end
    endtask
endmodule

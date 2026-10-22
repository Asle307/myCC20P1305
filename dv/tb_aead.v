`timescale 1ns / 1ps

// Full RFC/boundary regression, including all five reset/recovery phases.
module tb_aead;

    aead_runner #(
        .FILE_NAME     ("suite_cases.txt"),
        .LP_REGRESSION (1)
    ) u_test ();

endmodule

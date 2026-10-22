`timescale 1ns / 1ps

// Only the cases generated from the user's random settings or scenario recipe.
module tb_aead_custom;

    aead_runner #(
        .FILE_NAME     ("custom_cases.txt"),
        .LP_REGRESSION (0)
    ) u_test ();

endmodule

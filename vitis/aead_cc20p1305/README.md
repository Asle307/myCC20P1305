# KV260 standalone JTAG self-test

`src/main.c` runs on `psu_cortexa53_0` with the `aead_kv260` standalone
platform. It uses the BSP's `XPAR_AEAD_AXI4L_0_BASEADDR` (currently
`0xA0000000`) and the [AXI-Lite register contract](../../docs/interfaces/aead_axi4l.md).
No Linux, UIO, DMA or external crypto library is needed.

The application checks RFC 8439 section 2.8.2 encryption, authenticated
decryption, and rejection of a tag with one flipped bit. It compares every
payload/tag word, byte count, LAST, tag marker and final authentication status.
Input feeding and output draining are interleaved. Tentative decrypt plaintext
is compared privately and never printed or returned as application data.
The public RFC key/nonce is for this self-test only.

## Build and debug in Vitis 2025.2

1. Open the existing `vitis/` workspace and refresh the component if needed.
2. Build `aead_kv260`, then build `aead_cc20p1305`. The application's CMake
   configuration links `xilstandalone`, `xiltimer` and `xil`, with `-O0 -g3`
   for source debugging. Its linker configuration was generated from this XSA.
3. Power the board, connect USB JTAG, and select the existing hardware debug
   launch `aead_cc20p1305_app_hw_1`. Verify the matching FPGA bitstream,
   `psu_init.tcl`, and `build/aead_cc20p1305.elf` paths.
4. Use system reset, FPGA programming, and **PSU Init** with PL power-up.
   The supplied launch selects PSU initialization because this platform was
   created without boot components and has no `sw/boot/fsbl.elf`.
   PS/DDR, the PS-to-PL AXI path, PL clock and peripheral reset must be ready
   before execution reaches the first MMIO access.
5. Set a breakpoint at `main`, start hardware debug, then resume execution.

The current BSP selects **`psu_coresight_0` for stdin/stdout**. View output in
the debugger's JTAG/DCC console. A UART terminal will not receive these logs.
To use UART instead, first enable and route the appropriate PS UART in the
hardware design, then regenerate the platform and select that UART as BSP
stdout.

Expected output includes:

```text
PASS RFC encrypt: 114 payload bytes, AUTH_OK=0
PASS RFC decrypt: 114 payload bytes, AUTH_OK=1
PASS RFC decrypt bad tag (rejected): 114 payload bytes, AUTH_OK=0
AEAD SELF-TEST PASS
```

## Debugger and ILA

Watch `g_aead_result` (`-1` running, `0` PASS, `1` FAIL), `g_aead_test`
(`1` encrypt, `2` decrypt, `3` bad tag), `g_aead_status`, `g_aead_sent`,
and `g_aead_received`. Break just after the final assignment to
`g_aead_result` to inspect the outcome before `main` returns.

For ILA, break at `run_case`, set `g_aead_pause_before_start=1`, and resume.
Arm ILA, halt the CPU, set that variable to zero, then resume to issue START.
On the slave-side monitor, trigger on AWVALID and AWREADY with local AWADDR=0;
AW and W need not handshake together. Use the `.ltx` matching the programmed
bitstream in Hardware Manager.

The program resets its exclusively owned peripheral at startup and aborts a
failed transfer with SOFT_RESET. Polling times out after five seconds per
readiness/transfer phase; halting inside a transfer may cause a timeout on
resume. A CPU MMIO access to an unclocked or inaccessible AXI slave can stall
before software can check its timeout. If execution stops at the first MMIO
access, inspect PS initialization, PL programming, clock/reset and address map.

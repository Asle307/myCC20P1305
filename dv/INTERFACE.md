# AXI4-Lite MMIO peripheral verification

Generate the standard suite with `script/gen_vectors.py generate`. Open
`vivado/core/core.xpr`, select `tb_aead_axi4l` as simulation top, and run
Behavioral Simulation in the GUI. Require its final PASS. Its preset is
`dv/waves/tb_aead_axi4l.wcfg`; wrapper HDL is simulation-only in this project.
The synthesis top remains the native core.

The bench runs all 484 default fixture operations, including 248 authentication
rejects, against independent golden words. It exercises AW-before-W, W-before-AW,
simultaneous channels, byte strobes, zero strobes, stalled B/R responses,
busy-command errors and abort recovery. It also checks PUSH/POP, input/output
slot ownership and interleaved draining/feeding.

Native cycle, configuration-capture, counter/block-reuse, reset and handshake
checks remain in `tb_aead` and `tb_aead_custom`; see [native DV](README.md).
The [host guide](../software/README.md) describes C99 transport-model tests and
the distinct physical-board observations. Local models are not board tests.

The MMIO host benchmark includes CPU feeding/draining and verification.
There is no hardware session-cycle counter in this peripheral.

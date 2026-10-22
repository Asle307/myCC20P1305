# AXI4-Lite + AXI4-Stream/DMA peripheral verification

Generate the standard suite with `script/gen_vectors.py generate`. Open
`vivado/core/core.xpr`, select `tb_aead_axi_dma` as simulation top, and run
Behavioral Simulation in the GUI. Require its final PASS. Its preset is
`dv/waves/tb_aead_axi_dma.wcfg`; wrapper HDL is simulation-only in this project.
The synthesis top remains the native core.

The bench runs all 484 default fixture operations, including 248 authentication
rejects, against independent golden words. It exercises AW-before-W, W-before-AW,
simultaneous channels, byte strobes, zero strobes, stalled B/R responses,
busy-command errors and abort recovery. It also checks word-padded packet
framing, empty sections, stream stability, malformed KEEP/TLAST rejection and
the exact START-to-DONE counter interval. It does not instantiate vendor AXI
DMA, PS HP ports or physical DDR.

Native cycle, configuration-capture, counter/block-reuse, reset and handshake
checks remain in `tb_aead` and `tb_aead_custom`; see [native DV](README.md).
The [host guide](../software/README.md) describes C99 transport-model tests and
the distinct physical-board observations. Local models are not board tests.

The DMA bench also accepts `AXIS_CATALOG=/absolute/path/to/custom_cases.txt`
and `AEAD_UNSTALLED` through GUI simulation plusargs. Restore default plusargs
after measurement. `PERF AXIS` counts payload bytes over core START-to-DONE
cycles, including DMA starvation/backpressure but excluding final DDR write
completion and host verification.

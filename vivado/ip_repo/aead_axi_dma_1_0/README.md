# AEAD AXI4-Stream/DMA IP package

VLNV: `user.org:user:aead_axi_dma:1.0`. This IP contains the control/stream
adapter and ChaCha20-Poly1305 core; add the vendor **AXI DMA** separately.

The package was generated and integrity-checked in the Vivado GUI. Both
synthesis and simulation groups contain the ten modules copied from `rtl/`.
Keep those copies synchronized when repackaging. No standalone clock XDC,
testbench, fixture catalog or board-specific interconnect is packaged.

Interfaces: 32-bit AXI-Lite `S_AXI` (six-bit byte address), 32-bit stream input
`S_AXIS`, 32-bit stream output `M_AXIS`, clock port `i_clk` / interface `CLK`,
active-low reset port `i_rst_n` / interface `RST`. All buses use that clock;
frequency is inherited from the connected source. Keep the 200 MHz target.

The [register and packet contract](../../../docs/interfaces/aead_axi_dma.md)
defines word padding, section lengths, empty directions, authentication and
the hardware cycle counter. Follow [KV260 GUI integration](../../kv260_dma/README.md)
and [the Linux DMA guide](../../../software/README.md) for board use.

The package is self-contained. `rtl/` is the source of truth for its ten HDL
files. After a source change, refresh the package copies and use the Vivado
GUI IP Packager to check integrity and repackage; refresh the IP Catalog and
upgrade the board instance before regenerating output products. Reopen edited
sources before repackaging to avoid restoring stale editor buffers.

The existing KV260 functional and throughput observations are recorded in the
Linux guide. A new routed-frequency or utilization claim requires fresh
implementation reports for the specific project and configuration.

See [integration references](../../../docs/interfaces/integration_references.md)
for the VJU_SoC boundary and AMD packaging/protocol guides.

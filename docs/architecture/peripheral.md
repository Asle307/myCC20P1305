# AXI4-Stream/DMA peripheral architecture

The peripheral top is `aead_axi_dma`. It contains an AXI4-Lite control slave,
a START-to-DONE cycle counter, `aead_axis` and the native `aead_cc20_p1305`
algorithm core. Vendor AXI DMA belongs to the board design and owns DDR
transactions. The peripheral processes one message at a time.

```text
CPU -> S_AXI -> configuration / commands / status / cycles
                           |
                           v
DDR -> AXI DMA MM2S -> S_AXIS -> aead_axis -> native AEAD word input
DDR <- AXI DMA S2MM <- M_AXIS <- aead_axis <- native AEAD word output
```

## Control and data ownership

`rtl/aead_axi_dma.v` accepts AXI4-Lite AW and W independently and commits a
write once both are held. Configuration registers hold key, nonce and original
AAD/payload byte lengths. START is retained until the native session handshake
accepts it. Session lengths and mode are captured for packet conversion;
configuration writes while busy return SLVERR. Registered B/R responses remain
stable while stalled. DONE is sticky until START, reset or CLEAR_DONE. AUTH_OK holds until the
next START or reset.

`rtl/aead_axis.v` owns section scheduling. Its input FSM walks AAD, payload,
received decrypt tag and completion. Every stream word has KEEP=1111; only the
last beat of a complete input/output packet has TLAST. AAD and payload each
occupy a whole number of transport words. Original byte lengths determine the
native byte count and section-LAST; padding lanes do not add authenticated
bytes. Empty AAD/payload terminators are injected internally. Encryption output
includes ciphertext followed by the tag; decryption output contains plaintext.
The interface contract specifies all empty-direction and packet cases.

The adapter forwards native output backpressure, with no whole-message buffer.
Input and output run concurrently. A received malformed KEEP/TLAST latches the
adapter error and stops input; the host must abort and reset the wrapper and
both DMA channels, discard partial output and restart the complete message.
Plaintext in DDR is tentative until core DONE with AUTH_OK=1. DMA completion
also must be observed before the CPU consumes a completed buffer.

## Clock and measurement boundary

The control slave, streams and native core share `i_clk` and synchronous
active-low `i_rst_n`; the saved board design clocks DMA and interconnect from
the same PL0 source. The cycle counter measures accepted native START to the
edge registering native DONE, excluding the wrapper's observation edge. DMA
starvation/backpressure is included; final DDR completion and host verification
are outside this interval. The 32-bit counter saturates and flags overflow.

## Source, package and verification boundaries

The ten modules under `rtl/` are authoritative. Their byte-identical copies in
`vivado/ip_repo/aead_axi_dma_1_0/src/` form the self-contained IP Catalog package.
`vivado/kv260_dma/kv260_dma.xpr` connects that package to AXI DMA and PS DDR.
`software/aead_dma_uio.c` owns fixture packing, buffer management, DMA setup,
polling, recovery and verification against independent expected outputs.

`tb_aead_axi_dma` verifies the control slave, packet conversion, authentication,
stalls, cycle counter, framing rejection and recovery. Native benches retain
algorithm cycle/handshake checks. The peripheral bench does not instantiate
vendor AXI DMA or the physical DDR path. The standalone OOC synthesis top
remains the native algorithm core for paper comparisons.

See the [register and packet contract](../interfaces/aead_axi_dma.md),
[native architecture](aead.md), [package guide](../../vivado/ip_repo/aead_axi_dma_1_0/README.md),
[board guide](../../vivado/kv260_dma/README.md) and [host guide](../../software/README.md).

## Interface requirements and verification

| ID | Requirement | Verification |
|---|---|---|
| REQ-IF-001 | Accept AW and W independently; keep B/R stable while stalled | `tb_aead_axi_dma` ordering, response-stall and byte-strobe checks |
| REQ-IF-002 | Preserve native byte order and authentication behavior | Complete independent fixture suite through the peripheral |
| REQ-IF-003 | Reject illegal accesses without corrupting held/configuration data | Peripheral SLVERR, zero-WSTRB, busy-command and recovery checks |
| REQ-IF-004 | Preserve output under backpressure and recover from abort | Peripheral stress and reset cases; host transport model |
| REQ-IF-005 | Preserve packet lengths/TKEEP/TLAST, empty directions and exact cycle snapshot | Packet framing rejection, empty/partial/boundary cases, counter comparison and host model |

See [interface analysis](interface_analysis.md), [editable figures](../figures/README.md)
and [integration references](../interfaces/integration_references.md). Native
core requirements remain in the inherited specification.

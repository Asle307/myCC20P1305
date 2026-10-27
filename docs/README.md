# Native core and AXI4-Lite design documents

| Contract | Document |
|---|---|
| Function, ports, timing, reset, requirements and verification | [Core specification](architecture/core_specification.md) |
| Hierarchy, instance roles and port semantics | [RTL analysis](architecture/vlsit_rtl_analysis.md) |
| Editable hierarchy and FSM flowcharts | [Figures](figures/README.md) |
| Native external handshake and framing | [Word interface](interfaces/aead_stream.md) |
| Session scheduling and data lifetimes | [AEAD](architecture/aead.md) |
| Quarter rounds, mapping and block generation | [ChaCha20](architecture/chacha20.md) |
| Paired authentication processing | [Poly1305](architecture/poly1305.md) |
| Unsigned multiplication | [Radix-4 multiplier](architecture/multiplier.md) |
| Canonical modular arithmetic | [Reducer](architecture/reducer.md) |
| Cryptographic bibliography | [Eight saved papers](references.md) |

These describe the maintained RTL, not a proposed architecture. The core has
custom native handshakes; it is not itself an AXI peripheral. The AXI4-Lite
wrapper has its own interface contract and normative interface references.
See [DV](../dv/README.md) for simulation and fixture handling.

## AXI4-Lite MMIO integration

The AXI4-Lite integration includes [peripheral architecture](architecture/peripheral.md),
[register contract](interfaces/aead_axi4l.md),
[interface analysis](architecture/interface_analysis.md) and
[integration references](interfaces/integration_references.md).
These documents describe the AXI4-Lite MMIO implementation maintained on
`main`. The alternative AXI4-Stream/DMA transport is developed separately on
`axi4l-axi4s-dma`; consult that branch for its interface and integration guides.
The core bibliography above remains the same eight PDFs.

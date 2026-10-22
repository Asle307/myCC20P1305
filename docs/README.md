# Native core design documents

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
custom native handshakes; it is not itself an AXI peripheral. Integration
branches add their own interface contracts and normative interface references.
See [DV](../dv/README.md) for simulation and fixture handling.

## AXI4-Lite MMIO integration

This branch adds [peripheral architecture](architecture/peripheral.md),
[register contract](interfaces/aead_axi4l.md),
[interface analysis](architecture/interface_analysis.md) and
[integration references](interfaces/integration_references.md).
The core bibliography above remains the same eight PDFs.

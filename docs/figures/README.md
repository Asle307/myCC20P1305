# Native design figures

 Editable sources use VLSIT library symbols, black/white styling and 18 pt text.
 Their SHA-256 provenance identifies the symbol library, not proof of behavior.
 The [RTL analysis](../architecture/vlsit_rtl_analysis.md) is the detailed source
 for instance inventory, port semantics and transition conditions. All thirteen
 physical native instances are represented, including generated copies.

 The focused VLSIT method keeps detailed function/interface tables in that
 analysis document rather than reproducing large tables inside every picture.
 Hierarchy arrows show functional transactions, not parent-child instantiation.
 Ready/status returns belong to the labeled group. Shared clock/reset is stated
 on each hierarchy page; diagrams abstract individual wires.

 | Editable source | Page | Preview |
 |---|---|---|
 | [Hierarchy](vlsit_rtl_hierarchy.drawio) | 01_LEVEL_1 — AEAD | [SVG](hierarchy_1.svg) |
 | Same source | 02_LEVEL_2 — round and lane controllers | [SVG](hierarchy_2.svg) |
 | Same source | 03_LEVEL_3 — generated QRs / multiplier / reducer leaves | [SVG](hierarchy_3.svg) |
 | [Control flow](core_control_flow.drawio) | AEAD | [SVG](control_1.svg) |
 | Same source | ChaCha20 | [SVG](control_2.svg) |
 | Same source | Poly1305 | [SVG](control_3.svg) |
 | Same source | Arithmetic lane controller | [SVG](control_4.svg) |
 | Same source | Radix-4 multiplier | [SVG](control_5.svg) |
 | Same source | Canonical reducer | [SVG](control_6.svg) |

 Controllers retain state when a transition condition is false. Global reset
 and illegal-state recovery go to IDLE. Fixed QR pipeline steps and word maps
 are documented in the [ChaCha20 architecture](../architecture/chacha20.md).
 SVG files are exported from the corresponding draw.io page; edit the source
 and refresh its preview together. No diagram PDF is added to the bibliography.

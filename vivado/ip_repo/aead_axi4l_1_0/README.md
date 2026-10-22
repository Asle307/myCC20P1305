# AEAD AXI4-Lite peripheral

32-bit AXI4-Lite slave, 16 local register offsets (64 bytes), one clock,
one held input word and one held output word. The native AEAD FSM is unchanged.

The complete register map, command bits, software sequence and handshake
semantics are in [the interface notes](../../../docs/interfaces/aead_axi4l.md).
Comments in `hdl/aead_axi4l_slave_lite_v1_0_S00_AXI.v` summarize the function
of each register and the bus/stream holding rules. `drivers/` provides named
C offsets/masks and an idle-only register self-test.

Refresh the IP repository and upgrade existing block-design instances after
updating this package. If an Edit IP project was already open, reload the
changed files from disk before repackaging; avoid saving old editor buffers
over the updated HDL. Part, clock and board integration belong to the parent
project. Simulation pass does not claim implementation timing.

VLNV: `user.org:user:aead_axi4l:1.0`. This is the only custom peripheral package
in this branch. All ten HDL files under `hdl/` belong to the package; the eight
native algorithm modules must match `rtl/`. The two AXI4-Lite wrapper modules are maintained under `rtl/`; all ten
`hdl/` copies must match their corresponding maintained RTL files. The
peripheral simulation bench uses `rtl/` directly.

Use the Vivado GUI IP Packager on this directory to check integrity and
repackage. No separate Edit IP project is needed. See
[KV260 integration](../../kv260/README.md) and the
[Linux host guide](../../../software/README.md) for board use.

See [integration references](../../../docs/interfaces/integration_references.md)
for the VJU_SoC boundary and AMD packaging/protocol guides.

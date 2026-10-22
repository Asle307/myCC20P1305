# Repository guidance

This project implements ChaCha20-Poly1305 in Verilog-2001. Use repository
sources and their documented contracts as the basis for changes. Before
changing a block, read `README.md`, its specification under
`docs/architecture/`, the RTL and its testbench.

## Configuration scope

`main` contains the eight native algorithm modules under `rtl/`, their
verification sources under `dv/`, and the supporting documentation, fixture
tools and native Vivado project. The native top is `aead_cc20_p1305`.
`axi4l` and `axi4l-axi4s-dma` are independent integration branches derived
from this core. Keep peripheral wrappers, peripheral benches, IP packages,
board designs and host software on their respective integration branches.

## Source and documentation contracts

- `docs/architecture/core_specification.md` defines native behavior, timing and verification requirements; `docs/architecture/vlsit_rtl_analysis.md` describes ports and hierarchy.
- `docs/interfaces/aead_stream.md` defines the native word interface. Preserve its framing, word order and stall behavior.
- `dv/README.md` describes native regression and custom fixtures. `script/README.md` describes fixture generation and repository tools.
- `vivado/core/README.md` describes the native GUI project. `docs/figures/README.md` indexes editable diagrams and previews.

## RTL style

- Use four spaces for indentation. Group ports, declarations, and instance connections by function, with a blank line between groups.
- Within each functional interface group, order ports by descending bus width: wider buses first, narrower buses next, then scalar signals. Keep clock/reset as the first group, preserve group boundaries and retain the relative order of equal-width ports. Apply the same ordering to named instance connections within each group; use named connections when changing declaration order.
- Align declaration widths, assignment operators, FSM constants, and named instance ports within related groups. Write connections as `.port (signal)`.
- Use explicit `begin`/`end` for control branches, and put `else` on its own line. Short combinational `case` assignments may stay on one line.
- Keep the `i_`/`o_`, `r_`/`w_`, `LP_`/`ST_`, and `f_` naming conventions. Use Verilog-2001 `wire`/`reg`, `always @(*)`, and `always @(posedge i_clk)`. Keep production RTL in `.v` files and first-party testbenches in Verilog-2001; do not introduce SystemVerilog-only syntax. Preserve published AXI signal names when editing wrappers.
- Use `assign` for simple wiring and arithmetic. Use `always @(*)` for muxes and byte loops; give combinational outputs defaults before selecting cases.
- Keep byte masks, encoding, and fixed wiring in the owning module. Use a small function when the same operation is reused. Create a module when it owns an independently meaningful datapath or protocol.
- Write ChaCha20 word maps as explicit rows in ascending word arrays. Each QR input row contains `{a, b, c, d}`; each output row restores the state matrix. Preserve those rows when formatting.
- `cc20_round` uses `i_valid`/`o_valid`; pipeline validity and column/diagonal mode through the two QR stages together. It accepts one independent round input per clock.
- Number QR intermediate signals by algorithm step 1..4. Show fixed rotations as bit concatenations.
- Keep FSM state names descriptive and datapath expressions visible near their registers. Comments should explain word order, arithmetic bounds, cycle boundaries, or handshake decisions.
- For a style cleanup, preserve ports, state encodings, reset behavior, handshake timing, register stages, and cycle counts.

## Validation and Vivado workflow

Use the Vivado GUI project `vivado/core/core.xpr` and follow its README.
Run behavioral simulation after RTL changes and require the final bench PASS.
Keep the existing cycle and handshake checks; documented block latency is a
design contract. On integration branches, run the peripheral bench as well
as native regression when shared RTL changes.

Keep waveform presets under `dv/waves/`. Use `script/gen_vectors.py` for
fixtures under `dv/vectors/`. Keep `dv/waves/gui.tcl` solely as the GUI
waveform-selection hook. Do not add batch/CLI simulation or implementation
runners, project-creation Tcl, or build Tcl unless requested. Maintain projects
and IP packaging through the Vivado GUI. Use a separate Xvfb display for GUI
automation.

The native core project targets XC7V2000T-FLG1925-2 with out-of-context
synthesis and implementation at 200 MHz. Keep its XDC limited to the standalone
clock declaration. Board constraints belong to the integrating project.
Generic-IP OOC results require at least +0.050 ns setup slack and nonnegative
hold slack. Resource and Fmax claims require fresh Vivado implementation
reports; simulation and readability changes do not establish implementation
timing.

## Documentation

Keep documentation in English. Architecture and interface specifications
belong under `docs/`. Keep the root README and DV guides focused on using
the current code. The cryptographic bibliography is limited to the eight
PDFs under `docs/references/`, indexed by `docs/references.md`.

Follow the structure of the existing specifications: describe ports,
clock/reset behavior, FSM transitions, timing and verification requirements
from RTL and testbench evidence. Keep editable diagrams and SVG previews
under `docs/figures/`; match hierarchy and transition conditions to RTL.
Distinguish native handshakes from standard AXI interfaces on integration
branches. Update affected contracts and diagrams when behavior changes.

## Generated output and cleanup

Keep generated logs, caches, simulation output and implementation products
untracked. Retain source, fixtures, waveform presets, GUI projects,
specifications and reference papers; do not add report archives or measurement
bundles unless requested.

Close Vivado before cleanup. Use `make clean DRY_RUN=1` to preview transient
cleanup and `make clean` to apply it. `make distclean` also removes rebuildable
implementation products; preview it with `DRY_RUN=1`. Preserve tracked files,
vectors/captures, `.venv/` and separately exported `artifacts/`. Use the
repository cleanup targets rather than broad `git clean` commands.

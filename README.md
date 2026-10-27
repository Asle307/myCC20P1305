# ChaCha20-Poly1305 FPGA core and AXI4-Lite MMIO

The `main` branch contains the native RFC 8439 core and its `aead_axi4l` peripheral.
The CPU transfers every AAD, payload and tag word through AXI4-Lite registers,
using one held input slot and one held output slot. Software feeds input and
drains output concurrently.

| Boundary | Entry point |
|---|---|
| Native algorithm core | [Core specification](docs/architecture/core_specification.md) |
| Peripheral behavior and requirements | [Peripheral architecture](docs/architecture/peripheral.md), [register contract](docs/interfaces/aead_axi4l.md) |
| Interface sources and protocol citations | [Integration references](docs/interfaces/integration_references.md) |
| Wrapper hierarchy and flowcharts | [Interface analysis](docs/architecture/interface_analysis.md), [figures](docs/figures/README.md) |
| IP Catalog package | [Package guide](vivado/ip_repo/aead_axi4l_1_0/README.md) |
| KV260 design | [Board guide](vivado/kv260/README.md), `vivado/kv260/kv260.xpr` |
| Linux host and existing board observations | [Software guide](software/README.md) |
| Verification | [Native DV](dv/README.md), [peripheral DV](dv/INTERFACE.md) |

The eight native RTL modules, their specifications and fixtures are maintained
alongside the AXI4-Lite wrapper on `main`. The separate `axi4l-axi4s-dma` branch
is under development for AXI4-Lite control with AXI4-Stream/DMA data transfer;
use that branch for its wrappers, package, board project and host.
The peripheral target is 200 MHz. Native OOC synthesis still targets the native
top on XC7V2000T-FLG1925-2, separately from the board design.

## Simulate

1. Create `.venv`: `python3 -m venv .venv`, then `.venv/bin/python -m pip install -r script/requirements.txt`.
2. Generate the native suite: `.venv/bin/python script/gen_vectors.py generate`.
3. Generate the RFC custom catalog: `.venv/bin/python script/gen_vectors.py generate --profile rfc`.
4. Open `vivado/core/core.xpr` in Vivado 2025.2. Select `tb_aead_axi4l` for the peripheral suite, `tb_aead` for native regression or `tb_aead_custom` for custom fixtures, then **Set as Top**.
5. Run **Behavioral Simulation** in the GUI; require the final bench PASS. The waveform hook selects the matching preset.

The saved simulation top is `tb_aead_axi4l`; the synthesis top remains
`aead_cc20_p1305`. Run both native and peripheral regression after changing
shared RTL. See the DV guides for block-specific and custom-fixture checks.

After running `tb_aead_custom`, check the captured RTL output independently:

```sh
.venv/bin/python script/gen_vectors.py verify --case dv/vectors/custom/rfc8439_enc
.venv/bin/python script/gen_vectors.py verify --case dv/vectors/custom/rfc8439_dec
```

Regenerate catalogs after moving a checkout: they contain absolute case paths.
Use `--force` only for generator-owned output you intend to replace; unmarked
fixtures are protected. See DV for custom binary inputs and coverage.

## Timing and authentication

Both modes authenticate ciphertext. Drain output while feeding input. Retain
tentative plaintext privately until DONE with `AUTH_OK=1`; discard it on failure.
Reset aborts a session and does not promise key/datapath zeroization.

Target 200 MHz. The native OOC project targets XC7V2000T-FLG1925-2 with setup
slack at least +0.050 ns and nonnegative hold slack. ChaCha20 result-valid is
41 clocks; paired arithmetic is 69; Poly1305 setup/pair/singleton contracts are
70/73/72. External stalls add session latency. Resource/Fmax claims require
fresh Vivado implementation reports.

## Housekeeping

Close Vivado before cleanup. Make provides no simulation or build runner.

```sh
make                       # show cleanup help
make clean DRY_RUN=1       # preview transient cleanup
make clean                 # logs, .Xil, cache and simulation output
make distclean DRY_RUN=1   # also preview implementation/product cleanup
make distclean
```

`clean` keeps synthesis/implementation products. `distclean` also removes
rebuildable `.runs`, `.gen`, `.hw`, `.ip_user_files`, imported utility checkpoints
and `build/`, including bitstreams inside those generated folders. Both preserve
tracked files, vectors/captures, `.venv/` and separately exported `artifacts/`.
Unknown user files and directory symlinks are left alone.

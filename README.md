# ChaCha20-Poly1305 native FPGA core

Verilog-2001 implementation of RFC 8439. This `main` branch contains the
native algorithm and its verification: four parallel ChaCha20 quarter rounds,
two Poly1305 arithmetic lanes and a little-endian 32-bit AEAD word interface.
One message is processed at a time.

| Use | Entry point |
|---|---|
| Design contract and verification requirements | [Core specification](docs/architecture/core_specification.md) |
| RTL hierarchy, ports and control flow | [Design analysis](docs/architecture/vlsit_rtl_analysis.md), [diagrams](docs/figures/README.md) |
| Algorithm architecture | [AEAD](docs/architecture/aead.md), [ChaCha20](docs/architecture/chacha20.md), [Poly1305](docs/architecture/poly1305.md) |
| Integration boundary | [Native word interface](docs/interfaces/aead_stream.md) |
| Golden fixtures, regression and captures | [DV guide](dv/README.md), `script/gen_vectors.py` |
| Vivado GUI simulation and OOC comparison | [Core project](vivado/core/README.md) |
| Cryptographic references | [Eight saved papers](docs/references.md) |

The two integration branches are independent children of this native core:
`axi4l` adds CPU word transfers through AXI4-Lite; `axi4l-axi4s-dma` adds
AXI4-Lite control and AXI4-Stream/DMA packet transfers. Select one branch for
its wrapper, peripheral bench, packaged IP, KV260 project and Linux host.

## Simulate

1. Create `.venv`: `python3 -m venv .venv`, then `.venv/bin/python -m pip install -r script/requirements.txt`.
2. Generate the native suite: `.venv/bin/python script/gen_vectors.py generate`.
3. Generate the RFC custom catalog: `.venv/bin/python script/gen_vectors.py generate --profile rfc`.
4. Open `vivado/core/core.xpr` in Vivado 2025.2. Select `tb_aead` for the full regression or **Set as Top** on `tb_aead_custom` for the custom catalog.
5. Run **Behavioral Simulation** in the GUI; require the final bench PASS. The waveform hook selects the matching preset.

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

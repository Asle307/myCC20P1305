# Native core Vivado GUI project

Open `core.xpr` in Vivado 2025.2. Its synthesis top is `aead_cc20_p1305`, targeting
**XC7V2000T-FLG1925-2** in out-of-context mode (`-mode out_of_context`). The sole
XDC declares the native 5 ns clock. Board pins and external I/O constraints
belong to the integrating design.

Generate both catalogs from the repository root before simulation:

```sh
.venv/bin/python script/gen_vectors.py generate
.venv/bin/python script/gen_vectors.py generate --profile rfc
```

Native simulation uses `tb_aead`; `tb_aead_custom` runs user fixtures.
Use **Set as Top**, then **Run Behavioral Simulation** in the GUI. Require the
bench's final PASS, not merely successful compilation. The project copies the
two catalogs; their absolute case paths require regeneration after a move.

The waveform-selection hook is `dv/waves/gui.tcl`. Its project property is
`../../../../../../dv/waves/gui.tcl`, relative to XSim's
`core.sim/sim_1/behav/xsim/` working directory, keeping presets checkout-local.
Do not put project creation, simulation launch or implementation commands in
this hook. See [DV](../../dv/README.md) and the
[core contract](../../docs/architecture/core_specification.md).

Run synthesis/implementation in the GUI. The comparison target stays 200 MHz,
with setup slack at least +0.050 ns and nonnegative hold slack. Only fresh
reports establish utilization or routed timing. RTL simulation does not prove
implementation frequency. After closing Vivado, preview `make clean DRY_RUN=1`
or `make distclean DRY_RUN=1` before removing generated products.

On this integration branch, `tb_aead_axi_dma` is the default peripheral top.
Its two wrapper sources are simulation-only. Use **Set as Top** to select
`tb_aead` or `tb_aead_custom` for native verification, then restore the peripheral
top. See [peripheral DV](../../dv/INTERFACE.md) and
[KV260 integration](../kv260_dma/README.md).

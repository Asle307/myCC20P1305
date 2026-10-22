# Repository tools

| Tool | Purpose | Invocation from repository root |
|---|---|---|
| `gen_vectors.py` | Independent AEAD fixtures, inspection and captured-output checks | `.venv/bin/python script/gen_vectors.py --help` |
| `clean.py` | Allowlisted, tracked-file-aware cleanup | `make clean DRY_RUN=1` or `make distclean DRY_RUN=1` |

The Makefile is for housekeeping only. Simulation, packaging and implementation
remain in the Vivado GUI. `dv/waves/gui.tcl` only selects/logs a waveform preset
and runs the already elaborated simulation; it is not a build runner.

Generator dependencies are in `requirements.txt`. Cleanup itself uses only
Python's standard library and Git. Run the tool tests with:

```sh
.venv/bin/python -m unittest discover -s script/tests -v
```

`test_vectors.py` exercises independent fixture generation and capture checking.
`test_clean.py` executes Makefile targets on temporary Git checkouts and verifies
preservation of tracked source, user inputs, vectors, environment and exported
artifacts, plus symlink boundaries and the two cleanup levels.

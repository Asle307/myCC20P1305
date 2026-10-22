# Native AEAD verification

The fixture generator is `script/gen_vectors.py`, with one directory per case.
It uses `cryptography` for golden ChaCha20-Poly1305 and Rich for console output;
there is no interactive menu. Install `script/requirements.txt` into `.venv`.

## Generate and inspect

```sh
.venv/bin/python script/gen_vectors.py selftest
.venv/bin/python script/gen_vectors.py generate
.venv/bin/python script/gen_vectors.py generate --profile rfc
.venv/bin/python script/gen_vectors.py inspect dv/vectors/custom
.venv/bin/python script/gen_vectors.py inspect dv/vectors/custom/rfc8439_enc
```

`generate` defaults to `--profile suite --mode both --seed 0x8439
--random-count 8 --max-random-bytes 512`. Suite output defaults to
`dv/vectors/suite`; other profiles default to `dv/vectors/custom`. Existing
nonempty output requires `--force` and the generator ownership marker. An
unmarked directory is never removed. No old menu or `edge/random/custom`
subcommands are retained.

Profiles:

| Profile | Contents | Bench |
|---|---|---|
| `suite` | RFC, fixed boundaries, all tails, long inputs, seeded random and authentication failures | `tb_aead` |
| `rfc` | RFC 8439 section 2.8.2 | `tb_aead_custom` |
| `random` | Exactly `--random-count` random messages, lengths 0..`--max-random-bytes` | `tb_aead_custom` |
| `custom` | One message from hex or binary files | `tb_aead_custom` |

`--mode encrypt|decrypt|both` controls generated operations; `suite` requires
`both` for its reset/recovery tests. A message in
`both` mode creates two case directories. Section sizes are limited to
65536 bytes for simulation; this is not a hardware limit. For example:

```sh
.venv/bin/python script/gen_vectors.py generate --profile random --random-count 10 --seed 123 --force
.venv/bin/python script/gen_vectors.py generate --profile custom --case-name hello --aad-hex 010203 --plaintext-hex 68656c6c6f --force
.venv/bin/python script/gen_vectors.py generate --profile custom --plaintext-file payload.bin --aad-file aad.bin --force
```

Custom key/nonce default to seeded bytes. Supply `--key-hex` (32 bytes) and
`--nonce-hex` (12 bytes) to fix them. Omit AAD/plaintext for empty data. Hex
accepts an optional `0x` prefix and whitespace, colon, underscore or dash
separators. AAD/plaintext hex and binary-file options are mutually exclusive.

## Vivado simulation

Open `vivado/core/core.xpr`, set a simulation top, then run Behavioral
Simulation. Select `tb_aead` for native regression. Generate the matching catalog
before selecting its bench. After changing fixtures, use Relaunch Simulation.

| Top | Catalog | Extra stress |
|---|---|---|
| `tb_aead` | `dv/vectors/suite/suite_cases.txt` | Five reset/abort recoveries and unstalled encrypt/decrypt replays |
| `tb_aead_custom` | `dv/vectors/custom/custom_cases.txt` | Runs exactly the generated operations with stream stalls |

The project copies only the two catalogs to the simulation directory. Each
catalog lists absolute case directories; the reader opens the individual files
there. Regenerate after moving the repository. An alternate `--out` directory
requires adding its catalog in the GUI instead of the default one; retain the
catalog basename expected by the selected bench. Filesystem paths must be
printable ASCII and at most 255 characters in the catalog.

The shared runner, case reader and stream monitor are Verilog-2001. Only
one case is held in memory. Checks cover data and padding, count/type/LAST,
VALID/data stability under backpressure, unknown transfer values, captured
configuration, DONE pulse and AUTH_OK hold, per-session cycle deadlines,
ChaCha20 counter order/block reuse and its 41-clock result-valid contract.
The simulation clock is 200 MHz. Simulation does not establish Fmax.

### Native core cycle measurement

The shared runner accepts the runtime test plusarg `AEAD_UNSTALLED` for a
cycle measurement using the current catalog. In the open
Vivado project's Tcl Console, configure the simulation without changing the
synthesis top:

```tcl
set_property top tb_aead_custom [get_filesets sim_1]
set_property -dict {xsim.simulate.xsim.more_options {-testplusarg AEAD_UNSTALLED}} [get_filesets sim_1]
```

Run **Behavioral Simulation** (or close and relaunch an existing simulation).
The source presents the next word on the first falling edge after a transfer,
with no idle cycle between consecutive accepted words. The sink holds READY
high. Existing data, metadata, handshake, key-capture, authentication and
ChaCha20 cycle checks remain enabled. A `PERF AEAD` line appears only after
successful completion and requires `input_starved=0` and `output_stalled=0`.
The suite's existing unstalled replays use this same source behavior.

`cycles` measures rising-edge intervals from accepted START to the edge that
registers DONE: the last tag-output transfer for encrypt, or complete received
tag comparison for decrypt. The monitor observes registered DONE on the next
rising edge, which is excluded from the result. `latency_ns = cycles * 5` and
`payload_Mbit_s = payload_bytes * 8 * 1000 / latency_ns`. Session setup, AAD,
keystream generation, the lengths block and tag processing are included in
latency; only payload bytes count toward throughput. These are native-stream
RTL results at the 5 ns simulation clock, with no AXI-Lite wrapper, CPU, UIO,
software verification or Linux scheduling cost.

For a separate workload, generate with `--profile custom --mode both --out
dv/vectors/throughput` and the desired input files. In **Simulation Sources**,
replace the `dv/vectors/custom/custom_cases.txt` catalog with the generated
`dv/vectors/throughput/custom_cases.txt` catalog for that run. Both catalogs
have the same basename; add only one at a time. Keep the original fixture
directories. Match key, nonce, AAD and plaintext to the host benchmark when
comparing paths; use the generator's independent golden outputs and `verify`
to check the captured transfers.

After measurement, restore the original catalog and the normal simulation
configuration:

```tcl
set_property xsim.simulate.xsim.more_options {} [get_filesets sim_1]
set_property top tb_aead [get_filesets sim_1]
```

The default suite covers all 1..16-byte AAD and payload tails, all 1..64-byte
ChaCha20 tails, payloads through 4096 bytes, large AAD, all-zero/all-FF data,
valid encrypt/decrypt, bad tags on each message, all 128 individual RFC tag
bit mutations, and modified AAD/ciphertext. Negative decrypt cases still
compare tentative plaintext. The five reset points are OTK calculation,
stalled payload output, stalled encrypt tag, partial input-block assembly and
half-received decrypt tag. Each abort is followed by a successful replay.
Suite reset stress requires both modes and an encrypt payload of at least
16 bytes; use the default `--mode both` with `tb_aead`.

## Case files and native RTL adapter

Every case folder contains `key.hex`, `nonce.hex`, `aad.hex`, `input.hex`,
`expected_output.hex`, computed tag files, concatenated streams, beat CSVs,
`lengths.hex` and `case.json`. Decrypt also has received-tag files. Generic
packing supports `--word-bits 32|128` and `--byteorder little|big`. Generic
empty sections have zero words; final partial words have zero padding lanes.
The expected output stream includes the tag only for encrypt.

The `rtl_*` files always use the actual core's 32-bit little-endian interface,
independently of generic export settings:

| File | Contents |
|---|---|
| `rtl_config.hex` | Key256, nonce96, computed tag128, then six 32-bit lines: encrypt flag, expected AUTH_OK, AAD bytes, payload bytes, input beat count, output beat count |
| `rtl_input.hex` / `rtl_input_control.hex` | Native AAD, payload and received decrypt tag, with exact input framing |
| `rtl_expected.hex` / `rtl_expected_control.hex` | Native expected payload and encrypt tag, with exact output framing |

Controls encode byte count in bits 2:0, input AAD/output tag in bit 8, and
section LAST in bit 9; all other bits are zero. Empty input AAD and payload
sections each have a zero-byte LAST beat. The decrypt tag is four full words
on input. Decrypt output is plaintext only. Both payload and encrypt tag end
with their own LAST. AUTH_OK is zero for encrypt and valid/invalid according
to the received decrypt tag. Generic CSV/control boundaries remain descriptive;
use the native adapter for actual DUT driving.

## Verify captured output

Successful bench operations write `actual_stream.hex`, `actual_output.hex`,
`actual_tag_words.hex` and `actual_status.hex` inside each case folder.
These files capture accepted DUT transfers; `actual_status.hex` contains
AUTH_OK sampled at DONE. Reset-aborted operations do not replace captures.

```sh
.venv/bin/python script/gen_vectors.py verify --case dv/vectors/custom/rfc8439_enc
.venv/bin/python script/gen_vectors.py verify --case dv/vectors/custom/rfc8439_dec
```

With no explicit actual-file options, verification checks native
`actual_stream.hex` against `rtl_expected.hex` and the captured authentication
status. For external dumps, supply `--actual-output`, `--actual-tag` or
`--actual-stream`; add `--rtl` for native 32-bit little-endian data instead of
generic export packing. `--actual-status` explicitly supplies authentication
status. Decrypt has no output tag; verify its status instead. Exit status is
0 for match, 1 for mismatch, and 2 for usage, file or format errors.

Waveform presets live under `dv/waves/`; `gui.tcl` selects the active top's
preset. Both native tops instantiate `u_test`. The reference group shows expected
word/control/tag and match signals; compare only with `w_sample_valid=1`.

Generator tests: `.venv/bin/python -m unittest discover -s script/tests -v`.

# AXI4-Lite Linux UIO tests and throughput

`aead_uio.c` is a standalone C99 host for the packaged 16-register AXI-Lite
peripheral. It follows the VJU_SoC Linux UIO mapping boundary and uses the
[AEAD register map](../docs/interfaces/aead_axi4l.md). It does not need the
Xilinx bare-metal BSP, DMA, Python packages or OpenSSL on the board.

Prerequisites: the matching bitstream has been loaded, FPGA Manager reports
`operating`, the PS PL clock is enabled, and an initial STATUS read returns
`0x00000001`. The lab board currently exposes `MY_IP` at `/dev/uio12`, map0
base `0xA0000000`, size `0x10000000`. The name/index comes from Linux's device
tree; rediscover it if that configuration changes. Run only one peripheral
client at a time, and do not reload the bitstream during a test.

## Copy, compile and run

From the repository root on the development machine, replace `BOARD_IP`:

```sh
scp software/aead_uio.c debian@BOARD_IP:~/Asle/
```

Compile **on the board** in the SSH terminal:

```sh
cd ~/Asle
gcc -std=c99 -O2 -Wall -Wextra aead_uio.c -o aead_uio
sudo ./aead_uio --device /dev/uio12
```

Expected successful results:

```text
PASS RFC encrypt: 114 payload bytes, AUTH_OK=0
PASS RFC decrypt: 114 payload bytes, AUTH_OK=1
PASS RFC decrypt bad tag (rejected): 114 payload bytes, AUTH_OK=0
```

The program loads the RFC 8439 section 2.8.2 key/nonce, supplies AAD and
payload, checks every accepted payload/tag word and all output metadata,
and verifies sticky DONE, final readiness and AUTH_OK. The third operation
flips one received-tag bit and expects authentication rejection. Tentative
plaintext is checked privately against the public RFC vector; the program
does not return it as authenticated application data.

Only one page of UIO map0 is mapped, rather than the lab demo's whole 256 MiB.
The program verifies map0's base, size and zero offset before mapping. MMIO
uses aligned volatile 32-bit accesses and memory barriers; input is packed
little endian. INPUT_DATA and PUSH are separate writes, and output data/meta
are read before POP. The polling loop drains output while feeding input so
the two single-word slots cannot deadlock each other.

Each operation has a 5000 ms polling timeout by default. A failing operation
started by this process is aborted with SOFT_RESET. An already busy device
or preexisting ERROR is reported without resetting someone else's session.
Exit codes: 0 for pass, 1 for test/timeout failure, 2 for usage or mapping error.

## Measure throughput on the board

Copy and recompile the updated `aead_uio.c` using the commands above. No RTL,
IP package or bitstream change is required. The existing lab setup has passed
the three RFC checks with Linux reporting PL0 at `199999998 Hz` (approximately
200 MHz). Check the running clock before measuring; the Vivado constraint
does not set the clock of an already running Linux system:

```sh
sudo cat /sys/kernel/debug/clk/clk_summary | grep -E '^[[:space:]]*pl0_ref[[:space:]]'
sudo ./aead_uio --device /dev/uio12 --bench
```

Defaults: 16384 payload bytes, 12 AAD bytes, 100 measured messages and 3
warm-up messages **per mode**, encrypt then decrypt. Select a mode or change
the workload with decimal byte counts:

```sh
sudo ./aead_uio --device /dev/uio12 --bench --test encrypt \
    --bytes 65536 --aad-bytes 12 --iterations 100 --warmup 5
sudo ./aead_uio --device /dev/uio12 --bench --test decrypt \
    --bytes 65536 --aad-bytes 12 --iterations 100 --warmup 5
```

Each result reports the processed payload byte count, message count, total
elapsed seconds, MB/s (10^6 bytes/s), MiB/s (2^20 bytes/s), Mbit/s (10^6
bits/s), mean microseconds/message and messages/s. Throughput is
`payload_bytes * iterations / elapsed_seconds`; AAD and authentication tags
are transferred but excluded from the numerator. A zero-byte payload reports
zero payload throughput while still measuring message latency.

Timing uses Linux `CLOCK_MONOTONIC` with nanosecond timestamps, from immediately
before the first measured session until after the last session returns ready.
It **includes** per-message readiness checks, all 8 key and 3 nonce writes,
START, CPU packing, AXI-Lite input/output transfers, polling, barriers, timeout
checks, full output/metadata comparisons and final DONE/AUTH_OK checks. It
also includes Linux scheduling delays. This is verified application throughput
through the CPU/UIO path; the current IP has no hardware cycle counter to
isolate core-only latency or throughput. Increasing PL frequency need not
increase these rates proportionally.

Before timing, all three independent RFC tests must pass. The host then
creates a public synthetic payload and AAD (`byte[i] = i mod 256`), captures
one untimed encryption and verifies its decryption against the source with
AUTH_OK=1. Every timed encrypt compares against the captured ciphertext/tag;
every timed decrypt compares against the original plaintext and checks
authentication. This round trip is a consistency check for the selected size,
not an independent cryptographic reference for that synthetic message. Data
preparation, RFC preflight, allocation, warm-up and printing are excluded from
the timed interval. No successful rate is printed for a mode that fails.
The benchmark replays one public fixture with a fixed key/nonce; use it only
for synthetic measurements, not for encrypting application data.

To compare message sizes, keeping AAD and iterations fixed:

```sh
for bytes in 64 256 1024 4096 16384 65536; do
    sudo ./aead_uio --device /dev/uio12 --bench \
        --bytes "$bytes" --aad-bytes 12 --iterations 100 --warmup 3 || break
done
```

Repeat runs on an otherwise idle board and retain the clock readback alongside
the terminal results. `--bytes` accepts 0..16777216, `--aad-bytes` accepts
0..1048576, `--iterations` accepts 1..1000000 and `--warmup` accepts 0..1000000.
`--timeout-ms` applies separately to each message; increase it for very large
messages if needed. Benchmark options require `--bench`; `--pause` and
`--test bad-tag` are unavailable in benchmark mode. The bad-tag RFC preflight
still runs regardless of the selected benchmark mode.

### Compare with the native stream

The [DV guide](../dv/README.md#native-core-cycle-measurement) describes the
`AEAD_UNSTALLED` simulation option. With the same public fixture as the UIO
benchmark (key bytes 0..31, nonce bytes 0..11, 12 AAD bytes, 16384 payload
bytes), behavioral simulation of the current RTL at a 5 ns clock produced:

| Mode | Native START-to-DONE cycles | Native latency | Native payload rate | Board UIO payload rate |
|---|---:|---:|---:|---:|
| Encrypt | 54721 | 273.605 us | 479.056 Mbit/s | 28.429 Mbit/s |
| Decrypt | 43714 | 218.570 us | 599.680 Mbit/s | 28.427 Mbit/s |

Both native runs passed every output/metadata/authentication check against
independent `cryptography` fixtures, with zero source-starvation and zero
output-backpressure cycles. The corresponding board results used 100 measured
messages, 3 warm-ups per mode and Linux PL0 readback of 199999998 Hz; their
mean latencies were 4610.425 us (encrypt) and 4610.897 us (decrypt).

For this workload the verified UIO path achieves about 5.93% and 4.74% of the
native-stream rates. The difference combines the wrapper, CPU, MMIO, barriers,
polling, software comparisons and scheduling; it does not identify an isolated
AXI transfer cost. Decrypt can overlap Poly1305 processing with keystream
generation after submitting ciphertext, whereas encrypt must first produce
ciphertext for the MAC. This explains the different native schedules even
though the UIO results are almost identical.

The often-used `256 bits * 200 MHz / 73 cycles = 701.370 Mbit/s` is an optimistic
Poly1305 arithmetic ceiling that omits block acceptance and top-level
scheduling. It is not the complete AEAD rate. Native results here are cycle
measurements in RTL simulation, not a board measurement of core-only throughput
or evidence of Fmax. Changing workload lengths or external stream stalls
changes the measured rate.

## ILA capture

To perform only encrypt and wait just before START:

```sh
sudo ./aead_uio --device /dev/uio12 --test encrypt --pause
```

While the SSH program waits, arm System ILA in Hardware Manager. Trigger on
the slave-side CONTROL address write: AWVALID=1, AWREADY=1, AWADDR=0. Then
press Enter in SSH. The monitor sits after SmartConnect, where addresses are
local offsets, not the full `0xA0000000` physical address. AW and W are
independent channels; do not require them to handshake on the same cycle.

`--test decrypt`, `--test bad-tag` and `--timeout-ms N` are also supported.
The program does not program PL or change PS clocks. Interpret ILA capture
time using the actual running clock. Hardware Manager needs an accessible
JTAG/hw_server path; SSH and UIO alone cannot read the current System ILA.

## Local validation

```sh
mkdir -p build/host
cc -std=c99 -O2 -Wall -Wextra -Werror software/aead_uio.c -o build/host/aead_uio
.venv/bin/python -m unittest discover -s software/tests -v
```

The local test compiles the C client and exercises its transport against a
single-input/single-output-slot model using fixtures produced by the independent
`cryptography` generator. It covers RFC encrypt/decrypt, bad tag, empty/partial
sections, a ChaCha20 block boundary, output framing and error/timeout recovery.
Benchmark tests also cover fixture capture, mode selection, warm-up, replay,
rate units and rejection of a faulty measured iteration, including a 16 KiB
payload. The model intentionally stops input consumption when output is held, exposing
clients that fail to interleave draining and feeding. Undefined-behavior checks
are enabled for that model. This does not replace RTL simulation or a real
board run; the FPGA board result must be reported separately.

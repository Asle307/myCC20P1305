# AXI DMA UIO test and throughput

`aead_dma_uio.c` is the C99 Linux host for this branch's
[AXI4-Stream/DMA peripheral](../docs/interfaces/aead_axi_dma.md). AXI4-Lite
programs session configuration; vendor AXI DMA transfers word-padded packets
between reserved DDR and the AEAD streams. The host checks the CONTROL
signature before accessing DMA registers.

Program the matching `vivado/kv260_dma/kv260_dma.xpr` bitstream and follow the
[KV260 integration guide](../vivado/kv260_dma/README.md). The existing board
checks and measured rates are recorded below with their boundaries. Local
transport-model tests cover host sequencing and fixture handling.

## Lab memory boundary

The default mappings match the user's existing lab image:

| Device | Base | Use |
|---|---:|---|
| `/dev/uio12` (`MY_IP`) | 0xA0000000 | AEAD registers and DMA at offset 0x10000 |
| `/dev/uio13` (`ddr_high`) | 0x800000000 | Reserved, uncached DMA data memory |

The host maps 68 KiB of control space and 512 KiB of DDR-high. TX starts at
offset 0; RX starts at offset 0x40000, so ranges are disjoint and aligned.
These bytes are scratch space during the test. Confirm `ddr_high` is still
reserved for the lab and its UIO mapping is uncached; do not substitute malloc,
ordinary cached RAM, arbitrary physical addresses or memory used by Linux.
Memory CPU accesses are aligned volatile 32-bit operations, with barriers
before DMA reads and before CPU verification of DMA writes.

This path assumes the selected PL master can use those physical DDR addresses
without SMMU translation. If the lab enables an IOMMU for that HP path, its
mapping must be provided by the platform; direct physical addresses cannot
replace Linux DMA mappings. UIO file descriptors stay open throughout the run,
including their runtime-PM references.

Only one client may use this peripheral/DMA/scratch region at a time. The
initial readiness check does not abort a preexisting busy session. An error
after starting this client's DMA transfer resets both DMA channels and the
AEAD wrapper. The host uses direct registers/polling, not interrupts, SG,
kernel DMAengine or the PS ZDMA devices listed as `dma-controller` in sysfs.

## Fixtures and deployment

### Align HP0 width with the running PS

The current board design configures `S_AXI_HP0_FPD` for **64 bits**. Loading
the PL bitstream does not apply PS initialization from the XSA. On this lab
image both HP0 AFIFM2 controls were read back as `0x000003b0`, selecting
**128 bits**. This mismatch can complete DMA without error while corrupting
data and leaving every other 8-byte group in RX unchanged.

AFIFM2 RDCTRL is at `0xFD380000`, WRCTRL at `0xFD380014`. In each, bits 1:0
select 0=128, 1=64, 2=32 bits. Vivado's ZynqMP register database and the
`PSU__SAXIGP2__DATA_WIDTH_INTERNAL` preset establish this mapping. Keep all
other control bits intact. With AEAD and both DMA directions idle, run after
programming the matching DMA bitstream:

```sh
sudo python3 - <<'PY'
import os, mmap, struct
ufd = os.open('/dev/uio12', os.O_RDWR | os.O_SYNC)
u = mmap.mmap(ufd, 0x11000, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ)
signature = struct.unpack_from('<I', u, 0)[0]
status = struct.unpack_from('<I', u, 4)[0]
tx = struct.unpack_from('<I', u, 0x10004)[0]
rx = struct.unpack_from('<I', u, 0x10034)[0]
assert signature & 0xffffff00 == 0x41453200, 'Load the AEAD DMA bitstream first'
assert status & 0x43 == 1 and tx & 3 and rx & 3, 'AEAD/DMA must be idle; no competing client'
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 4096, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ | mmap.PROT_WRITE, offset=0xFD380000)
r = struct.unpack_from('<I', m, 0)[0]
w = struct.unpack_from('<I', m, 0x14)[0]
struct.pack_into('<I', m, 0, (r & ~3) | 1)
struct.pack_into('<I', m, 0x14, (w & ~3) | 1)
rr = struct.unpack_from('<I', m, 0)[0]
ww = struct.unpack_from('<I', m, 0x14)[0]
print('HP0 RDCTRL: 0x%08x -> 0x%08x' % (r, rr))
print('HP0 WRCTRL: 0x%08x -> 0x%08x' % (w, ww))
assert rr == (r & ~3) | 1 and ww == (w & ~3) | 1, 'HP0 width readback failed'
m.close()
os.close(fd)
u.close()
os.close(ufd)
PY
```

For the observed state, both values become `0x000003b1`. This changes only
the current PS hardware state, not boot files. Recheck after reboot or
programming another design. A permanent platform setup should apply matching
PS initialization/AFI configuration; do not change these controls while a
master using HP0 is active.

After applying the 64-bit setting on the KV260, the RFC 8439 encrypt/decrypt
fixtures both passed every output word and expected AUTH_OK. Hardware cycle
snapshots were 616 (encrypt) and 531 (decrypt), matching the unstalled native
RTL results for 114 payload bytes / 12 AAD bytes. The diagnostic host elapsed
times include register reads and terminal printing and are not benchmark
results; use larger fixtures without `--diagnose` for throughput.

On the development machine, generate independent reference fixtures:

```sh
.venv/bin/python script/gen_vectors.py generate --profile rfc
scp software/aead_dma_uio.c debian@BOARD_IP:~/Asle/
scp -r dv/vectors/custom debian@BOARD_IP:~/Asle/dma_vectors
```

If custom already contains user data, use `--out dv/vectors/dma_rfc` and copy
that directory instead; do not overwrite unmarked/user fixtures. For the
bad-tag test, copy `dv/vectors/suite/rfc8439_bad_tag_dec` from the generated
full suite and pass that folder to `--case`.

On the board:

```sh
cd ~/Asle
gcc -std=c99 -O2 -Wall -Wextra aead_dma_uio.c -o aead_dma_uio
sudo ./aead_dma_uio --case dma_vectors/rfc8439_enc
sudo ./aead_dma_uio --case dma_vectors/rfc8439_dec
```

The host reads `rtl_config.hex`, `rtl_input.hex`, `rtl_input_control.hex` and
`rtl_expected.hex`. It skips native zero-byte terminators on the DMA wire,
preserves per-section word padding, arms S2MM before MM2S and issues core
START last. It checks core DONE/AUTH_OK, both DMA completion/error states,
actual received DMA length and every output word. RX is poisoned before each
run to expose stale data. Negative fixtures expect AUTH_OK=0 and compare
public tentative plaintext privately; no output is released to an application.
The DMA address upper registers are programmed, including the high `0x8`.

For a 16 KiB benchmark, generate a deterministic workload:

```sh
.venv/bin/python - <<'PY'
from pathlib import Path
Path('/tmp/dma_payload.bin').write_bytes(bytes(i & 255 for i in range(16384)))
Path('/tmp/dma_aad.bin').write_bytes(bytes(range(12)))
PY
.venv/bin/python script/gen_vectors.py generate --profile custom --case-name dma_16k \
    --key-hex 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f \
    --nonce-hex 000102030405060708090a0b --aad-file /tmp/dma_aad.bin \
    --plaintext-file /tmp/dma_payload.bin --mode both --out dv/vectors/dma_16k
scp -r dv/vectors/dma_16k debian@BOARD_IP:~/Asle/dma_16k
```

Check the board's actual PL clock, then supply that readback explicitly:

```sh
sudo cat /sys/kernel/debug/clk/clk_summary | grep -E '^[[:space:]]*pl0_ref[[:space:]]'
sudo ./aead_dma_uio --case dma_16k/dma_16k_enc \
    --iterations 100 --warmup 3 --clock-hz 199999998
sudo ./aead_dma_uio --case dma_16k/dma_16k_dec \
    --iterations 100 --warmup 3 --clock-hz 199999998
```

`--device`, `--memory`, `--timeout-ms` and `--inspect` are also supported.
`--inspect` validates/prints fixture transport sizes without hardware access.
Fixture sections are limited to 65536 bytes to match the DV generator.

## Measurement boundaries

PL reports mean/min/max accepted-START-to-DONE cycles across measured messages;
warm-ups are excluded. Conversion to microseconds/Mbit/s requires the supplied
actual clock. These cycles include DMA input starvation/output backpressure,
so they are not automatically the stall-free native core rate. Neither fixture
loading nor AXI-Lite/DMA setup before START is timed by the PL counter.

The host separately measures each verified transfer with CLOCK_MONOTONIC:
RX poisoning, key/nonce/length programming, DMA setup, core START, polling,
DDR completion and output verification are included. Initial mapping, fixture
loading, one-time DMA reset, TX buffer population and warm-ups are excluded.
Both rates count original payload bytes only, excluding AAD, tag and padding.
The fixed fixture key/nonce is replayed solely for synthetic testing.

### Observed KV260 results

After aligning the live HP0 width to 64 bits, the user ran the independent
16 KiB payload / 12-byte AAD fixtures with 100 measured messages and 3 warm-ups
per mode, without `--diagnose`. Linux reported PL0 at 199999998 Hz:

| Mode | PL cycles (mean/min/max) | PL payload rate | PL latency | Host verified rate | Host mean latency |
|---|---:|---:|---:|---:|---:|
| Encrypt | 54721 / 54721 / 54721 | 479.056 Mbit/s | 273.605 us | 172.798 Mbit/s | 758.529 us |
| Decrypt | 43714 / 43714 / 43714 | 599.680 Mbit/s | 218.570 us | 186.451 Mbit/s | 702.983 us |

All output words and expected AUTH_OK passed on every message. For this
workload the hardware cycle intervals equal the unstalled native RTL results;
the DMA path adds no net START-to-DONE cycles in these runs. This does not
establish Fmax or a workload-independent maximum throughput.

After resynchronizing the bad-tag fixture files on the board, the RFC
`rfc8439_bad_tag_dec` hardware test also passed: expected AUTH_OK=0 was
confirmed, every tentative plaintext word matched the public reference, and
the PL interval was 531 cycles, equal to the valid-tag RFC decrypt run.
Its single-message host timing is a functional-test observation, not the
16 KiB throughput benchmark above. The board has now passed RFC encrypt,
valid-tag decrypt and bad-tag rejection through the DMA path.

Local validation:

```sh
mkdir -p build/host
cc -std=c99 -O2 -Wall -Wextra -Werror software/aead_dma_uio.c -o build/host/aead_dma_uio
.venv/bin/python -m unittest discover -s software/tests -v
```

The transport model checks ordering, 64-bit addresses, empty directions,
partial/block-boundary/16 KiB fixtures, short RX, corrupt output, unexpected
auth, DMA/core errors, timeout and recovery, with undefined-behavior checking.
It does not simulate AXI DMA vendor IP, the SMMU, PS HP ports or physical DDR.

## Diagnose a board mismatch

Recompile the updated host and run one fixture with `--diagnose`:

```sh
sudo ./aead_dma_uio --case rfc8439_enc --diagnose
```

This checks CPU readback of the populated TX buffer and poisoned RX buffer,
then all key/nonce/length registers, before arming DMA. A preflight mismatch
returns without starting a DMA transfer. Such a mismatch must be resolved
before interpreting the result as a cryptographic failure.

On any transfer failure the host prints register and buffer snapshots **before**
resetting the DMA/core. These include completion/auth flags, cycles, actual
received length, address upper/lower registers and the first eight TX/RX words.
`--diagnose` also prints snapshots on success; its readback/printing cost is
included in host timing, so use normal mode for throughput measurements.

A successful DRC/timing report and a passing register/slot model do not prove
the CPU mapping and PL DMA see the same DDR bytes. Inspect this readback and
the DMA status before changing RTL, cache policy or PS protection settings.

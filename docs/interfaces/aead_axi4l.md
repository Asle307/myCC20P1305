# AEAD AXI4-Lite register interface

The packaged `aead_axi4l` IP follows the VJU_SoC lab's AXI-Lite peripheral and
user-compute boundary. Its adapter instantiates the unchanged
`aead_cc20_p1305` core and owns one input slot and one output slot. This is a
CPU polling interface, with no DMA, interrupt or whole-message buffer.

The IP has a 32-bit data bus and a six-bit local byte address: 16 word offsets,
64 bytes total. The system Address Editor may reserve a larger address window;
software must use the assigned base plus the offsets below. Use one clock
for `s00_axi_aclk` and the core, with active-low `s00_axi_aresetn`. No CDC is
implemented. The AXI address protection inputs are accepted but not interpreted.
The input clock inherits the connected source frequency; the package does
not force `FREQ_HZ=200000000`. Keep the design target at 200 MHz. With the
KV260 board preset, a requested 200 MHz PS PL0 clock can be reported as
199.998001 MHz; use that actual clock metadata throughout the AXI domain.
The standalone core's 5 ns timing constraint remains unchanged.

## Register map

All accesses are aligned 32-bit MMIO. Reads are snapshots with no side effects.
Ordinary writable fields honor `WSTRB`. Command bits are masked by `WSTRB`
and take effect once, when both AW and W have been accepted. AW may arrive
before W, with W, or after W. The peripheral supports one outstanding read
and one outstanding write, holding B/R responses under backpressure.

| Offset | Name | Access | Function |
|---|---|---|---|
| `0x00` | CONTROL | RW/commands | Mode and write-one commands below |
| `0x04` | STATUS | RO | Readiness, completion and error flags |
| `0x08`..`0x24` | KEY0..KEY7 | RW | Key, low word first |
| `0x28`..`0x30` | NONCE0..NONCE2 | RW | Nonce, low word first |
| `0x34` | INPUT_DATA | RW | Input staging word |
| `0x38` | STREAM_CTRL / OUTPUT_META | W / R | PUSH/POP and input metadata / held output metadata |
| `0x3c` | OUTPUT_DATA | RO | Held output word, or zero when slot empty |

CONTROL bits:

| Bit | Name | Meaning |
|---|---|---|
| 0 | START | Write 1 to launch a session; requires STATUS.START_READY |
| 1 | MODE_ENCRYPT | Stored mode: 1 encrypt, 0 decrypt |
| 2 | CLEAR_DONE | Write 1 to clear sticky DONE |
| 3 | SOFT_RESET | Abort core and clear slots, BUSY, pending START, DONE, AUTH_OK and ERROR |
| 4 | CLEAR_ERROR | Write 1 to clear sticky ERROR |

Only MODE_ENCRYPT reads back in CONTROL; commands read as zero. Include the
desired mode in each CONTROL write using byte lane 0: `0x03` starts encrypt,
`0x01` starts decrypt, `0x06` clears encrypt DONE, `0x04` clears decrypt DONE.
SOFT_RESET takes priority over START, preserves key/nonce and INPUT_DATA,
and stores the mode supplied in the same write. External reset also clears
configuration. A new START clears DONE and AUTH_OK. START is held internally
until the core accepts configuration; software never has to generate a pulse.

STATUS bits:

| Bit | Name | Meaning |
|---|---|---|
| 0 | START_READY | Core idle, no pending START and both stream slots empty |
| 1 | BUSY | Session launched and core has not reported DONE |
| 2 | DONE | Sticky core completion; remains until START, CLEAR_DONE or reset |
| 3 | AUTH_OK | Latched at core DONE; zero on encrypt, pass/fail on decrypt |
| 4 | INPUT_FREE | Active session can accept a PUSH into its input slot |
| 5 | OUTPUT_VALID | A word and its metadata are held for software |
| 6 | ERROR | Sticky invalid-access/invalid-command indication |
| 7 | START_PENDING | START captured by wrapper, awaiting core acceptance |

DONE means the core finished. The wrapper may still hold the final output
word: drain it before START_READY can become 1. INPUT_FREE describes the
wrapper slot, not instantaneous core readiness. A PUSH can wait there while
the core computes or is backpressured.

STREAM_CTRL write encoding:

| Bits | Name | Meaning |
|---|---|---|
| 2:0 | COUNT | Valid input bytes, 0..4 |
| 8 | IS_AAD | 1 AAD; 0 payload or received decrypt tag |
| 9 | LAST | Final word of this input section |
| 16 | PUSH | Copy INPUT_DATA and these metadata into the input slot |
| 17 | POP | Release the held output slot |

Use full 32-bit writes for PUSH so its metadata is supplied together. COUNT
must be 4 on nonfinal words; a zero-byte word requires LAST=1. Empty AAD and
empty payload each require a zero-byte LAST PUSH. Decrypt receives four tag
words after ciphertext: COUNT=4 throughout and LAST=1 only on word four.
Read both output data and metadata before writing POP (`0x00020000`). PUSH
and POP may be combined; the entire command is rejected if either is invalid.

OUTPUT_META reads COUNT[2:0], IS_TAG[8], LAST[9], VALID[16]; other bits zero.
The word and metadata remain fixed until POP. Reading OUTPUT_DATA never pops.
Both payload LAST and encrypt tag LAST mark their own section endings.

KEY/NONCE writes and changes of mode are rejected while a session is busy.
INPUT_DATA staging may be updated while the input slot is occupied; it cannot
change the already queued word. Failed PUSH, POP or START does not overwrite
or consume a slot. Read-only writes, unaligned accesses, count/framing errors
and commands used when not ready return AXI SLVERR and set ERROR. Zero WSTRB
is a no-op on aligned writes. Other reserved command bits are ignored.
The wrapper checks counts and basic LAST legality; the caller still owns
AAD/payload/tag order and section length scheduling.

## Software sequence

1. Poll START_READY; configure KEY/NONCE while idle.
2. Write CONTROL `0x03` for encrypt or `0x01` for decrypt.
3. Repeatedly read STATUS. When OUTPUT_VALID, read OUTPUT_META and OUTPUT_DATA,
   retain the result, then write POP. When INPUT_FREE and input remains,
   write INPUT_DATA followed by a PUSH with its metadata.
4. Send AAD, payload and, for decrypt, the received tag, in that order.
5. Finish when DONE is set, every expected output word has been read and
   the output slot is empty. Check AUTH_OK for decrypt.

Always interleave feeding input with draining output. Waiting to finish all
input before reading output can deadlock. Decrypt plaintext is tentative;
retain it privately until DONE and AUTH_OK=1, otherwise discard it. A failed
decrypt is a normal authentication result and does not set ERROR.

Key and nonce bytes are little endian inside each word. For RFC bytes
`80 81 82 83`, write KEY0=`0x83828180`. The example software self-test only
checks key/nonce/input staging readback while idle; it does not start AEAD.

## Validation and package

For the running lab Linux image, [the UIO host](../../software/README.md)
provides RFC encrypt/decrypt/bad-tag checks and a pause-before-START option
for ILA capture. It uses map0 at `0xA0000000` without the demo DMA driver.

`dv/tb_aead_axi4l.v` exercises the AXI bus and runs the generated suite through
the wrapper using MMIO reads/writes. The core GUI project saves `tb_aead_axi4l`
as its simulation top. Select `tb_aead` for native regression or
`tb_aead_custom` for custom fixtures; generate the matching catalog first.
The synthesis top remains `aead_cc20_p1305`. No AXI master VIP or board build
is required.

The package is `vivado/ip_repo/aead_axi4l_1_0`. It must contain the two AXI
files and all eight core RTL modules in its synthesis/simulation file groups.
After a user-opened IP editor detects disk changes, reload sources before
repackaging. In the board project, refresh the IP repository and upgrade its
instance to use the updated package. No bitstream or board integration is
implied by behavioral simulation.

Protocol and workflow sources: [integration references](integration_references.md).
The register/packet choices here are this IP's contract, not universal AXI requirements.

# AEAD AXI4-Stream/DMA peripheral interface

`aead_axi_dma` is the peripheral top: `rtl/aead_axi_dma.v` owns AXI4-Lite
control and the cycle counter; `rtl/aead_axis.v` adapts one AXI4-Stream input
packet and one output packet to the native `aead_cc20_p1305` word interface.
AXI DMA is a separate memory mover in the board design. The AEAD IP owns no
DDR master or whole-message RAM. All three peripheral buses use one clock
and synchronous active-low reset. The target is 200 MHz.

The CPU transfers key, nonce, lengths and commands through AXI4-Lite. Message
data uses only `S_AXIS` and `M_AXIS`. CONTROL reads a protocol signature so
software can reject incompatible hardware before accessing DMA registers.

## Registers

32-bit data, six-bit local address, 16 offsets / 64 bytes. AW and W are accepted
independently, WSTRB selects bytes, and B/R responses remain fixed when stalled.
Unaligned accesses, read-only writes and changes of configuration while busy
return SLVERR and set ERROR. Zero WSTRB is a no-op on aligned writes.

| Offset | Access | Function |
|---|---|---|
| 00 | RW/commands | START[0], ENCRYPT[1], CLEAR_DONE[2], SOFT_RESET[3], CLEAR_ERROR[4] |
| 04 | RO | READY[0], BUSY[1], sticky DONE[2], AUTH_OK[3], ERROR[6], cycle OVERFLOW[7] |
| 08..24 | RW | KEY0..7, low word first |
| 28..30 | RW | NONCE0..2, low word first |
| 34 | RW | Original AAD byte length |
| 38 | RW | Original plaintext/ciphertext byte length, excluding tag |
| 3c | RO | Last completed START-to-DONE cycle interval |

CONTROL reads `0x41453200 | (ENCRYPT << 1)`. Command bits read zero; they honor
WSTRB. Include the intended mode in each low-byte CONTROL write. START is
held until accepted, captures the session lengths, clears DONE/AUTH_OK/cycles
and starts one session. SOFT_RESET aborts control/stream state and clears
status/counters, preserving key/nonce/length configuration. CLEAR_ERROR alone
cannot recover a framing error held by the adapter: use SOFT_RESET.

`CYCLES` starts on the accepted native START, stops on the edge asserting core
DONE, and excludes the extra edge at which the wrapper observes registered
DONE. It includes OTK, AAD, payload, lengths, tag work and DMA starvation or
backpressure. The 32-bit counter saturates and marks OVERFLOW; reject that
measurement. It is stable until the next START/reset. It does not time DDR
write completion after the final stream beat, CPU buffer preparation or
software verification. Check both DMA completion and core DONE.

## DMA packet format

`pad4(X)` appends 0..3 zero bytes to a word boundary; `pad4(empty)` is empty.
Bytes within each word retain native little-endian order.

| Mode | MM2S input packet | S2MM output packet |
|---|---|---|
| Encrypt | `pad4(AAD) || pad4(plaintext)` | `pad4(ciphertext) || tag16` |
| Decrypt | `pad4(AAD) || pad4(ciphertext) || tag16` | `pad4(plaintext)` |

TKEEP is `1111` on **every** 32-bit beat, including the last. TLAST appears
only on the final beat of the entire packet. The two original length registers
identify section boundaries and exclude padding. The adapter derives native
byte counts and section-LAST internally and drops no actual message bytes.
Native unused input lanes are ignored; unused output lanes are zero. Encrypt's
payload-LAST must not terminate the DMA output packet before the tag.

This transport deliberately pads each section instead of placing a partial
KEEP beat in the middle of a packet or requiring a cross-word byte realigner.
It works with AXI DMA's full-word packet path and has at most six input padding
bytes. These padding bytes are not authenticated as additional message bytes.

Empty AAD/payload terminators are injected inside the adapter. Skip MM2S when
the input packet is empty (empty encrypt). Skip S2MM when the output packet is
empty (empty decrypt). Never program a zero DMA length. Empty decrypt still
transmits its received tag and checks authentication.

Invalid input TKEEP or early/missing TLAST on an accepted beat latches ERROR
and stops forwarding input. Missing input without an accepted malformed beat
requires the host timeout. Recovery resets **both DMA channels and the AEAD
wrapper**, then reloads/restarts the whole message. Discard partial output.

## Host order

1. Check CONTROL signature, READY and DMA ownership/idle state.
2. Populate a reserved uncached TX buffer and prepare a disjoint RX buffer.
3. Configure key, nonce and the original byte lengths while idle.
4. Clear prior DMA completion flags; arm S2MM first, then MM2S, skipping empty
   directions. The input adapter holds READY low until START.
5. Write START. Poll core ERROR/DONE and DMA errors/completion concurrently.
6. After both channels and core complete, check received DMA byte length,
   AUTH_OK, cycle validity and every expected output word (including padding).

Plaintext written to DDR remains tentative until DONE with AUTH_OK=1. The test
host compares public negative-test plaintext privately but releases no buffer
to an application. An integrating application must enforce the same rule.

See [KV260 GUI wiring](../../vivado/kv260_dma/README.md) and
[Linux fixture-based host](../../software/README.md). DV exercises the peripheral
and streams, not the vendor AXI DMA memory master or the board DDR path.

Protocol and workflow sources: [integration references](integration_references.md).
The register/packet choices here are this IP's contract, not universal AXI requirements.

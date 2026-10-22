# Native ChaCha20-Poly1305 design specification

## Scope and evidence

The top is `aead_cc20_p1305`; its eight Verilog-2001 modules are listed in the
[RTL analysis](vlsit_rtl_analysis.md). This is an implementation specification:
state transitions and timing come from RTL and its native benches. Functional
cryptographic requirements come from the [saved RFC 8439](../references.md),
sections 2.3, 2.5, 2.6 and 2.8. Other saved papers provide architecture context;
their measurements are not measurements of this design.

The document follows the VLSIT method of explicit ports, source-grounded
hierarchy, control flow and testable requirements. It intentionally uses a
focused soft-RTL contract rather than the complete ASIC template. Repository
naming and Verilog-2001 rules remain in `AGENTS.md`. There are no software-visible
registers, standard AXI ports, interrupt, CDC or whole-message RAM on this top.

## Function and data ownership

START captures the key, nonce and encrypt/decrypt mode. ChaCha20 counter zero
produces the one-time Poly1305 key. Payload counters start at one; each held
64-byte keystream serves four consecutive 16-byte payload blocks. Poly1305
clamps `r`, precomputes `r²`, and evaluates paired blocks through two arithmetic
lanes. The final authenticator is the low 128 bits of the accumulator plus `s`.

Both modes authenticate ciphertext. Encryption XORs plaintext before MAC;
decryption submits ciphertext to MAC before replacing the payload bank with
plaintext. AAD and ciphertext are independently padded to 16-byte boundaries.
The final Poly1305 block contains the two original byte lengths in LE64 order.
Padding and tag bytes are excluded from those lengths.

One cleared 128-bit assembly bank collects native words. A held 128-bit payload
bank serves XOR, MAC and output serialization. The core owns scheduling and
backpressure; the caller owns legal input framing and concurrent output drain.
See [AEAD architecture](aead.md) for register lifetimes and byte masking.

## External interface

See the [complete word contract](../interfaces/aead_stream.md) for all ports,
section framing and word order. Every port's function, qualifier, clock domain
and signal type is tabulated in [RTL analysis](vlsit_rtl_analysis.md#port-semantics).

| Group | Acceptance / validity | Contract |
|---|---|---|
| Configuration | `i_start && o_start_ready` | Key256, nonce96 and mode captured once; `i_mode=1` encrypts |
| Input | `i_data_valid && o_data_ready` | Little-endian word32, byte count 0..4, AAD/type and section-LAST held under stalls |
| Output | `o_data_valid && i_data_ready` | Word32, byte count, tag/type and section-LAST held under stalls |
| Completion | `o_done` | One-clock pulse after final tag output transfer or received-tag comparison |
| Authentication | completion | `o_auth_ok` holds until START/reset; zero for encrypt |

Input order is AAD, payload, then the received tag for decrypt. Every nonfinal
AAD/payload word has count 4. The final nonempty word has count 1..4 and LAST;
each empty input section instead requires one count-0 LAST word. The decrypt
tag is four count-4 words. A mismatched input type stalls without acceptance.
The core does not validate all illegal count/LAST sequences or time out.

Encryption emits ciphertext then four tag words, with section-LAST on both
the final ciphertext word and final tag word. Decryption emits only plaintext.
Empty payload emits no payload word. Unused output lanes are zero. Tag bytes
and words use little-endian order. Waiting for DONE before reading output can
deadlock a backpressured session.

## Clock, reset and pipeline validity

All logic uses the rising edge of `i_clk`. Active-low `i_rst_n` is sampled
synchronously. Reset aborts controller/valid state and completion/authentication;
it does not promise to erase key or datapath storage. New sessions start only
from IDLE, not on the same edge that completes the preceding message.

Quarter-round data registers advance when reset is deasserted; they have no
independent valid signal. `cc20_round` pipelines valid and column/diagonal mode
through the two QR stages. It accepts independent inputs every clock. The
single-context `cc20_core` instead launches dependent rounds every two clocks,
performs feed-forward and holds the resulting keystream until consumed.

## Control flow and arithmetic bounds

[Editable figures](../figures/README.md) show the actual state names and
transition conditions for AEAD, ChaCha20, Poly1305, lane control, multiplication
and reduction. A controller retains its state while its stated condition is
false. Reset and illegal-state recovery return these controllers to IDLE.

For Poly1305, `p=2^130-5`. Residues are canonical in `0..p-1`. Accumulator/block
and two-lane residue sums are each below `2p`, permitting one correction. The
260-bit product's first fold is below `6*2^130`; the second fold and parallel
carry-selected correction produce a canonical 130-bit result. See
[multiplication](multiplier.md) and [reduction](reducer.md) for equations and
stage boundaries.

## Cycle and size contracts

| Boundary | Cycles / bound | Qualification |
|---|---:|---|
| ChaCha20 request acceptance to result-valid | 41 | Result then holds until ready |
| Multiplier accepted START to DONE | 67 | DONE pulses; product held afterward |
| Pair arithmetic START to result-valid | 69 | Single or pair; output handshake may stall |
| Poly1305 key setup | 70 | OTK/r² schedule |
| Poly1305 accepted second block to pair accumulator update | 73 | Excludes input waits and tag stalls |
| Poly1305 accepted final singleton to accumulator update | 72 | Tag addition follows in one clock |
| Payload maximum | `(2^32-1)*64` bytes | Caller prevents ChaCha20 counter wrap |
| AAD length | unsigned 64-bit bytes | Caller prevents length overflow |

The native target clock is 5 ns / 200 MHz. Session cycles include OTK, AAD,
payload, lengths and tag processing; source starvation and output backpressure
add latency. Simulation at 5 ns is not an Fmax result. OOC comparisons target
XC7V2000T-FLG1925-2; require fresh reports with +0.050 ns setup margin and
nonnegative hold slack for an implementation claim.

## Requirements and verification traceability

| ID | Requirement | Evidence / verification method |
|---|---|---|
| REQ-CORE-001 | Match RFC 8439 encrypt/decrypt and 128-bit tag | `tb_aead` and generator RFC fixtures; independent `cryptography` outputs |
| REQ-CORE-002 | Authenticate ciphertext in both modes; reject modified tag/AAD/ciphertext | Suite negative fixtures, all 128 RFC tag-bit mutations and AUTH_OK checks |
| REQ-CORE-003 | Capture session configuration once on accepted START | `aead_runner` changes driven inputs after acceptance and checks results |
| REQ-CORE-004 | Preserve data and qualifiers under backpressure | `aead_stream_monitor`, stalled input/output suite and metadata comparison |
| REQ-CORE-005 | Preserve byte order, section-LAST and valid-byte masking | All AAD tails 1..16, ChaCha20 tails 1..64, empty/partial/boundary fixtures |
| REQ-CORE-006 | Use counter 0 only for OTK and retain each payload keystream block | Runner's internal request counter/order and block-reuse checks |
| REQ-CORE-007 | Maintain 41-clock ChaCha20 contract and per-session cycle bounds | Runner cycle monitor, exact unstalled RFC replays and deadlines |
| REQ-CORE-008 | Abort and recover from reset in all five tested phases | `tb_aead` OTK, partial assembly, stalled payload, stalled tag and partial received-tag resets |
| REQ-CORE-009 | Pulse DONE once and hold AUTH_OK until START/reset | Runner completion and authentication-hold checks |
| REQ-CORE-010 | Maintain the documented multiplier/reducer/Poly1305 stages | RTL review of state/register boundaries; top-level golden tests exercise arithmetic indirectly |

The suite contains 484 operations with default generator settings, including
248 authentication rejects. Regression adds five abort/recoveries and two
unstalled replays, giving 491 completed operations. `tb_aead_custom` runs only
its selected catalog. Generator unit tests verify fixture generation and
capture checks; they do not simulate hardware. There are no standalone
arithmetic benches or formal proofs in this repository: REQ-CORE-010's exact
internal latencies are implementation contracts supported by RTL inspection,
not independent direct block-test claims.

## Security and integration limits

The caller must use a unique nonce per key and message, hold tentative
plaintext privately until authenticated completion, and discard it on failure.
The interface provides no nonce allocator, access control, key erasure claim,
side-channel countermeasure, fault detection or retry protocol. Native framing
and size limits are caller obligations. These are integration boundaries of
this soft IP, not claims about a surrounding SoC or application.

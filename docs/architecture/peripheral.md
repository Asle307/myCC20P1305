# AXI4-Lite peripheral architecture

The peripheral top is `aead_axi4l`. Its `aead_axi4l_slave_lite_v1_0_S00_AXI`
instance contains the register slave, one input holding slot, one output
holding slot and the native `aead_cc20_p1305` algorithm core. The CPU transfers
every message word through MMIO and schedules one message at a time.

```text
CPU -> S00_AXI -> key / nonce / commands / status
CPU -> INPUT_DATA + PUSH -> input slot -> native AEAD word input
CPU <- OUTPUT_DATA/META  <- output slot <- native AEAD word output
CPU -> POP -------------------^
```

## Register and word ownership

The slave accepts AW and W independently and commits a write once both are
held. Byte strobes apply to writable registers and command bits. B/R responses
are registered and remain stable under backpressure. Reads are snapshots with
no side effects. A pending START holds until the native session handshake;
key, nonce and mode are captured by the core on that handshake. Illegal
commands and busy configuration writes return SLVERR and latch ERROR.

Software stages a 32-bit word in INPUT_DATA, then PUSH supplies its byte count,
AAD/type and section-LAST metadata. The input slot keeps those fields stable
until the native core accepts them. PUSH is legal only when the slot is free.
Software schedules AAD, payload and the received decrypt tag in that order,
including the native zero-byte terminators for empty AAD/payload sections.

The output slot captures payload/tag data and metadata. Software reads them
before POP releases that word. A held output propagates backpressure to the
core. Feed input and drain output concurrently: waiting for DONE before
reading can deadlock the single-word slots. Decrypt plaintext must be retained
privately until DONE with AUTH_OK=1 and discarded if authentication fails.

DONE and AUTH_OK are captured for polling. SOFT_RESET aborts the core, clears
slot/control state and preserves the configuration specified by the register
contract. This interface has no per-session hardware cycle counter; host
throughput includes MMIO feeding, draining and verification.

## Source, package and verification boundaries

The eight native algorithm modules under `rtl/` are authoritative. Their
byte-identical package copies and the two wrapper modules under
`vivado/ip_repo/aead_axi4l_1_0/hdl/` form the self-contained IP Catalog package.
Both wrapper modules are maintained under `rtl/` and copied byte-identically
into the package. `tb_aead_axi4l` uses the maintained RTL; no additional Edit IP
checkout is needed.

`vivado/kv260/kv260.xpr` connects the peripheral to PS HPM0 through SmartConnect
and monitors its AXI4-Lite bus with System ILA. All logic uses the PL0 clock
and synchronized active-low reset. `software/aead_uio.c` owns word scheduling,
polling, timeout recovery and output/authentication checks.

The peripheral bench runs the complete fixture suite and stresses AW/W order,
byte strobes, stalled responses, word slots and reset/error recovery. Native
benches retain algorithm cycle/handshake checks. The standalone OOC synthesis
top remains the native algorithm core for paper comparisons. Linux slot-model
tests verify host sequencing; physical board validation is separate.

See the [register contract](../interfaces/aead_axi4l.md),
[native architecture](aead.md), [package guide](../../vivado/ip_repo/aead_axi4l_1_0/README.md),
[board guide](../../vivado/kv260/README.md) and [host guide](../../software/README.md).

## Interface requirements and verification

| ID | Requirement | Verification |
|---|---|---|
| REQ-IF-001 | Accept AW and W independently; keep B/R stable while stalled | `tb_aead_axi4l` ordering, response-stall and byte-strobe checks |
| REQ-IF-002 | Preserve native byte order and authentication behavior | Complete independent fixture suite through the peripheral |
| REQ-IF-003 | Reject illegal accesses without corrupting held/configuration data | Peripheral SLVERR, zero-WSTRB, busy-command and recovery checks |
| REQ-IF-004 | Preserve output under backpressure and recover from abort | Peripheral stress and reset cases; host transport model |

See [interface analysis](interface_analysis.md), [editable figures](../figures/README.md)
and [integration references](../interfaces/integration_references.md). Native
core requirements remain in the native core specification.

# Native RTL analysis

## Scope and evidence

Source: `rtl/`, top `aead_cc20_p1305`, complete elaborated hierarchy. The eight
module definitions are expanded into thirteen physical instances, including
four generated QRs and two generated multiplier/reducer lanes. The native
project has one clock domain and no conditional transport configuration.

The VLSIT hierarchy parser was used for inventory; its symbolic generate-loop
instances were expanded against the actual constant loop bounds in RTL.
Functions and interfaces below are RTL-grounded; functional groups are explanatory
partitions of existing logic, not extra instantiated modules. The focused
workflow keeps complete tables here and compact diagrams in `docs/figures/`.
The library's visual/provenance checks do not establish protocol compliance.

## Instance inventory

| Depth | Instance path | Module | Parent | Instantiation source |
|---|---|---|---|---|
| 0 | `TOP` | `aead_cc20_p1305` | `-` | `rtl/aead_cc20_p1305.v` |
| 1 | `TOP/u_cc20_core` | `cc20_core` | `TOP` | `rtl/aead_cc20_p1305.v` |
| 1 | `TOP/u_p1305_core` | `p1305_core` | `TOP` | `rtl/aead_cc20_p1305.v` |
| 2 | `TOP/u_cc20_core/u_cc20_round` | `cc20_round` | `u_cc20_core` | `rtl/cc20_core.v` |
| 2 | `TOP/u_p1305_core/u_arithmetic` | `p1305_pair` | `u_p1305_core` | `rtl/p1305_core.v` |
| 3 | `TOP/u_cc20_core/u_cc20_round/g_qr[0]/u_qr` | `cc20_qr` | `u_cc20_round` | `rtl/cc20_round.v` |
| 3 | `TOP/u_cc20_core/u_cc20_round/g_qr[1]/u_qr` | `cc20_qr` | `u_cc20_round` | `rtl/cc20_round.v` |
| 3 | `TOP/u_cc20_core/u_cc20_round/g_qr[2]/u_qr` | `cc20_qr` | `u_cc20_round` | `rtl/cc20_round.v` |
| 3 | `TOP/u_cc20_core/u_cc20_round/g_qr[3]/u_qr` | `cc20_qr` | `u_cc20_round` | `rtl/cc20_round.v` |
| 3 | `TOP/u_p1305_core/u_arithmetic/g_lane[0]/u_mul` | `p1305_mul_r4` | `u_arithmetic` | `rtl/p1305_pair.v` |
| 3 | `TOP/u_p1305_core/u_arithmetic/g_lane[0]/u_reduce` | `p1305_reduce` | `u_arithmetic` | `rtl/p1305_pair.v` |
| 3 | `TOP/u_p1305_core/u_arithmetic/g_lane[1]/u_mul` | `p1305_mul_r4` | `u_arithmetic` | `rtl/p1305_pair.v` |
| 3 | `TOP/u_p1305_core/u_arithmetic/g_lane[1]/u_reduce` | `p1305_reduce` | `u_arithmetic` | `rtl/p1305_pair.v` |

## Module functions and functional groups

All groups use `i_clk`; reset/control contracts are in the core specification.
The Source links point to the maintained module, not a packaged copy.

### aead_cc20_p1305

Captures one session; assembles native words, schedules OTK/AAD/payload/length/tag work, retains each keystream slice, serializes output and compares the received tag.

Source: [rtl/aead_cc20_p1305.v](../../rtl/aead_cc20_p1305.v).

| Group | Principal state/signals | Function |
|---|---|---|
| Assembly | `r_input_data, r_input_byte_count, r_input_valid` | Native words and count/type/LAST become one zero-padded block. |
| Cipher/MAC scheduling | `r_state, r_key, r_nonce, r_mode` | Session control selects cipher-before-MAC or MAC-before-cipher and retains session configuration. |
| Payload and keystream | `r_payload_data, r_keystream_index, r_payload_counter` | A held payload bank consumes consecutive 16-byte slices of one 64-byte keystream. |
| Length accounting | `r_aad_length, r_payload_length` | Accepted actual bytes produce the final two-LE64 length block. |
| Serialization and completion | `r_output_word, o_done, o_auth_ok` | Held blocks become native output words; final transfer/comparison completes the session. |

### cc20_core

Captures a block counter, launches twenty dependent rounds, performs wordwise feed-forward and holds one 512-bit result until transfer. Key and nonce are supplied by the session owner.

Source: [rtl/cc20_core.v](../../rtl/cc20_core.v).

| Group | Principal state/signals | Function |
|---|---|---|
| Round launch and mapping | `r_round_index, w_round_input_state` | Initial state or the preceding round result starts the next dependent round. |
| Feed-forward and result bank | `r_working_state, r_counter` | The captured final round is added wordwise to the original counter/key/nonce state. |
| Request/result control | `r_state` | IDLE accepts one context; HOLD retains the result until ready. |

### cc20_round

Maps sixteen state words into four parallel quarter rounds, restores the matrix using the pipelined column/diagonal selection, and pipelines validity through two clocks.

Source: [rtl/cc20_round.v](../../rtl/cc20_round.v).

| Group | Principal state/signals | Function |
|---|---|---|
| Input word mapping | `w_qr_in, w_st_in` | Explicit column/diagonal rows feed four independent QRs. |
| Output matrix restoration | `w_st_out, w_qr_out` | Pipelined mode selects the inverse placement of returned words. |
| Mode and validity pipeline | `r_valid_p, r_diagonal_p` | Both qualifiers traverse the same two register stages as the QR data. |

### cc20_qr

Performs the four add/XOR/fixed-rotation algorithm steps, with a register boundary after step 2 and output registers after step 4. It samples each enabled clock without a valid/ready port.

Source: [rtl/cc20_qr.v](../../rtl/cc20_qr.v).

| Group | Principal state/signals | Function |
|---|---|---|
| Algorithm steps 1 and 2 | `r_a, r_b, r_c, r_d` | Add/XOR and rotate by 16 then 12; register the intermediate words. |
| Algorithm steps 3 and 4 | `o_a, o_b, o_c, o_d` | Add/XOR and rotate by 8 then 7; register the final words. |

### p1305_core

Clamps the one-time key, computes its square, buffers one or two encoded blocks, schedules parallel arithmetic, corrects pair sums, updates the accumulator and holds the final tag.

Source: [rtl/p1305_core.v](../../rtl/p1305_core.v).

| Group | Principal state/signals | Function |
|---|---|---|
| Key setup and encoding | `r_r, r_s, r_r2, w_block_n` | Clamp r, retain s, encode a full block with its fixed high one bit and compute r squared. |
| Block pairing | `r_n0, r_n1, r_pair_mode, r_pair_last` | Buffer the first block; accept a second block unless the first is the final singleton. |
| Operand selection and correction | `r_sum, w_sum_reduced, w_operand_a, w_operand_b` | Prepare h+n0, select r or r squared per lane, and canonically correct a pair sum. |
| Accumulator, tag and control | `r_h, r_state` | Capture residues, add s for the tag and retain the final result through its handshake. |

### p1305_pair

Starts lane zero in single mode or both lanes in pair mode; waits for multiplier completion, launches the corresponding reducers and holds an atomic result handshake.

Source: [rtl/p1305_pair.v](../../rtl/p1305_pair.v).

| Group | Principal state/signals | Function |
|---|---|---|
| Atomic lane scheduling | `r_state, r_pair_mode` | Coordinate single/pair starts, multiplier completion and reducer transfers. |
| Lane wiring | `w_operand_a, w_operand_b, w_product, w_residue` | Keep a separate product and canonical residue datapath for each physical lane. |

### p1305_mul_r4

Captures unsigned operands, prepares three-times-A, consumes sixty-five radix-4 digits and pulses completion after the preserved alignment/setup schedule.

Source: [rtl/p1305_mul_r4.v](../../rtl/p1305_mul_r4.v).

| Group | Principal state/signals | Function |
|---|---|---|
| Operand preparation | `r_operand_a, r_operand_b_shift, r_partial_3a` | Capture operands and precompute 3A once. |
| Radix-4 accumulation | `r_product_high, r_product_low` | Select 0/A/2A/3A from the current two-bit digit and shift generated product bits. |
| Iteration control | `r_state, r_iteration, o_done` | Preserve the setup/alignment slot and iterate sixty-five digits; pulse DONE. |

### p1305_reduce

Folds a 260-bit product modulo 2^130-5, combines the second fold with carry-selected canonical correction and holds the residue under backpressure.

Source: [rtl/p1305_reduce.v](../../rtl/p1305_reduce.v).

| Group | Principal state/signals | Function |
|---|---|---|
| Fold and canonical correction | `r_fold` | Store the first fold, then choose the unbiased or corrected second-fold candidate. |
| Held-result controller | `r_state` | IDLE captures a product, FINISH registers its residue and HOLD waits for consumption. |

## Interface inventory

Every interface below is **native/custom**, not AXI. A single connector in the
hierarchy represents a functional transaction; returned ready/data/status
signals are part of that group. Full endpoint ports are tabulated below.

| Group | Active endpoint to responding endpoint | Signals / behavior |
|---|---|---|
| CFG | Caller → AEAD | Key, nonce, mode and accepted START; START_READY returns readiness |
| IN | Caller → AEAD | Data/count/AAD/LAST with VALID/READY |
| OUT | AEAD → caller | Data/count/tag/LAST with VALID/READY |
| CMP | AEAD → caller | One-cycle DONE and held authentication result |
| CCREQ | AEAD → ChaCha20 core | Key, nonce, counter, request-valid and returned request-ready |
| CCRES | ChaCha20 core → AEAD | Held 512-bit keystream, result-valid and returned result-ready |
| ROUND | ChaCha20 core → round → core | 512-bit state, input valid and column/diagonal mode; output state/valid after two stages; no READY |
| QR | Round → each QR → round | Four 32-bit words in and out; fixed two-stage sampling, no independent validity |
| OTK | AEAD → Poly1305 | Low 256 keystream bits and start pulse while Poly1305 is idle |
| BLOCK | AEAD → Poly1305 | Full block128, valid/ready and final-length-block marker |
| TAG | Poly1305 → AEAD | Held tag128 with valid/ready |
| ARITH | Poly1305 → pair controller → Poly1305 | Four operand130 buses, start and pair-mode; two held residues and result-valid/ready |
| MUL0 / MUL1 | Pair controller → corresponding multiplier → pair controller | Two operands, start pulse, held product260 and one-cycle done |
| RED0 / RED1 | Pair controller → corresponding reducer → pair controller | Product260, start; residue130 and held valid/ready |
| CLK/RST | Integrator → all native instances | Common rising-edge clock and synchronous active-low reset; QR data registers hold while reset is asserted |

The two arithmetic lanes have independent products and residues. In single
mode only lane zero starts; pair mode waits for both multiplier completions
and consumes both reducer results atomically. Clock/reset is shared, not a
CDC boundary. Configuration is captured at AEAD START; standalone ChaCha20
users must keep key and nonce stable through feed-forward/result transfer.

## Port semantics

Every table is derived from the actual module header. Domain is `i_clk` for
all data/control ports. `pulse` means one rising-edge interval; `level` can
remain asserted across several clocks. Bus values are meaningful only under
the stated qualifier. Reset aborts validity/control, not all datapath contents.

### aead_cc20_p1305 ports

| Port | Direction / width | Domain | Type / qualifier | Function |
|---|---|---|---|---|
| `i_clk` | input `1 bit` | `i_clk` | clock | Rising-edge module clock |
| `i_rst_n` | input `1 bit` | `i_clk` | reset | Synchronous active-low reset; QR data holds while asserted |
| `i_key` | input `[255:0]` | `i_clk` | data (accepted START) | Key256 |
| `i_nonce` | input `[ 95:0]` | `i_clk` | data (accepted START) | Nonce96 |
| `i_start` | input `1 bit` | `i_clk` | level until accepted | Session acceptance with o_start_ready |
| `i_mode` | input `1 bit` | `i_clk` | level | 1 encrypt, 0 decrypt; captured at START |
| `o_start_ready` | output `1 bit` | `i_clk` | level | Acceptance qualifier for the associated VALID or START |
| `i_data` | input `[ 31:0]` | `i_clk` | data (i_data_valid) | Native input word32; hold under backpressure |
| `i_data_byte_count` | input `[  2:0]` | `i_clk` | data (associated word VALID) | Actual valid bytes of native word, 0..4 on input and 1..4 on output |
| `i_is_aad` | input `1 bit` | `i_clk` | data (associated VALID) | 1 AAD, 0 payload/received tag |
| `i_data_valid` | input `1 bit` | `i_clk` | level | Validity qualifier; handshake channels hold until READY, round pipeline has no READY |
| `o_data_ready` | output `1 bit` | `i_clk` | level | Acceptance qualifier for the associated VALID or START |
| `i_data_last` | input `1 bit` | `i_clk` | data (associated VALID) | Ends current native input section |
| `o_data` | output `[ 31:0]` | `i_clk` | data (o_data_valid) | Native output word32; hold under backpressure |
| `o_data_byte_count` | output `[  2:0]` | `i_clk` | data (associated word VALID) | Actual valid bytes of native word, 0..4 on input and 1..4 on output |
| `o_is_tag` | output `1 bit` | `i_clk` | data (associated VALID) | 1 encrypt tag, 0 payload |
| `o_data_last` | output `1 bit` | `i_clk` | data (associated VALID) | Ends current native output section |
| `o_data_valid` | output `1 bit` | `i_clk` | level | Validity qualifier; handshake channels hold until READY, round pipeline has no READY |
| `i_data_ready` | input `1 bit` | `i_clk` | level | Acceptance qualifier for the associated VALID or START |
| `o_done` | output `1 bit` | `i_clk` | pulse (1 clock) | Completion indication; sample result/authentication as specified |
| `o_auth_ok` | output `1 bit` | `i_clk` | level | Decrypt authentication result held until START/reset; zero for encrypt |

### cc20_core ports

| Port | Direction / width | Domain | Type / qualifier | Function |
|---|---|---|---|---|
| `i_clk` | input `1 bit` | `i_clk` | clock | Rising-edge module clock |
| `i_rst_n` | input `1 bit` | `i_clk` | reset | Synchronous active-low reset; QR data holds while asserted |
| `i_key` | input `[255:0]` | `i_clk` | data (accepted block request; key/nonce held through result transfer) | Key256 |
| `i_nonce` | input `[ 95:0]` | `i_clk` | data (accepted block request; key/nonce held through result transfer) | Nonce96 |
| `i_counter` | input `[ 31:0]` | `i_clk` | data (accepted block request; key/nonce held through result transfer) | ChaCha20 counter32 |
| `i_request_valid` | input `1 bit` | `i_clk` | level | Validity qualifier; handshake channels hold until READY, round pipeline has no READY |
| `o_request_ready` | output `1 bit` | `i_clk` | level | Acceptance qualifier for the associated VALID or START |
| `o_keystream` | output `[511:0]` | `i_clk` | data (o_result_valid) | Held ChaCha20 block512 |
| `o_result_valid` | output `1 bit` | `i_clk` | level | Validity qualifier; handshake channels hold until READY, round pipeline has no READY |
| `i_result_ready` | input `1 bit` | `i_clk` | level | Acceptance qualifier for the associated VALID or START |

### cc20_round ports

| Port | Direction / width | Domain | Type / qualifier | Function |
|---|---|---|---|---|
| `i_clk` | input `1 bit` | `i_clk` | clock | Rising-edge module clock |
| `i_rst_n` | input `1 bit` | `i_clk` | reset | Synchronous active-low reset; QR data holds while asserted |
| `i_state` | input `[511:0]` | `i_clk` | data (i_valid) | Round input state512, sixteen ascending little-endian words |
| `i_valid` | input `1 bit` | `i_clk` | level | Validity qualifier; handshake channels hold until READY, round pipeline has no READY |
| `i_diagonal` | input `1 bit` | `i_clk` | level | Column/diagonal selection accompanies i_valid through both stages |
| `o_state` | output `[511:0]` | `i_clk` | data (o_valid) | Round output state512, restored column/diagonal matrix |
| `o_valid` | output `1 bit` | `i_clk` | level | Validity qualifier; handshake channels hold until READY, round pipeline has no READY |

### cc20_qr ports

| Port | Direction / width | Domain | Type / qualifier | Function |
|---|---|---|---|---|
| `i_clk` | input `1 bit` | `i_clk` | clock | Rising-edge module clock |
| `i_rst_n` | input `1 bit` | `i_clk` | reset | Synchronous active-low reset; QR data holds while asserted |
| `i_a` | input `[31:0]` | `i_clk` | data (each clock with reset deasserted) | Quarter-round input word32; no valid/ready port |
| `i_b` | input `[31:0]` | `i_clk` | data (each clock with reset deasserted) | Quarter-round input word32; no valid/ready port |
| `i_c` | input `[31:0]` | `i_clk` | data (each clock with reset deasserted) | Quarter-round input word32; no valid/ready port |
| `i_d` | input `[31:0]` | `i_clk` | data (each clock with reset deasserted) | Quarter-round input word32; no valid/ready port |
| `o_a` | output `[31:0]` | `i_clk` | data (fixed two-stage latency) | Quarter-round output word32; validity belongs to parent round |
| `o_b` | output `[31:0]` | `i_clk` | data (fixed two-stage latency) | Quarter-round output word32; validity belongs to parent round |
| `o_c` | output `[31:0]` | `i_clk` | data (fixed two-stage latency) | Quarter-round output word32; validity belongs to parent round |
| `o_d` | output `[31:0]` | `i_clk` | data (fixed two-stage latency) | Quarter-round output word32; validity belongs to parent round |

### p1305_core ports

| Port | Direction / width | Domain | Type / qualifier | Function |
|---|---|---|---|---|
| `i_clk` | input `1 bit` | `i_clk` | clock | Rising-edge module clock |
| `i_rst_n` | input `1 bit` | `i_clk` | reset | Synchronous active-low reset; QR data holds while asserted |
| `i_otk` | input `[255:0]` | `i_clk` | data (i_start while idle) | One-time key256 |
| `i_start` | input `1 bit` | `i_clk` | pulse (1 clock while idle) | Start while idle; busy requests are not queued |
| `i_block` | input `[127:0]` | `i_clk` | data (i_block_valid) | Full Poly1305 block128, already RFC-padded by the owner |
| `i_block_valid` | input `1 bit` | `i_clk` | level | Validity qualifier; handshake channels hold until READY, round pipeline has no READY |
| `o_block_ready` | output `1 bit` | `i_clk` | level | Acceptance qualifier for the associated VALID or START |
| `i_last` | input `1 bit` | `i_clk` | data (associated VALID) | Final full Poly1305 block; AEAD marks the length block |
| `o_tag` | output `[127:0]` | `i_clk` | data (o_tag_valid) | Held Poly1305 tag128 |
| `o_tag_valid` | output `1 bit` | `i_clk` | level | Validity qualifier; handshake channels hold until READY, round pipeline has no READY |
| `i_tag_ready` | input `1 bit` | `i_clk` | level | Acceptance qualifier for the associated VALID or START |

### p1305_pair ports

| Port | Direction / width | Domain | Type / qualifier | Function |
|---|---|---|---|---|
| `i_clk` | input `1 bit` | `i_clk` | clock | Rising-edge module clock |
| `i_rst_n` | input `1 bit` | `i_clk` | reset | Synchronous active-low reset; QR data holds while asserted |
| `i_operand_a0` | input `[129:0]` | `i_clk` | data (i_start while idle) | Unsigned operand130 captured by the active arithmetic lane |
| `i_operand_b0` | input `[129:0]` | `i_clk` | data (i_start while idle) | Unsigned operand130 captured by the active arithmetic lane |
| `i_operand_a1` | input `[129:0]` | `i_clk` | data (i_start while idle) | Unsigned operand130 captured by the active arithmetic lane |
| `i_operand_b1` | input `[129:0]` | `i_clk` | data (i_start while idle) | Unsigned operand130 captured by the active arithmetic lane |
| `i_start` | input `1 bit` | `i_clk` | pulse (1 clock while idle) | Start while idle; busy requests are not queued |
| `i_pair_mode` | input `1 bit` | `i_clk` | level | 1 starts both arithmetic lanes, 0 lane zero; captured on start |
| `o_residue_0` | output `[129:0]` | `i_clk` | data (o_result_valid or o_valid) | Canonical residue130; lane one is ignored in single mode |
| `o_residue_1` | output `[129:0]` | `i_clk` | data (o_result_valid or o_valid) | Canonical residue130; lane one is ignored in single mode |
| `o_result_valid` | output `1 bit` | `i_clk` | level | Validity qualifier; handshake channels hold until READY, round pipeline has no READY |
| `i_result_ready` | input `1 bit` | `i_clk` | level | Acceptance qualifier for the associated VALID or START |

### p1305_mul_r4 ports

| Port | Direction / width | Domain | Type / qualifier | Function |
|---|---|---|---|---|
| `i_clk` | input `1 bit` | `i_clk` | clock | Rising-edge module clock |
| `i_rst_n` | input `1 bit` | `i_clk` | reset | Synchronous active-low reset; QR data holds while asserted |
| `i_operand_a` | input `[129:0]` | `i_clk` | data (i_start while idle) | Unsigned operand130 captured by the active arithmetic lane |
| `i_operand_b` | input `[129:0]` | `i_clk` | data (i_start while idle) | Unsigned operand130 captured by the active arithmetic lane |
| `i_start` | input `1 bit` | `i_clk` | pulse (1 clock while idle) | Start while idle; busy requests are not queued |
| `o_product` | output `[259:0]` | `i_clk` | data (o_done, then held) | Unsigned product260; intermediate before completion |
| `o_done` | output `1 bit` | `i_clk` | pulse (1 clock) | Completion indication; sample result/authentication as specified |

### p1305_reduce ports

| Port | Direction / width | Domain | Type / qualifier | Function |
|---|---|---|---|---|
| `i_clk` | input `1 bit` | `i_clk` | clock | Rising-edge module clock |
| `i_rst_n` | input `1 bit` | `i_clk` | reset | Synchronous active-low reset; QR data holds while asserted |
| `i_product` | input `[259:0]` | `i_clk` | data (i_start while idle) | Full product260 sampled for first fold |
| `i_start` | input `1 bit` | `i_clk` | pulse (1 clock while idle) | Start while idle; busy requests are not queued |
| `o_residue` | output `[129:0]` | `i_clk` | data (o_result_valid or o_valid) | Canonical residue130; lane one is ignored in single mode |
| `o_valid` | output `1 bit` | `i_clk` | level | Validity qualifier; handshake channels hold until READY, round pipeline has no READY |
| `i_ready` | input `1 bit` | `i_clk` | level | Acceptance qualifier for the associated VALID or START |

## Conditional and unresolved items

Generate bounds are fixed: four QR instances and two arithmetic lanes. No
unresolved module is present in the native RTL. No external bus-compliance
claim is made. Internal latency contracts are inspected in RTL and exercised
through top-level golden tests; direct arithmetic unit timing tests and formal
proofs are outside the maintained DV scope. The compact diagrams abstract
signal groups; this report and module port maps remain the detailed evidence.

## Controller transition inventory

A false condition retains state; synchronous reset and default recovery return
to ST_IDLE. Conditions below are concise descriptions of RTL expressions.

### aead_cc20_p1305 transitions

| ID | From | To | Condition / action |
|---|---|---|---|
| T01 | `ST_IDLE` | `ST_OTK_REQUEST` | START accepted |
| T02 | `ST_OTK_REQUEST` | `ST_OTK_WAIT` | request ready |
| T03 | `ST_OTK_WAIT` | `ST_AAD` | result valid |
| T04 | `ST_AAD` | `ST_PAYLOAD` | AAD block transfer / section LAST |
| T05 | `ST_PAYLOAD` | `ST_LENGTH` | empty payload block |
| T06 | `ST_PAYLOAD` | `ST_PAYLOAD_MAC` | decrypt / nonempty |
| T07 | `ST_PAYLOAD` | `ST_PAYLOAD_XOR` | encrypt / keystream retained |
| T08 | `ST_PAYLOAD` | `ST_CC_REQUEST` | encrypt / need keystream |
| T09 | `ST_CC_REQUEST` | `ST_CC_WAIT` | request ready |
| T10 | `ST_CC_WAIT` | `ST_PAYLOAD_XOR` | result valid |
| T11 | `ST_PAYLOAD_XOR` | `ST_PAYLOAD_MAC` | encrypt |
| T12 | `ST_PAYLOAD_XOR` | `ST_PAYLOAD_OUTPUT` | decrypt |
| T13 | `ST_PAYLOAD_MAC` | `ST_PAYLOAD_OUTPUT` | MAC transfer / encrypt |
| T14 | `ST_PAYLOAD_MAC` | `ST_PAYLOAD_XOR` | MAC transfer / decrypt / retained |
| T15 | `ST_PAYLOAD_MAC` | `ST_CC_REQUEST` | MAC transfer / decrypt / need keystream |
| T16 | `ST_PAYLOAD_OUTPUT` | `ST_PAYLOAD` | output block transfer / more |
| T17 | `ST_PAYLOAD_OUTPUT` | `ST_LENGTH` | output block transfer / LAST |
| T18 | `ST_LENGTH` | `ST_TAG_OUTPUT` | MAC transfer / encrypt |
| T19 | `ST_LENGTH` | `ST_TAG_INPUT` | MAC transfer / decrypt |
| T20 | `ST_TAG_OUTPUT` | `ST_IDLE` | final tag output transfer |
| T21 | `ST_TAG_INPUT` | `ST_IDLE` | received tag block transfer |

### cc20_core transitions

| ID | From | To | Condition / action |
|---|---|---|---|
| T01 | `ST_IDLE` | `ST_ROUND` | request VALID and READY |
| T02 | `ST_ROUND` | `ST_ROUND` | round result / index below 19 |
| T03 | `ST_ROUND` | `ST_FEED_FORWARD` | round 19 result |
| T04 | `ST_FEED_FORWARD` | `ST_HOLD` | one clock |
| T05 | `ST_HOLD` | `ST_IDLE` | result READY |

### p1305_core transitions

| ID | From | To | Condition / action |
|---|---|---|---|
| T01 | `ST_IDLE` | `ST_R2_WAIT` | START while idle |
| T02 | `ST_R2_WAIT` | `ST_ACCEPT_FIRST` | arithmetic transfer |
| T03 | `ST_ACCEPT_FIRST` | `ST_WAIT_SECOND` | block transfer / not LAST |
| T04 | `ST_ACCEPT_FIRST` | `ST_SUM` | block transfer / final singleton |
| T05 | `ST_WAIT_SECOND` | `ST_SUM` | second block transfer |
| T06 | `ST_SUM` | `ST_MUL_START` | one clock |
| T07 | `ST_MUL_START` | `ST_WAIT_ARITH` | one clock |
| T08 | `ST_WAIT_ARITH` | `ST_PAIR_CORRECT` | arithmetic transfer / pair |
| T09 | `ST_WAIT_ARITH` | `ST_TAG_MAKE` | arithmetic transfer / singleton |
| T10 | `ST_PAIR_CORRECT` | `ST_ACCEPT_FIRST` | more pairs |
| T11 | `ST_PAIR_CORRECT` | `ST_TAG_MAKE` | final pair |
| T12 | `ST_TAG_MAKE` | `ST_TAG_HOLD` | one clock |
| T13 | `ST_TAG_HOLD` | `ST_IDLE` | tag VALID and READY |

### p1305_pair transitions

| ID | From | To | Condition / action |
|---|---|---|---|
| T01 | `ST_IDLE` | `ST_WAIT_MUL` | START while idle |
| T02 | `ST_WAIT_MUL` | `ST_WAIT_REDUCE` | active multiplier DONE / start reduce |
| T03 | `ST_WAIT_REDUCE` | `ST_IDLE` | active residues VALID and result READY |

### p1305_mul_r4 transitions

| ID | From | To | Condition / action |
|---|---|---|---|
| T01 | `ST_IDLE` | `ST_SETUP` | START while idle |
| T02 | `ST_SETUP` | `ST_WAIT_ITER` | one clock |
| T03 | `ST_WAIT_ITER` | `ST_ITER` | one clock |
| T04 | `ST_ITER` | `ST_ITER` | digits 0..63 |
| T05 | `ST_ITER` | `ST_IDLE` | digit 64 / pulse DONE |

### p1305_reduce transitions

| ID | From | To | Condition / action |
|---|---|---|---|
| T01 | `ST_IDLE` | `ST_FINISH` | START while idle |
| T02 | `ST_FINISH` | `ST_HOLD` | one clock |
| T03 | `ST_HOLD` | `ST_IDLE` | result READY |

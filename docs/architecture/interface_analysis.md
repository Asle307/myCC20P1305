# axi4l-axi4s-dma interface RTL analysis

## Scope and instance hierarchy

The complete native hierarchy and eight-module port tables remain in
[vlsit_rtl_analysis.md](vlsit_rtl_analysis.md). This document adds the two
maintained adapter modules under `rtl/`. Their packaged copies are identical;
the native controller, stage boundaries and framing contract are unchanged.

```text
TOP (aead_axi_dma)
  u_stream (aead_axis)
    u_aead (aead_cc20_p1305)
      [all 12 descendants expanded in the inherited native analysis]
```

`aead_axi_dma` owns AXI-Lite AW/W capture, byte-strobe writes, aligned read
snapshots, busy/configuration checks, sticky status, soft abort and the
saturating cycle counter. `aead_axis` owns original-length counters, section
scheduling, native empty terminators, final-packet TLAST selection and the
framing-error latch. It forwards native output backpressure without a message
buffer. The vendor AXI DMA belongs to the BD and is external to this hierarchy.

| Group | Initiator → responder | Signals and type |
|---|---|---|
| LITE | CPU/interconnect → control slave | AWADDR/AWPROT/AWVALID/AWREADY; WDATA/WSTRB/WVALID/WREADY; BRESP/BVALID/BREADY; ARADDR/ARPROT/ARVALID/ARREADY; RDATA/RRESP/RVALID/RREADY |
| CTRL | Control registers → packet adapter | Key256, nonce96, AAD/payload bytes32, start/mode and returned ready/done/auth/error |
| S_AXIS | DMA MM2S → adapter | TDATA32/TKEEP4/TVALID/TLAST with returned TREADY |
| M_AXIS | Adapter → DMA S2MM | TDATA32/TKEEP4/TVALID/TLAST with returned TREADY |
| CORE | Packet adapter → native AEAD | All native configuration, input, output and completion ports; see inherited native port table |

The adapter's five actual states are IDLE, AAD, PAYLOAD, TAG and WAIT. An
accepted native section-LAST moves to the next section; empty sections inject
one native count-0 LAST beat without consuming a stream word. Decrypt TAG
consumes four full words. WAIT returns to IDLE on core DONE. An accepted
bad KEEP/TLAST sets `o_error` and disables further input forwarding; it is a
sticky error flag, **not another FSM state**. SOFT_RESET resets the adapter
and core; the host also resets both DMA channels before restarting the message.

The cycle counter starts at accepted native START. It snapshots the interval
at the wrapper's observation of registered core DONE, excluding that observer
edge. DMA starvation/backpressure is included; DDR completion and CPU
verification are not. Overflow saturates and invalidates the measurement.

## Clock, reset, widths and source ownership

All peripheral and native signals use the same clock; reset is synchronous
active-low in the maintained RTL. The package associates every bus with its
clock and reset and inherits frequency from the connected source. There is
no CDC. Each module header below lists every port. AXI qualifiers/ready are
levels; data and metadata are valid only with the corresponding VALID signal.
READY/VALID transfers occur on rising edges. Protection attributes are accepted
but not interpreted. The 32-bit data path and six-bit local address map are
the supported peripheral configuration.

### aead_axi_dma ports

Source: [rtl/aead_axi_dma.v](../../rtl/aead_axi_dma.v).

| Port | Direction / width | Domain | Type / qualifier | Function |
|---|---|---|---|---|
| `i_clk` | input `1 bit` | `i_clk` | clock | Module rising-edge clock |
| `i_rst_n` | input `1 bit` | `i_clk` | reset | Synchronous active-low abort/reset |
| `s_axi_wdata` | input `[31:0]` | `i_clk` | data (associated VALID) | Write data32, sampled at W handshake |
| `s_axi_awaddr` | input `[ 5:0]` | `i_clk` | data (associated VALID) | Write byte address, aligned 32-bit accesses |
| `s_axi_wstrb` | input `[ 3:0]` | `i_clk` | data (associated VALID) | Per-byte write enables, sampled at W handshake |
| `s_axi_awprot` | input `[ 2:0]` | `i_clk` | data (associated VALID) | Write protection attributes, accepted but not interpreted |
| `s_axi_awvalid` | input `1 bit` | `i_clk` | level | Associated AXI channel validity/acceptance |
| `s_axi_awready` | output `1 bit` | `i_clk` | level | Associated AXI channel validity/acceptance |
| `s_axi_wvalid` | input `1 bit` | `i_clk` | level | Associated AXI channel validity/acceptance |
| `s_axi_wready` | output `1 bit` | `i_clk` | level | Associated AXI channel validity/acceptance |
| `s_axi_bresp` | output `[ 1:0]` | `i_clk` | data (associated VALID) | Held write response: OKAY or SLVERR |
| `s_axi_bvalid` | output `1 bit` | `i_clk` | level | Associated AXI channel validity/acceptance |
| `s_axi_bready` | input `1 bit` | `i_clk` | level | Associated AXI channel validity/acceptance |
| `s_axi_rdata` | output `[31:0]` | `i_clk` | data (associated VALID) | Read snapshot held with RVALID |
| `s_axi_araddr` | input `[ 5:0]` | `i_clk` | data (associated VALID) | Read byte address, aligned 32-bit accesses |
| `s_axi_arprot` | input `[ 2:0]` | `i_clk` | data (associated VALID) | Read protection attributes, accepted but not interpreted |
| `s_axi_rresp` | output `[ 1:0]` | `i_clk` | data (associated VALID) | Held read response: OKAY or SLVERR |
| `s_axi_arvalid` | input `1 bit` | `i_clk` | level | Associated AXI channel validity/acceptance |
| `s_axi_arready` | output `1 bit` | `i_clk` | level | Associated AXI channel validity/acceptance |
| `s_axi_rvalid` | output `1 bit` | `i_clk` | level | Associated AXI channel validity/acceptance |
| `s_axi_rready` | input `1 bit` | `i_clk` | level | Associated AXI channel validity/acceptance |
| `s_axis_tdata` | input `[31:0]` | `i_clk` | data (TVALID) | Word-padded message word32 |
| `s_axis_tkeep` | input `[ 3:0]` | `i_clk` | data (TVALID) | Full 4-byte mask on every transport beat |
| `s_axis_tvalid` | input `1 bit` | `i_clk` | level | Stream validity/acceptance |
| `s_axis_tlast` | input `1 bit` | `i_clk` | data (TVALID) | Only the final beat of the whole transport packet |
| `s_axis_tready` | output `1 bit` | `i_clk` | level | Stream validity/acceptance |
| `m_axis_tdata` | output `[31:0]` | `i_clk` | data (TVALID) | Word-padded message word32 |
| `m_axis_tkeep` | output `[ 3:0]` | `i_clk` | data (TVALID) | Full 4-byte mask on every transport beat |
| `m_axis_tvalid` | output `1 bit` | `i_clk` | level | Stream validity/acceptance |
| `m_axis_tlast` | output `1 bit` | `i_clk` | data (TVALID) | Only the final beat of the whole transport packet |
| `m_axis_tready` | input `1 bit` | `i_clk` | level | Stream validity/acceptance |

### aead_axis ports

Source: [rtl/aead_axis.v](../../rtl/aead_axis.v).

| Port | Direction / width | Domain | Type / qualifier | Function |
|---|---|---|---|---|
| `i_clk` | input `1 bit` | `i_clk` | clock | Module rising-edge clock |
| `i_rst_n` | input `1 bit` | `i_clk` | reset | Synchronous active-low abort/reset |
| `i_key` | input `[255:0]` | `i_clk` | data (accepted START) | Session key/nonce or original section byte length; excludes tag/padding |
| `i_nonce` | input `[ 95:0]` | `i_clk` | data (accepted START) | Session key/nonce or original section byte length; excludes tag/padding |
| `i_aad_bytes` | input `[ 31:0]` | `i_clk` | data (accepted START) | Session key/nonce or original section byte length; excludes tag/padding |
| `i_payload_bytes` | input `[ 31:0]` | `i_clk` | data (accepted START) | Session key/nonce or original section byte length; excludes tag/padding |
| `i_start` | input `1 bit` | `i_clk` | level until accepted | Start packet conversion on native session acceptance |
| `i_mode` | input `1 bit` | `i_clk` | level | Encrypt/decrypt mode captured at accepted START |
| `o_start_ready` | output `1 bit` | `i_clk` | level | Idle, error-free adapter and native core can accept START |
| `o_done` | output `1 bit` | `i_clk` | pulse (1 clock) | Native completion pulse forwarded by adapter |
| `o_auth_ok` | output `1 bit` | `i_clk` | level | Native authentication result held until START/reset |
| `o_error` | output `1 bit` | `i_clk` | level | Framing-error latch; cleared by reset |
| `s_axis_tdata` | input `[31:0]` | `i_clk` | data (TVALID) | Word-padded message word32 |
| `s_axis_tkeep` | input `[ 3:0]` | `i_clk` | data (TVALID) | Full 4-byte mask on every transport beat |
| `s_axis_tvalid` | input `1 bit` | `i_clk` | level | Stream validity/acceptance |
| `s_axis_tlast` | input `1 bit` | `i_clk` | data (TVALID) | Only the final beat of the whole transport packet |
| `s_axis_tready` | output `1 bit` | `i_clk` | level | Stream validity/acceptance |
| `m_axis_tdata` | output `[31:0]` | `i_clk` | data (TVALID) | Word-padded message word32 |
| `m_axis_tkeep` | output `[ 3:0]` | `i_clk` | data (TVALID) | Full 4-byte mask on every transport beat |
| `m_axis_tvalid` | output `1 bit` | `i_clk` | level | Stream validity/acceptance |
| `m_axis_tlast` | output `1 bit` | `i_clk` | data (TVALID) | Only the final beat of the whole transport packet |
| `m_axis_tready` | input `1 bit` | `i_clk` | level | Stream validity/acceptance |

## Verification and external boundaries

Plaintext is never application-authenticated until AUTH_OK=1. A fixture that
expects AUTH_OK=0 may pass the negative test, while its plaintext remains
private and is discarded; reporting a verified fixture is not releasing data.

`tb_aead_axi_dma` exercises the complete fixture catalog plus channel/order/error
stress. Host-model tests check software sequencing independently of physical
board access. Package integrity and BD validation check project/package
structure; they do not prove physical DDR behavior or implementation timing.
See [peripheral requirements](peripheral.md#interface-requirements-and-verification),
[peripheral DV](../../dv/INTERFACE.md), [figures](../figures/README.md) and
[integration references](../interfaces/integration_references.md).

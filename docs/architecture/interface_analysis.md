# axi4l interface RTL analysis

## Scope and instance hierarchy

The complete native hierarchy and eight-module port tables remain in
[vlsit_rtl_analysis.md](vlsit_rtl_analysis.md). This document adds the two
maintained adapter modules under `rtl/`. Their packaged copies are identical;
the native controller, stage boundaries and framing contract are unchanged.

```text
TOP (aead_axi4l)
  aead_axi4l_slave_lite_v1_0_S00_AXI_inst (aead_axi4l_slave_lite_v1_0_S00_AXI)
    u_aead (aead_cc20_p1305)
      [all 12 descendants expanded in the inherited native analysis]
```

The outer module supplies parameters and named bus wiring. The slave adapter
owns independent AW/W holding registers, a single outstanding B response,
an aligned AR snapshot and a single outstanding R response. Its configuration
bank stores key/nonce/mode. A held START waits for native acceptance; busy
configuration changes and illegal commands return SLVERR without partially
applying them. ERROR is sticky and wins over an error-clear on the same edge.

The input staging register is separate from the input holding slot. PUSH
captures staging data plus count/AAD/LAST only while INPUT_FREE. Native input
VALID remains set until acceptance. The output slot captures native data and
metadata while free and belongs to software until POP; reads do not pop it.
The native core stalls while that slot is occupied. Software must interleave
input feed and output drain. SOFT_RESET aborts the core and clears both slots,
completion/authentication/error, while preserving key/nonce and staging data
and writing the mode supplied by that CONTROL command.

| Group | Initiator → responder | Signals and type |
|---|---|---|
| S00_AXI | CPU/interconnect → outer module → slave | AWADDR/AWPROT/AWVALID/AWREADY; WDATA/WSTRB/WVALID/WREADY; BRESP/BVALID/BREADY; ARADDR/ARPROT/ARVALID/ARREADY; RDATA/RRESP/RVALID/RREADY |
| CORE | Slot adapter → native AEAD | All native configuration, input, output and completion ports; see inherited native port table |

The transaction flowcharts describe register/slot ownership; their process
boxes are explanatory phases, **not extra RTL state encodings**. B/R responses
remain stable until READY. Configuration is captured by the native core on
START acceptance, not on the CPU's earlier configuration writes.

## Clock, reset, widths and source ownership

All peripheral and native signals use the same clock; reset is synchronous
active-low in the maintained RTL. The package associates every bus with its
clock and reset and inherits frequency from the connected source. There is
no CDC. Each module header below lists every port. AXI qualifiers/ready are
levels; data and metadata are valid only with the corresponding VALID signal.
READY/VALID transfers occur on rising edges. Protection attributes are accepted
but not interpreted. The 32-bit data path and six-bit local address map are
the supported peripheral configuration.

### aead_axi4l ports

Source: [rtl/aead_axi4l.v](../../rtl/aead_axi4l.v).

| Port | Direction / width | Domain | Type / qualifier | Function |
|---|---|---|---|---|
| `s00_axi_aclk` | input `1 bit` | `s00_axi_aclk` | clock | Module rising-edge clock |
| `s00_axi_aresetn` | input `1 bit` | `s00_axi_aclk` | reset | Synchronous active-low abort/reset |
| `s00_axi_awaddr` | input `[C_S00_AXI_ADDR_WIDTH-1 : 0]` | `s00_axi_aclk` | data (associated VALID) | Write byte address, aligned 32-bit accesses |
| `s00_axi_awprot` | input `[2 : 0]` | `s00_axi_aclk` | data (associated VALID) | Write protection attributes, accepted but not interpreted |
| `s00_axi_awvalid` | input `1 bit` | `s00_axi_aclk` | level | Associated AXI channel validity/acceptance |
| `s00_axi_awready` | output `1 bit` | `s00_axi_aclk` | level | Associated AXI channel validity/acceptance |
| `s00_axi_wdata` | input `[C_S00_AXI_DATA_WIDTH-1 : 0]` | `s00_axi_aclk` | data (associated VALID) | Write data32, sampled at W handshake |
| `s00_axi_wstrb` | input `[(C_S00_AXI_DATA_WIDTH/8)-1 : 0]` | `s00_axi_aclk` | data (associated VALID) | Per-byte write enables, sampled at W handshake |
| `s00_axi_wvalid` | input `1 bit` | `s00_axi_aclk` | level | Associated AXI channel validity/acceptance |
| `s00_axi_wready` | output `1 bit` | `s00_axi_aclk` | level | Associated AXI channel validity/acceptance |
| `s00_axi_bresp` | output `[1 : 0]` | `s00_axi_aclk` | data (associated VALID) | Held write response: OKAY or SLVERR |
| `s00_axi_bvalid` | output `1 bit` | `s00_axi_aclk` | level | Associated AXI channel validity/acceptance |
| `s00_axi_bready` | input `1 bit` | `s00_axi_aclk` | level | Associated AXI channel validity/acceptance |
| `s00_axi_araddr` | input `[C_S00_AXI_ADDR_WIDTH-1 : 0]` | `s00_axi_aclk` | data (associated VALID) | Read byte address, aligned 32-bit accesses |
| `s00_axi_arprot` | input `[2 : 0]` | `s00_axi_aclk` | data (associated VALID) | Read protection attributes, accepted but not interpreted |
| `s00_axi_arvalid` | input `1 bit` | `s00_axi_aclk` | level | Associated AXI channel validity/acceptance |
| `s00_axi_arready` | output `1 bit` | `s00_axi_aclk` | level | Associated AXI channel validity/acceptance |
| `s00_axi_rdata` | output `[C_S00_AXI_DATA_WIDTH-1 : 0]` | `s00_axi_aclk` | data (associated VALID) | Read snapshot held with RVALID |
| `s00_axi_rresp` | output `[1 : 0]` | `s00_axi_aclk` | data (associated VALID) | Held read response: OKAY or SLVERR |
| `s00_axi_rvalid` | output `1 bit` | `s00_axi_aclk` | level | Associated AXI channel validity/acceptance |
| `s00_axi_rready` | input `1 bit` | `s00_axi_aclk` | level | Associated AXI channel validity/acceptance |

### aead_axi4l_slave_lite_v1_0_S00_AXI ports

Source: [rtl/aead_axi4l_slave_lite_v1_0_S00_AXI.v](../../rtl/aead_axi4l_slave_lite_v1_0_S00_AXI.v).

| Port | Direction / width | Domain | Type / qualifier | Function |
|---|---|---|---|---|
| `S_AXI_ACLK` | input `1 bit` | `S_AXI_ACLK` | clock | Module rising-edge clock |
| `S_AXI_ARESETN` | input `1 bit` | `S_AXI_ACLK` | reset | Synchronous active-low abort/reset |
| `S_AXI_AWADDR` | input `[C_S_AXI_ADDR_WIDTH-1:0]` | `S_AXI_ACLK` | data (associated VALID) | Write byte address, aligned 32-bit accesses |
| `S_AXI_AWPROT` | input `[2:0]` | `S_AXI_ACLK` | data (associated VALID) | Write protection attributes, accepted but not interpreted |
| `S_AXI_AWVALID` | input `1 bit` | `S_AXI_ACLK` | level | Associated AXI channel validity/acceptance |
| `S_AXI_AWREADY` | output `1 bit` | `S_AXI_ACLK` | level | Associated AXI channel validity/acceptance |
| `S_AXI_WDATA` | input `[C_S_AXI_DATA_WIDTH-1:0]` | `S_AXI_ACLK` | data (associated VALID) | Write data32, sampled at W handshake |
| `S_AXI_WSTRB` | input `[(C_S_AXI_DATA_WIDTH/8)-1:0]` | `S_AXI_ACLK` | data (associated VALID) | Per-byte write enables, sampled at W handshake |
| `S_AXI_WVALID` | input `1 bit` | `S_AXI_ACLK` | level | Associated AXI channel validity/acceptance |
| `S_AXI_WREADY` | output `1 bit` | `S_AXI_ACLK` | level | Associated AXI channel validity/acceptance |
| `S_AXI_BRESP` | output `[1:0]` | `S_AXI_ACLK` | data (associated VALID) | Held write response: OKAY or SLVERR |
| `S_AXI_BVALID` | output `1 bit` | `S_AXI_ACLK` | level | Associated AXI channel validity/acceptance |
| `S_AXI_BREADY` | input `1 bit` | `S_AXI_ACLK` | level | Associated AXI channel validity/acceptance |
| `S_AXI_ARADDR` | input `[C_S_AXI_ADDR_WIDTH-1:0]` | `S_AXI_ACLK` | data (associated VALID) | Read byte address, aligned 32-bit accesses |
| `S_AXI_ARPROT` | input `[2:0]` | `S_AXI_ACLK` | data (associated VALID) | Read protection attributes, accepted but not interpreted |
| `S_AXI_ARVALID` | input `1 bit` | `S_AXI_ACLK` | level | Associated AXI channel validity/acceptance |
| `S_AXI_ARREADY` | output `1 bit` | `S_AXI_ACLK` | level | Associated AXI channel validity/acceptance |
| `S_AXI_RDATA` | output `[C_S_AXI_DATA_WIDTH-1:0]` | `S_AXI_ACLK` | data (associated VALID) | Read snapshot held with RVALID |
| `S_AXI_RRESP` | output `[1:0]` | `S_AXI_ACLK` | data (associated VALID) | Held read response: OKAY or SLVERR |
| `S_AXI_RVALID` | output `1 bit` | `S_AXI_ACLK` | level | Associated AXI channel validity/acceptance |
| `S_AXI_RREADY` | input `1 bit` | `S_AXI_ACLK` | level | Associated AXI channel validity/acceptance |

## Verification and external boundaries

Plaintext is never application-authenticated until AUTH_OK=1. A fixture that
expects AUTH_OK=0 may pass the negative test, while its plaintext remains
private and is discarded; reporting a verified fixture is not releasing data.

`tb_aead_axi4l` exercises the complete fixture catalog plus channel/order/error
stress. Host-model tests check software sequencing independently of physical
board access. Package integrity and BD validation check project/package
structure; they do not prove physical DDR behavior or implementation timing.
See [peripheral requirements](peripheral.md#interface-requirements-and-verification),
[peripheral DV](../../dv/INTERFACE.md), [figures](../figures/README.md) and
[integration references](../interfaces/integration_references.md).

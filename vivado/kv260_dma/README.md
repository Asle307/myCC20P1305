# KV260 AXI4-Stream/DMA board project

Open `vivado/kv260_dma/kv260_dma.xpr` in Vivado 2025.2. This project contains
`SOC_top` with the AEAD stream peripheral, AXI DMA, a control SmartConnect and
a DDR-path SmartConnect. Maintain wiring, addresses and output products in
the GUI. Use the existing project directly; all required custom IP sources
are in this branch's IP repository.

The design is PS AXI-Lite control plus a full-duplex AXI DMA DDR path:

```text
PS M_AXI_HPM0_FPD -> control SmartConnect -> AEAD S_AXI
                                         AXI DMA S_AXI_LITE
DDR <- PS S_AXI_HP0_FPD <- memory SmartConnect <- DMA M_AXI_MM2S / M_AXI_S2MM
DMA M_AXIS_MM2S -> AEAD S_AXIS
AEAD M_AXIS -> DMA S_AXIS_S2MM
```

## IP package and source ownership

The project uses `user.org:user:aead_axi_dma:1.0` from
`vivado/ip_repo/aead_axi_dma_1_0`. Refresh this IP repository in IP Catalog
when updating the package. The IP contains all eight native algorithm modules,
`aead_axis.v` and `aead_axi_dma.v`; the vendor AXI DMA remains a separate BD cell.

For a manual package refresh, use the Vivado GUI IP Packager on this package,
check integrity and repackage. Exclude testbenches and the standalone core XDC.
The bus interfaces are `S_AXI`, `S_AXIS` and `M_AXIS`, associated with `CLK`
and active-low `RST`. Inherit clock frequency from the connected source.
Upgrade the BD instance and regenerate output products after repackaging.

## Configure AXI DMA

The saved **AXI Direct Memory Access** cell (`xilinx.com:ip:axi_dma:7.1`) uses:

| Setting | Value |
|---|---|
| Scatter Gather | Disabled (simple/direct-register mode) |
| Read / Write channels | Both enabled (MM2S and S2MM) |
| Address width | **64 bits**, required for existing DDR-high address 0x800000000 |
| Memory-mapped data widths | 64 bits |
| Stream data widths | **32 bits**, both directions |
| Data Realignment Engine | Enabled on both channels |
| Buffer length register width | 23 bits |
| Micro DMA | Disabled |
| Asynchronous primary clocks | Disabled; all clocks use PL0 |
| Control/status streams | Disabled |

The host polls completion, with DMA IRQ enables zero. No PS interrupt wiring
or new interrupt driver is needed for this polling host. Unused DMA
interrupt outputs can remain unconnected. Do not confuse this PL AXI DMA with
the PS `dma-controller` UIO devices at 0xFD5xxxxx on the lab image.

## Clock, reset and DDR path

1. Retain the KV260 SOM PS preset, HPM0_FPD control master, PL0 clock and reset
   synchronizer. Enable **S_AXI_HP0_FPD**, with a 64-bit memory data interface,
   in the PS configuration for PL masters to access DDR.
2. The control SmartConnect has two slave targets: AEAD S_AXI and
   DMA S_AXI_LITE. Its one upstream port is PS M_AXI_HPM0_FPD.
3. The memory path uses a second SmartConnect, **2 SI / 1 MI / 1 clock**. Connect both DMA
   memory masters to its SI ports, and its MI port to PS S_AXI_HP0_FPD.
4. Connect DMA M_AXIS_MM2S to AEAD S_AXIS, and AEAD M_AXIS to DMA S_AXIS_S2MM.
   This permits simultaneous input and output; do not serialize the two DMA
   directions in software.
5. Connect PL0 to both SmartConnect clocks, AEAD CLK, all three AXI DMA clocks,
   PS HPM0 clock and PS **saxihp0_fpd_aclk**. Connect synchronized
   `peripheral_aresetn` to AEAD RST, both interconnect resets and DMA axi_resetn.
6. Keep requested PL0 at 200 MHz and propagate its actual Vivado frequency.
   Keep `dcm_locked=1`, auxiliary/debug resets=0 and PS external reset polarity
   as in the working design. No extra Clocking Wizard or clock domain is needed.

Changing a PS setting in an XSA is not itself applied by loading a PL bitstream
into already running Linux. The lab boot configuration must permit the chosen
HP/DDR path; verify it if DMA reports decode/slave errors. Keep Linux clock
readback with measurements and use the lab's FCLK helper when needed.

Also verify the **running HP0 data width**, not only the BD setting. The lab
PS was observed with AFIFM2 RDCTRL/WRCTRL both `0x000003b0` (128 bits), while
this BD exposes 64 bits. This causes silent DMA data corruption even with
correct CPU buffer readback and successful DMA completion. See
[HP0 runtime width alignment](../../software/README.md#align-hp0-width-with-the-running-ps)
for the guarded read-modify-write procedure; with idle masters only, bits 1:0
must be 1 for this 64-bit design. Changing the Vivado setting alone does not
update those live registers.

## Address Editor and implementation

Assign these control addresses, which match the supplied host:

| Slave | Base | Assigned range |
|---|---:|---:|
| AEAD S_AXI | 0xA0000000 | 4 KiB (64 bytes used) |
| DMA S_AXI_LITE | 0xA0010000 | 64 KiB |

For **both** DMA master address spaces, assign the PS DDR-high segment that
contains **0x0000000800000000 .. 0x000000087FFFFFFF**. Confirm it is reachable
through HP0. A map containing only low DDR cannot serve the lab's `ddr_high`
buffer. Do not enable a 32-bit DMA address width or truncate the address.

The existing Linux `MY_IP` UIO aperture is 0xA0000000 + 256 MiB, covering both
control windows. Existing `ddr_high` is a separate reserved UIO memory mapping.
This host path uses those existing mappings with polling; it does not need
AXI DMA registered with the Linux DMAengine driver. Do not let another driver
or process own the same DMA/registers/buffers concurrently.

Validate Design (F6), save, regenerate the HDL wrapper/output products, run
synthesis and implementation in the GUI, check routed timing/DRC, then
Generate Bitstream. Use fresh reports for timing claims. The saved DMA design has no System ILA.
Add one manually if capture is needed;
Hardware Manager requires an accessible JTAG/hw_server path.

Program the bitstream generated from this project with the lab's existing
`update_bitstream.sh`, then follow [the host guide](../../software/README.md).
The saved design has passed RFC encrypt, valid-tag decrypt, bad-tag rejection
and the documented 16 KiB benchmark on KV260 after runtime HP0 width alignment.
Those observations describe that board setup; regenerating a bitstream or
changing the platform requires checking the resulting design again.

The board's `SOC_top_wrapper.v` is kept under the project's
`.srcs/sources_1/hdl/` directory so a checkout has its HDL top before output
products are generated. After changing BD external ports, regenerate the HDL
wrapper in the GUI and refresh this source. Rebuild output products from the
saved BD; no local incremental-synthesis checkpoint is required by the project.

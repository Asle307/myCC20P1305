# KV260 AXI4-Lite board project

Open `vivado/kv260/kv260.xpr` in Vivado 2025.2. The saved `SOC_top` connects
PS `M_AXI_HPM0_FPD` through SmartConnect to `aead_axi4l` and monitors the
control bus with System ILA. The CPU transfers all message words through
the peripheral's AXI4-Lite register interface.

```text
CPU / Linux UIO -> PS M_AXI_HPM0_FPD -> SmartConnect -> AEAD S00_AXI
                                                    System ILA monitor
```

## IP package and source ownership

The custom IP is `user.org:user:aead_axi4l:1.0`, from
`vivado/ip_repo/aead_axi4l_1_0`. Its `hdl/` directory contains copies of all ten maintained modules under
`rtl/`, including both wrappers. Keep every packaged copy synchronized. The [package guide](../ip_repo/aead_axi4l_1_0/README.md)
and [register contract](../../docs/interfaces/aead_axi4l.md) describe ownership,
commands, word framing and handshake rules.

For source updates, open the package in the Vivado GUI IP Packager, check
integrity and repackage. Refresh the IP Catalog, upgrade the BD instance,
and regenerate output products. Reload files before repackaging if an editor
was already open. Maintain the board project through the GUI.

## Clock, reset and address map

Use the KV260 SOM PS preset, PL0 clock and synchronized active-low peripheral
reset. Connect the AXI-Lite slave, control SmartConnect, PS HPM0 AXI clock and
monitor to this clock domain. Keep the saved PS external reset polarity,
`dcm_locked=1` and auxiliary/debug resets=0.

Keep the requested PL0 frequency at 200 MHz and propagate the actual Vivado
frequency through the AXI domain. Loading a PL bitstream does not apply PS
initialization to an already running Linux system. Read back the running
clock before interpreting measurements; the recorded lab readback is
199999998 Hz.

| Slave | Base | Assigned range |
|---|---:|---:|
| AEAD S00_AXI | 0xA0000000 | 64 KiB (64 bytes used) |

The lab Linux image exposes `MY_IP` at `/dev/uio12`, with map0 base
0xA0000000 and size 256 MiB. The host maps one page and verifies the UIO
metadata. Device indices depend on the platform device tree.

## Generate and exercise the design

Validate Design (F6), save, regenerate the HDL wrapper/output products, and
run synthesis and implementation in the GUI. Check routed timing and DRC,
then Generate Bitstream. Use fresh reports for resource/frequency claims.
The standalone Virtex-7 OOC project remains a separate native-core comparison.

Program this project's bitstream with the lab's existing `update_bitstream.sh`.
Follow the [Linux host guide](../../software/README.md) to compile `aead_uio.c`
and run RFC encrypt, valid-tag decrypt, bad-tag rejection and optional verified
throughput measurements. The guide records the existing KV260 observations.

For ILA, use the host's `--pause` option, arm the slave-side CONTROL-write
trigger in Hardware Manager, then release the host. AW and W are independent
channels. Hardware Manager needs an accessible JTAG/hw_server connection;
SSH/UIO alone does not read ILA captures.

The board's `SOC_top_wrapper.v` is kept under the project's
`.srcs/sources_1/hdl/` directory so a checkout has its HDL top before output
products are generated. After changing BD external ports, regenerate the HDL
wrapper in the GUI and refresh this source. Rebuild output products from the
saved BD; no local incremental-synthesis checkpoint is required by the project.

# Integration references and protocol scope

The cryptographic reference collection contains eight PDFs under
`docs/references/`. This guide records the additional sources for the
AXI4-Lite interface and Vivado workflow maintained on `main`. Vendor manuals
are linked, not redistributed.

## VJU_SoC lab boundary

[VJU_SoC](https://github.com/duonglvtnaist/VJU_SoC/tree/2538156b57ada50628eaba91e1abd29790f5b6b7)
is the requested wrapping/integration reference. `RTL_Lab/MY_IP_AXI_Lite.v`
connects an AXI-Lite slave to the example `compute.v` user block.
`RTL_Lab/component.xml` instead packages the separate `MY_IP.v` AXI Full
variant; it is not an AXI-Lite packaging template for this peripheral.
Use its lab compute boundary and Linux UIO workflow as integration examples,
not as the normative definition of AXI transactions. This specification is
written for the maintained AEAD wrapper; no reference RTL is copied into it.

## AMD/Xilinx sources

The following editions were read for these topics. Their document revision
is distinct from the AXI protocol revision and the local Vivado 2025.2 tool
version. Local GUI validation checks this project's actual integration.

| ID | Official document | Edition | Relevant sections |
|---|---|---|---|
| UG1037 | [Vivado AXI Reference Guide](https://www.xilinx.com/support/documents/ip_documentation/axi_ref_guide/latest/ug1037-vivado-axi-reference-guide.pdf) | v4.0, 2017-07-15 | Chapter 1 Introducing AXI for Vivado; Appendix A AXI4/AXI4-Lite and AXI4-Stream signal tables |
| UG1118 | [Creating and Packaging Custom IP](https://www.xilinx.com/support/documents/sw_manuals/xilinx2022_2/ug1118-vivado-creating-packaging-custom-ip.pdf) | v2022.2, 2022-11-02 | Chapter 3 Packaging a Specified Directory / Creating a New AXI4 Peripheral; Chapter 4 Ports and Interfaces / Addressing and Memory |
| UG994 | [Designing IP Subsystems Using IP Integrator](https://www.xilinx.com/support/documents/sw_manuals/xilinx2022_2/ug994-vivado-ip-subsystems.pdf) | v2022.2, 2022-10-19 | Chapter 2 Creating a Block Design; Chapter 3 Addressing for Block Designs; Chapter 4 Working with Block Designs |

## Standard interface versus this IP's choices

UG1037's AXI4/AXI4-Lite channel definitions distinguish write address, write
data, write response, read address and read data. The slave's AW/W holding
registers support independent arrival; its B/R registers retain responses
while the corresponding READY is low. Byte strobes select written bytes.

The 16-register map, single-word slots, PUSH/POP commands, native byte counts,
sticky status and polling host are this peripheral's choices. They are not
AXI-Lite-mandated register behavior. The supported configuration is 32-bit
MMIO with six local byte-address bits; wider template parameter settings are
not claimed as supported or tested.

UG1118 supports explicit bus/clock/reset association and self-contained IP
packaging; UG994 covers connecting/customizing BD cells, address assignment,
validation and output-product generation. Follow the [package guide](../../vivado/ip_repo/aead_axi4l_1_0/README.md),
[board guide](../../vivado/kv260/README.md) and [host guide](../../software/README.md)
for this configuration. Protocol citations explain the bus; peripheral tests
establish the behavior exercised here, without claiming universal compliance.

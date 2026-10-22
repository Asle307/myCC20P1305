# Standalone core clock: 200 MHz.
create_clock -name i_clk -period 5.000 -waveform {0.000 2.500} [get_ports {i_clk}]

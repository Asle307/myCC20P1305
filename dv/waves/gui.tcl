# Select the waveform preset for the elaborated testbench.
set wave_dir   [file dirname [file normalize [info script]]]
set wave_files {}

foreach preset [glob -nocomplain [file join $wave_dir tb_*.wcfg]] {
    set top [file rootname [file tail $preset]]
    if {[llength [get_objects -quiet /${top}/u_test/i_clk]] != 0 ||
        [llength [get_objects -quiet /${top}/i_clk]] != 0} {
        lappend wave_files $preset
    }
}

if {[llength $wave_files] != 1} {
    error "Expected one testbench waveform preset; found [llength $wave_files]"
}

set preset [lindex $wave_files 0]
set matching_configs [get_wave_configs -quiet [file tail $preset]]
if {[llength $matching_configs] == 0} {
    open_wave_config $preset
} else {
    current_wave_config [lindex $matching_configs 0]
}

# Log the preset's signals before simulation, including internal state.
set preset_file [open $preset r]
set preset_xml  [read $preset_file]
close $preset_file

foreach {match signal} [regexp -all -inline {fp_name="(/[^"]+)"} $preset_xml] {
    log_wave [get_objects $signal]
}

puts "GUI_WAVE_PRESET [file tail $preset]"
run all

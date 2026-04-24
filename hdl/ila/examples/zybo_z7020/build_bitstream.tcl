# build_bitstream.tcl -- Vivado batch build: project create -> bitstream
#
# Usage (batch mode):
#   vivado -mode batch -source build_bitstream.tcl
#
# Output: vivado_proj/ila_bringup.runs/impl_1/ila_bringup_top.bit

set script_dir  [file normalize [file dirname [info script]]]
set rtl_dir     [file normalize [file join $script_dir ../../rtl]]
set example_dir $script_dir
set proj_dir    [file join $script_dir vivado_proj]

# ------------------------------------------------------------------
# Create project (force overwrites existing)
# ------------------------------------------------------------------
create_project ila_bringup $proj_dir -part xc7z020clg400-1 -force

set_property target_language    Verilog [current_project]
set_property simulator_language Mixed   [current_project]

# ------------------------------------------------------------------
# RTL sources
# ------------------------------------------------------------------
add_files -norecurse [list \
    [file join $rtl_dir ila_trigger.sv]        \
    [file join $rtl_dir ila_capture_fsm.sv]    \
    [file join $rtl_dir ila_bram.sv]           \
    [file join $rtl_dir ila_bscane2_top.sv]    \
    [file join $example_dir ila_bringup_top.sv]\
]
foreach f [get_files *.sv] {
    set_property file_type SystemVerilog $f
}

# ------------------------------------------------------------------
# Constraints
# ------------------------------------------------------------------
add_files -fileset constrs_1 -norecurse \
    [file join $example_dir ila_bringup.xdc]

# ------------------------------------------------------------------
# Top module
# ------------------------------------------------------------------
set_property top ila_bringup_top [current_fileset]
update_compile_order -fileset sources_1

# ------------------------------------------------------------------
# Synthesis
# ------------------------------------------------------------------
puts "\n=== Synthesis ===\n"
launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} {
    puts "ERROR: Synthesis failed"
    exit 1
}
puts "Synthesis COMPLETE"

# ------------------------------------------------------------------
# Implementation + Bitstream
# ------------------------------------------------------------------
puts "\n=== Implementation + Write Bitstream ===\n"
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} {
    puts "ERROR: Implementation failed"
    exit 1
}

set bit_file [file join $proj_dir ila_bringup.runs impl_1 ila_bringup_top.bit]
if {[file exists $bit_file]} {
    puts "\n=========================================="
    puts "Bitstream ready: $bit_file"
    puts "==========================================\n"
} else {
    puts "ERROR: Bitstream file not found at $bit_file"
    exit 1
}

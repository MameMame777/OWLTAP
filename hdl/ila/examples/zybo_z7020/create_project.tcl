# create_project.tcl -- Vivado project for ila_bringup on Zybo Z7-20
#
# Usage:
#   Vivado GUI Tcl console:  source create_project.tcl
#   Batch mode:              vivado -mode batch -source create_project.tcl
#
# Output: ila_bringup/ directory with Vivado project.
# Bitstream: File > Bitstream Settings > OK, then Run Implementation.

set script_dir  [file normalize [file dirname [info script]]]
set rtl_dir     [file normalize [file join $script_dir ../../rtl]]
set example_dir $script_dir

# ------------------------------------------------------------------
# Create project
# ------------------------------------------------------------------
create_project ila_bringup $script_dir/vivado_proj \
    -part xc7z020clg400-1 -force

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

# Mark all as SystemVerilog
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

puts ""
puts "-----------------------------------------------------------"
puts "Project created: [get_property DIRECTORY [current_project]]"
puts "Part:            xc7z020clg400-1  (Zybo Z7-20)"
puts ""
puts "Next steps:"
puts "  launch_runs synth_1 -jobs 4"
puts "  wait_on_run synth_1"
puts "  launch_runs impl_1 -to_step write_bitstream -jobs 4"
puts "  wait_on_run impl_1"
puts "  # then program with: open_hw_manager, connect, program"
puts "-----------------------------------------------------------"

## program_board.tcl
## Vivado Hardware Manager script for Zybo Z7020
## Usage:
##   vivado -mode batch -source program_board.tcl
## Or open Vivado Tcl Console and source this file.

set script_dir [file dirname [file normalize [info script]]]
set bit_file   [file join $script_dir \
    "vivado_proj/ila_bringup.runs/impl_1/ila_bringup_top.bit"]

if {![file exists $bit_file]} {
    puts "ERROR: Bitstream not found at $bit_file"
    puts "       Run build_bitstream.tcl first."
    exit 1
}

puts "\n=== Program Zybo Z7020 ==="
puts "Bitstream : $bit_file"

open_hw_manager
connect_hw_server -allow_non_jtag
open_hw_target

## Zybo Z7020: JTAG chain has two TAPs
##   index 0: ARM DAP (PS)
##   index 1: xc7z020 PL Config TAP  -- this is what we program
set devices [get_hw_devices]
puts "Devices found: $devices"

## Select the PL device (xc7z020)
set pl_dev ""
foreach d $devices {
    if {[string match "xc7z020*" $d]} {
        set pl_dev $d
    }
}
if {$pl_dev eq ""} {
    ## Fallback: try xc7z*
    foreach d $devices {
        if {[string match "xc7z*" $d]} {
            set pl_dev $d
        }
    }
}
if {$pl_dev eq ""} {
    puts "ERROR: No xc7z020 device found. Connected devices: $devices"
    close_hw_target
    disconnect_hw_server
    exit 1
}

puts "Programming: $pl_dev"
current_hw_device $pl_dev
refresh_hw_device -update_hw_probes false $pl_dev

set_property PROGRAM.FILE $bit_file $pl_dev
program_hw_devices $pl_dev
refresh_hw_device $pl_dev

puts "\n=== Programming COMPLETE ==="
puts "LED\[3:0\] should now blink at ~0.46 Hz (counter\[27:24\] / 125 MHz)"

close_hw_target
disconnect_hw_server

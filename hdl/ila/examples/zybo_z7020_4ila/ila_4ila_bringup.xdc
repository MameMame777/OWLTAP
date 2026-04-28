## ila_4ila_bringup.xdc -- Zybo Z7-20 (XC7Z020CLG400-1)
##
## Four ila_bscane2_top instances on BSCANE2 USER1..USER4.
## No extra I/O pins are needed for JTAG.

## --------------------------------------------------------------------------
## Clock
## --------------------------------------------------------------------------
set_property -dict { PACKAGE_PIN K17  IOSTANDARD LVCMOS33 } [get_ports { sysclk }]
create_clock -add -name sys_clk_pin -period 8.000 -waveform {0 4} [get_ports { sysclk }]

## --------------------------------------------------------------------------
## LEDs
## --------------------------------------------------------------------------
set_property -dict { PACKAGE_PIN M14  IOSTANDARD LVCMOS33 } [get_ports { led[0] }]
set_property -dict { PACKAGE_PIN M15  IOSTANDARD LVCMOS33 } [get_ports { led[1] }]
set_property -dict { PACKAGE_PIN G14  IOSTANDARD LVCMOS33 } [get_ports { led[2] }]
set_property -dict { PACKAGE_PIN D18  IOSTANDARD LVCMOS33 } [get_ports { led[3] }]

## --------------------------------------------------------------------------
## CDC: all four BSCANE2 TCK signals are asynchronous to sys_clk_pin.
## One clock_groups constraint covers all instances because every BSCANE2
## shares the same physical TCK pin; Vivado names the clock on the TCK net
## automatically.  The wildcard matches u_ila0..u_ila3.
## --------------------------------------------------------------------------
set_clock_groups -asynchronous -quiet \
    -group [get_clocks sys_clk_pin] \
    -group [get_clocks -of_objects [get_nets -hier -filter {NAME =~ *bscan_tck*}] -quiet]

## --------------------------------------------------------------------------
## CDC false paths inside ILA (ASYNC_REG synchronizer inputs).
## One constraint covers all ASYNC_REG FFs across all four instances.
## This is the Xilinx-recommended approach: target by attribute, not by name.
## --------------------------------------------------------------------------
set_false_path -to [get_cells -hier -filter {ASYNC_REG == TRUE}]

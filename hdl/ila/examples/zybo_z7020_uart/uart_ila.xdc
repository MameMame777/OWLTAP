## uart_ila.xdc -- Zybo Z7-20 (XC7Z020CLG400-1)
##
## UART loopback ILA demo.  The UART serial wire is internal (no PMOD
## pins are used), so only sysclk, LEDs, and BSCANE2 CDC constraints
## are needed here.

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
## CDC: BSCANE2 TCK is asynchronous to sys_clk_pin.
## --------------------------------------------------------------------------
set_clock_groups -asynchronous -quiet \
    -group [get_clocks sys_clk_pin] \
    -group [get_clocks -of_objects [get_nets -hier -filter {NAME =~ *bscan_tck*}] -quiet]

## --------------------------------------------------------------------------
## CDC false paths into ILA synchronizer registers
## --------------------------------------------------------------------------
set_false_path -to [get_cells -hier -filter {NAME =~ *u_ila*bscane2_pulse_sync*sync0*}]
set_false_path -to [get_cells -hier -filter {NAME =~ *u_ila*mask_sc_m*}]
set_false_path -to [get_cells -hier -filter {NAME =~ *u_ila*val_sc_m*}]
set_false_path -to [get_cells -hier -filter {NAME =~ *u_ila*pre_sc_m*}]
set_false_path -to [get_cells -hier -filter {NAME =~ *u_ila*armed_tck_m*}]
set_false_path -to [get_cells -hier -filter {NAME =~ *u_ila*triggered_tck_m*}]
set_false_path -to [get_cells -hier -filter {NAME =~ *u_ila*full_tck_m*}]

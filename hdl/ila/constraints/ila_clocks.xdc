# OwlTAP ILA — Clock & CDC constraints
#
# Applies when ila_top is instantiated at the top level with a dedicated
# JTAG connector. If the TAP shares pins with the FPGA's existing JTAG,
# remove the create_clock line and instead adjust set_clock_groups to
# reference the platform's TCK clock name.

# ---- JTAG TCK ---------------------------------------------------------
# 10 MHz typical (FTDI MPSSE). Adjust if your cable drives faster TCK.
create_clock -name tck -period 100.000 [get_ports tck]

# ---- Async clock groups ----------------------------------------------
# TCK is asynchronous to the user sample clock. Name the sample clock to
# match the clock the user actually wires in, then uncomment or edit:
#
#   set_clock_groups -asynchronous \
#       -group [get_clocks tck] \
#       -group [get_clocks sample_clk]

# ---- CDC false paths -------------------------------------------------
# All ASYNC_REG flops inside ila_top have the attribute applied. These
# false-path statements ensure the synthesizer does not attempt to meet
# timing across the unrelated clocks.

set_false_path -to [get_cells -hier -filter {NAME =~ *u_*_sync/sync0*}]
set_false_path -to [get_cells -hier -filter {NAME =~ */mask_sc_m*}]
set_false_path -to [get_cells -hier -filter {NAME =~ */val_sc_m*}]
set_false_path -to [get_cells -hier -filter {NAME =~ */pre_sc_m*}]
set_false_path -to [get_cells -hier -filter {NAME =~ */armed_tck_m*}]
set_false_path -to [get_cells -hier -filter {NAME =~ */triggered_tck_m*}]
set_false_path -to [get_cells -hier -filter {NAME =~ */full_tck_m*}]

# ---- TAP input/output timing (safe defaults for 10 MHz TCK) -----------
set_input_delay  -clock tck 10.0 [get_ports {tms tdi}]
set_output_delay -clock tck 10.0 [get_ports tdo]

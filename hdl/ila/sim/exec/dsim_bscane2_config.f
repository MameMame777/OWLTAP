# DSim compile/run filelist for ila_bscane2_top UVM testbench.
#
# The BSCANE2 behavioral model (bscane2_model.sv) is included BEFORE
# ila_bscane2_top.sv so DSim resolves the module definition first.
#
# +define+SIMULATION enables the sim-only JTAG ports on ila_bscane2_top
# and the JTAG_TCK/TMS/TDI/TRST_N ports on the BSCANE2 behavioral model.
+define+SIMULATION

# RTL (no ila_tap.sv / ila_top.sv -- not needed for BSCANE2 variant)
+incdir+../../rtl
../../rtl/ila_trigger.sv
../../rtl/ila_capture_fsm.sv
../../rtl/ila_bram.sv
../../rtl/ila_bscane2_top.sv

# Testbench
+incdir+../tb
../tb/ila_jtag_if.sv
../tb/bscane2_model.sv
../tb/ila_bscane2_test_pkg.sv
../tb/ila_bscane2_tb_top.sv

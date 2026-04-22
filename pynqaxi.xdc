# =============================================================================
# pynqaxi.xdc
#
# Pin constraints for pynqaxi overlay on Digilent Zybo Z7-20.
#
# Connected signals (from block design make_bd_pins_external output):
#   led_out[3:0]  -- LED outputs   -> LD0..LD3
#   sw_in[3:0]    -- Switch inputs -> SW0..SW3
#   btn_in[3:0]   -- Button inputs -> BTN0..BTN3
#
# Reference: Digilent Zybo Z7-20 Master XDC v1.2 / schematic rev C.
#   LED / BTN pins confirmed from pynqZybo2/board/Z7-20/constraints/base.xdc.
#   SW pins from Digilent master XDC (not present in pynqZybo2 base.xdc).
# =============================================================================

# --- LEDs (Bank 35, LVCMOS33) ------------------------------------------------
# LD0 -> M14, LD1 -> M15, LD2 -> G14, LD3 -> D18
set_property -dict {PACKAGE_PIN M14 IOSTANDARD LVCMOS33} [get_ports {led_out[0]}]
set_property -dict {PACKAGE_PIN M15 IOSTANDARD LVCMOS33} [get_ports {led_out[1]}]
set_property -dict {PACKAGE_PIN G14 IOSTANDARD LVCMOS33} [get_ports {led_out[2]}]
set_property -dict {PACKAGE_PIN D18 IOSTANDARD LVCMOS33} [get_ports {led_out[3]}]

# --- Slide Switches (Bank 34, LVCMOS33) --------------------------------------
# SW0 -> G15, SW1 -> P15, SW2 -> W13, SW3 -> T16
set_property -dict {PACKAGE_PIN G15 IOSTANDARD LVCMOS33} [get_ports {sw_in[0]}]
set_property -dict {PACKAGE_PIN P15 IOSTANDARD LVCMOS33} [get_ports {sw_in[1]}]
set_property -dict {PACKAGE_PIN W13 IOSTANDARD LVCMOS33} [get_ports {sw_in[2]}]
set_property -dict {PACKAGE_PIN T16 IOSTANDARD LVCMOS33} [get_ports {sw_in[3]}]

# --- Push Buttons (Bank 34, LVCMOS33) ----------------------------------------
# BTN0 -> K18, BTN1 -> P16, BTN2 -> K19, BTN3 -> Y16
set_property -dict {PACKAGE_PIN K18 IOSTANDARD LVCMOS33} [get_ports {btn_in[0]}]
set_property -dict {PACKAGE_PIN P16 IOSTANDARD LVCMOS33} [get_ports {btn_in[1]}]
set_property -dict {PACKAGE_PIN K19 IOSTANDARD LVCMOS33} [get_ports {btn_in[2]}]
set_property -dict {PACKAGE_PIN Y16 IOSTANDARD LVCMOS33} [get_ports {btn_in[3]}]

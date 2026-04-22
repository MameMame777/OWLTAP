#pragma once

#include <map>
#include <string>

namespace jtag::xdc {

/// Maps uppercase package pin name to user port name from an XDC file.
/// Example: "M14" -> "led_out[0]"
using PinAliasMap = std::map<std::string, std::string>;

/// Parse a Vivado XDC constraints file and extract PACKAGE_PIN -> port_name
/// mappings from lines of the form:
///   set_property -dict {PACKAGE_PIN <pin> IOSTANDARD ...} [get_ports {<port>}]
///
/// All other directives (timing constraints, etc.) are silently ignored.
///
/// @param path  Path to the .xdc file.
/// @return      Map from uppercase package pin to port name.
///              Empty map if file cannot be opened or no pins found.
PinAliasMap parseXdc(const std::string& path);

}  // namespace jtag::xdc

#pragma once

#include <string>
#include <vector>

#include "pin_driver.h"
#include "scanner.h"

namespace jtag {

/// A single pin reference: device index in the chain + BSDL pin name.
struct DevPin {
    int device_index = 0;
    std::string pin;   // BSDL port name (upper-case)
};

/// One net entry from an .ict file.
struct NetDef {
    std::string name;       // logical net name
    DevPin driver;          // the device/pin that drives the net
    std::vector<DevPin> receivers;  // expected receivers
};

/// Per-net test result (one entry per value tested: 0 and 1).
struct NetTestStep {
    int driven_value = 0;           // 0 or 1
    std::vector<PinState> observed; // indexed same as NetDef::receivers
    bool pass = true;
};

struct NetResult {
    std::string name;
    std::vector<NetTestStep> steps;  // two steps: drive 0, drive 1
    bool pass = true;
};

struct InterconnectResult {
    std::vector<NetResult> nets;
    int pass_count = 0;
    int fail_count = 0;
};

/// Parse an .ict file.
///
/// File format (whitespace-separated; # comments):
///   net_name  driver_dev:pin  receiver_dev:pin [receiver_dev:pin ...]
///
/// Device indices are integers (0 = first device in JTAG chain).
///
/// Example:
///   # name    driver      receiver
///   CLK       0:IO_L1P    1:IO_L2N
///   DATA      0:IO_L2P    1:IO_L3P   1:IO_L4P
///
std::vector<NetDef> parseIctFile(const std::string& path, std::string& error);

/// Runs an interconnect test:
/// For each net, drives 0 then 1 on the driver pin and observes all receivers.
///
/// @param nets      Net list from parseIctFile()
/// @param scanners  One Scanner* per device (index = device_index in chain).
///                  Entries may be nullptr for devices without BSDL.
/// @param drivers   One PinDriver* per device.
///                  Entries may be nullptr.
///
InterconnectResult runInterconnectTest(
    const std::vector<NetDef>& nets,
    const std::vector<Scanner*>& scanners,
    const std::vector<PinDriver*>& drivers);

/// Format a plain-text report from an interconnect result.
std::string formatInterconnectReport(const InterconnectResult& result,
                                     const std::vector<NetDef>& nets);

} // namespace jtag

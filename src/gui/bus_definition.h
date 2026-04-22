#pragma once

#include <cstdint>

#include "app_config.h"
#include "src/boundary_scan/scanner.h"

namespace jtag::gui {

/// Compute the numeric value of a bus from a scan result.
/// signals[0] = MSB, signals.back() = LSB.
/// Unknown pin states are treated as 0.
uint64_t computeBusValue(const BusDefinition& bus,
                         const jtag::ScanResult& result);

}  // namespace jtag::gui

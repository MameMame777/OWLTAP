#pragma once

#include <string>
#include <vector>

#include "src/boundary_scan/scanner.h"

namespace jtag::gui {

/// Display format for hex panel
enum class DisplayFormat {
    HEX,
    DECIMAL,
    BINARY,
    ASCII,
};

/// Panel that shows selected signals' values in various numeric formats.
class HexPanel {
public:
    /// Draw the hex display panel (call each frame).
    static void draw();

    /// Update displayed values from a scan result.
    static void updateValues(const jtag::ScanResult& result,
                             const std::vector<std::string>& signals);

    /// Define a bus grouping (group multiple signals as one multi-bit value).
    static void defineBus(const std::string& bus_name,
                          const std::vector<std::string>& signal_names);

    /// Clear bus definitions.
    static void clearBuses();

private:
    struct BusDefinition {
        std::string name;
        std::vector<std::string> signals;  // MSB first
    };

    static DisplayFormat format_;
    static std::vector<std::string> current_signals_;
    static jtag::ScanResult current_result_;
    static std::vector<BusDefinition> buses_;
    static bool show_bsr_dump_;
};

} // namespace jtag::gui

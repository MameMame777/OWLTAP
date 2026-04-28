#pragma once

#include <string>
#include <vector>

#include "app_config.h"
#include "src/boundary_scan/scanner.h"
#include "src/xdc/xdc_parser.h"

namespace jtag::gui {

/// Display format for hex panel (individual signals only; buses use BusFormat).
enum class DisplayFormat {
    HEX,
    DECIMAL,
    BINARY,
    ASCII,
};

/// Panel that shows selected signals' values in various numeric formats.
class HexPanel {
public:
    struct DrawActions {
        bool single_requested = false;
    };

    /// Draw the hex display panel (call each frame).
    static DrawActions draw(bool can_capture);

    /// Update displayed values from a scan result.
    static void updateValues(const jtag::ScanResult& result,
                             const std::vector<std::string>& signals);

    /// Set bus definitions (replaces all existing buses).
    static void setBuses(const std::vector<BusDefinition>& buses);

    /// Apply XDC pin aliases (BSDL signal name -> display label).
    static void setXdcAliases(const jtag::xdc::PinAliasMap& aliases);

private:
    static DisplayFormat format_;
    static std::vector<std::string> current_signals_;
    static jtag::ScanResult current_result_;
    static std::vector<BusDefinition> buses_;
    static bool show_bsr_dump_;
    static jtag::xdc::PinAliasMap xdc_aliases_;
};

} // namespace jtag::gui

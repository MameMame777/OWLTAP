#pragma once

#include <string>
#include <vector>

#include "src/capture/capture_engine.h"

namespace jtag::gui {

/// Pure-logic VCD and CSV export (no GUI dependency).
class VcdExport {
public:
    /// Export captured data to VCD format.
    /// Returns empty string on success, error message on failure.
    static std::string exportVcd(
        const std::string& path,
        const std::vector<jtag::SampleFrame>& samples,
        const std::vector<std::string>& signals);

    /// Export captured data to CSV format.
    /// Returns empty string on success, error message on failure.
    static std::string exportCsv(
        const std::string& path,
        const std::vector<jtag::SampleFrame>& samples,
        const std::vector<std::string>& signals);

private:
    static std::string vcdSymbol(int index);
};

} // namespace jtag::gui

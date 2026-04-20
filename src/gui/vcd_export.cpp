#include "vcd_export.h"

#include <chrono>
#include <fstream>
#include <sstream>

namespace jtag::gui {

std::string VcdExport::vcdSymbol(int index) {
    std::string sym;
    do {
        sym += static_cast<char>('!' + (index % 94));
        index /= 94;
    } while (index > 0);
    return sym;
}

std::string VcdExport::exportVcd(
    const std::string& path,
    const std::vector<jtag::SampleFrame>& samples,
    const std::vector<std::string>& signals) {
    if (samples.empty()) return "No data to export.";

    std::ofstream ofs(path);
    if (!ofs.is_open()) return "Cannot write to file.";

    // VCD header
    ofs << "$date\n  Export from JTAG FPGA Viewer\n$end\n";
    ofs << "$version\n  1.0\n$end\n";
    ofs << "$timescale 1us $end\n";

    // Declare signals
    ofs << "$scope module fpga $end\n";
    for (int i = 0; i < static_cast<int>(signals.size()); i++) {
        std::string symbol = vcdSymbol(i);
        ofs << "$var wire 1 " << symbol << " "
            << signals[i] << " $end\n";
    }
    ofs << "$upscope $end\n";
    ofs << "$enddefinitions $end\n";

    // VCD data
    auto t0 = samples.front().timestamp;
    for (const auto& frame : samples) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            frame.timestamp - t0).count();
        ofs << "#" << us << "\n";

        for (int i = 0; i < static_cast<int>(signals.size()); i++) {
            std::string symbol = vcdSymbol(i);
            jtag::PinState state = frame.data.getPin(signals[i]);
            char val = (state == jtag::PinState::HIGH) ? '1' :
                       (state == jtag::PinState::LOW)  ? '0' : 'x';
            ofs << val << symbol << "\n";
        }
    }

    return {};
}

std::string VcdExport::exportCsv(
    const std::string& path,
    const std::vector<jtag::SampleFrame>& samples,
    const std::vector<std::string>& signals) {
    if (samples.empty()) return "No data to export.";

    std::ofstream ofs(path);
    if (!ofs.is_open()) return "Cannot write to file.";

    // Header
    ofs << "timestamp_us";
    for (const auto& sig : signals) {
        ofs << "," << sig;
    }
    ofs << "\n";

    // Data rows
    auto t0 = samples.front().timestamp;
    for (const auto& frame : samples) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            frame.timestamp - t0).count();
        ofs << us;
        for (const auto& sig : signals) {
            jtag::PinState state = frame.data.getPin(sig);
            ofs << "," << (state == jtag::PinState::HIGH ? 1 :
                          state == jtag::PinState::LOW  ? 0 : -1);
        }
        ofs << "\n";
    }

    return {};
}

} // namespace jtag::gui

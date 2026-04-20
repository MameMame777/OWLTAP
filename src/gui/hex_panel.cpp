#include "hex_panel.h"

#include <imgui.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace jtag::gui {

DisplayFormat HexPanel::format_ = DisplayFormat::HEX;
bool HexPanel::show_bsr_dump_ = false;
std::vector<std::string> HexPanel::current_signals_;
jtag::ScanResult HexPanel::current_result_;
std::vector<HexPanel::BusDefinition> HexPanel::buses_;

void HexPanel::updateValues(const jtag::ScanResult& result,
                             const std::vector<std::string>& signals) {
    current_result_ = result;
    current_signals_ = signals;
}

void HexPanel::defineBus(const std::string& bus_name,
                          const std::vector<std::string>& signal_names) {
    buses_.push_back({bus_name, signal_names});
}

void HexPanel::clearBuses() {
    buses_.clear();
}

void HexPanel::draw() {
    ImGui::Begin("Bus Values");

    // Format selector
    const char* formats[] = {"Hex", "Decimal", "Binary", "ASCII"};
    int fmt_idx = static_cast<int>(format_);
    if (ImGui::Combo("Format", &fmt_idx, formats, 4)) {
        format_ = static_cast<DisplayFormat>(fmt_idx);
    }

    ImGui::Separator();

    if (ImGui::BeginTable("values", 3,
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Signal/Bus");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("State");
        ImGui::TableHeadersRow();

        // Bus values
        for (const auto& bus : buses_) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", bus.name.c_str());

            int bits = static_cast<int>(bus.signals.size());
            uint64_t value = 0;
            bool all_known = true;

            for (int i = 0; i < bits; i++) {
                jtag::PinState state =
                    current_result_.getPin(bus.signals[i]);
                if (state == jtag::PinState::UNKNOWN) {
                    all_known = false;
                    break;
                }
                if (state == jtag::PinState::HIGH) {
                    value |= (1ULL << (bits - 1 - i));
                }
            }

            ImGui::TableNextColumn();
            if (!all_known) {
                ImGui::Text("???");
            } else {
                char buf[128];
                switch (format_) {
                    case DisplayFormat::HEX:
                        snprintf(buf, sizeof(buf), "0x%0*llX",
                                 (bits + 3) / 4,
                                 static_cast<unsigned long long>(value));
                        break;
                    case DisplayFormat::DECIMAL:
                        snprintf(buf, sizeof(buf), "%llu",
                                 static_cast<unsigned long long>(value));
                        break;
                    case DisplayFormat::BINARY: {
                        std::string bin = "0b";
                        for (int i = bits - 1; i >= 0; i--) {
                            bin += (value & (1ULL << i)) ? '1' : '0';
                        }
                        snprintf(buf, sizeof(buf), "%s", bin.c_str());
                        break;
                    }
                    case DisplayFormat::ASCII:
                        if (bits == 8 && value >= 0x20 && value < 0x7F) {
                            snprintf(buf, sizeof(buf), "'%c' (0x%02X)",
                                     static_cast<char>(value),
                                     static_cast<unsigned>(value));
                        } else {
                            snprintf(buf, sizeof(buf), "0x%0*llX",
                                     (bits + 3) / 4,
                                     static_cast<unsigned long long>(value));
                        }
                        break;
                }
                ImGui::Text("%s", buf);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%s", all_known
                        ? (std::to_string(bits) + "-bit bus").c_str()
                        : "Unknown");
        }

        // Individual signals
        for (const auto& sig : current_signals_) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", sig.c_str());

            jtag::PinState state = current_result_.getPin(sig);
            ImGui::TableNextColumn();
            switch (state) {
                case jtag::PinState::HIGH:
                    ImGui::Text("1");
                    break;
                case jtag::PinState::LOW:
                    ImGui::Text("0");
                    break;
                case jtag::PinState::UNKNOWN:
                    ImGui::Text("?");
                    break;
            }

            ImGui::TableNextColumn();
            switch (state) {
                case jtag::PinState::HIGH:
                    ImGui::Text("HIGH");
                    break;
                case jtag::PinState::LOW:
                    ImGui::Text("LOW");
                    break;
                case jtag::PinState::UNKNOWN:
                    ImGui::Text("UNKNOWN");
                    break;
            }
        }

        ImGui::EndTable();
    }

    // ── Raw BSR hex dump ────────────────────────────────────────────
    ImGui::Separator();
    ImGui::Checkbox("Show Raw BSR", &show_bsr_dump_);
    if (show_bsr_dump_ && !current_result_.raw_bsr.empty()) {
        const auto& bsr = current_result_.raw_bsr;
        ImGui::TextDisabled("%zu bytes (%zu bits)", bsr.size(), bsr.size() * 8);
        // Hex dump: 16 bytes per row
        char line[128];
        for (size_t row = 0; row * 16 < bsr.size(); row++) {
            size_t offset = row * 16;
            int n = snprintf(line, sizeof(line), "%04zX: ", offset);
            for (size_t i = offset; i < offset + 16 && i < bsr.size(); i++) {
                n += snprintf(line + n, sizeof(line) - n, "%02X ", bsr[i]);
            }
            ImGui::TextUnformatted(line);
        }
        // Per-pin bit positions
        ImGui::Separator();
        ImGui::TextDisabled("Signal BSR bits:");
        for (const auto& sig : current_signals_) {
            jtag::PinState state = current_result_.getPin(sig);
            const char* val = (state == jtag::PinState::HIGH) ? "1" :
                              (state == jtag::PinState::LOW)  ? "0" : "?";
            ImGui::Text("  %-20s = %s", sig.c_str(), val);
        }
    }

    ImGui::End();
}

} // namespace jtag::gui

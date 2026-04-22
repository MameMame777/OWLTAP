#include "hex_panel.h"

#include <imgui.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "bus_definition.h"

namespace jtag::gui {

DisplayFormat HexPanel::format_ = DisplayFormat::HEX;
bool HexPanel::show_bsr_dump_ = false;
std::vector<std::string> HexPanel::current_signals_;
jtag::ScanResult HexPanel::current_result_;
std::vector<BusDefinition> HexPanel::buses_;
jtag::xdc::PinAliasMap HexPanel::xdc_aliases_;

void HexPanel::updateValues(const jtag::ScanResult& result,
                             const std::vector<std::string>& signals) {
    current_result_ = result;
    current_signals_ = signals;
}

void HexPanel::setBuses(const std::vector<BusDefinition>& buses) {
    buses_ = buses;
}

void HexPanel::setXdcAliases(const jtag::xdc::PinAliasMap& aliases) {
    xdc_aliases_ = aliases;
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

            const int bits = static_cast<int>(bus.signals.size());
            bool all_known = true;
            for (const auto& sig : bus.signals) {
                if (current_result_.getPin(sig) == jtag::PinState::UNKNOWN) {
                    all_known = false;
                    break;
                }
            }

            ImGui::TableNextColumn();
            if (!all_known) {
                ImGui::Text("???");
            } else {
                const uint64_t value = computeBusValue(bus, current_result_);
                char buf[128];
                switch (bus.format) {
                    case BusFormat::DEC:
                        snprintf(buf, sizeof(buf), "%llu",
                                 static_cast<unsigned long long>(value));
                        break;
                    case BusFormat::BIN: {
                        std::string bin = "0b";
                        for (int b = bits - 1; b >= 0; b--)
                            bin += (value & (1ULL << b)) ? '1' : '0';
                        snprintf(buf, sizeof(buf), "%s", bin.c_str());
                        break;
                    }
                    default:  // HEX
                        snprintf(buf, sizeof(buf), "0x%0*llX",
                                 (bits + 3) / 4,
                                 static_cast<unsigned long long>(value));
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
            // Show "alias (sig)" when XDC alias available, otherwise just sig.
            auto alias_it = xdc_aliases_.find(sig);
            std::string display_name;
            if (alias_it != xdc_aliases_.end()) {
                display_name = alias_it->second + " (" + sig + ")";
            } else {
                display_name = sig;
            }
            ImGui::Text("%s", display_name.c_str());

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

#include "protocol_panel.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "src/protocol/i2c_decoder.h"
#include "src/protocol/spi_decoder.h"
#include "src/protocol/uart_decoder.h"

namespace jtag::gui {

bool ProtocolPanel::visible_        = false;
bool ProtocolPanel::new_frames_ready_ = false;
std::vector<jtag::protocol::DecodedFrame> ProtocolPanel::frames_;

// UART defaults
int ProtocolPanel::uart_rx_idx_      = 0;
int ProtocolPanel::uart_baud_idx_    = 5;   // index into baud table (115200)
int ProtocolPanel::uart_data_bits_   = 8;
bool ProtocolPanel::uart_parity_en_  = false;
bool ProtocolPanel::uart_parity_odd_ = false;
int ProtocolPanel::uart_stop_bits_   = 1;

// SPI defaults
int ProtocolPanel::spi_clk_idx_      = 0;
int ProtocolPanel::spi_mosi_idx_     = 0;
int ProtocolPanel::spi_miso_idx_     = 0;
int ProtocolPanel::spi_cs_idx_       = 0;
bool ProtocolPanel::spi_cpol_        = false;
bool ProtocolPanel::spi_cpha_        = false;
bool ProtocolPanel::spi_lsb_first_   = false;
int ProtocolPanel::spi_bits_per_word_ = 8;

// I2C defaults
int ProtocolPanel::i2c_scl_idx_ = 0;
int ProtocolPanel::i2c_sda_idx_ = 0;

// Common
int ProtocolPanel::selected_protocol_  = 0;
int ProtocolPanel::selected_frame_idx_ = -1;

// ── Helpers ──────────────────────────────────────────────────────────────────

static const char* kBaudLabels[] = {
    "1200", "2400", "4800", "9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"
};
static const uint32_t kBaudRates[] = {
    1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600
};
static const int kBaudCount = 11;

static std::string signalAtIdx(const std::vector<std::string>& sigs, int idx) {
    if (sigs.empty() || idx < 0 || idx >= static_cast<int>(sigs.size()))
        return "";
    return sigs[idx];
}

static void signalCombo(const char* label, int& idx,
                        const std::vector<std::string>& sigs,
                        bool allow_none = false) {
    std::vector<const char*> ptrs;
    if (allow_none) ptrs.push_back("(none)");
    for (const auto& s : sigs) ptrs.push_back(s.c_str());

    int combo_idx = allow_none ? idx + 1 : idx;
    if (combo_idx < 0) combo_idx = 0;

    ImGui::SetNextItemWidth(160);
    if (ImGui::Combo(label, &combo_idx, ptrs.data(),
                     static_cast<int>(ptrs.size()))) {
        idx = allow_none ? combo_idx - 1 : combo_idx;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

const std::vector<jtag::protocol::DecodedFrame>& ProtocolPanel::decodedFrames() {
    return frames_;
}

bool ProtocolPanel::consumeNewFrames() {
    const bool v = new_frames_ready_;
    new_frames_ready_ = false;
    return v;
}

void ProtocolPanel::setVisible(bool visible) { visible_ = visible; }
bool ProtocolPanel::isVisible()              { return visible_; }

// ── draw ─────────────────────────────────────────────────────────────────────

void ProtocolPanel::draw(const std::vector<std::string>& available_signals,
                         const std::vector<jtag::SampleFrame>& samples) {
    if (!visible_) return;

    ImGui::Begin("Protocol Analyzer", &visible_);

    const char* protocols[] = {"UART", "SPI", "I2C"};
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("Protocol", &selected_protocol_, protocols, 3);
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu samples)", samples.size());

    ImGui::Separator();

    // ── Per-protocol config ──────────────────────────────────────────────────
    if (selected_protocol_ == 0) {
        // UART
        signalCombo("RX Pin##uart", uart_rx_idx_, available_signals);
        ImGui::SetNextItemWidth(100);
        ImGui::Combo("Baud Rate##uart", &uart_baud_idx_,
                     kBaudLabels, kBaudCount);
        ImGui::SetNextItemWidth(80);
        ImGui::SliderInt("Data Bits##uart", &uart_data_bits_, 5, 8);
        ImGui::SetNextItemWidth(80);
        ImGui::SliderInt("Stop Bits##uart", &uart_stop_bits_, 1, 2);
        ImGui::Checkbox("Parity##uart", &uart_parity_en_);
        if (uart_parity_en_) {
            ImGui::SameLine();
            ImGui::RadioButton("Even##uart", reinterpret_cast<int*>(&uart_parity_odd_), 0);
            ImGui::SameLine();
            ImGui::RadioButton("Odd##uart",  reinterpret_cast<int*>(&uart_parity_odd_), 1);
        }

        double bit_us = (uart_baud_idx_ >= 0 && uart_baud_idx_ < kBaudCount)
                       ? 1e6 / kBaudRates[uart_baud_idx_] : 0.0;
        ImGui::TextDisabled("Bit duration: %.2f us  (need >=2 samples/bit)", bit_us);

    } else if (selected_protocol_ == 1) {
        // SPI
        signalCombo("CLK Pin##spi",  spi_clk_idx_,  available_signals);
        signalCombo("MOSI Pin##spi", spi_mosi_idx_, available_signals, true);
        signalCombo("MISO Pin##spi", spi_miso_idx_, available_signals, true);
        signalCombo("CS Pin##spi",   spi_cs_idx_,   available_signals, true);
        ImGui::Checkbox("CPOL##spi", &spi_cpol_);
        ImGui::SameLine();
        ImGui::Checkbox("CPHA##spi", &spi_cpha_);
        ImGui::SameLine();
        ImGui::Checkbox("LSB first##spi", &spi_lsb_first_);
        ImGui::SetNextItemWidth(80);
        ImGui::SliderInt("Bits/Word##spi", &spi_bits_per_word_, 1, 64);

    } else {
        // I2C
        signalCombo("SCL Pin##i2c", i2c_scl_idx_, available_signals);
        signalCombo("SDA Pin##i2c", i2c_sda_idx_, available_signals);
    }

    ImGui::Separator();

    const bool can_decode = !samples.empty() && !available_signals.empty();
    if (!can_decode) ImGui::BeginDisabled();
    if (ImGui::Button("Decode", ImVec2(100, 0))) {
        frames_.clear();
        if (selected_protocol_ == 0) {
            jtag::protocol::UartConfig cfg;
            cfg.rx_pin       = signalAtIdx(available_signals, uart_rx_idx_);
            cfg.baud_rate    = (uart_baud_idx_ >= 0 && uart_baud_idx_ < kBaudCount)
                               ? kBaudRates[uart_baud_idx_] : 9600;
            cfg.data_bits    = uart_data_bits_;
            cfg.stop_bits    = uart_stop_bits_;
            cfg.parity_enable = uart_parity_en_;
            cfg.parity_odd   = uart_parity_odd_ != 0;
            frames_ = jtag::protocol::decodeUart(samples, cfg);

        } else if (selected_protocol_ == 1) {
            jtag::protocol::SpiConfig cfg;
            cfg.clk_pin      = signalAtIdx(available_signals, spi_clk_idx_);
            cfg.mosi_pin     = signalAtIdx(available_signals, spi_mosi_idx_ - 1);
            cfg.miso_pin     = signalAtIdx(available_signals, spi_miso_idx_ - 1);
            cfg.cs_pin       = signalAtIdx(available_signals, spi_cs_idx_ - 1);
            cfg.cpol         = spi_cpol_;
            cfg.cpha         = spi_cpha_;
            cfg.lsb_first    = spi_lsb_first_;
            cfg.bits_per_word = spi_bits_per_word_;
            frames_ = jtag::protocol::decodeSpi(samples, cfg);

        } else {
            jtag::protocol::I2cConfig cfg;
            cfg.scl_pin = signalAtIdx(available_signals, i2c_scl_idx_);
            cfg.sda_pin = signalAtIdx(available_signals, i2c_sda_idx_);
            frames_ = jtag::protocol::decodeI2c(samples, cfg);
        }
        new_frames_ready_ = true;
        selected_frame_idx_ = -1;
    }
    if (!can_decode) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Clear Results", ImVec2(100, 0))) {
        frames_.clear();
        new_frames_ready_ = true;
        selected_frame_idx_ = -1;
    }

    ImGui::Separator();
    ImGui::TextDisabled("%zu frames decoded", frames_.size());

    // ── Results table ────────────────────────────────────────────────────────
    if (!frames_.empty()) {
        if (ImGui::BeginTable("proto_results", 5,
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable,
                              ImVec2(0, 0))) {
            ImGui::TableSetupColumn("Start (us)",  ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("End (us)",    ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Protocol",    ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Label",       ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Error",       ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableHeadersRow();

            const char* kind_names[] = {"UART", "SPI", "I2C"};

            for (int fi = 0; fi < static_cast<int>(frames_.size()); fi++) {
                const auto& f = frames_[fi];
                ImGui::TableNextRow();

                const bool selected = (fi == selected_frame_idx_);
                if (f.error_flag) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                           IM_COL32(120, 40, 40, 128));
                }

                ImGui::PushID(fi);
                ImGui::TableNextColumn();
                char start_buf[32]; snprintf(start_buf, sizeof(start_buf), "%.1f", f.start_us);
                if (ImGui::Selectable(start_buf,
                                      selected,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    selected_frame_idx_ = fi;
                }
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", f.end_us);
                ImGui::TableNextColumn();
                ImGui::Text("%s", kind_names[static_cast<int>(f.kind)]);
                ImGui::TableNextColumn();
                ImGui::Text("%s", f.label.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s", f.error_flag ? "ERR" : "");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}

} // namespace jtag::gui

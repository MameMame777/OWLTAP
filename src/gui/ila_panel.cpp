// SPDX-License-Identifier: Apache-2.0
#include "ila_panel.h"

#include <imgui.h>
#include <implot.h>
#include <cstdio>
#include <cstring>

#include "src/jtag/jtag_chain.h"

namespace jtag::gui {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static uint32_t parseHex(const char* buf, uint32_t fallback) {
    uint32_t v = fallback;
    // sscanf is safe here: buffer is always null-terminated and 11 bytes max
    if (std::sscanf(buf, "%x", &v) != 1) // NOLINT(cert-err34-c)
        v = fallback;
    return v;
}

static void statusLed(const char* label, bool on, ImVec4 color_on) {
    ImVec4 col = on ? color_on : ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  col);
    ImGui::SmallButton(label);
    ImGui::PopStyleColor(3);
}

/// Format a sample value for display inside a bus lane block.
static void formatSampleValue(char* buf, size_t sz,
                               uint32_t val, BusFormat fmt, int width) {
    switch (fmt) {
        case BusFormat::DEC:
            std::snprintf(buf, sz, "%u", val);
            break;
        case BusFormat::BIN:
            if (width <= 8) {
                char bin[9];
                for (int k = 0; k < width; k++)
                    bin[width - 1 - k] = ((val >> k) & 1u) ? '1' : '0';
                bin[width] = '\0';
                std::snprintf(buf, sz, "0b%s", bin);
                break;
            }
            [[fallthrough]];
        case BusFormat::HEX: {
            const int nib = (width + 3) / 4;
            std::snprintf(buf, sz, "0x%0*X", nib, val);
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / setChain
// ─────────────────────────────────────────────────────────────────────────────

IlaPanel::IlaPanel() {
    std::snprintf(mask_buf_.data(), mask_buf_.size(), "FFFFFFFF");
    std::snprintf(val_buf_.data(),  val_buf_.size(),  "00000000");
}

IlaPanel::~IlaPanel() = default;

void IlaPanel::resetSignalDefs() {
    signals_.clear();
    const int dw = driverOk() ? driver_->dataWidth()
                              : jtag::ila::IlaDriver::kDataWidth;
    IlaSignalDef d{};
    std::snprintf(d.name, sizeof(d.name), "data[%d:0]", dw - 1);
    d.hi  = dw - 1;
    d.lo  = 0;
    d.fmt = BusFormat::HEX;
    signals_.push_back(d);
}

void IlaPanel::setChain(jtag::JtagChain* chain, int device_index) {
    driver_.reset();
    backend_.reset();
    status_valid_ = false;
    has_samples_  = false;
    last_error_.clear();

    if (chain) {
        backend_ = std::make_unique<jtag::ila::ChainIlaTapBackend>(
            *chain, device_index);
        driver_  = std::make_unique<jtag::ila::IlaDriver>(*backend_);
        jtag::ila::IlaCaps caps{};
        (void)driver_->probe(caps);  // best-effort; falls back to defaults
        if (pre_samples_ >= driver_->depth())
            pre_samples_ = driver_->depth() / 4;
        resetSignalDefs();
    }
}

void IlaPanel::setBscaneChain(jtag::JtagChain* chain, int pl_tap_index) {
    driver_.reset();
    backend_.reset();
    status_valid_ = false;
    has_samples_  = false;
    last_error_.clear();

    if (chain) {
        backend_ = std::make_unique<jtag::ila::BscaneIlaTapBackend>(
            *chain, pl_tap_index);
        driver_  = std::make_unique<jtag::ila::IlaDriver>(*backend_);
        jtag::ila::IlaCaps caps{};
        (void)driver_->probe(caps);  // best-effort; falls back to defaults
        if (pre_samples_ >= driver_->depth())
            pre_samples_ = driver_->depth() / 4;
        resetSignalDefs();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Button handlers
// ─────────────────────────────────────────────────────────────────────────────

void IlaPanel::doArm() {
    if (!driverOk()) return;
    uint32_t mask = parseHex(mask_buf_.data(), 0xFFFFFFFFu);
    uint32_t val  = parseHex(val_buf_.data(),  0u);
    auto     pre  = static_cast<uint16_t>(pre_samples_);
    if (!driver_->configureTrigger(mask, val, pre)) {
        last_error_ = driver_->lastError();
        return;
    }
    trigger_dirty_ = false;
    if (!driver_->arm()) {
        last_error_ = driver_->lastError();
        return;
    }
    last_error_.clear();
    pollStatus();
}

void IlaPanel::doStop() {
    if (!driverOk()) return;
    if (!driver_->stop())
        last_error_ = driver_->lastError();
    else
        last_error_.clear();
    pollStatus();
}

void IlaPanel::doForce() {
    if (!driverOk()) return;
    if (!driver_->forceTrigger())
        last_error_ = driver_->lastError();
    else
        last_error_.clear();
    pollStatus();
}

void IlaPanel::doReset() {
    if (!driverOk()) return;
    if (!driver_->resetCapture())
        last_error_ = driver_->lastError();
    else
        last_error_.clear();
    status_valid_ = false;
    has_samples_  = false;
}

void IlaPanel::doRead() {
    if (!driverOk()) return;
    // Start from depth-1 so the BscaneIlaTapBackend prime scan (which
    // auto-increments READ_ADDR once) wraps around to 0 before the first read.
    if (!driver_->setReadAddr(static_cast<uint16_t>(driver_->depth() - 1))) {
        last_error_ = driver_->lastError();
        return;
    }
    samples_.clear();
    if (!driver_->readSamples(samples_)) {
        last_error_ = driver_->lastError();
        return;
    }
    last_error_.clear();
    has_samples_ = !samples_.empty();

    // Build time axis (8 ns/sample)
    const int n = static_cast<int>(samples_.size());
    wave_x_.resize(n);
    for (int i = 0; i < n; ++i)
        wave_x_[i] = static_cast<double>(i) * 8.0;

    if (has_samples_ && sample_cb_)
        sample_cb_(samples_);
}

void IlaPanel::pollStatus() {
    if (!driverOk()) return;
    jtag::ila::IlaStatus s{};
    if (driver_->readStatus(s)) {
        status_       = s;
        status_valid_ = true;
        last_error_.clear();
    } else {
        last_error_ = driver_->lastError();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// draw()
// ─────────────────────────────────────────────────────────────────────────────

void IlaPanel::draw() {
    if (!visible_) return;
    ImGui::SetNextWindowSize(ImVec2(700, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Internal Logic Analyzer", &visible_)) {
        ImGui::End();
        return;
    }

    const bool hw_ok = driverOk();

    // ── CAPS line (shown when probed) ────────────────────────────────
    if (hw_ok && driver_->probed()) {
        const auto& c = driver_->caps();
        ImGui::TextDisabled("CAPS: DW=%d  Depth=%d  NUM_CH=%d  ver=0x%02X",
                            c.data_w, c.depth, c.num_ch, c.version);
    } else if (hw_ok) {
        ImGui::TextDisabled("CAPS: (not probed) -- %s",
                            driver_->lastError().c_str());
    } else {
        ImGui::TextDisabled("CAPS: --");
    }

    // ── Status LEDs ──────────────────────────────────────────────────
    ImGui::Text("Status:");
    ImGui::SameLine();
    statusLed(" Armed ",     status_valid_ && status_.armed,
              ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
    ImGui::SameLine();
    statusLed(" Triggered ", status_valid_ && status_.triggered,
              ImVec4(0.9f, 0.7f, 0.1f, 1.0f));
    ImGui::SameLine();
    statusLed(" Full ",      status_valid_ && status_.full,
              ImVec4(0.2f, 0.5f, 0.9f, 1.0f));
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) pollStatus();

    ImGui::Separator();

    // ── Trigger config ───────────────────────────────────────────────
    ImGui::Text("Trigger Mask (hex):");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputText("##mask", mask_buf_.data(), mask_buf_.size(),
                         ImGuiInputTextFlags_CharsHexadecimal))
        trigger_dirty_ = true;

    ImGui::Text("Trigger Value(hex):");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputText("##val", val_buf_.data(), val_buf_.size(),
                         ImGuiInputTextFlags_CharsHexadecimal))
        trigger_dirty_ = true;

    ImGui::Text("Pre-samples:      ");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderInt("##pre", &pre_samples_, 0,
                         (driverOk() ? driver_->depth() : jtag::ila::IlaDriver::kDepth) - 1))
        trigger_dirty_ = true;

    if (trigger_dirty_)
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "(unsaved changes)");
    else
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "(in sync)       ");

    ImGui::Separator();

    // ── Action buttons ───────────────────────────────────────────────
    ImGui::BeginDisabled(!hw_ok);

    if (ImGui::Button("Arm", ImVec2(70, 0)))    doArm();
    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(70, 0)))   doStop();
    ImGui::SameLine();
    if (ImGui::Button("Force", ImVec2(70, 0)))  doForce();
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(70, 0)))  doReset();

    ImGui::Spacing();

    const bool can_read = hw_ok && status_valid_ && status_.full;
    ImGui::BeginDisabled(!can_read);
    if (ImGui::Button("Read Samples", ImVec2(150, 0))) doRead();
    ImGui::EndDisabled();

    ImGui::EndDisabled();

    // ── Sample summary ───────────────────────────────────────────────
    if (has_samples_) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1),
                           "%zu samples read", samples_.size());
    }

    // ── Signal lane editor ───────────────────────────────────────────
    if (hw_ok)
        drawSignalEditor();

    // ── Error display ────────────────────────────────────────────────
    if (!last_error_.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                           "Error: %s", last_error_.c_str());
    }

    // ── Embedded waveform ────────────────────────────────────────────
    if (has_samples_)
        drawWaveform();

    // ── Auto-poll when armed/triggered ──────────────────────────────
    if (hw_ok && status_valid_ && (status_.armed || status_.triggered)) {
        double now = ImGui::GetTime();
        if (now - last_poll_time_ > 0.2) {   // 5 Hz
            last_poll_time_ = now;
            pollStatus();
        }
    }

    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
// drawSignalEditor  -- collapsible lane-definition table
// ─────────────────────────────────────────────────────────────────────────────

void IlaPanel::drawSignalEditor() {
    if (!ImGui::CollapsingHeader("Signal Definitions")) return;

    if (ImGui::SmallButton("Reset from CAPS")) resetSignalDefs();
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Add")) {
        const int dw = driverOk() ? driver_->dataWidth()
                                  : jtag::ila::IlaDriver::kDataWidth;
        IlaSignalDef d{};
        std::snprintf(d.name, sizeof(d.name), "sig");
        d.hi  = dw - 1;
        d.lo  = 0;
        d.fmt = BusFormat::HEX;
        signals_.push_back(d);
    }

    static const char* kFmtNames[] = {"HEX", "DEC", "BIN"};
    constexpr ImGuiTableFlags kTblFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingFixedFit;

    if (!ImGui::BeginTable("##sigdefs", 6, kTblFlags)) return;

    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("Hi",   ImGuiTableColumnFlags_WidthFixed,  36.0f);
    ImGui::TableSetupColumn("Lo",   ImGuiTableColumnFlags_WidthFixed,  36.0f);
    ImGui::TableSetupColumn("W",    ImGuiTableColumnFlags_WidthFixed,  28.0f);
    ImGui::TableSetupColumn("Fmt",  ImGuiTableColumnFlags_WidthFixed,  52.0f);
    ImGui::TableSetupColumn("",     ImGuiTableColumnFlags_WidthFixed,  20.0f);
    ImGui::TableHeadersRow();

    int to_remove = -1;
    const int n = static_cast<int>(signals_.size());
    for (int i = 0; i < n; i++) {
        IlaSignalDef& s = signals_[i];
        ImGui::TableNextRow();
        ImGui::PushID(i);

        ImGui::TableSetColumnIndex(0);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##n", s.name, sizeof(s.name));

        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputInt("##hi", &s.hi, 0, 0);

        ImGui::TableSetColumnIndex(2);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputInt("##lo", &s.lo, 0, 0);

        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%d", s.width());

        ImGui::TableSetColumnIndex(4);
        int fi = static_cast<int>(s.fmt);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##f", &fi, kFmtNames, 3))
            s.fmt = static_cast<BusFormat>(fi);

        ImGui::TableSetColumnIndex(5);
        if (ImGui::SmallButton("X")) to_remove = i;

        ImGui::PopID();
    }
    if (to_remove >= 0)
        signals_.erase(signals_.begin() + to_remove);

    ImGui::EndTable();
}

// ─────────────────────────────────────────────────────────────────────────────
// drawWaveform  -- embedded ImPlot chart
// ─────────────────────────────────────────────────────────────────────────────

void IlaPanel::drawWaveform() {
    const int n = static_cast<int>(wave_x_.size());
    if (n == 0) return;

    ImGui::Separator();
    ImGui::Text("Waveform  (8 ns/sample @ 125 MHz)");

    const int n_lanes = static_cast<int>(signals_.size());
    if (n_lanes == 0) {
        ImGui::TextDisabled("No signal definitions. Expand 'Signal Definitions' above.");
        return;
    }

    // Y-axis tick positions and labels (lane 0 = topmost = highest Y value)
    std::vector<double>      tick_y(n_lanes);
    std::vector<const char*> tick_names(n_lanes);
    for (int i = 0; i < n_lanes; i++) {
        tick_y[i]     = static_cast<double>(n_lanes - i) - 0.5;
        tick_names[i] = signals_[i].name;
    }

    // Lane color palette (6-color cyclic)
    static const ImU32 kFill[] = {
        IM_COL32(100,149,237, 80), IM_COL32(144,238,144, 80),
        IM_COL32(255,165,  0, 80), IM_COL32(255,105,180, 80),
        IM_COL32( 64,224,208, 80), IM_COL32(238,130,238, 80),
    };
    static const ImU32 kLine[] = {
        IM_COL32(100,149,237,220), IM_COL32(144,238,144,220),
        IM_COL32(255,165,  0,220), IM_COL32(255,105,180,220),
        IM_COL32( 64,224,208,220), IM_COL32(238,130,238,220),
    };
    constexpr int kPalette = 6;
    const ImU32 col_text = IM_COL32(220,220,220,255);
    const double sample_period = 8.0;

    // Scale plot height to number of lanes (≥80 px floor)
    const float lane_px  = 28.0f;
    const float avail_h  = ImGui::GetContentRegionAvail().y - 8.0f;
    const float target_h = lane_px * static_cast<float>(n_lanes) + 20.0f;
    const float plot_h   = std::max({avail_h, target_h, 80.0f});

    if (!ImPlot::BeginPlot("##ila_wave", ImVec2(-1.0f, plot_h),
                           ImPlotFlags_NoTitle))
        return;

    ImPlot::SetupAxes("Time (ns)", nullptr,
                      ImPlotAxisFlags_None,
                      ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, static_cast<double>(n_lanes),
                            ImPlotCond_Always);
    ImPlot::SetupAxisTicks(ImAxis_Y1,
                           tick_y.data(), n_lanes, tick_names.data());

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    for (int lane = 0; lane < n_lanes; lane++) {
        const IlaSignalDef& sig = signals_[lane];
        const int    c    = lane % kPalette;
        const double y_hi = static_cast<double>(n_lanes - lane) - 0.1;
        const double y_lo = static_cast<double>(n_lanes - lane) - 0.9;

        if (sig.width() == 1) {
            // ── Digital lane: step-function ──────────────────────────
            for (int j = 0; j < n; j++) {
                const bool  high = (sig.extract(samples_[j]) != 0u);
                const double y   = high ? y_hi : y_lo;
                const double t0  = wave_x_[j];
                const double t1  = (j + 1 < n) ? wave_x_[j + 1]
                                                : wave_x_[n - 1] + sample_period;
                dl->AddLine(ImPlot::PlotToPixels(t0, y),
                            ImPlot::PlotToPixels(t1, y),
                            kLine[c], 1.5f);
                // Vertical edge on transition
                if (j + 1 < n &&
                    sig.extract(samples_[j + 1]) != sig.extract(samples_[j])) {
                    dl->AddLine(ImPlot::PlotToPixels(t1, y_lo),
                                ImPlot::PlotToPixels(t1, y_hi),
                                kLine[c], 1.5f);
                }
            }
        } else {
            // ── Bus lane: colored run-length blocks ──────────────────
            int run = 0;
            for (int j = 1; j <= n; j++) {
                const bool flush = (j == n) ||
                    (sig.extract(samples_[j]) != sig.extract(samples_[run]));
                if (!flush) continue;

                const double t0 = wave_x_[run];
                const double t1 = (j < n) ? wave_x_[j]
                                          : wave_x_[n - 1] + sample_period;
                const ImVec2 p0 = ImPlot::PlotToPixels(t0, y_hi);
                const ImVec2 p1 = ImPlot::PlotToPixels(t1, y_lo);

                dl->AddRectFilled(p0, p1, kFill[c]);
                dl->AddRect      (p0, p1, kLine[c], 0.0f, 0, 1.5f);

                char label[24];
                formatSampleValue(label, sizeof(label),
                                  sig.extract(samples_[run]),
                                  sig.fmt, sig.width());
                const float bw = p1.x - p0.x;
                const float tw = ImGui::CalcTextSize(label).x;
                if (bw > tw + 6.0f) {
                    dl->AddText(
                        ImVec2((p0.x + p1.x) * 0.5f - tw * 0.5f,
                               (p0.y + p1.y) * 0.5f
                               - ImGui::GetTextLineHeight() * 0.5f),
                        col_text, label);
                }
                run = j;
            }
        }
    }

    // Trigger cursor
    if (pre_samples_ > 0 && pre_samples_ < n) {
        const double tx = wave_x_[pre_samples_];
        dl->AddLine(ImPlot::PlotToPixels(tx, static_cast<double>(n_lanes)),
                    ImPlot::PlotToPixels(tx, 0.0),
                    IM_COL32(255, 80, 80, 220), 1.5f);
    }

    ImPlot::PopPlotClipRect();
    ImPlot::EndPlot();
}

} // namespace jtag::gui

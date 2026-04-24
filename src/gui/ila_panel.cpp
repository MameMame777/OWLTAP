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

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / setChain
// ─────────────────────────────────────────────────────────────────────────────

IlaPanel::IlaPanel() {
    std::snprintf(mask_buf_.data(), mask_buf_.size(), "FFFFFFFF");
    std::snprintf(val_buf_.data(),  val_buf_.size(),  "00000000");
}

IlaPanel::~IlaPanel() = default;

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
// drawWaveform  -- embedded ImPlot chart
// ─────────────────────────────────────────────────────────────────────────────

void IlaPanel::drawWaveform() {
    const int n = static_cast<int>(wave_x_.size());
    if (n == 0) return;

    ImGui::Separator();
    ImGui::Text("Waveform  (8 ns/sample @ 125 MHz)");

    const float plot_h = ImGui::GetContentRegionAvail().y - 8.0f;
    if (ImPlot::BeginPlot("##ila_wave",
                          ImVec2(-1, plot_h > 80.0f ? plot_h : 150.0f),
                          ImPlotFlags_NoTitle)) {
        // Y axis fixed [0,1] normalized — tick labels hidden for bus display
        ImPlot::SetupAxes("Time (ns)", nullptr,
                          ImPlotAxisFlags_None,
                          ImPlotAxisFlags_NoDecorations);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.0, ImPlotCond_Always);

        // Dummy scatter so ImPlot registers the "ILA_DATA[31:0]" legend item
        // (needed to give the plot a stable identity; actual drawing via DrawList)
        {
            static const double dummy_x = 0.0, dummy_y = 0.5;
            ImPlotSpec s;
            s.Marker = ImPlotMarker_None;
            ImPlot::PlotLine("ILA_DATA[31:0]", &dummy_x, &dummy_y, 1, s);
        }

        // Bus blocks drawn directly with ImDrawList
        {
            ImDrawList* dl       = ImPlot::GetPlotDrawList();
            ImPlot::PushPlotClipRect();

            const ImU32 col_fill = IM_COL32(100, 149, 237,  80);
            const ImU32 col_line = IM_COL32(100, 149, 237, 220);
            const ImU32 col_text = IM_COL32(220, 220, 220, 255);
            const double y_lo = 0.1, y_hi = 0.9;

            // Extend last sample one extra period to the right
            const double sample_period = 8.0;

            int run_start = 0;
            for (int j = 1; j <= n; ++j) {
                const bool flush = (j == n) ||
                    (samples_[j] != samples_[run_start]);
                if (!flush) continue;

                const double t0_run = wave_x_[run_start];
                const double t1_run = (j < n)
                    ? wave_x_[j]
                    : wave_x_[n - 1] + sample_period;

                ImVec2 p0 = ImPlot::PlotToPixels(t0_run, y_hi);
                ImVec2 p1 = ImPlot::PlotToPixels(t1_run, y_lo);

                dl->AddRectFilled(p0, p1, col_fill);
                dl->AddRect(p0, p1, col_line, 0.0f, 0, 1.5f);

                // Hex label inside the block
                char label[12];
                std::snprintf(label, sizeof(label), "0x%08X", samples_[run_start]);
                const float block_w = p1.x - p0.x;
                const float text_w  = ImGui::CalcTextSize(label).x;
                if (block_w > text_w + 6.0f) {
                    const float cx = (p0.x + p1.x) * 0.5f - text_w * 0.5f;
                    const float cy = (p0.y + p1.y) * 0.5f
                                   - ImGui::GetTextLineHeight() * 0.5f;
                    dl->AddText(ImVec2(cx, cy), col_text, label);
                }
                run_start = j;
            }

            // Trigger cursor
            if (pre_samples_ > 0 && pre_samples_ < n) {
                const double tx = wave_x_[pre_samples_];
                ImVec2 tp0 = ImPlot::PlotToPixels(tx, 1.2);
                ImVec2 tp1 = ImPlot::PlotToPixels(tx, -0.2);
                dl->AddLine(tp0, tp1, IM_COL32(255, 80, 80, 220), 1.5f);
            }

            ImPlot::PopPlotClipRect();
        }

        ImPlot::EndPlot();
    }
}

} // namespace jtag::gui

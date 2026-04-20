#include "waveform_view.h"

#include <imgui.h>
#include <implot.h>

#include <chrono>

namespace jtag::gui {

std::mutex WaveformView::mutex_;
std::vector<WaveformView::SignalLane> WaveformView::lanes_;
int WaveformView::cursor_pos_ = -1;
int WaveformView::trigger_sample_ = -1;
double WaveformView::trigger_time_ = 0.0;
size_t WaveformView::selected_signal_count_ = 0;
double WaveformView::latest_time_ = 0.0;
bool WaveformView::auto_scroll_ = false;
double WaveformView::window_us_ = 10000.0;  // 10ms default window

void WaveformView::setData(const std::vector<std::string>& signals,
                            const std::vector<jtag::SampleFrame>& samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    lanes_.clear();
    trigger_sample_ = -1;
    trigger_time_ = 0.0;
    latest_time_ = 0.0;
    selected_signal_count_ = signals.size();

    if (samples.empty() || signals.empty()) return;

    auto t0 = samples.front().timestamp;

    for (const auto& sig : signals) {
        SignalLane lane;
        lane.name = sig;
        lane.times.reserve(samples.size());
        lane.values.reserve(samples.size());

        for (size_t i = 0; i < samples.size(); i++) {
            double us = static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    samples[i].timestamp - t0).count());
            lane.times.push_back(us);

            jtag::PinState state = samples[i].data.getPin(sig);
            lane.values.push_back(state == jtag::PinState::HIGH ? 1.0 : 0.0);

            if (samples[i].trigger_point && trigger_sample_ < 0) {
                trigger_sample_ = static_cast<int>(i);
                trigger_time_ = us;
            }
        }
        lanes_.push_back(std::move(lane));
    }
    // Record the latest time for auto-scroll
    if (!lanes_.empty() && !lanes_[0].times.empty())
        latest_time_ = lanes_[0].times.back();
}

void WaveformView::clearData() {
    std::lock_guard<std::mutex> lock(mutex_);
    lanes_.clear();
    cursor_pos_ = -1;
    trigger_sample_ = -1;
    trigger_time_ = 0.0;
    latest_time_ = 0.0;
    selected_signal_count_ = 0;
}

void WaveformView::setCursorPosition(int sample_index) {
    cursor_pos_ = sample_index;
}

int WaveformView::cursorPosition() {
    return cursor_pos_;
}

bool WaveformView::draw(bool can_clear) {
    bool clear_requested = false;
    ImGui::Begin("Waveforms");

    if (!can_clear) {
        ImGui::BeginDisabled();
    }
    clear_requested = ImGui::Button("Clear");
    if (!can_clear) {
        ImGui::EndDisabled();
    }
    ImGui::Separator();

    std::lock_guard<std::mutex> lock(mutex_);

    if (lanes_.empty()) {
        const char* message = (selected_signal_count_ == 0)
            ? "No signals selected.\nLoad BSDL and select signals."
            : "No waveform data.\nRun capture to display selected signals.";
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", message);
        ImGui::End();
        return clear_requested;
    }

    // ── Sweep controls ─────────────────────────────────────────────
    ImGui::Checkbox("Auto Scroll", &auto_scroll_);
    if (auto_scroll_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        float win_ms = static_cast<float>(window_us_ / 1000.0);
        if (ImGui::DragFloat("Window (ms)", &win_ms, 0.5f, 1.0f, 60000.0f, "%.1f ms"))
            window_us_ = win_ms * 1000.0;
    }
    ImGui::Separator();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 50 || avail.y < 50) {
        ImGui::End();
        return clear_requested;
    }

    // One subplot per signal lane (stacked vertically)
    int num_lanes = static_cast<int>(lanes_.size());
    if (ImPlot::BeginSubplots("##waveforms", num_lanes, 1, avail,
                               ImPlotSubplotFlags_LinkAllX)) {
        for (int i = 0; i < num_lanes; i++) {
            const auto& lane = lanes_[i];
            if (lane.times.empty()) continue;

            char plot_id[128];
            snprintf(plot_id, sizeof(plot_id), "##lane_%d", i);

            if (ImPlot::BeginPlot(plot_id, ImVec2(-1, 0),
                                   ImPlotFlags_NoLegend |
                                   ImPlotFlags_NoMouseText)) {
                // Auto-scroll: always show [latest-window, latest]
                if (auto_scroll_ && latest_time_ > 0.0) {
                    ImPlot::SetupAxisLimits(ImAxis_X1,
                        latest_time_ - window_us_, latest_time_,
                        ImPlotCond_Always);
                }
                ImPlot::SetupAxisLimits(ImAxis_Y1, -0.2, 1.4,
                                        ImPlotCond_Always);
                ImPlot::SetupAxis(ImAxis_X1, (i == num_lanes - 1)
                                  ? "Time (us)" : nullptr);
                ImPlot::SetupAxis(ImAxis_Y1, lane.name.c_str());
                ImPlot::SetupAxisTicks(ImAxis_Y1, nullptr, 0);

                // Digital waveform as step plot
                ImPlotSpec spec_wave(
                    ImPlotProp_LineColor, ImVec4(0.53f, 0.70f, 0.98f, 1.0f),
                    ImPlotProp_LineWeight, 2.0f);
                ImPlot::PlotStairs(lane.name.c_str(),
                                   lane.times.data(),
                                   lane.values.data(),
                                   static_cast<int>(lane.times.size()),
                                   spec_wave);

                // Trigger marker
                if (trigger_sample_ >= 0) {
                    double t = trigger_time_;
                    ImPlotSpec spec_trig(
                        ImPlotProp_LineColor, ImVec4(1.0f, 0.4f, 0.4f, 0.8f),
                        ImPlotProp_LineWeight, 2.0f);
                    double trigger_xs[2] = {t, t};
                    double trigger_ys[2] = {-0.2, 1.4};
                    ImPlot::PlotLine("##trigger", trigger_xs, trigger_ys, 2,
                                     spec_trig);
                }

                ImPlot::EndPlot();
            }
        }
        ImPlot::EndSubplots();
    }

    ImGui::End();
    return clear_requested;
}

} // namespace jtag::gui

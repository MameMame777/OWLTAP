#include "waveform_view.h"

#include <imgui.h>
#include <implot.h>

#include <chrono>
#include <cstdio>

#include "bus_definition.h"

namespace jtag::gui {

std::mutex WaveformView::mutex_;
std::vector<WaveformView::SignalLane> WaveformView::lanes_;
int WaveformView::cursor_pos_ = -1;
int WaveformView::trigger_sample_ = -1;
double WaveformView::trigger_time_ = 0.0;
size_t WaveformView::selected_signal_count_ = 0;
double WaveformView::latest_time_ = 0.0;
double WaveformView::earliest_time_ = 0.0;
bool WaveformView::auto_scroll_ = false;
double WaveformView::window_us_ = 10000.0;  // 10ms default window
bool WaveformView::fit_requested_ = false;
bool WaveformView::reset_view_requested_ = false;
std::vector<jtag::protocol::DecodedFrame> WaveformView::annotations_;
jtag::xdc::PinAliasMap WaveformView::signal_aliases_;

std::chrono::steady_clock::time_point WaveformView::t0_;
std::vector<std::string> WaveformView::lane_signals_;
std::vector<std::string> WaveformView::lane_bus_names_;

void WaveformView::setData(const std::vector<std::string>& signals,
                            const std::vector<BusDefinition>& buses,
                            const std::vector<jtag::SampleFrame>& samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    lanes_.clear();
    trigger_sample_ = -1;
    trigger_time_ = 0.0;
    latest_time_ = 0.0;
    earliest_time_ = 0.0;
    selected_signal_count_ = signals.size() + buses.size();

    if (samples.empty() || (signals.empty() && buses.empty())) return;

    auto t0 = samples.front().timestamp;

    // During auto-scroll, only rebuild samples within 2x the visible window to
    // keep lane reconstruction O(window) instead of O(ring-buffer-depth).
    // Exception: when a fit is pending, rebuild the full buffer so the fit
    // covers the complete acquisition range.
    const double total_us = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            samples.back().timestamp - t0).count());
    const double clip_start_us = (auto_scroll_ && !fit_requested_)
        ? (total_us - window_us_ * 2.0) : -1.0;

    // Binary signal lanes
    for (const auto& sig : signals) {
        SignalLane lane;
        lane.kind = LaneKind::BINARY;
        // Apply XDC alias for display; data lookup still uses `sig` (BSDL name).
        {
            auto it = signal_aliases_.find(sig);
            if (it != signal_aliases_.end()) {
                lane.name = it->second + " (" + sig + ")";
            } else {
                lane.name = sig;
            }
        }
        lane.times.reserve(samples.size());
        lane.values.reserve(samples.size());

        for (size_t i = 0; i < samples.size(); i++) {
            double us = static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    samples[i].timestamp - t0).count());
            if (us < clip_start_us) continue;  // outside visible window, skip
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

    // Bus lanes
    for (const auto& bus : buses) {
        SignalLane lane;
        lane.kind       = LaneKind::BUS;
        lane.name       = bus.name;
        lane.bus_width  = static_cast<int>(bus.signals.size());
        lane.bus_format = bus.format;
        lane.times.reserve(samples.size());
        lane.bus_values.reserve(samples.size());

        for (size_t i = 0; i < samples.size(); i++) {
            double us = static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    samples[i].timestamp - t0).count());
            if (us < clip_start_us) continue;  // outside visible window, skip
            lane.times.push_back(us);
            lane.bus_values.push_back(computeBusValue(bus, samples[i].data));

            if (samples[i].trigger_point && trigger_sample_ < 0) {
                trigger_sample_ = static_cast<int>(i);
                trigger_time_ = us;
            }
        }
        lanes_.push_back(std::move(lane));
    }

    // Record the latest time for auto-scroll
    if (!lanes_.empty() && !lanes_[0].times.empty()) {
        earliest_time_ = lanes_[0].times.front();
        latest_time_ = lanes_[0].times.back();
    }

    // Store incremental-append state.
    t0_ = samples.front().timestamp;
    lane_signals_ = signals;
    lane_bus_names_.clear();
    for (const auto& b : buses) lane_bus_names_.push_back(b.name);
}

void WaveformView::clearData() {
    std::lock_guard<std::mutex> lock(mutex_);
    lanes_.clear();
    cursor_pos_ = -1;
    trigger_sample_ = -1;
    trigger_time_ = 0.0;
    latest_time_ = 0.0;
    earliest_time_ = 0.0;
    selected_signal_count_ = 0;
    fit_requested_ = false;
    reset_view_requested_ = false;
    lane_signals_.clear();
    lane_bus_names_.clear();
}

bool WaveformView::appendData(const std::vector<std::string>& signals,
                               const std::vector<BusDefinition>& buses,
                               const std::vector<jtag::SampleFrame>& new_frames,
                               size_t evict_front_count) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Verify signal/bus config matches the current lanes.
    if (signals != lane_signals_) return false;
    if (buses.size() != lane_bus_names_.size()) return false;
    for (size_t i = 0; i < buses.size(); i++) {
        if (buses[i].name != lane_bus_names_[i]) return false;
    }
    // Lanes not yet initialised (e.g. first call before setData).
    if (lanes_.empty() && (!signals.empty() || !buses.empty())) return false;

    // Evict oldest data points from the front of each lane.
    if (evict_front_count > 0) {
        for (auto& lane : lanes_) {
            const size_t n = std::min(evict_front_count, lane.times.size());
            lane.times.erase(lane.times.begin(), lane.times.begin() + static_cast<ptrdiff_t>(n));
            if (lane.kind == LaneKind::BINARY) {
                lane.values.erase(lane.values.begin(),
                                  lane.values.begin() + static_cast<ptrdiff_t>(n));
            } else {
                lane.bus_values.erase(lane.bus_values.begin(),
                                      lane.bus_values.begin() + static_cast<ptrdiff_t>(n));
            }
        }
        // Recalculate earliest_time_ after eviction.
        if (!lanes_.empty() && !lanes_[0].times.empty()) {
            earliest_time_ = lanes_[0].times.front();
        }
    }

    if (new_frames.empty()) return true;

    // Append new frames to each lane.
    size_t lane_idx = 0;
    for (const auto& sig : signals) {
        auto& lane = lanes_[lane_idx++];
        for (const auto& frame : new_frames) {
            const double us = static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    frame.timestamp - t0_).count());
            lane.times.push_back(us);
            const jtag::PinState state = frame.data.getPin(sig);
            lane.values.push_back(state == jtag::PinState::HIGH ? 1.0 : 0.0);
            if (frame.trigger_point && trigger_sample_ < 0) {
                trigger_sample_ = static_cast<int>(lane.times.size()) - 1;
                trigger_time_ = us;
            }
        }
    }
    for (const auto& bus : buses) {
        auto& lane = lanes_[lane_idx++];
        for (const auto& frame : new_frames) {
            const double us = static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    frame.timestamp - t0_).count());
            lane.times.push_back(us);
            lane.bus_values.push_back(computeBusValue(bus, frame.data));
        }
    }

    // Update time bounds.
    if (!lanes_.empty() && !lanes_[0].times.empty()) {
        if (lanes_[0].times.size() == new_frames.size()) {
            // All old data was evicted; reset earliest_time_.
            earliest_time_ = lanes_[0].times.front();
        }
        latest_time_ = lanes_[0].times.back();
    }

    return true;
}

void WaveformView::setSignalAliases(const jtag::xdc::PinAliasMap& aliases) {
    signal_aliases_ = aliases;
    // Note: lane names are rebuilt on the next setData() call.
    // Existing lanes are not retroactively renamed to keep this lock-free.
}

void WaveformView::setCursorPosition(int sample_index) {
    cursor_pos_ = sample_index;
}

int WaveformView::cursorPosition() {
    return cursor_pos_;
}

void WaveformView::requestFit() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto_scroll_ = false;
    fit_requested_ = true;
}

void WaveformView::requestResetView() {
    std::lock_guard<std::mutex> lock(mutex_);
    fit_requested_ = false;
    reset_view_requested_ = true;
}

void WaveformView::setAnnotations(
        const std::vector<jtag::protocol::DecodedFrame>& frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    annotations_ = frames;
}

WaveformView::DrawActions WaveformView::draw(bool can_capture, bool can_stop) {
    DrawActions actions;
    ImGui::Begin("Waveforms");

    if (!can_capture) {
        ImGui::BeginDisabled();
    }
    actions.run_requested = ImGui::Button("Run");
    ImGui::SameLine();
    actions.single_requested = ImGui::Button("Single");
    if (!can_capture) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (!can_stop) {
        ImGui::BeginDisabled();
    }
    actions.stop_requested = ImGui::Button("Stop");
    if (!can_stop) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (!can_capture) {
        ImGui::BeginDisabled();
    }
    actions.clear_requested = ImGui::Button("Clear");
    ImGui::SameLine();
    actions.fit_requested = ImGui::Button("Fit");
    if (!can_capture) {
        ImGui::EndDisabled();
    }

    // Capturing indicator (FREE_RUN without auto-scroll: view is frozen but
    // acquisition is still live, so show a visible in-panel animation).
    if (can_stop && !auto_scroll_) {
        static const char* kDots[] = { ".", "..", "...", "...." };
        int idx = static_cast<int>(ImGui::GetTime() * 4.0) % 4;
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f),
                           "Capturing%s", kDots[idx]);
    }

    ImGui::Separator();

    std::lock_guard<std::mutex> lock(mutex_);

    if (actions.fit_requested && !lanes_.empty()) {
        fit_requested_ = true;
    }

    if (lanes_.empty()) {
        const char* message = (selected_signal_count_ == 0)
            ? "No signals selected.\nLoad BSDL and select signals."
            : "No waveform data.\nRun capture to display selected signals.";
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", message);
        ImGui::End();
        return actions;
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
        return actions;
    }

    bool apply_fit = false;
    double fit_min_x = earliest_time_;
    double fit_max_x = latest_time_;
    if (fit_requested_ && latest_time_ >= earliest_time_) {
        apply_fit = true;
        if (fit_max_x <= fit_min_x) {
            fit_max_x = fit_min_x + 1.0;
        }
    }

    bool apply_reset = false;
    double reset_max_x = 1.0;
    if (reset_view_requested_) {
        apply_reset = true;
        reset_max_x = auto_scroll_
            ? (latest_time_ > window_us_ ? latest_time_ : window_us_)
            : (latest_time_ > 1.0 ? latest_time_ : 1.0);
    }

    // One subplot per signal lane (stacked vertically), plus 1 annotation lane
    const bool has_annotations = !annotations_.empty();
    int num_lanes = static_cast<int>(lanes_.size()) + (has_annotations ? 1 : 0);
    if (ImPlot::BeginSubplots("##waveforms", num_lanes, 1, avail,
                               ImPlotSubplotFlags_LinkAllX)) {
        // Signal/bus lanes
        for (int i = 0; i < static_cast<int>(lanes_.size()); i++) {
            const auto& lane = lanes_[i];
            if (lane.times.empty()) continue;

            char plot_id[128];
            snprintf(plot_id, sizeof(plot_id), "##lane_%d", i);

            if (ImPlot::BeginPlot(plot_id, ImVec2(-1, 0),
                                   ImPlotFlags_NoLegend |
                                   ImPlotFlags_NoMouseText)) {
                if (apply_fit) {
                    ImPlot::SetupAxisLimits(ImAxis_X1, fit_min_x, fit_max_x,
                                            ImPlotCond_Always);
                } else if (apply_reset) {
                    ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, reset_max_x,
                                            ImPlotCond_Always);
                } else if (auto_scroll_ && latest_time_ >= 0.0) {
                    // Auto-scroll: clamp the visible range to start at 0 for
                    // a new acquisition, then follow the latest data.
                    const double min_x =
                        latest_time_ > window_us_ ? latest_time_ - window_us_ : 0.0;
                    const double max_x =
                        latest_time_ > window_us_ ? latest_time_ : window_us_;
                    ImPlot::SetupAxisLimits(ImAxis_X1,
                        min_x, max_x,
                        ImPlotCond_Always);
                }
                ImPlot::SetupAxisLimits(ImAxis_Y1, -0.2, 1.4,
                                        ImPlotCond_Always);
                ImPlot::SetupAxis(ImAxis_X1, (!has_annotations && i == static_cast<int>(lanes_.size()) - 1)
                                  ? "Time (us)" : nullptr);
                ImPlot::SetupAxis(ImAxis_Y1, lane.name.c_str());
                ImPlot::SetupAxisTicks(ImAxis_Y1, nullptr, 0);

                // Digital waveform as step plot (BINARY lane)
                if (lane.kind == LaneKind::BINARY) {
                    ImPlotSpec spec_wave(
                        ImPlotProp_LineColor, ImVec4(0.53f, 0.70f, 0.98f, 1.0f),
                        ImPlotProp_LineWeight, 2.0f);
                    ImPlot::PlotStairs(lane.name.c_str(),
                                       lane.times.data(),
                                       lane.values.data(),
                                       static_cast<int>(lane.times.size()),
                                       spec_wave);
                }

                // Bus lane: draw colored blocks with value labels
                if (lane.kind == LaneKind::BUS && !lane.bus_values.empty()) {
                    const int n = static_cast<int>(lane.times.size());
                    const int width = lane.bus_width;
                    ImDrawList* dl = ImPlot::GetPlotDrawList();
                    ImPlot::PushPlotClipRect();

                    const ImU32 col_fill = IM_COL32(100, 149, 237, 80);   // cornflower blue, semi-transparent
                    const ImU32 col_line = IM_COL32(100, 149, 237, 220);
                    const ImU32 col_text = IM_COL32(220, 220, 220, 255);
                    const float y_lo = 0.15f, y_hi = 0.85f;

                    // Determine end time of each run of equal value
                    int run_start = 0;
                    for (int j = 1; j <= n; j++) {
                        bool flush = (j == n) ||
                                     (lane.bus_values[j] != lane.bus_values[run_start]);
                        if (!flush) continue;

                        double t0_run = lane.times[run_start];
                        double t1_run = (j < n) ? lane.times[j] : lane.times[n - 1];
                        uint64_t val  = lane.bus_values[run_start];

                        ImVec2 p0 = ImPlot::PlotToPixels(t0_run, y_hi);
                        ImVec2 p1 = ImPlot::PlotToPixels(t1_run, y_lo);

                        dl->AddRectFilled(p0, p1, col_fill);
                        dl->AddRect(p0, p1, col_line, 0.0f, 0, 1.5f);

                        // Format label
                        char label[64];
                        switch (lane.bus_format) {
                            case BusFormat::DEC:
                                snprintf(label, sizeof(label), "%llu",
                                         static_cast<unsigned long long>(val));
                                break;
                            case BusFormat::BIN: {
                                int show_bits = (width <= 16) ? width : 16;
                                int out = 0;
                                label[out++] = '0'; label[out++] = 'b';
                                for (int b = show_bits - 1; b >= 0; b--)
                                    label[out++] = (val >> b) & 1 ? '1' : '0';
                                if (width > 16) { label[out++] = '.'; label[out++] = '.'; }
                                label[out] = '\0';
                                break;
                            }
                            default:  // HEX
                                snprintf(label, sizeof(label), "0x%0*llX",
                                         (width + 3) / 4,
                                         static_cast<unsigned long long>(val));
                                break;
                        }

                        // Draw label only if the block is wide enough
                        float block_w = p1.x - p0.x;
                        float text_w  = ImGui::CalcTextSize(label).x;
                        if (block_w > text_w + 6.0f) {
                            float cx = (p0.x + p1.x) * 0.5f - text_w * 0.5f;
                            float cy = (p0.y + p1.y) * 0.5f -
                                       ImGui::GetTextLineHeight() * 0.5f;
                            dl->AddText(ImVec2(cx, cy), col_text, label);
                        }
                        run_start = j;
                    }
                    ImPlot::PopPlotClipRect();
                }

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
        }  // end signal/bus lanes loop

        // Annotation lane (protocol decode results)
        if (has_annotations) {
            if (ImPlot::BeginPlot("##annotations", ImVec2(-1, 0),
                                   ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
                if (apply_fit) {
                    ImPlot::SetupAxisLimits(ImAxis_X1, fit_min_x, fit_max_x, ImPlotCond_Always);
                } else if (apply_reset) {
                    ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, reset_max_x, ImPlotCond_Always);
                } else if (auto_scroll_ && latest_time_ >= 0.0) {
                    const double min_x = latest_time_ > window_us_ ? latest_time_ - window_us_ : 0.0;
                    const double max_x = latest_time_ > window_us_ ? latest_time_ : window_us_;
                    ImPlot::SetupAxisLimits(ImAxis_X1, min_x, max_x, ImPlotCond_Always);
                }
                ImPlot::SetupAxisLimits(ImAxis_Y1, -0.2, 1.4, ImPlotCond_Always);
                ImPlot::SetupAxis(ImAxis_X1, "Time (us)");
                ImPlot::SetupAxis(ImAxis_Y1, "Protocol");
                ImPlot::SetupAxisTicks(ImAxis_Y1, nullptr, 0);

                ImDrawList* dl = ImPlot::GetPlotDrawList();
                ImPlot::PushPlotClipRect();

                for (const auto& frame : annotations_) {
                    const ImU32 col_fill = frame.error_flag
                        ? IM_COL32(180, 50, 50, 90)
                        : IM_COL32(80, 180, 100, 90);
                    const ImU32 col_line = frame.error_flag
                        ? IM_COL32(220, 80, 80, 200)
                        : IM_COL32(100, 200, 120, 200);
                    const ImU32 col_text = IM_COL32(230, 230, 230, 255);

                    ImVec2 p0 = ImPlot::PlotToPixels(frame.start_us, 0.85);
                    ImVec2 p1 = ImPlot::PlotToPixels(frame.end_us,   0.15);

                    dl->AddRectFilled(p0, p1, col_fill, 3.0f);
                    dl->AddRect(p0, p1, col_line, 3.0f, 0, 1.5f);

                    float block_w = p1.x - p0.x;
                    float text_w  = ImGui::CalcTextSize(frame.label.c_str()).x;
                    if (block_w > text_w + 4.0f) {
                        float cx = (p0.x + p1.x) * 0.5f - text_w * 0.5f;
                        float cy = (p0.y + p1.y) * 0.5f -
                                   ImGui::GetTextLineHeight() * 0.5f;
                        dl->AddText(ImVec2(cx, cy), col_text, frame.label.c_str());
                    }
                }
                ImPlot::PopPlotClipRect();
                ImPlot::EndPlot();
            }
        }

        ImPlot::EndSubplots();
    }

    if (apply_fit) {
        fit_requested_ = false;
    }
    if (apply_reset) {
        reset_view_requested_ = false;
    }

    ImGui::End();
    return actions;
}

} // namespace jtag::gui

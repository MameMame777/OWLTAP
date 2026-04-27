#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "app_config.h"
#include "src/capture/capture_engine.h"
#include "src/protocol/protocol.h"
#include "src/xdc/xdc_parser.h"

namespace jtag::gui {

/// Waveform rendering panel using ImPlot digital signals.
class WaveformView {
public:
    /// Draw the waveform panel (call each frame).
    /// Returns which panel actions were requested.
    struct DrawActions {
        bool run_requested = false;
        bool single_requested = false;
        bool stop_requested = false;
        bool clear_requested = false;
        bool fit_requested = false;
        bool trigger_requested = false;
    };

    static DrawActions draw(bool can_capture, bool can_stop);

    /// Set waveform data (thread-safe; called from main thread after copy).
    /// @param signals  Binary signal names in display order.
    /// @param buses    Multi-bit bus definitions (rendered below binary lanes).
    /// @param samples  Captured sample frames.
    static void setData(const std::vector<std::string>& signals,
                        const std::vector<BusDefinition>& buses,
                        const std::vector<jtag::SampleFrame>& samples);

    /// Incrementally append new frames to existing lanes.
    /// Evicts @p evict_front_count oldest data points from the front of each lane.
    /// @return false if signal/bus config changed; caller must call setData() instead.
    static bool appendData(const std::vector<std::string>& signals,
                           const std::vector<BusDefinition>& buses,
                           const std::vector<jtag::SampleFrame>& new_frames,
                           size_t evict_front_count);

    /// Clear displayed data.
    static void clearData();

    /// Apply XDC pin aliases: maps BSDL pin name -> display label for waveform lanes.
    /// Call after setData() or independently. Pass empty map to clear all aliases.
    static void setSignalAliases(const jtag::xdc::PinAliasMap& aliases);

    /// Set cursor position (sample index).
    static void setCursorPosition(int sample_index);
    static int cursorPosition();

    /// Request a one-shot fit to the current buffered time range.
    static void requestFit();

    /// Reset the waveform viewport when the next acquisition data arrives.
    static void requestResetView();

    /// Returns true when Auto Scroll is enabled.
    static bool isAutoScroll() { return auto_scroll_; }

    /// Set protocol decoder annotations to overlay on the bottom of the plot area.
    static void setAnnotations(const std::vector<jtag::protocol::DecodedFrame>& frames);

private:
    enum class LaneKind { BINARY, BUS };

    struct SignalLane {
        LaneKind kind = LaneKind::BINARY;
        std::string name;
        // BINARY data
        std::vector<double> times;   // microseconds
        std::vector<double> values;  // 0.0 or 1.0
        // BUS data
        std::vector<uint64_t> bus_values;
        int bus_width = 0;
        BusFormat bus_format = BusFormat::HEX;
    };

    static std::mutex mutex_;
    static std::vector<SignalLane> lanes_;
    static int cursor_pos_;
    static int trigger_sample_;
    static double trigger_time_;
    static size_t selected_signal_count_;
    static double latest_time_;
    static double earliest_time_;
    static bool auto_scroll_;
    static double window_us_;
    static bool fit_requested_;
    static bool reset_view_requested_;
    static std::vector<jtag::protocol::DecodedFrame> annotations_;
    static jtag::xdc::PinAliasMap signal_aliases_;

    // Incremental-append state
    static std::chrono::steady_clock::time_point t0_;  // timestamp of first sample
    static std::vector<std::string> lane_signals_;     // signals used to build current lanes
    static std::vector<std::string> lane_bus_names_;   // bus names used to build current lanes
};

} // namespace jtag::gui

#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "src/capture/capture_engine.h"

namespace jtag::gui {

/// Waveform rendering panel using ImPlot digital signals.
class WaveformView {
public:
    /// Draw the waveform panel (call each frame).
    /// Returns true when the Clear button is pressed.
    static bool draw(bool can_clear);

    /// Set waveform data (thread-safe; called from main thread after copy).
    static void setData(const std::vector<std::string>& signals,
                        const std::vector<jtag::SampleFrame>& samples);

    /// Clear displayed data.
    static void clearData();

    /// Set cursor position (sample index).
    static void setCursorPosition(int sample_index);
    static int cursorPosition();

private:
    struct SignalLane {
        std::string name;
        std::vector<double> times;   // in microseconds
        std::vector<double> values;  // 0.0 or 1.0
    };

    static std::mutex mutex_;
    static std::vector<SignalLane> lanes_;
    static int cursor_pos_;
    static int trigger_sample_;
    static double trigger_time_;
    static size_t selected_signal_count_;
    static double latest_time_;   // latest sample time (us), for auto-scroll
    static bool auto_scroll_;     // follow latest data live
    static double window_us_;     // visible time window width (us)
};

} // namespace jtag::gui

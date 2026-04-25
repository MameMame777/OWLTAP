#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_config.h"
#include "src/ila/bscane_ila_tap_backend.h"
#include "src/ila/ila_driver.h"
#include "src/ila/ila_tap_backend.h"

namespace jtag {
class JtagChain;
} // namespace jtag

namespace jtag::gui {

/// Describes a named bit-field slice of the ILA captured data word.
///
/// hi and lo are inclusive bit indices (0 = LSB).  When width() == 1 the lane
/// is rendered as a digital step-function; wider fields use bus-block style.
struct IlaSignalDef {
    char      name[32] = {};       ///< Display label (null-terminated)
    int       hi  = 31;            ///< MSB index (inclusive, 0-based)
    int       lo  = 0;             ///< LSB index (inclusive, 0-based)
    BusFormat fmt = BusFormat::HEX;

    int width() const { return hi - lo + 1; }

    /// Extract this field's value from a raw 32-bit sample word.
    uint32_t extract(uint32_t sample) const {
        const int w = width();
        if (w <= 0 || lo < 0) return 0u;
        const uint32_t mask = (w >= 32) ? 0xFFFF'FFFFu : ((1u << w) - 1u);
        return (sample >> lo) & mask;
    }
};

/// Dockable ImGui panel for the Internal Logic Analyzer IP.
///
/// Provides trigger configuration (mask/value/pre-samples), Arm/Stop/Force
/// buttons, status LEDs, a one-click "Read Samples", and an embedded
/// ImPlot waveform display scaled in nanoseconds (8 ns/sample @ 125 MHz).
class IlaPanel {
public:
    /// Called after a successful sample read.  `samples` contains exactly
    /// `IlaDriver::kDepth` uint32 words (oldest first, relative to
    /// trigger position).  Optional — IlaPanel also shows its own plot.
    using SampleCallback =
        std::function<void(const std::vector<uint32_t>& samples)>;

    IlaPanel();
    ~IlaPanel();

    IlaPanel(const IlaPanel&)            = delete;
    IlaPanel& operator=(const IlaPanel&) = delete;

    /// Attach (or detach with nullptr) the JTAG chain used for ILA access.
    void setChain(jtag::JtagChain* chain, int device_index);

    /// Attach the chain for a BSCANE2-based ILA (`BscaneIlaTapBackend`).
    void setBscaneChain(jtag::JtagChain* chain, int pl_tap_index);

    /// Optional external callback that also receives samples after a read.
    void setSampleCallback(SampleCallback cb) { sample_cb_ = std::move(cb); }

    /// Render the panel.  Call each frame inside an ImGui window/dockspace.
    void draw();

    bool isVisible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }

    // ── Signal lane persistence (cfg.json round-trip) ───────────────────────

    /// Export current signal-lane definitions as persistable configs.
    std::vector<IlaSignalConfig> exportSignalConfigs() const;

    /// Replace lane definitions with persisted configs (e.g. from cfg.json).
    /// If `cfgs` is empty, the current definitions are kept untouched.
    void importSignalConfigs(const std::vector<IlaSignalConfig>& cfgs);

    /// Returns true when signal definitions were successfully read from the
    /// RTL SIG_DEF register during the last setBscaneChain()/setChain() call.
    bool hasSigDefsFromRtl() const { return rtl_sig_defs_loaded_; }

private:
    void doArm();
    void doStop();
    void doForce();
    void doReset();
    void doRead();
    void pollStatus();
    void drawWaveform();      // multi-lane ImPlot chart
    void drawSignalEditor();   // collapsible lane-definition table
    void resetSignalDefs();    // populate default from IlaCaps.data_w

    bool driverOk() const { return driver_ != nullptr; }

    // ILA backend
    std::unique_ptr<jtag::ila::IlaTapBackend> backend_;
    std::unique_ptr<jtag::ila::IlaDriver>     driver_;

    // Trigger config UI state
    std::array<char, 11> mask_buf_{};
    std::array<char, 11> val_buf_{};
    int                  pre_samples_   = jtag::ila::IlaDriver::kDepth / 4;
    bool                 trigger_dirty_ = false;

    // Status
    jtag::ila::IlaStatus status_{};
    bool                 status_valid_ = false;
    std::string          last_error_;

    // Read results
    std::vector<uint32_t> samples_;
    bool                  has_samples_ = false;

    // Waveform plot data (x = time in ns; y not needed for bus display)
    std::vector<double>    wave_x_;

    // Signal lane definitions (auto-populated from probe(); user-editable)
    std::vector<IlaSignalDef> signals_;
    bool                      rtl_sig_defs_loaded_ = false; // true when SIG_DEF read from RTL

    // Poll timer
    double last_poll_time_ = 0.0;

    SampleCallback sample_cb_;
    bool           visible_ = false;
};

} // namespace jtag::gui

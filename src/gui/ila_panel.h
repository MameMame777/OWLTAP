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

// Forward-declare to avoid including gui_daemon_client.h in the header.
namespace jtag::gui { class GuiDaemonClient; }

namespace jtag::gui {

/// Per-signal-lane trigger condition selector.
enum class TriggerCond {
    None   = 0,  ///< Lane excluded from trigger
    Eq     = 1,  ///< (data & lane_bits) == trig_value
    Neq    = 2,  ///< (data & lane_bits) != trig_value
    Rise   = 3,  ///< Rising edge on any bit in lane
    Fall   = 4,  ///< Falling edge on any bit in lane
    Either = 5,  ///< Either edge on any bit in lane
};

/// Per-lane trigger configuration (mirrored in IlaSignalConfig for persistence).
struct LaneTrigger {
    TriggerCond cond    = TriggerCond::None;  ///< Group A condition
    uint32_t    value   = 0;                  ///< Group A value (Eq/Neq only)
    TriggerCond cond_b  = TriggerCond::None;  ///< Group B condition (None/Eq/Neq only)
    uint32_t    value_b = 0;                  ///< Group B value (Eq/Neq only)
};

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
    /// @param user_chain  1=USER1, 2=USER2, 3=USER3, 4=USER4 (default 1).
    void setBscaneChain(jtag::JtagChain* chain, int pl_tap_index,
                        int user_chain = 1);

    /// Attach a daemon client for daemon-mode ILA access.
    /// Pass nullptr to detach and fall back to direct mode.
    /// use_bscane=true selects BscaneIlaTapBackend on the daemon side.
    /// user_chain: BSCANE2 chain 1..4 (USER1..USER4), default 1.
    void setDaemonClient(GuiDaemonClient* client, int device_index,
                         bool use_bscane = false, int user_chain = 1);

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

    /// OR mode accessors (used by AppWindow to persist the setting).
    bool orMode() const     { return or_mode_; }
    void setOrMode(bool v)  { or_mode_ = v; }

private:
    void doArm();
    void doStop();
    void doForce();
    void doReset();
    void doRead();
    void pollCaps();    ///< Re-probe ILA caps via daemon RPC (daemon mode only)
    void pollStatus();
    void drawWaveform();      // multi-lane ImPlot chart
    void drawSignalEditor();   // collapsible lane-definition table
    void resetSignalDefs();    // populate default from IlaCaps.data_w

    /// Compute hardware trigger registers from per-lane settings.
    /// Produces Group A (mask/value/rise/fall) and Group B (mask2/val2).
    void computeTriggerRegisters(uint32_t& mask, uint32_t& value,
                                  uint32_t& rise_mask, uint32_t& fall_mask,
                                  uint32_t& mask2, uint32_t& val2) const;

    bool driverOk() const;   // true when driver_ != nullptr OR daemon is connected
    bool daemonOk() const;   // true when daemon_client_ != nullptr && isConnected()

    // Helpers to access active caps/error in either direct or daemon mode.
    bool isProbed() const;
    const jtag::ila::IlaCaps& activeCaps() const;
    const std::string& activeLastError() const;
    int activeDepth() const;

    // ILA backend (direct mode)
    std::unique_ptr<jtag::ila::IlaTapBackend> backend_;
    std::unique_ptr<jtag::ila::IlaDriver>     driver_;

    // Daemon mode state
    GuiDaemonClient*        daemon_client_       = nullptr;
    int                     daemon_device_index_ = 0;
    bool                    daemon_use_bscane_   = false;
    int                     daemon_user_chain_   = 1;  ///< BSCANE2 chain 1..4
    jtag::ila::IlaCaps      caps_remote_{};   ///< caps received via ila/probe RPC

    // Trigger config UI state
    int                  pre_samples_   = jtag::ila::IlaDriver::kDepth / 4;
    bool                 trigger_dirty_ = false;
    bool                 or_mode_       = false;  ///< OR mode toggle

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
    std::vector<IlaSignalDef>  signals_;
    std::vector<LaneTrigger>   lane_triggers_;  // size always == signals_.size()
    bool                       rtl_sig_defs_loaded_ = false;

    // Poll timer
    double last_poll_time_ = 0.0;

    SampleCallback sample_cb_;
    bool           visible_ = true;
};

} // namespace jtag::gui

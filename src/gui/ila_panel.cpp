// SPDX-License-Identifier: Apache-2.0
#include "ila_panel.h"

#include <imgui.h>
#include <implot.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "gui_daemon_client.h"
#include "src/jtag/jtag_chain.h"
#include "src/protocol/i2c_decoder.h"
#include "src/protocol/spi_decoder.h"
#include "src/protocol/uart_decoder.h"

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
// ILA -> SampleFrame adapter
// ─────────────────────────────────────────────────────────────────────────────

/// Convert raw ILA uint32 samples to SampleFrame vector for protocol decoders.
/// Only 1-bit signals from `defs` are placed into pin_states; bus lanes are
/// skipped because the protocol decoders work on single-bit lines.
static std::vector<jtag::SampleFrame> ilaToSampleFrames(
    const std::vector<uint32_t>&    raw,
    const std::vector<IlaSignalDef>& defs,
    double                           ns_per_sample)
{
    std::vector<jtag::SampleFrame> out;
    out.reserve(raw.size());
    const jtag::SampleFrame* t0 = nullptr;  // set on first frame
    for (size_t i = 0; i < raw.size(); i++) {
        jtag::SampleFrame f{};
        // Synthesise a timestamp from sample index and configured period.
        const auto ns = std::chrono::nanoseconds(
            static_cast<long long>(i * ns_per_sample));
        f.timestamp = std::chrono::steady_clock::time_point{} +
                      std::chrono::duration_cast<
                          std::chrono::steady_clock::duration>(ns);
        for (const auto& sig : defs) {
            if (sig.width() != 1) continue;  // skip bus lanes
            const jtag::PinState ps = (sig.extract(raw[i]) != 0u)
                ? jtag::PinState::HIGH
                : jtag::PinState::LOW;
            f.data.pin_states[sig.name] = ps;
        }
        out.push_back(std::move(f));
        if (i == 0) t0 = &out.back();
    }
    (void)t0;
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / setChain
// ─────────────────────────────────────────────────────────────────────────────

IlaPanel::IlaPanel() = default;

IlaPanel::~IlaPanel() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Daemon mode helpers
// ─────────────────────────────────────────────────────────────────────────────

bool IlaPanel::daemonOk() const {
    return daemon_client_ != nullptr && daemon_client_->isConnected();
}

bool IlaPanel::driverOk() const {
    return driver_ != nullptr || daemonOk();
}

bool IlaPanel::isProbed() const {
    return driver_ ? driver_->probed() : caps_remote_.probed;
}

const jtag::ila::IlaCaps& IlaPanel::activeCaps() const {
    return driver_ ? driver_->caps() : caps_remote_;
}

const std::string& IlaPanel::activeLastError() const {
    return driver_ ? driver_->lastError() : last_error_;
}

int IlaPanel::activeDepth() const {
    return driver_ ? driver_->depth()
                   : static_cast<int>(caps_remote_.depth ? caps_remote_.depth
                                                         : jtag::ila::IlaDriver::kDepth);
}

// ─────────────────────────────────────────────────────────────────────────────
// setDaemonClient — switch ILA panel to daemon mode
// ─────────────────────────────────────────────────────────────────────────────

void IlaPanel::setDaemonClient(GuiDaemonClient* client, int device_index,
                                bool use_bscane, int user_chain) {
    // Drop any direct-mode driver.
    driver_.reset();
    backend_.reset();

    daemon_client_       = client;
    daemon_device_index_ = device_index;
    daemon_use_bscane_   = use_bscane;
    daemon_user_chain_   = (user_chain >= 1 && user_chain <= 4) ? user_chain : 1;
    caps_remote_         = {};
    status_valid_        = false;
    has_samples_         = false;
    last_error_.clear();

    if (!client || !client->isConnected()) return;

    pollCaps();
}

// ─────────────────────────────────────────────────────────────────────────────
// pollCaps — re-probe ILA caps via daemon RPC (daemon mode only)
// ─────────────────────────────────────────────────────────────────────────────

void IlaPanel::pollCaps() {
    auto* client = daemon_client_;
    if (!client || !client->isConnected()) return;

    caps_remote_ = {};
    last_error_.clear();

    // Probe the ILA via daemon RPC to populate caps and signal defs.
    try {
        auto r = client->ilaProbe(daemon_device_index_, daemon_use_bscane_, daemon_user_chain_);
        caps_remote_.version   = r.value("version",   0);
        caps_remote_.num_ch    = r.value("num_ch",    1);
        caps_remote_.sig_count = r.value("sig_count", 0);
        caps_remote_.data_w    = r.value("data_w",    32);
        caps_remote_.addr_w    = r.value("addr_w",    10);
        caps_remote_.depth     = r.value("depth",     static_cast<uint32_t>(jtag::ila::IlaDriver::kDepth));
        caps_remote_.raw       = r.value("raw",       0u);
        caps_remote_.idcode    = r.value("idcode",    0u);
        caps_remote_.probed    = true;

        if (pre_samples_ >= activeDepth())
            pre_samples_ = activeDepth() / 4;

        bool loaded = false;
        if (r.contains("signal_defs") && r["signal_defs"].is_array()) {
            signals_.clear();
            for (const auto& e : r["signal_defs"]) {
                IlaSignalDef d{};
                const int hi       = e.value("hi", 31);
                const int lo       = e.value("lo", 0);
                const int name_idx = e.value("name_idx", 0xFF);
                if (name_idx == 0xFF)
                    std::snprintf(d.name, sizeof(d.name), "data[%d:%d]", hi, lo);
                else
                    std::snprintf(d.name, sizeof(d.name), "sig%u", name_idx);
                d.hi  = hi;
                d.lo  = lo;
                d.fmt = static_cast<BusFormat>(e.value("fmt", 0));
                signals_.push_back(d);
            }
            lane_triggers_.assign(signals_.size(), LaneTrigger{});
            loaded = true;
        }
        rtl_sig_defs_loaded_ = loaded;
        if (!loaded) resetSignalDefs();
    } catch (const std::exception& e) {
        last_error_ = e.what();
        resetSignalDefs();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// resetSignalDefs
// ─────────────────────────────────────────────────────────────────────────────

void IlaPanel::resetSignalDefs() {
    signals_.clear();
    const int dw = driver_ ? driver_->dataWidth()
                 : (caps_remote_.probed ? caps_remote_.data_w
                                        : jtag::ila::IlaDriver::kDataWidth);
    IlaSignalDef d{};
    std::snprintf(d.name, sizeof(d.name), "data[%d:0]", dw - 1);
    d.hi  = dw - 1;
    d.lo  = 0;
    d.fmt = BusFormat::HEX;
    signals_.push_back(d);
    lane_triggers_.assign(1, LaneTrigger{});
}

std::vector<IlaSignalConfig> IlaPanel::exportSignalConfigs() const {
    std::vector<IlaSignalConfig> out;
    out.reserve(signals_.size());
    for (size_t i = 0; i < signals_.size(); ++i) {
        const auto& s = signals_[i];
        IlaSignalConfig c;
        c.name  = s.name;
        c.hi    = s.hi;
        c.lo    = s.lo;
        c.fmt   = s.fmt;
        if (i < lane_triggers_.size()) {
            c.trig_cond   = static_cast<int>(lane_triggers_[i].cond);
            c.trig_value  = lane_triggers_[i].value;
            c.trig_cond_b  = static_cast<int>(lane_triggers_[i].cond_b);
            c.trig_value_b = lane_triggers_[i].value_b;
        }
        out.push_back(std::move(c));
    }
    return out;
}

void IlaPanel::importSignalConfigs(const std::vector<IlaSignalConfig>& cfgs) {
    if (cfgs.empty()) return;
    signals_.clear();
    lane_triggers_.clear();
    signals_.reserve(cfgs.size());
    lane_triggers_.reserve(cfgs.size());
    for (const auto& c : cfgs) {
        IlaSignalDef d{};
        std::snprintf(d.name, sizeof(d.name), "%s", c.name.c_str());
        d.hi  = c.hi;
        d.lo  = c.lo;
        d.fmt = c.fmt;
        signals_.push_back(d);
        LaneTrigger lt{};
        lt.cond    = static_cast<TriggerCond>(c.trig_cond);
        lt.value   = c.trig_value;
        lt.cond_b  = static_cast<TriggerCond>(c.trig_cond_b);
        lt.value_b = c.trig_value_b;
        lane_triggers_.push_back(lt);
    }
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
        // Populate signal lanes from RTL SIG_DEF register when available.
        bool loaded = false;
        if (caps.sig_count > 0) {
            std::vector<jtag::ila::IlaSignalEntry> entries;
            if (driver_->readSignalDefs(entries)) {
                signals_.clear();
                for (const auto& e : entries) {
                    IlaSignalDef d{};
                    if (e.name_idx == 0xFF)
                        std::snprintf(d.name, sizeof(d.name), "data[%d:%d]", e.hi, e.lo);
                    else
                        std::snprintf(d.name, sizeof(d.name), "sig%u", e.name_idx);
                    d.hi  = static_cast<int>(e.hi);
                    d.lo  = static_cast<int>(e.lo);
                    d.fmt = static_cast<BusFormat>(e.fmt < 3 ? e.fmt : 0);
                    signals_.push_back(d);
                }
                lane_triggers_.assign(signals_.size(), LaneTrigger{});
                loaded = true;
            }
        }
        rtl_sig_defs_loaded_ = loaded;
        if (!loaded) resetSignalDefs();
    }
}

void IlaPanel::setBscaneChain(jtag::JtagChain* chain, int pl_tap_index,
                               int user_chain) {
    driver_.reset();
    backend_.reset();
    status_valid_ = false;
    has_samples_  = false;
    last_error_.clear();

    if (chain) {
        backend_ = std::make_unique<jtag::ila::BscaneIlaTapBackend>(
            *chain, pl_tap_index, user_chain);
        driver_  = std::make_unique<jtag::ila::IlaDriver>(*backend_);
        jtag::ila::IlaCaps caps{};
        (void)driver_->probe(caps);  // best-effort; falls back to defaults
        if (pre_samples_ >= driver_->depth())
            pre_samples_ = driver_->depth() / 4;
        // Populate signal lanes from RTL SIG_DEF register when available.
        bool loaded = false;
        if (caps.sig_count > 0) {
            std::vector<jtag::ila::IlaSignalEntry> entries;
            if (driver_->readSignalDefs(entries)) {
                signals_.clear();
                for (const auto& e : entries) {
                    IlaSignalDef d{};
                    if (e.name_idx == 0xFF)
                        std::snprintf(d.name, sizeof(d.name), "data[%d:%d]", e.hi, e.lo);
                    else
                        std::snprintf(d.name, sizeof(d.name), "sig%u", e.name_idx);
                    d.hi  = static_cast<int>(e.hi);
                    d.lo  = static_cast<int>(e.lo);
                    d.fmt = static_cast<BusFormat>(e.fmt < 3 ? e.fmt : 0);
                    signals_.push_back(d);
                }
                lane_triggers_.assign(signals_.size(), LaneTrigger{});
                loaded = true;
            }
        }
        rtl_sig_defs_loaded_ = loaded;
        if (!loaded) resetSignalDefs();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// computeTriggerRegisters
// ─────────────────────────────────────────────────────────────────────────────

void IlaPanel::computeTriggerRegisters(uint32_t& mask, uint32_t& value,
                                        uint32_t& rise_mask,
                                        uint32_t& fall_mask,
                                        uint32_t& mask2, uint32_t& val2) const {
    mask = value = rise_mask = fall_mask = mask2 = val2 = 0;
    const int n = static_cast<int>(signals_.size());
    for (int i = 0; i < n; ++i) {
        const IlaSignalDef& sig = signals_[i];
        const LaneTrigger& lt  = (i < static_cast<int>(lane_triggers_.size()))
                                 ? lane_triggers_[i] : LaneTrigger{};
        // Build lane bitmask for this signal's [hi:lo] range.
        const int w = sig.width();
        if (w <= 0 || sig.lo < 0) continue;
        const uint32_t lane_bits = (w >= 32) ? 0xFFFF'FFFFu
                                             : (((1u << w) - 1u) << sig.lo);
        // --- Group A ---
        switch (lt.cond) {
            case TriggerCond::None:   break;
            case TriggerCond::Eq:
                mask  |= lane_bits;
                value |= (lt.value << sig.lo) & lane_bits;
                break;
            case TriggerCond::Neq:
                mask  |= lane_bits;
                value |= (~(lt.value << sig.lo)) & lane_bits;
                break;
            case TriggerCond::Rise:
                rise_mask |= lane_bits;
                break;
            case TriggerCond::Fall:
                fall_mask |= lane_bits;
                break;
            case TriggerCond::Either:
                rise_mask |= lane_bits;
                fall_mask |= lane_bits;
                break;
        }
        // --- Group B (Eq/Neq only; used in OR mode) ---
        switch (lt.cond_b) {
            case TriggerCond::None:  break;
            case TriggerCond::Eq:
                mask2 |= lane_bits;
                val2  |= (lt.value_b << sig.lo) & lane_bits;
                break;
            case TriggerCond::Neq:
                mask2 |= lane_bits;
                val2  |= (~(lt.value_b << sig.lo)) & lane_bits;
                break;
            default: break;  // Rise/Fall/Either not supported in Group B
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Button handlers
// ─────────────────────────────────────────────────────────────────────────────

void IlaPanel::doArm() {
    if (!driverOk()) return;
    if (daemonOk()) {
        uint32_t mask = 0, val = 0, rise = 0, fall = 0, m2 = 0, v2 = 0;
        computeTriggerRegisters(mask, val, rise, fall, m2, v2);
        try {
            daemon_client_->ilaArm(daemon_device_index_, daemon_use_bscane_,
                                   daemon_user_chain_,
                                   mask, val, rise, fall, m2, v2, or_mode_,
                                   pre_samples_);
            last_error_.clear();
            trigger_dirty_ = false;
        } catch (const std::exception& e) {
            last_error_ = e.what();
        }
        pollStatus();
        return;
    }
    uint32_t mask = 0, val = 0, rise = 0, fall = 0, m2 = 0, v2 = 0;
    computeTriggerRegisters(mask, val, rise, fall, m2, v2);
    auto pre = static_cast<uint16_t>(pre_samples_);
    if (!driver_->configureTrigger(mask, val, rise, fall, m2, v2, or_mode_, pre)) {
        last_error_ = driver_->lastError();
        return;
    }
    trigger_dirty_ = false;
    // Always reset first: FSM in ST_FULL transitions FULL→IDLE on ARM,
    // not IDLE→ARMED. resetCapture() guarantees FSM is in ST_IDLE before arm().
    (void)driver_->resetCapture();
    if (!driver_->arm()) {
        last_error_ = driver_->lastError();
        return;
    }
    last_error_.clear();
    pollStatus();
}

void IlaPanel::doStop() {
    if (!driverOk()) return;
    if (daemonOk()) {
        try {
            daemon_client_->ilaStop(daemon_device_index_, daemon_use_bscane_,
                                    daemon_user_chain_);
            last_error_.clear();
        } catch (const std::exception& e) { last_error_ = e.what(); }
        pollStatus();
        return;
    }
    if (!driver_->stop())
        last_error_ = driver_->lastError();
    else
        last_error_.clear();
    pollStatus();
}

void IlaPanel::doForce() {
    if (!driverOk()) return;
    if (daemonOk()) {
        try {
            daemon_client_->ilaForce(daemon_device_index_, daemon_use_bscane_,
                                     daemon_user_chain_);
            last_error_.clear();
        } catch (const std::exception& e) { last_error_ = e.what(); }
        pollStatus();
        return;
    }
    if (!driver_->forceTrigger())
        last_error_ = driver_->lastError();
    else
        last_error_.clear();
    pollStatus();
}

void IlaPanel::doReset() {
    if (!driverOk()) return;
    if (daemonOk()) {
        try {
            daemon_client_->ilaReset(daemon_device_index_, daemon_use_bscane_,
                                     daemon_user_chain_);
            last_error_.clear();
        } catch (const std::exception& e) { last_error_ = e.what(); }
        status_valid_ = false;
        has_samples_  = false;
        return;
    }
    if (!driver_->resetCapture())
        last_error_ = driver_->lastError();
    else
        last_error_.clear();
    status_valid_ = false;
    has_samples_  = false;
}

void IlaPanel::doRead() {
    if (!driverOk()) return;
    if (daemonOk()) {
        try {
            auto r = daemon_client_->ilaReadSamples(daemon_device_index_,
                                                    daemon_use_bscane_,
                                                    daemon_user_chain_);
            if (!r.value("ok", false)) {
                last_error_ = r.value("error", "read failed");
                return;
            }
            samples_.clear();
            if (r.contains("samples") && r["samples"].is_array()) {
                for (const auto& v : r["samples"])
                    samples_.push_back(v.get<uint32_t>());
            }
            last_error_.clear();
            has_samples_ = !samples_.empty();
            const int n = static_cast<int>(samples_.size());
            wave_x_.resize(n);
            for (int i = 0; i < n; ++i)
                wave_x_[i] = static_cast<double>(i) * 8.0;
            if (has_samples_ && sample_cb_) sample_cb_(samples_);
        } catch (const std::exception& e) { last_error_ = e.what(); }
        return;
    }
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
    if (daemonOk()) {
        try {
            auto r = daemon_client_->ilaStatus(daemon_device_index_,
                                               daemon_use_bscane_,
                                               daemon_user_chain_);
            if (r.contains("armed")) {
                status_.armed     = r.value("armed",     false);
                status_.triggered = r.value("triggered", false);
                status_.full      = r.value("full",      false);
                status_valid_ = true;
                last_error_.clear();
            }
        } catch (const std::exception& e) { last_error_ = e.what(); }
        return;
    }
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

    // ── BSCANE2 chain selector (daemon + BSCANE2 mode only) ──────────
    if (daemonOk() && daemon_use_bscane_) {
        static const char* kChainLabels[] = { "USER1", "USER2", "USER3", "USER4" };
        int sel = daemon_user_chain_ - 1;  // 0-based for combo
        ImGui::SetNextItemWidth(90);
        if (ImGui::Combo("##user_chain", &sel, kChainLabels, 4)) {
            const int new_chain = sel + 1;
            if (new_chain != daemon_user_chain_) {
                // Re-probe on the new chain.
                setDaemonClient(daemon_client_, daemon_device_index_,
                                daemon_use_bscane_, new_chain);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("BSCANE2 chain");
        ImGui::SameLine(0, 16);
    }

    // ── CAPS line (shown when probed) ────────────────────────────────
    if (hw_ok && isProbed()) {
        const auto& c = activeCaps();
        ImGui::TextDisabled("CAPS: DW=%d  Depth=%d  NUM_CH=%d  SIG_COUNT=%d  ver=0x%02X  IDCODE=0x%08X  raw=0x%08X",
                            c.data_w, c.depth, c.num_ch, c.sig_count, c.version, c.idcode, c.raw);
    } else if (hw_ok) {
        ImGui::TextDisabled("CAPS: (not probed) -- %s",
                            activeLastError().c_str());
    } else {
        ImGui::TextDisabled("CAPS: --");
    }
    if (daemonOk()) {
        ImGui::SameLine(0, 12);
        if (ImGui::SmallButton("Probe")) pollCaps();
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

    // ── Trigger config (computed from per-lane settings) ─────────────
    {
        uint32_t cm = 0, cv = 0, cr = 0, cf = 0, cm2 = 0, cv2 = 0;
        computeTriggerRegisters(cm, cv, cr, cf, cm2, cv2);
        ImGui::Text("Trigger:");
        ImGui::SameLine();
        // AND/OR radio buttons (disabled while armed/triggered)
        const bool busy = hw_ok && status_valid_
                          && (status_.armed || status_.triggered);
        ImGui::BeginDisabled(busy);
        bool and_mode = !or_mode_;
        if (ImGui::RadioButton("AND", and_mode)) {
            or_mode_       = false;
            trigger_dirty_ = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("OR", or_mode_)) {
            or_mode_       = true;
            trigger_dirty_ = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Mask=0x%08X  Value=0x%08X", cm, cv);
        const bool has_edge = hw_ok && isProbed()
                              && activeCaps().version >= 2;
        if (has_edge) {
            ImGui::SameLine();
            ImGui::TextDisabled(" Rise=0x%08X  Fall=0x%08X", cr, cf);
        }
        const bool has_or = hw_ok && isProbed()
                            && activeCaps().version >= 3;
        if (has_or && or_mode_) {
            ImGui::SameLine();
            ImGui::TextDisabled(" B: Mask=0x%08X  Val=0x%08X", cm2, cv2);
        }
    }

    ImGui::Text("Pre-samples:      ");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderInt("##pre", &pre_samples_, 0, activeDepth() - 1))
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

    // ── Protocol decode ──────────────────────────────────────────────
    if (has_samples_)
        drawProtocolDecode();

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
    if (!ImGui::CollapsingHeader("Signal Definitions & Trigger")) return;

    if (ImGui::SmallButton("Reset from CAPS")) resetSignalDefs();
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Add")) {
        const int dw = driver_ ? driver_->dataWidth()
                     : (caps_remote_.probed ? caps_remote_.data_w
                                            : jtag::ila::IlaDriver::kDataWidth);
        IlaSignalDef d{};
        std::snprintf(d.name, sizeof(d.name), "sig");
        d.hi  = dw - 1;
        d.lo  = 0;
        d.fmt = BusFormat::HEX;
        signals_.push_back(d);
        lane_triggers_.emplace_back();
    }

    static const char* kFmtNames[] = {"HEX", "DEC", "BIN"};
    static const char* kCondNames[] = {"None", "==", "!=", "Rise", "Fall", "Either"};
    static const char* kCondNamesB[] = {"None", "==", "!="};  // Group B: level only
    constexpr ImGuiTableFlags kTblFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingFixedFit;

    // Column count: add 2 extra columns (Trig B / Val B) in OR mode.
    const int n_cols = or_mode_ ? 10 : 8;
    if (!ImGui::BeginTable("##sigdefs", n_cols, kTblFlags)) return;

    ImGui::TableSetupColumn("Name",   ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("Hi",     ImGuiTableColumnFlags_WidthFixed,  36.0f);
    ImGui::TableSetupColumn("Lo",     ImGuiTableColumnFlags_WidthFixed,  36.0f);
    ImGui::TableSetupColumn("W",      ImGuiTableColumnFlags_WidthFixed,  28.0f);
    ImGui::TableSetupColumn("Fmt",    ImGuiTableColumnFlags_WidthFixed,  52.0f);
    ImGui::TableSetupColumn("Trig A", ImGuiTableColumnFlags_WidthFixed,  72.0f);
    ImGui::TableSetupColumn("Val A",  ImGuiTableColumnFlags_WidthFixed,  90.0f);
    if (or_mode_) {
        ImGui::TableSetupColumn("Trig B", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Val B",  ImGuiTableColumnFlags_WidthFixed, 90.0f);
    }
    ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed,  20.0f);
    ImGui::TableHeadersRow();

    int to_remove = -1;
    const int n = static_cast<int>(signals_.size());
    // Ensure lane_triggers_ stays in sync (safety net).
    lane_triggers_.resize(static_cast<size_t>(n));

    for (int i = 0; i < n; i++) {
        IlaSignalDef& s  = signals_[i];
        LaneTrigger&  lt = lane_triggers_[static_cast<size_t>(i)];
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
        {
            int ci = static_cast<int>(lt.cond);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("##cond", &ci, kCondNames, 6)) {
                lt.cond = static_cast<TriggerCond>(ci);
                trigger_dirty_ = true;
            }
        }

        ImGui::TableSetColumnIndex(6);
        {
            const bool need_val = (lt.cond == TriggerCond::Eq ||
                                   lt.cond == TriggerCond::Neq);
            if (need_val) {
                char vbuf[12];
                std::snprintf(vbuf, sizeof(vbuf), "%X", lt.value);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##tv", vbuf, sizeof(vbuf),
                                     ImGuiInputTextFlags_CharsHexadecimal |
                                     ImGuiInputTextFlags_AutoSelectAll)) {
                    lt.value = parseHex(vbuf, lt.value);
                    trigger_dirty_ = true;
                }
            } else {
                ImGui::TextDisabled("--");
            }
        }

        // Group B columns (visible in OR mode only)
        if (or_mode_) {
            ImGui::TableSetColumnIndex(7);
            {
                int ci = static_cast<int>(lt.cond_b);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##condb", &ci, kCondNamesB, 3)) {
                    lt.cond_b = static_cast<TriggerCond>(ci);
                    trigger_dirty_ = true;
                }
            }

            ImGui::TableSetColumnIndex(8);
            {
                const bool need_val = (lt.cond_b == TriggerCond::Eq ||
                                       lt.cond_b == TriggerCond::Neq);
                if (need_val) {
                    char vbuf[12];
                    std::snprintf(vbuf, sizeof(vbuf), "%X", lt.value_b);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputText("##tvb", vbuf, sizeof(vbuf),
                                         ImGuiInputTextFlags_CharsHexadecimal |
                                         ImGuiInputTextFlags_AutoSelectAll)) {
                        lt.value_b = parseHex(vbuf, lt.value_b);
                        trigger_dirty_ = true;
                    }
                } else {
                    ImGui::TextDisabled("--");
                }
            }
        }

        ImGui::TableSetColumnIndex(or_mode_ ? 9 : 7);
        if (ImGui::SmallButton("X")) to_remove = i;

        ImGui::PopID();
    }
    if (to_remove >= 0) {
        signals_.erase(signals_.begin() + to_remove);
        lane_triggers_.erase(lane_triggers_.begin() + to_remove);
    }

    ImGui::EndTable();
}

// ─────────────────────────────────────────────────────────────────────────────
// drawProtocolDecode  -- collapsible protocol analyzer section
// ─────────────────────────────────────────────────────────────────────────────

void IlaPanel::drawProtocolDecode() {
    if (!has_samples_ || samples_.empty()) return;
    if (!ImGui::CollapsingHeader("Protocol Decode")) return;

    // Collect 1-bit signal names for pin selection combos.
    std::vector<const char*> bit_names;
    for (const auto& sig : signals_)
        if (sig.width() == 1) bit_names.push_back(sig.name);

    if (bit_names.empty()) {
        ImGui::TextDisabled("No 1-bit signals defined. Add single-bit lanes in Signal Definitions.");
        return;
    }

    const int n_bits = static_cast<int>(bit_names.size());

    // Clamp stored indices to valid range when signal list changes.
    auto clamp_idx = [&](int& idx) { if (idx >= n_bits) idx = 0; };
    clamp_idx(proto_uart_rx_idx_);
    clamp_idx(proto_spi_clk_idx_);
    clamp_idx(proto_spi_mosi_idx_);
    clamp_idx(proto_spi_miso_idx_);
    clamp_idx(proto_spi_cs_idx_);
    clamp_idx(proto_i2c_scl_idx_);
    clamp_idx(proto_i2c_sda_idx_);

    // ns/sample setting
    ImGui::SetNextItemWidth(120);
    ImGui::InputDouble("ns / sample", &proto_ns_per_sample_, 0.0, 0.0, "%.2f");
    if (proto_ns_per_sample_ <= 0.0) proto_ns_per_sample_ = 1.0;

    ImGui::SameLine(0, 16);

    // Protocol selector
    static const char* kProtoNames[] = {"UART", "SPI", "I2C"};
    ImGui::SetNextItemWidth(80);
    ImGui::Combo("Protocol##proto_sel", &proto_selected_, kProtoNames, 3);

    ImGui::Spacing();

    if (proto_selected_ == 0) {
        // ── UART ──────────────────────────────────────────────────────
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("RX pin##urx", &proto_uart_rx_idx_, bit_names.data(), n_bits);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        static const char* kBauds[] = {"1200","2400","4800","9600","19200","38400","57600","115200","230400","460800","921600","1000000"};
        static const int   kBaudVals[] = {1200,2400,4800,9600,19200,38400,57600,115200,230400,460800,921600,1000000};
        // Find closest index
        int baud_idx = 3;
        for (int i = 0; i < 12; ++i)
            if (kBaudVals[i] == proto_uart_baud_) { baud_idx = i; break; }
        if (ImGui::Combo("Baud##ubaud", &baud_idx, kBauds, 12))
            proto_uart_baud_ = kBaudVals[baud_idx];

        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::SliderInt("Bits##udbits", &proto_uart_data_bits_, 5, 8);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(50);
        ImGui::SliderInt("Stop##ustop", &proto_uart_stop_bits_, 1, 2);

        ImGui::SameLine();
        ImGui::Checkbox("Parity##upar", &proto_uart_parity_en_);
        if (proto_uart_parity_en_) {
            ImGui::SameLine();
            ImGui::Checkbox("Odd##uodd", &proto_uart_parity_odd_);
        }

    } else if (proto_selected_ == 1) {
        // ── SPI ───────────────────────────────────────────────────────
        ImGui::SetNextItemWidth(160); ImGui::Combo("CLK##sclk",  &proto_spi_clk_idx_,  bit_names.data(), n_bits);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160); ImGui::Combo("MOSI##smosi",&proto_spi_mosi_idx_, bit_names.data(), n_bits);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160); ImGui::Combo("MISO##smiso",&proto_spi_miso_idx_, bit_names.data(), n_bits);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160); ImGui::Combo("CS##scs",    &proto_spi_cs_idx_,   bit_names.data(), n_bits);

        ImGui::Checkbox("CPOL##scpol", &proto_spi_cpol_);
        ImGui::SameLine();
        ImGui::Checkbox("CPHA##scpha", &proto_spi_cpha_);
        ImGui::SameLine();
        ImGui::Checkbox("LSB first##slsb", &proto_spi_lsb_first_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::SliderInt("Bits/word##sbpw", &proto_spi_bpw_, 1, 64);

    } else {
        // ── I2C ───────────────────────────────────────────────────────
        ImGui::SetNextItemWidth(160); ImGui::Combo("SCL##iscl", &proto_i2c_scl_idx_, bit_names.data(), n_bits);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160); ImGui::Combo("SDA##isda", &proto_i2c_sda_idx_, bit_names.data(), n_bits);
    }

    ImGui::Spacing();
    if (ImGui::Button("Decode", ImVec2(90, 0))) {
        // Build synthetic SampleFrame vector from ILA raw samples.
        auto frames = ilaToSampleFrames(samples_, signals_, proto_ns_per_sample_);

        proto_frames_.clear();
        if (proto_selected_ == 0) {
            jtag::protocol::UartConfig cfg;
            cfg.rx_pin      = bit_names[proto_uart_rx_idx_];
            cfg.baud_rate   = static_cast<uint32_t>(proto_uart_baud_);
            cfg.data_bits   = proto_uart_data_bits_;
            cfg.stop_bits   = proto_uart_stop_bits_;
            cfg.parity_enable = proto_uart_parity_en_;
            cfg.parity_odd  = proto_uart_parity_odd_;
            proto_frames_ = jtag::protocol::decodeUart(frames, cfg);
        } else if (proto_selected_ == 1) {
            jtag::protocol::SpiConfig cfg;
            cfg.clk_pin      = bit_names[proto_spi_clk_idx_];
            cfg.mosi_pin     = bit_names[proto_spi_mosi_idx_];
            cfg.miso_pin     = bit_names[proto_spi_miso_idx_];
            cfg.cs_pin       = bit_names[proto_spi_cs_idx_];
            cfg.cpol         = proto_spi_cpol_;
            cfg.cpha         = proto_spi_cpha_;
            cfg.lsb_first    = proto_spi_lsb_first_;
            cfg.bits_per_word = proto_spi_bpw_;
            proto_frames_ = jtag::protocol::decodeSpi(frames, cfg);
        } else {
            jtag::protocol::I2cConfig cfg;
            cfg.scl_pin = bit_names[proto_i2c_scl_idx_];
            cfg.sda_pin = bit_names[proto_i2c_sda_idx_];
            proto_frames_ = jtag::protocol::decodeI2c(frames, cfg);
        }
    }

    // Results table
    if (proto_frames_.empty()) {
        if (ImGui::IsItemDeactivatedAfterEdit() || true)
            ImGui::TextDisabled("(no frames decoded)");
        return;
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                       "%zu frame(s)", proto_frames_.size());

    constexpr ImGuiTableFlags kTbl =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    const float tbl_h = std::min(
        static_cast<float>(proto_frames_.size()) * ImGui::GetTextLineHeightWithSpacing() + 30.0f,
        200.0f);

    if (!ImGui::BeginTable("##proto_results", 5, kTbl, ImVec2(-1, tbl_h)))
        return;

    ImGui::TableSetupColumn("Start (ns)", ImGuiTableColumnFlags_WidthFixed,  90.0f);
    ImGui::TableSetupColumn("End (ns)",   ImGuiTableColumnFlags_WidthFixed,  90.0f);
    ImGui::TableSetupColumn("Label",      ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Data",       ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Err",        ImGuiTableColumnFlags_WidthFixed,  30.0f);
    ImGui::TableHeadersRow();

    const double us_to_ns = 1000.0;
    for (const auto& f : proto_frames_) {
        ImGui::TableNextRow();
        if (f.error_flag)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   IM_COL32(100, 30, 30, 120));
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%.1f", f.start_us * us_to_ns);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.1f", f.end_us * us_to_ns);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(f.label.c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(f.data.c_str());
        ImGui::TableSetColumnIndex(4);
        if (f.error_flag)
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "!");
    }

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
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit")) fit_wave_ = true;

    const int n_lanes = static_cast<int>(signals_.size());
    if (n_lanes == 0) {
        ImGui::TextDisabled("No signal definitions. Expand 'Signal Definitions' above.");
        return;
    }

    // Y-axis tick positions and labels (lane 0 = topmost = highest Y value).
    // When protocol frames exist, a "Protocol" row is appended below lane 0.
    const bool has_proto = !proto_frames_.empty();
    std::vector<double>      tick_y(n_lanes + (has_proto ? 1 : 0));
    std::vector<const char*> tick_names(n_lanes + (has_proto ? 1 : 0));
    for (int i = 0; i < n_lanes; i++) {
        tick_y[i]     = static_cast<double>(n_lanes - i) - 0.5;
        tick_names[i] = signals_[i].name;
    }
    if (has_proto) {
        tick_y[n_lanes]     = -0.5;
        tick_names[n_lanes] = "Protocol";
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
    const float effective_lanes = static_cast<float>(n_lanes) + (has_proto ? 1.0f : 0.0f);
    const float target_h = lane_px * effective_lanes + 20.0f;
    const float plot_h   = std::max({avail_h, target_h, 80.0f});

    if (!ImPlot::BeginPlot("##ila_wave", ImVec2(-1.0f, plot_h),
                           ImPlotFlags_NoTitle))
        return;

    ImPlot::SetupAxes("Time (ns)", nullptr,
                      ImPlotAxisFlags_None,
                      ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks);
    if (fit_wave_) {
        const double t_end = wave_x_.back() + sample_period;
        ImPlot::SetupAxisLimits(ImAxis_X1, wave_x_.front(), t_end, ImPlotCond_Always);
        fit_wave_ = false;
    }
    ImPlot::SetupAxisLimits(ImAxis_Y1, has_proto ? -1.0 : 0.0,
                            static_cast<double>(n_lanes),
                            ImPlotCond_Always);
    ImPlot::SetupAxisTicks(ImAxis_Y1,
                           tick_y.data(), static_cast<int>(tick_y.size()),
                           tick_names.data());

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

    // Protocol annotation blocks (drawn in the "Protocol" row at y = [-0.9, -0.1])
    if (has_proto) {
        const double py_hi = -0.1;
        const double py_lo = -0.9;
        const ImU32 kProtoFill  = IM_COL32( 80,160,255, 90);
        const ImU32 kProtoLine  = IM_COL32( 80,160,255,220);
        const ImU32 kErrFill    = IM_COL32(255, 80, 80, 90);
        const ImU32 kErrLine    = IM_COL32(255, 80, 80,220);
        // proto timestamps are in microseconds; wave_x_ is in nanoseconds
        const double us_to_ns = 1000.0;
        for (const auto& pf : proto_frames_) {
            const double t0 = pf.start_us * us_to_ns;
            const double t1 = pf.end_us   * us_to_ns;
            if (t1 <= t0) continue;
            const ImVec2 p0 = ImPlot::PlotToPixels(t0, py_hi);
            const ImVec2 p1 = ImPlot::PlotToPixels(t1, py_lo);
            dl->AddRectFilled(p0, p1, pf.error_flag ? kErrFill : kProtoFill);
            dl->AddRect      (p0, p1, pf.error_flag ? kErrLine : kProtoLine,
                              0.0f, 0, 1.5f);
            const float bw = p1.x - p0.x;
            const float tw = ImGui::CalcTextSize(pf.label.c_str()).x;
            if (bw > tw + 6.0f) {
                dl->AddText(
                    ImVec2((p0.x + p1.x) * 0.5f - tw * 0.5f,
                           (p0.y + p1.y) * 0.5f
                           - ImGui::GetTextLineHeight() * 0.5f),
                    col_text, pf.label.c_str());
            }
        }
    }

    // Trigger cursor
    if (pre_samples_ > 0 && pre_samples_ < n) {
        const double tx = wave_x_[pre_samples_];
        const double y_bot = has_proto ? -1.0 : 0.0;
        dl->AddLine(ImPlot::PlotToPixels(tx, static_cast<double>(n_lanes)),
                    ImPlot::PlotToPixels(tx, y_bot),
                    IM_COL32(255, 80, 80, 220), 1.5f);
    }

    ImPlot::PopPlotClipRect();
    ImPlot::EndPlot();
}

} // namespace jtag::gui

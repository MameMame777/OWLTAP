#pragma once

#include <map>
#include <string>
#include <vector>

#include "app_config.h"
#include "src/bsdl/bsdl_model.h"
#include "src/xdc/xdc_parser.h"

namespace jtag::gui {

/// Panel displaying FPGA pins from BSDL with checkboxes for selection.
class SignalPanel {
public:
    /// Draw the signal tree panel (call each frame).
    static void draw();

    /// Populate from BSDL device model.
    static void populateFromBsdl(const jtag::bsdl::BSDLDevice& device);

    /// Populate from pin name lists (daemon mode: no local BSDL model).
    /// observable: pins readable via JTAG SAMPLE.
    /// drivable:   pins drivable via JTAG EXTEST.
    static void populateFromPinLists(const std::vector<std::string>& observable,
                                     const std::vector<std::string>& drivable);

    /// Get currently selected (checked) signal names.
    static std::vector<std::string> selectedSignals();

    /// Return and clear the pending selection-changed flag.
    static bool consumeSelectionChanged();

    /// Restore a saved selection (call after populateFromBsdl).
    static void setSelectedSignals(const std::vector<std::string>& names);

    /// Clear all signals.
    static void clear();

    // ── Bus management ────────────────────────────────────────────────────────

    /// Get all defined buses.
    static const std::vector<BusDefinition>& buses();

    /// Replace all bus definitions (e.g. on config load).
    static void setBuses(const std::vector<BusDefinition>& buses);

    /// Add a bus definition. Replaces any existing bus with the same name.
    static void addBus(const BusDefinition& bus);

    /// Remove a bus by name.
    static void removeBus(const std::string& name);

    /// Returns true if any bus was added/removed/changed since last consume.
    static bool consumeBusChanged();

    // ── XDC alias management ─────────────────────────────────────────────────

    /// Apply XDC pin aliases: maps BSDL pin name -> display label.
    /// Call after populateFromBsdl(). Pass empty map to clear all aliases.
    static void setXdcAliases(const jtag::xdc::PinAliasMap& aliases);

private:
    struct PinEntry {
        std::string name;       // BSDL pin name (internal, stable)
        std::string label;      // Display name (alias from XDC, or same as name)
        std::string direction;
        int bsr_cell = -1;
        bool selected = false;
    };

    struct PinGroup {
        std::string name;
        std::vector<PinEntry> pins;
    };

    static std::vector<PinGroup> groups_;
    static std::vector<std::string> selected_order_;
    static bool selection_changed_;

    static std::vector<BusDefinition> buses_;
    static bool bus_changed_;

    static jtag::xdc::PinAliasMap xdc_aliases_;

    // Popup state for "Define Bus" dialog
    static char bus_name_buf_[64];
    static std::vector<std::string> bus_pending_signals_;
    static bool bus_dialog_open_;

    static PinEntry* findPin(const std::string& name);
    static void addToSelectedOrder(const std::string& name);
    static void removeFromSelectedOrder(const std::string& name);
    static void setPinSelected(PinEntry& pin, bool selected);
};

} // namespace jtag::gui

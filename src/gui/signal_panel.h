#pragma once

#include <map>
#include <string>
#include <vector>

#include "src/bsdl/bsdl_model.h"

namespace jtag::gui {

/// Panel displaying FPGA pins from BSDL with checkboxes for selection.
class SignalPanel {
public:
    /// Draw the signal tree panel (call each frame).
    static void draw();

    /// Populate from BSDL device model.
    static void populateFromBsdl(const jtag::bsdl::BSDLDevice& device);

    /// Get currently selected (checked) signal names.
    static std::vector<std::string> selectedSignals();

    /// Return and clear the pending selection-changed flag.
    static bool consumeSelectionChanged();

    /// Restore a saved selection (call after populateFromBsdl).
    static void setSelectedSignals(const std::vector<std::string>& names);

    /// Clear all signals.
    static void clear();

private:
    struct PinEntry {
        std::string name;
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

    static PinEntry* findPin(const std::string& name);
    static void addToSelectedOrder(const std::string& name);
    static void removeFromSelectedOrder(const std::string& name);
    static void setPinSelected(PinEntry& pin, bool selected);
};

} // namespace jtag::gui

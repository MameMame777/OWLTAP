#include "signal_panel.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

namespace jtag::gui {

std::vector<SignalPanel::PinGroup> SignalPanel::groups_;
std::vector<std::string> SignalPanel::selected_order_;
bool SignalPanel::selection_changed_ = false;
bool SignalPanel::open_bsdl_requested_ = false;
bool SignalPanel::connect_requested_   = false;

std::vector<BusDefinition> SignalPanel::buses_;
bool SignalPanel::bus_changed_ = false;
char SignalPanel::bus_name_buf_[64] = {};
std::vector<std::string> SignalPanel::bus_pending_signals_;
bool SignalPanel::bus_dialog_open_ = false;

jtag::xdc::PinAliasMap SignalPanel::xdc_aliases_;

SignalPanel::PinEntry* SignalPanel::findPin(const std::string& name) {
    for (auto& group : groups_) {
        for (auto& pin : group.pins) {
            if (pin.name == name) {
                return &pin;
            }
        }
    }
    return nullptr;
}

void SignalPanel::addToSelectedOrder(const std::string& name) {
    auto it = std::find(selected_order_.begin(), selected_order_.end(), name);
    if (it == selected_order_.end()) {
        selected_order_.push_back(name);
    }
}

void SignalPanel::removeFromSelectedOrder(const std::string& name) {
    selected_order_.erase(
        std::remove(selected_order_.begin(), selected_order_.end(), name),
        selected_order_.end());
}

void SignalPanel::setPinSelected(PinEntry& pin, bool selected) {
    if (pin.selected == selected) {
        return;
    }

    pin.selected = selected;
    if (selected) {
        addToSelectedOrder(pin.name);
    } else {
        removeFromSelectedOrder(pin.name);
    }
}

void SignalPanel::populateFromBsdl(const jtag::bsdl::BSDLDevice& device) {
    groups_.clear();
    selected_order_.clear();
    selection_changed_ = true;

    // Build from boundary_cells — this is always populated from BOUNDARY_REGISTER
    // regardless of whether PORT parsing succeeded fully.
    std::map<std::string, std::vector<PinEntry>> grouped;

    // Deduplicate: for BIDIR pins there are two cells (input + output); keep one entry.
    std::map<std::string, PinEntry> seen;

    for (const auto& cell : device.boundary_cells) {
        if (!cell.hasPin()) continue;
        if (cell.function == bsdl::CellFunction::CONTROL ||
            cell.function == bsdl::CellFunction::INTERNAL) continue;

        const std::string& name = cell.pin_name;
        auto it = seen.find(name);
        if (it != seen.end()) {
            // Prefer the INPUT cell position for observable pins
            if (cell.isInput()) it->second.bsr_cell = cell.position;
            continue;
        }

        const char* dir_str = "IN";
        switch (cell.function) {
            case bsdl::CellFunction::INPUT:   dir_str = "IN";    break;
            case bsdl::CellFunction::CLOCK:   dir_str = "IN";    break;
            case bsdl::CellFunction::OUTPUT2:
            case bsdl::CellFunction::OUTPUT3: dir_str = "OUT";   break;
            case bsdl::CellFunction::BIDIR:   dir_str = "INOUT"; break;
            default: dir_str = "IN"; break;
        }

        // Also check device.pins for a more accurate direction
        auto pit = device.pins.find(name);
        if (pit != device.pins.end()) {
            switch (pit->second.direction) {
                case bsdl::PinDirection::IN:      dir_str = "IN";     break;
                case bsdl::PinDirection::OUT:     dir_str = "OUT";    break;
                case bsdl::PinDirection::INOUT:   dir_str = "INOUT";  break;
                case bsdl::PinDirection::BUFFER:  dir_str = "BUFFER"; break;
                case bsdl::PinDirection::LINKAGE: dir_str = "LINK";   break;
            }
        }

        PinEntry entry;
        entry.name      = name;
        entry.label     = name;  // default; overridden by setXdcAliases()
        entry.direction = dir_str;
        entry.bsr_cell  = cell.position;
        entry.selected  = false;
        seen[name] = std::move(entry);
    }

    for (auto& [name, entry] : seen) {
        // Group by prefix (up to first '_' or digit transition)
        std::string prefix;
        for (size_t i = 0; i < name.size(); i++) {
            if (name[i] == '_' ||
                (i > 0 && std::isdigit(static_cast<unsigned char>(name[i])) &&
                 !std::isdigit(static_cast<unsigned char>(name[i - 1])))) {
                prefix = name.substr(0, i);
                break;
            }
        }
        if (prefix.empty()) prefix = name;
        grouped[prefix].push_back(std::move(entry));
    }

    for (auto& [prefix, pins] : grouped) {
        PinGroup group;
        group.name = prefix;
        group.pins = std::move(pins);
        groups_.push_back(std::move(group));
    }

    // Sort groups alphabetically
    std::sort(groups_.begin(), groups_.end(),
              [](const PinGroup& a, const PinGroup& b) { return a.name < b.name; });

    // Re-apply any existing XDC aliases so they survive a BSDL reload
    if (!xdc_aliases_.empty()) {
        for (auto& group : groups_) {
            for (auto& pin : group.pins) {
                auto it = xdc_aliases_.find(pin.name);
                if (it != xdc_aliases_.end()) pin.label = it->second;
            }
        }
    }
}

void SignalPanel::populateFromPinLists(
    const std::vector<std::string>& observable,
    const std::vector<std::string>& drivable) {
    groups_.clear();
    selected_order_.clear();
    selection_changed_ = true;

    // Build a set of drivable names for O(1) lookup.
    std::set<std::string> drv_set(drivable.begin(), drivable.end());

    std::map<std::string, PinEntry> seen;
    for (const auto& name : observable) {
        if (seen.count(name)) continue;
        PinEntry entry;
        entry.name      = name;
        entry.label     = name;
        entry.bsr_cell  = -1;  // not available without BSDL model
        entry.selected  = false;
        entry.direction = drv_set.count(name) ? "INOUT" : "IN";
        seen.emplace(name, std::move(entry));
    }
    for (const auto& name : drivable) {
        if (seen.count(name)) continue;
        PinEntry entry;
        entry.name      = name;
        entry.label     = name;
        entry.bsr_cell  = -1;
        entry.selected  = false;
        entry.direction = "OUT";
        seen.emplace(name, std::move(entry));
    }

    // Group by prefix (same logic as populateFromBsdl).
    std::map<std::string, std::vector<PinEntry>> grouped;
    for (auto& [name, entry] : seen) {
        std::string prefix;
        for (size_t i = 0; i < name.size(); i++) {
            if (name[i] == '_' ||
                (i > 0 && std::isdigit(static_cast<unsigned char>(name[i])) &&
                 !std::isdigit(static_cast<unsigned char>(name[i - 1])))) {
                prefix = name.substr(0, i);
                break;
            }
        }
        if (prefix.empty()) prefix = name;
        grouped[prefix].push_back(std::move(entry));
    }
    for (auto& [prefix, pins] : grouped) {
        PinGroup group;
        group.name = prefix;
        group.pins = std::move(pins);
        groups_.push_back(std::move(group));
    }
    std::sort(groups_.begin(), groups_.end(),
              [](const PinGroup& a, const PinGroup& b) { return a.name < b.name; });

    // Re-apply XDC aliases.
    if (!xdc_aliases_.empty()) {
        for (auto& group : groups_) {
            for (auto& pin : group.pins) {
                auto it = xdc_aliases_.find(pin.name);
                if (it != xdc_aliases_.end()) pin.label = it->second;
            }
        }
    }
}

void SignalPanel::setSelectedSignals(const std::vector<std::string>& names) {
    for (auto& group : groups_) {
        for (auto& pin : group.pins) {
            pin.selected = false;
        }
    }

    selected_order_.clear();
    for (const auto& name : names) {
        if (auto* pin = findPin(name)) {
            pin->selected = true;
            selected_order_.push_back(name);
        }
    }

    selection_changed_ = true;
}

bool SignalPanel::consumeSelectionChanged() {
    const bool changed = selection_changed_;
    selection_changed_ = false;
    return changed;
}

bool SignalPanel::consumeOpenBsdlRequest() {
    const bool req = open_bsdl_requested_;
    open_bsdl_requested_ = false;
    return req;
}

bool SignalPanel::consumeConnectRequest() {
    const bool req = connect_requested_;
    connect_requested_ = false;
    return req;
}

std::vector<std::string> SignalPanel::selectedSignals() {
    return selected_order_;
}

void SignalPanel::clear() {
    groups_.clear();
    selected_order_.clear();
    selection_changed_ = true;
}

// ── Bus management ──────────────────────────────────────────────────────────

const std::vector<BusDefinition>& SignalPanel::buses() {
    return buses_;
}

void SignalPanel::setBuses(const std::vector<BusDefinition>& buses) {
    buses_ = buses;
    bus_changed_ = true;
}

void SignalPanel::addBus(const BusDefinition& bus) {
    for (auto& b : buses_) {
        if (b.name == bus.name) {
            b = bus;
            bus_changed_ = true;
            return;
        }
    }
    buses_.push_back(bus);
    bus_changed_ = true;
}

void SignalPanel::removeBus(const std::string& name) {
    auto it = std::remove_if(buses_.begin(), buses_.end(),
                             [&name](const BusDefinition& b) { return b.name == name; });
    if (it != buses_.end()) {
        buses_.erase(it, buses_.end());
        bus_changed_ = true;
    }
}

bool SignalPanel::consumeBusChanged() {
    const bool changed = bus_changed_;
    bus_changed_ = false;
    return changed;
}

void SignalPanel::setXdcAliases(const jtag::xdc::PinAliasMap& aliases) {
    xdc_aliases_ = aliases;

    // Apply (or clear) labels on all existing PinEntry objects.
    for (auto& group : groups_) {
        for (auto& pin : group.pins) {
            auto it = aliases.find(pin.name);
            pin.label = (it != aliases.end()) ? it->second : pin.name;
        }
    }
}

void SignalPanel::draw(bool connected) {
    ImGui::Begin("Signals");

    if (groups_.empty()) {
        ImGui::TextDisabled("No BSDL loaded.");
        ImGui::Spacing();
        // Step 1: Connect
        if (connected) { ImGui::BeginDisabled(); }
        if (ImGui::Button("Connect...")) {
            connect_requested_ = true;
        }
        if (connected) { ImGui::EndDisabled(); }
        ImGui::SameLine();
        // Step 2: Open BSDL (only after connected)
        if (!connected) { ImGui::BeginDisabled(); }
        if (ImGui::Button("Open BSDL...")) {
            open_bsdl_requested_ = true;
        }
        if (!connected) { ImGui::EndDisabled(); }
        ImGui::End();
        return;
    }

    // Select All / Deselect All buttons
    if (ImGui::SmallButton("Select All")) {
        selected_order_.clear();
        for (auto& g : groups_) {
            for (auto& p : g.pins) {
                p.selected = true;
                selected_order_.push_back(p.name);
            }
        }
        selection_changed_ = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Deselect All")) {
        for (auto& g : groups_)
            for (auto& p : g.pins) p.selected = false;
        selected_order_.clear();
        selection_changed_ = true;
    }

    // ── Group into Bus button ───────────────────────────────────────────
    int checked_count = 0;
    for (const auto& order_name : selected_order_) {
        (void)order_name;
        checked_count++;
    }
    // Count checked pins (those in selected_order_)
    const bool can_group = checked_count >= 2;
    if (!can_group) ImGui::BeginDisabled();
    if (ImGui::SmallButton("Group into Bus")) {
        bus_pending_signals_ = selected_order_;
        bus_name_buf_[0] = '\0';
        bus_dialog_open_ = true;
        ImGui::OpenPopup("Define Bus");
    }
    if (!can_group) ImGui::EndDisabled();
    if (can_group) {
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Group %d selected signal(s) into a named multi-bit bus",
                              checked_count);
        }
    }

    // ── Define Bus popup ────────────────────────────────────────────────────
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Define Bus", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Bus name:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("##busname", bus_name_buf_, sizeof(bus_name_buf_));

        ImGui::Separator();
        ImGui::TextDisabled("Signal order (MSB first, use ^ v to reorder):");

        int move_from = -1, move_to = -1;
        for (size_t i = 0; i < bus_pending_signals_.size(); i++) {
            ImGui::PushID(static_cast<int>(i));
            bool can_up   = (i > 0);
            bool can_down = (i + 1 < bus_pending_signals_.size());
            if (!can_up) ImGui::BeginDisabled();
            if (ImGui::SmallButton("^")) { move_from = static_cast<int>(i); move_to = static_cast<int>(i - 1); }
            if (!can_up) ImGui::EndDisabled();
            ImGui::SameLine();
            if (!can_down) ImGui::BeginDisabled();
            if (ImGui::SmallButton("v")) { move_from = static_cast<int>(i); move_to = static_cast<int>(i + 1); }
            if (!can_down) ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Text("%zu: %s", i, bus_pending_signals_[i].c_str());
            ImGui::PopID();
        }
        if (move_from >= 0 && move_to >= 0)
            std::swap(bus_pending_signals_[move_from], bus_pending_signals_[move_to]);

        ImGui::Separator();
        const char* fmt_names[] = {"HEX", "DEC", "BIN"};
        // find any existing bus with same name to prefill format
        static int fmt_idx = 0;
        ImGui::SetNextItemWidth(100);
        ImGui::Combo("Format##busdef", &fmt_idx, fmt_names, 3);

        ImGui::Separator();
        bool name_ok = (bus_name_buf_[0] != '\0');
        if (!name_ok) ImGui::BeginDisabled();
        if (ImGui::Button("OK", ImVec2(80, 0))) {
            BusDefinition newbus;
            newbus.name = bus_name_buf_;
            newbus.signals = bus_pending_signals_;
            newbus.format  = static_cast<BusFormat>(fmt_idx);
            addBus(newbus);
            ImGui::CloseCurrentPopup();
        }
        if (!name_ok) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── Defined Buses section ───────────────────────────────────────────────
    if (!buses_.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Defined Buses:");
        for (size_t i = 0; i < buses_.size(); i++) {
            const auto& bus = buses_[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::BulletText("%s [%zu bits]", bus.name.c_str(), bus.signals.size());
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                removeBus(bus.name);
                ImGui::PopID();
                break;  // vector modified; restart loop next frame
            }
            ImGui::PopID();
        }
    }

    ImGui::Separator();

    ImGui::TextDisabled("Selected Order");
    if (selected_order_.empty()) {
        ImGui::TextDisabled("Select signals to control waveform order.");
    } else {
        ImGui::BeginChild("selected_order", ImVec2(0.0f, 120.0f), true);
        int move_from = -1;
        int move_to = -1;

        for (size_t i = 0; i < selected_order_.size(); i++) {
            const bool can_move_up = i > 0;
            const bool can_move_down = i + 1 < selected_order_.size();

            ImGui::PushID(static_cast<int>(i));
            if (!can_move_up) {
                ImGui::BeginDisabled();
            }
            if (ImGui::SmallButton("^")) {
                move_from = static_cast<int>(i);
                move_to = static_cast<int>(i - 1);
            }
            if (!can_move_up) {
                ImGui::EndDisabled();
            }

            ImGui::SameLine();
            if (!can_move_down) {
                ImGui::BeginDisabled();
            }
            if (ImGui::SmallButton("v")) {
                move_from = static_cast<int>(i);
                move_to = static_cast<int>(i + 1);
            }
            if (!can_move_down) {
                ImGui::EndDisabled();
            }

            ImGui::SameLine();
            ImGui::Text("%zu. %s", i + 1, selected_order_[i].c_str());
            ImGui::PopID();
        }

        if (move_from >= 0 && move_to >= 0) {
            std::swap(selected_order_[move_from], selected_order_[move_to]);
            selection_changed_ = true;
        }

        ImGui::EndChild();
    }

    ImGui::Separator();

    for (auto& group : groups_) {
        // Group tree node with tri-state check
        int selected_count = 0;
        for (const auto& p : group.pins) {
            if (p.selected) selected_count++;
        }
        bool all = (selected_count == static_cast<int>(group.pins.size()));
        bool none = (selected_count == 0);

        ImGuiTreeNodeFlags node_flags =
            ImGuiTreeNodeFlags_OpenOnArrow |
            ImGuiTreeNodeFlags_DefaultOpen;

        if (ImGui::TreeNodeEx(group.name.c_str(), node_flags)) {
            // Group-level checkbox
            ImGui::SameLine();
            bool group_check = all;
            bool mixed = !all && !none;
            if (mixed) {
                ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            }
            char group_id[128];
            snprintf(group_id, sizeof(group_id), "##grp_%s",
                     group.name.c_str());
            if (ImGui::Checkbox(group_id, &group_check)) {
                for (auto& p : group.pins) {
                    setPinSelected(p, group_check);
                }
                selection_changed_ = true;
            }
            if (mixed) {
                ImGui::PopItemFlag();
            }

            // Individual pins
            for (auto& pin : group.pins) {
                // If XDC alias differs from BSDL name, show "alias (bsdl_name)##bsdl_name".
                char pin_id[512];
                if (pin.label != pin.name) {
                    snprintf(pin_id, sizeof(pin_id), "%s (%s)##%s",
                             pin.label.c_str(), pin.name.c_str(), pin.name.c_str());
                } else {
                    snprintf(pin_id, sizeof(pin_id), "%s##%s",
                             pin.label.c_str(), pin.name.c_str());
                }
                if (ImGui::Checkbox(pin_id, &pin.selected)) {
                    if (pin.selected) {
                        addToSelectedOrder(pin.name);
                    } else {
                        removeFromSelectedOrder(pin.name);
                    }
                    selection_changed_ = true;
                }
                ImGui::SameLine(200);
                ImGui::TextDisabled("%s", pin.direction.c_str());
                if (pin.bsr_cell >= 0) {
                    ImGui::SameLine(260);
                    ImGui::TextDisabled("[%d]", pin.bsr_cell);
                }
            }

            ImGui::TreePop();
        }
    }

    ImGui::End();
}

} // namespace jtag::gui

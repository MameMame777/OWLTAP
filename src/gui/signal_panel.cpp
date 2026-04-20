#include "signal_panel.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <set>

namespace jtag::gui {

std::vector<SignalPanel::PinGroup> SignalPanel::groups_;
bool SignalPanel::selection_changed_ = false;

void SignalPanel::populateFromBsdl(const jtag::bsdl::BSDLDevice& device) {
    groups_.clear();
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
        entry.name = name;
        entry.direction = dir_str;
        entry.bsr_cell = cell.position;
        entry.selected = false;
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
}

void SignalPanel::setSelectedSignals(const std::vector<std::string>& names) {
    std::set<std::string> name_set(names.begin(), names.end());
    for (auto& group : groups_)
        for (auto& pin : group.pins)
            pin.selected = name_set.count(pin.name) > 0;
    selection_changed_ = true;
}

bool SignalPanel::consumeSelectionChanged() {
    const bool changed = selection_changed_;
    selection_changed_ = false;
    return changed;
}

std::vector<std::string> SignalPanel::selectedSignals() {
    std::vector<std::string> result;
    for (const auto& group : groups_) {
        for (const auto& pin : group.pins) {
            if (pin.selected) {
                result.push_back(pin.name);
            }
        }
    }
    return result;
}

void SignalPanel::clear() {
    groups_.clear();
    selection_changed_ = true;
}

void SignalPanel::draw() {
    ImGui::Begin("Signals");

    if (groups_.empty()) {
        ImGui::TextDisabled("Load a BSDL file to see signals.");
        ImGui::End();
        return;
    }

    // Select All / Deselect All buttons
    if (ImGui::SmallButton("Select All")) {
        for (auto& g : groups_)
            for (auto& p : g.pins) p.selected = true;
        selection_changed_ = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Deselect All")) {
        for (auto& g : groups_)
            for (auto& p : g.pins) p.selected = false;
        selection_changed_ = true;
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
                for (auto& p : group.pins) p.selected = group_check;
                selection_changed_ = true;
            }
            if (mixed) {
                ImGui::PopItemFlag();
            }

            // Individual pins
            for (auto& pin : group.pins) {
                char pin_id[256];
                snprintf(pin_id, sizeof(pin_id), "%s##%s",
                         pin.name.c_str(), pin.name.c_str());
                if (ImGui::Checkbox(pin_id, &pin.selected)) {
                    selection_changed_ = true;
                }
                ImGui::SameLine(200);
                ImGui::TextDisabled("%s", pin.direction.c_str());
                ImGui::SameLine(260);
                if (pin.bsr_cell >= 0) {
                    ImGui::TextDisabled("[%d]", pin.bsr_cell);
                }
            }

            ImGui::TreePop();
        }
    }

    ImGui::End();
}

} // namespace jtag::gui

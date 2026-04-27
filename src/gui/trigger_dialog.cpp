#include "trigger_dialog.h"

#include <imgui.h>

#include <cstdio>

namespace jtag::gui {

jtag::Scanner* TriggerDialog::scanner_ = nullptr;
jtag::TriggerEngine* TriggerDialog::trigger_ = nullptr;
int TriggerDialog::mode_idx_ = 0;
int TriggerDialog::pin_idx_ = 0;
int TriggerDialog::type_idx_ = 0;
int TriggerDialog::edge_idx_ = 0;
int TriggerDialog::level_idx_ = 0;
float TriggerDialog::pretrigger_ = 0.5f;
std::vector<std::string> TriggerDialog::pin_names_;
std::vector<jtag::TriggerCondition> TriggerDialog::conditions_;

void TriggerDialog::bind(jtag::Scanner* scanner,
                          jtag::TriggerEngine* trigger) {
    scanner_ = scanner;
    trigger_ = trigger;
    if (trigger_) {
        conditions_ = trigger_->conditions();
        pretrigger_ = trigger_->preTriggerRatio();
        // Map TriggerMode to dialog index (Single not shown; defaults to Free Run)
        auto m = trigger_->mode();
        mode_idx_ = (m == jtag::TriggerMode::NORMAL) ? 1 : 0;
    }
    if (scanner_) {
        pin_names_ = scanner_->getObservablePins();
    }
}

void TriggerDialog::draw(bool* p_open, int* p_buffer_depth) {
    if (!ImGui::IsPopupOpen("Trigger Configuration")) {
        ImGui::OpenPopup("Trigger Configuration");
    }

    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopupModal("Trigger Configuration", p_open)) {

        // Trigger mode
        if (ImGui::CollapsingHeader("Trigger Mode",
                                     ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* modes[] = {"Free Run", "Normal"};
            ImGui::Combo("Mode", &mode_idx_, modes, 2);
            ImGui::SliderFloat("Pre-trigger ratio", &pretrigger_, 0.0f, 1.0f,
                               "%.1f");
            if (p_buffer_depth) {
                ImGui::InputInt("Buffer Depth (samples)", p_buffer_depth);
                if (*p_buffer_depth < 100)  *p_buffer_depth = 100;
                if (*p_buffer_depth > 100000) *p_buffer_depth = 100000;
                ImGui::SameLine();
                ImGui::TextDisabled("(?");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Ring buffer size.\n"
                        "Older samples are overwritten when full.\n"
                        "Range: 100 - 100000.");
                    ImGui::SameLine(); ImGui::TextDisabled(")");
                } else {
                    ImGui::SameLine(); ImGui::TextDisabled(")");
                }
            }
        }

        // Condition editor (only available when a local capture engine is bound)
        if (trigger_ &&
            ImGui::CollapsingHeader("Trigger Conditions (AND)",
                                     ImGuiTreeNodeFlags_DefaultOpen)) {
            // Add condition row
            if (!pin_names_.empty()) {
                const char* preview = pin_names_[pin_idx_].c_str();
                if (ImGui::BeginCombo("Pin", preview)) {
                    for (int i = 0; i < static_cast<int>(pin_names_.size());
                         i++) {
                        if (ImGui::Selectable(pin_names_[i].c_str(),
                                              i == pin_idx_)) {
                            pin_idx_ = i;
                        }
                    }
                    ImGui::EndCombo();
                }

                const char* types[] = {"Edge", "Level"};
                ImGui::Combo("Type", &type_idx_, types, 2);

                if (type_idx_ == 0) {
                    const char* edges[] = {"Rising", "Falling", "Either"};
                    ImGui::Combo("Edge", &edge_idx_, edges, 3);
                } else {
                    const char* levels[] = {"HIGH", "LOW"};
                    ImGui::Combo("Level", &level_idx_, levels, 2);
                }

                if (ImGui::Button("Add Condition")) {
                    jtag::TriggerCondition cond;
                    cond.pin_name = pin_names_[pin_idx_];
                    cond.type = static_cast<jtag::TriggerType>(type_idx_);
                    cond.edge = static_cast<jtag::TriggerEdge>(edge_idx_);
                    cond.level = (level_idx_ == 0)
                        ? jtag::PinState::HIGH
                        : jtag::PinState::LOW;
                    conditions_.push_back(cond);
                }
            }

            ImGui::Separator();

            // Condition list
            int remove_idx = -1;
            for (int i = 0; i < static_cast<int>(conditions_.size()); i++) {
                const auto& cond = conditions_[i];
                char desc[256];
                if (cond.type == jtag::TriggerType::EDGE) {
                    const char* edge_name =
                        (cond.edge == jtag::TriggerEdge::RISING) ? "Rising" :
                        (cond.edge == jtag::TriggerEdge::FALLING) ? "Falling" :
                        "Any";
                    snprintf(desc, sizeof(desc), "%s - %s Edge",
                             cond.pin_name.c_str(), edge_name);
                } else {
                    snprintf(desc, sizeof(desc), "%s - Level %s",
                             cond.pin_name.c_str(),
                             (cond.level == jtag::PinState::HIGH)
                             ? "HIGH" : "LOW");
                }
                ImGui::BulletText("%s", desc);
                ImGui::SameLine();
                char btn_id[32];
                snprintf(btn_id, sizeof(btn_id), "X##%d", i);
                if (ImGui::SmallButton(btn_id)) {
                    remove_idx = i;
                }
            }
            if (remove_idx >= 0) {
                conditions_.erase(conditions_.begin() + remove_idx);
            }
        }

        ImGui::Separator();

        if (ImGui::Button("OK", ImVec2(120, 0))) {
            // Map dialog index back to TriggerMode (0=Free Run, 1=Normal)
            if (trigger_) {
                trigger_->setMode(mode_idx_ == 1 ? jtag::TriggerMode::NORMAL
                                                 : jtag::TriggerMode::FREE_RUN);
                trigger_->setPreTriggerRatio(pretrigger_);
                trigger_->setConditions(conditions_);
            }
            *p_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            *p_open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace jtag::gui

#include "interconnect_panel.h"

#include <fstream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#endif

#include "imgui.h"
#include "src/boundary_scan/interconnect_test.h"

namespace jtag::gui {

bool InterconnectPanel::visible_ = true;
std::string InterconnectPanel::ict_path_;
std::vector<jtag::NetDef> InterconnectPanel::nets_;
std::string InterconnectPanel::load_error_;
jtag::InterconnectResult InterconnectPanel::last_result_;
bool InterconnectPanel::has_result_ = false;
std::string InterconnectPanel::run_error_;

void InterconnectPanel::setVisible(bool visible) { visible_ = visible; }
bool InterconnectPanel::isVisible() { return visible_; }

namespace {

std::string openIctDialog() {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrTitle = "Load Interconnect Test";
    ofn.lpstrFilter = "Interconnect Test\0*.ict\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return path;
#endif
    return {};
}

std::string saveReportDialog() {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrTitle = "Save Report";
    ofn.lpstrFilter = "Text Files\0*.txt\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.lpstrDefExt = "txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) return path;
#endif
    return {};
}

const char* pinStateName(jtag::PinState s) {
    switch (s) {
        case jtag::PinState::LOW:     return "LOW";
        case jtag::PinState::HIGH:    return "HIGH";
        case jtag::PinState::UNKNOWN: return "UNK";
    }
    return "UNK";
}

} // namespace

void InterconnectPanel::draw(const std::vector<jtag::Scanner*>& scanners,
                             const std::vector<jtag::PinDriver*>& drivers,
                             bool connected) {
    if (!visible_) return;
    if (!ImGui::Begin("Interconnect Test", &visible_)) {
        ImGui::End();
        return;
    }

    // ── Load ──────────────────────────────────────────────────────
    if (ImGui::Button("Load .ict...")) {
        const std::string path = openIctDialog();
        if (!path.empty()) {
            load_error_.clear();
            has_result_ = false;
            run_error_.clear();
            nets_ = jtag::parseIctFile(path, load_error_);
            if (load_error_.empty()) {
                ict_path_ = path;
            }
        }
    }
    ImGui::SameLine();
    if (ict_path_.empty()) {
        ImGui::TextDisabled("No file loaded");
    } else {
        ImGui::TextUnformatted(ict_path_.c_str());
    }

    if (!load_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::TextWrapped("Error: %s", load_error_.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    // ── Net list preview ──────────────────────────────────────────
    if (!nets_.empty()) {
        ImGui::Text("Nets loaded: %d", static_cast<int>(nets_.size()));
        if (ImGui::BeginTable("IctNets", 3,
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, 120.0f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Net");
            ImGui::TableSetupColumn("Driver");
            ImGui::TableSetupColumn("Receivers");
            ImGui::TableHeadersRow();

            for (const auto& nd : nets_) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(nd.name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d:%s", nd.driver.device_index,
                            nd.driver.pin.c_str());
                ImGui::TableSetColumnIndex(2);
                std::string rx;
                for (size_t r = 0; r < nd.receivers.size(); r++) {
                    if (r) rx += "  ";
                    rx += std::to_string(nd.receivers[r].device_index) +
                          ":" + nd.receivers[r].pin;
                }
                ImGui::TextUnformatted(rx.c_str());
            }
            ImGui::EndTable();
        }
    } else {
        ImGui::TextDisabled("Load an .ict file to see the net list.");
    }

    ImGui::Separator();

    // ── Run ───────────────────────────────────────────────────────
    const bool can_run = !nets_.empty() && connected;
    if (!can_run) ImGui::BeginDisabled();
    if (ImGui::Button("Run Test")) {
        run_error_.clear();
        last_result_ = jtag::runInterconnectTest(nets_, scanners, drivers);
        has_result_ = true;
    }
    if (!can_run) ImGui::EndDisabled();
    if (!connected && !nets_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(connect device first)");
    }

    if (!run_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::TextWrapped("Error: %s", run_error_.c_str());
        ImGui::PopStyleColor();
    }

    // ── Results ───────────────────────────────────────────────────
    if (has_result_) {
        ImGui::Separator();
        ImGui::Text("Results:  PASS %d / %d",
                    last_result_.pass_count,
                    static_cast<int>(last_result_.nets.size()));

        if (ImGui::BeginTable("IctResults", 4,
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, 200.0f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Net");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Drive=0");
            ImGui::TableSetupColumn("Drive=1");
            ImGui::TableHeadersRow();

            for (size_t i = 0;
                 i < last_result_.nets.size() && i < nets_.size(); i++) {
                const auto& nr = last_result_.nets[i];
                const auto& nd = nets_[i];

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(nr.name.c_str());

                ImGui::TableSetColumnIndex(1);
                if (nr.pass) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
                    ImGui::TextUnformatted("PASS");
                } else {
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                    ImGui::TextUnformatted("FAIL");
                }
                ImGui::PopStyleColor();

                for (int s = 0; s < 2; s++) {
                    ImGui::TableSetColumnIndex(2 + s);
                    if (s < static_cast<int>(nr.steps.size())) {
                        const auto& step = nr.steps[s];
                        std::string cell;
                        for (size_t r = 0;
                             r < step.observed.size() &&
                             r < nd.receivers.size(); r++) {
                            if (r) cell += " ";
                            cell += nd.receivers[r].pin + "=";
                            cell += pinStateName(step.observed[r]);
                        }
                        if (!step.pass) {
                            ImGui::PushStyleColor(
                                ImGuiCol_Text,
                                ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
                        }
                        ImGui::TextUnformatted(cell.c_str());
                        if (!step.pass) ImGui::PopStyleColor();
                    }
                }
            }
            ImGui::EndTable();
        }

        if (ImGui::Button("Export Report...")) {
            const std::string path = saveReportDialog();
            if (!path.empty()) {
                const std::string report =
                    jtag::formatInterconnectReport(last_result_, nets_);
                std::ofstream f(path);
                if (f.is_open()) {
                    f << report;
                }
            }
        }
    }

    ImGui::End();
}

} // namespace jtag::gui

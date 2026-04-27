#include "debug_log_panel.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>
#include <vector>

namespace jtag::gui {

namespace {

constexpr size_t kMaxLogEntries = 512;

std::vector<std::string> entries_;
std::string              log_text_;   // joined text for selectable display
bool auto_scroll_ = true;
bool scroll_to_bottom_ = false;

} // namespace

void DebugLogPanel::draw(const std::string& latest_status) {
    ImGui::Begin("Debug Log");

    const bool log_empty = entries_.empty();
    if (log_empty) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Clear Log")) {
        clear();
    }
    if (log_empty) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto Scroll", &auto_scroll_);
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", latest_status.c_str());
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::InputTextMultiline(
        "##debug_log",
        const_cast<char*>(log_text_.c_str()),
        log_text_.size() + 1,
        ImVec2(avail.x, avail.y),
        ImGuiInputTextFlags_ReadOnly);

    if (auto_scroll_ && scroll_to_bottom_) {
        // Scroll the InputTextMultiline to the bottom by setting scroll on
        // the inner child window that ImGui creates for it.
        if (ImGui::IsItemVisible()) {
            ImGui::SetScrollHereY(1.0f);
        }
    }

    scroll_to_bottom_ = false;
    ImGui::End();
}

void DebugLogPanel::append(const std::string& message) {
    size_t start = 0;
    while (start <= message.size()) {
        size_t end = message.find('\n', start);
        std::string line = (end == std::string::npos)
            ? message.substr(start)
            : message.substr(start, end - start);

        if (!line.empty()) {
            entries_.push_back(formatEntry(line));
        }

        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    if (entries_.size() > kMaxLogEntries) {
        entries_.erase(entries_.begin(),
                       entries_.begin() + (entries_.size() - kMaxLogEntries));
    }

    // Rebuild joined text.
    log_text_.clear();
    for (const auto& e : entries_) {
        log_text_ += e;
        log_text_ += '\n';
    }

    scroll_to_bottom_ = true;
}

void DebugLogPanel::clear() {
    entries_.clear();
    log_text_.clear();
    scroll_to_bottom_ = false;
}

std::string DebugLogPanel::formatEntry(const std::string& message_line) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time = {};
#ifdef _WIN32
    localtime_s(&local_time, &now_time);
#else
    localtime_r(&now_time, &local_time);
#endif

    char timestamp[16];
    std::strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &local_time);
    return "[" + std::string(timestamp) + "] " + message_line;
}

} // namespace jtag::gui
#include "ila_generator_dialog.h"

#include <imgui.h>

#include <array>
#include <string>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "app_config.h"
#include "src/ila/ila_generator.h"

namespace jtag::gui {
namespace {

struct LaneRow {
    std::array<char, 64> name{};
    int hi = 0;
    int lo = 0;
    int fmt = 0;
};

std::array<char, 260> output_subdir_{};
std::array<char, 64> project_name_{};
std::array<char, 64> top_module_{};
std::array<char, 64> fpga_part_{};
std::array<char, 64> clock_port_name_{};
std::array<char, 64> reset_port_name_{};
std::array<char, 64> data_port_name_{};
std::array<char, 64> valid_port_name_{};
std::array<char, 16> idcode_{};
int sample_clock_hz_ = 125000000;
int data_width_ = 32;
int depth_ = 1024;
std::vector<LaneRow> lane_rows_;
std::string error_message_;
std::string success_message_;
std::string pending_status_message_;
bool was_open_ = false;

template <size_t N>
void copyText(std::array<char, N>& dest, const std::string& value) {
    std::snprintf(dest.data(), dest.size(), "%s", value.c_str());
}

BusFormat toBusFormat(int fmt) {
    switch (fmt) {
        case 1: return BusFormat::DEC;
        case 2: return BusFormat::BIN;
        case 0:
        default: return BusFormat::HEX;
    }
}

jtag::ila::IlaGeneratorFormat toGeneratorFormat(int fmt) {
    switch (fmt) {
        case 1: return jtag::ila::IlaGeneratorFormat::DEC;
        case 2: return jtag::ila::IlaGeneratorFormat::BIN;
        case 0:
        default: return jtag::ila::IlaGeneratorFormat::HEX;
    }
}

int fromBusFormat(BusFormat fmt) {
    switch (fmt) {
        case BusFormat::DEC: return 1;
        case BusFormat::BIN: return 2;
        case BusFormat::HEX:
        default: return 0;
    }
}

void resetLaneRowsToSingleWord() {
    lane_rows_.clear();
    LaneRow row;
    std::snprintf(row.name.data(), row.name.size(), "data_word");
    row.hi = data_width_ - 1;
    row.lo = 0;
    row.fmt = 0;
    lane_rows_.push_back(row);
}

void loadFromConfig(const AppConfig* config) {
    if (config) {
        copyText(output_subdir_, config->ila_generator_output_subdir.empty()
            ? std::string("hdl/ila/generated/ila_generated")
            : config->ila_generator_output_subdir);
        copyText(project_name_, config->ila_generator_project_name.empty()
            ? std::string("ila_generated")
            : config->ila_generator_project_name);
        copyText(top_module_, config->ila_generator_top_module.empty()
            ? std::string("ila_generated_top")
            : config->ila_generator_top_module);
        copyText(fpga_part_, config->ila_generator_fpga_part.empty()
            ? std::string("xc7z020clg400-1")
            : config->ila_generator_fpga_part);
        copyText(clock_port_name_, config->ila_generator_clock_port_name.empty()
            ? std::string("sample_clk")
            : config->ila_generator_clock_port_name);
        copyText(reset_port_name_, config->ila_generator_reset_port_name.empty()
            ? std::string("sample_rst_n")
            : config->ila_generator_reset_port_name);
        copyText(data_port_name_, config->ila_generator_data_port_name.empty()
            ? std::string("data_in")
            : config->ila_generator_data_port_name);
        copyText(valid_port_name_, config->ila_generator_valid_port_name.empty()
            ? std::string("data_valid")
            : config->ila_generator_valid_port_name);
        std::snprintf(idcode_.data(), idcode_.size(), "%08X", config->ila_generator_idcode);
        sample_clock_hz_ = config->ila_generator_sample_clock_hz > 0
            ? static_cast<int>(config->ila_generator_sample_clock_hz)
            : 125000000;
        data_width_ = config->ila_generator_data_width > 0
            ? config->ila_generator_data_width : 32;
        depth_ = config->ila_generator_depth > 0
            ? config->ila_generator_depth : 1024;

        lane_rows_.clear();
        for (const auto& lane : config->ila_generator_lanes) {
            LaneRow row;
            std::snprintf(row.name.data(), row.name.size(), "%s", lane.name.c_str());
            row.hi = lane.hi;
            row.lo = lane.lo;
            row.fmt = fromBusFormat(lane.fmt);
            lane_rows_.push_back(row);
        }
        if (lane_rows_.empty()) {
            resetLaneRowsToSingleWord();
        }
    } else {
        copyText(output_subdir_, "hdl/ila/generated/ila_generated");
        copyText(project_name_, "ila_generated");
        copyText(top_module_, "ila_generated_top");
        copyText(fpga_part_, "xc7z020clg400-1");
        copyText(clock_port_name_, "sample_clk");
        copyText(reset_port_name_, "sample_rst_n");
        copyText(data_port_name_, "data_in");
        copyText(valid_port_name_, "data_valid");
        std::snprintf(idcode_.data(), idcode_.size(), "%08X", 0xA17A0001u);
        sample_clock_hz_ = 125000000;
        data_width_ = 32;
        depth_ = 1024;
        resetLaneRowsToSingleWord();
    }
    error_message_.clear();
    success_message_.clear();
}

std::filesystem::path detectRepoRoot() {
    std::error_code ec;
    auto path = std::filesystem::current_path(ec);
    if (ec) return {};
    for (;;) {
        if (std::filesystem::exists(path / "hdl" / "ila" / "rtl" / "ila_bscane2_top.sv")) {
            return path;
        }
        const auto parent = path.parent_path();
        if (parent.empty() || parent == path) {
            break;
        }
        path = parent;
    }
    return {};
}

bool parseIdcode(uint32_t& out) {
    unsigned int value = 0;
    if (std::sscanf(idcode_.data(), "%x", &value) != 1) {
        return false;
    }
    out = value;
    return true;
}

void persistToConfig(AppConfig* config) {
    if (!config) return;
    config->ila_generator_output_subdir = output_subdir_.data();
    config->ila_generator_project_name = project_name_.data();
    config->ila_generator_top_module = top_module_.data();
    config->ila_generator_fpga_part = fpga_part_.data();
    config->ila_generator_clock_port_name = clock_port_name_.data();
    config->ila_generator_reset_port_name = reset_port_name_.data();
    config->ila_generator_data_port_name = data_port_name_.data();
    config->ila_generator_valid_port_name = valid_port_name_.data();
    config->ila_generator_sample_clock_hz = static_cast<uint32_t>(sample_clock_hz_ > 0 ? sample_clock_hz_ : 0);
    config->ila_generator_data_width = data_width_;
    config->ila_generator_depth = depth_;
    uint32_t parsed_idcode = 0xA17A0001u;
    if (parseIdcode(parsed_idcode)) {
        config->ila_generator_idcode = parsed_idcode;
    }
    config->ila_generator_lanes.clear();
    for (const auto& row : lane_rows_) {
        IlaSignalConfig lane;
        lane.name = row.name.data();
        lane.hi = row.hi;
        lane.lo = row.lo;
        lane.fmt = toBusFormat(row.fmt);
        config->ila_generator_lanes.push_back(std::move(lane));
    }
    config->save("cfg.json");
}

} // namespace

void IlaGeneratorDialog::draw(bool* p_open, AppConfig* config) {
    if (*p_open && !was_open_) {
        loadFromConfig(config);
    }
    was_open_ = *p_open;

    if (!ImGui::IsPopupOpen("Generate ILA Core")) {
        ImGui::OpenPopup("Generate ILA Core");
    }

    ImGui::SetNextWindowSize(ImVec2(760, 0), ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopupModal("Generate ILA Core", p_open)) {
        ImGui::TextWrapped("Generate a BSCANE2-based ILA wrapper and Vivado helper package. Output path is relative to the repository root.");
        ImGui::Separator();

        ImGui::InputText("Output Subdir", output_subdir_.data(), output_subdir_.size());
        ImGui::InputText("Project Name", project_name_.data(), project_name_.size());
        ImGui::InputText("Top Module", top_module_.data(), top_module_.size());
        ImGui::InputText("FPGA Part", fpga_part_.data(), fpga_part_.size());
        ImGui::InputText("Clock Port", clock_port_name_.data(), clock_port_name_.size());
        ImGui::InputText("Reset Port", reset_port_name_.data(), reset_port_name_.size());
        ImGui::InputText("Data Port", data_port_name_.data(), data_port_name_.size());
        ImGui::InputText("Valid Port", valid_port_name_.data(), valid_port_name_.size());
        ImGui::InputInt("Sample Clock (Hz)", &sample_clock_hz_);
        ImGui::InputInt("Data Width", &data_width_);
        ImGui::InputInt("Depth", &depth_);
        ImGui::InputText("IDCODE (hex)", idcode_.data(), idcode_.size());

        ImGui::Separator();
        if (ImGui::Button("Reset Full-Width Lane")) {
            if (data_width_ < 1) data_width_ = 1;
            resetLaneRowsToSingleWord();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Lane")) {
            LaneRow row;
            std::snprintf(row.name.data(), row.name.size(), "lane_%u", static_cast<unsigned>(lane_rows_.size()));
            row.hi = data_width_ > 0 ? data_width_ - 1 : 0;
            row.lo = 0;
            row.fmt = 0;
            lane_rows_.push_back(row);
        }

        if (ImGui::BeginTable("ila_generator_lanes", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Hi");
            ImGui::TableSetupColumn("Lo");
            ImGui::TableSetupColumn("Fmt");
            ImGui::TableSetupColumn("Delete");
            ImGui::TableHeadersRow();
            int remove_index = -1;
            for (int i = 0; i < static_cast<int>(lane_rows_.size()); ++i) {
                auto& row = lane_rows_[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                std::string name_id = "##lane_name_" + std::to_string(i);
                ImGui::InputText(name_id.c_str(), row.name.data(), row.name.size());
                ImGui::TableSetColumnIndex(1);
                std::string hi_id = "##lane_hi_" + std::to_string(i);
                ImGui::InputInt(hi_id.c_str(), &row.hi);
                ImGui::TableSetColumnIndex(2);
                std::string lo_id = "##lane_lo_" + std::to_string(i);
                ImGui::InputInt(lo_id.c_str(), &row.lo);
                ImGui::TableSetColumnIndex(3);
                const char* fmts[] = {"HEX", "DEC", "BIN"};
                std::string fmt_id = "##lane_fmt_" + std::to_string(i);
                ImGui::Combo(fmt_id.c_str(), &row.fmt, fmts, 3);
                ImGui::TableSetColumnIndex(4);
                std::string del_id = "X##lane_del_" + std::to_string(i);
                if (ImGui::SmallButton(del_id.c_str())) {
                    remove_index = i;
                }
            }
            if (remove_index >= 0) {
                lane_rows_.erase(lane_rows_.begin() + remove_index);
            }
            ImGui::EndTable();
        }

        if (!error_message_.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s", error_message_.c_str());
        }
        if (!success_message_.empty()) {
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.35f, 1.0f), "%s", success_message_.c_str());
        }

        if (ImGui::Button("Generate", ImVec2(120, 0))) {
            jtag::ila::IlaGeneratorConfig gen;
            gen.output_subdir = output_subdir_.data();
            gen.project_name = project_name_.data();
            gen.top_module = top_module_.data();
            gen.fpga_part = fpga_part_.data();
            gen.clock_port_name = clock_port_name_.data();
            gen.reset_port_name = reset_port_name_.data();
            gen.data_port_name = data_port_name_.data();
            gen.valid_port_name = valid_port_name_.data();
            gen.sample_clock_hz = static_cast<uint32_t>(sample_clock_hz_ > 0 ? sample_clock_hz_ : 0);
            gen.data_width = data_width_;
            gen.depth = depth_;
            uint32_t parsed_idcode = 0;
            if (!parseIdcode(parsed_idcode)) {
                error_message_ = "IDCODE must be a hexadecimal value";
                success_message_.clear();
            } else {
                gen.idcode = parsed_idcode;
                gen.lanes.clear();
                for (const auto& row : lane_rows_) {
                    gen.lanes.push_back({row.name.data(), row.hi, row.lo, toGeneratorFormat(row.fmt)});
                }
                const auto repo_root = detectRepoRoot();
                if (repo_root.empty()) {
                    error_message_ = "failed to locate repository root from current working directory";
                    success_message_.clear();
                } else {
                    jtag::ila::IlaGeneratedFiles files;
                    std::string error;
                    if (!jtag::ila::writeIlaGeneratedFiles(gen, repo_root, files, error)) {
                        error_message_ = error;
                        success_message_.clear();
                    } else {
                        persistToConfig(config);
                        error_message_.clear();
                        success_message_ = "Generated " + std::to_string(files.written_paths.size()) +
                                           " files under " + files.output_dir.generic_string();
                        pending_status_message_ = success_message_;
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            *p_open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    if (!*p_open) {
        was_open_ = false;
    }
}

bool IlaGeneratorDialog::consumeStatusMessage(std::string& message) {
    if (pending_status_message_.empty()) return false;
    message = pending_status_message_;
    pending_status_message_.clear();
    return true;
}

} // namespace jtag::gui

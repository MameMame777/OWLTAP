#include "device_dialog.h"

#include <imgui.h>

#include <cstdlib>
#include <cstring>

#include "src/ftdi/ftdi_device.h"

namespace jtag::gui {

DeviceConfig DeviceDialog::config_;
bool DeviceDialog::accepted_ = false;
char DeviceDialog::vid_buf_[5] = "0403";
char DeviceDialog::pid_buf_[5] = "6010";
int DeviceDialog::interface_idx_ = 0;
int DeviceDialog::clock_hz_ = 6000000;
int DeviceDialog::device_idx_ = 0;
std::vector<DeviceDialog::DeviceEntry> DeviceDialog::devices_;

bool DeviceDialog::accepted() {
    bool v = accepted_;
    accepted_ = false;
    return v;
}

void DeviceDialog::refreshDevices() {
    devices_.clear();
    device_idx_ = 0;

    auto found = jtag::FtdiDevice::enumerate();
    if (found.empty()) {
        DeviceEntry e;
        e.label = "(No FTDI devices found)";
        devices_.push_back(std::move(e));
        return;
    }

    for (const auto& dev : found) {
        DeviceEntry e;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s - %s [%04X]",
                 dev.description.c_str(), dev.serial.c_str(),
                 dev.product_id);
        e.label = buf;
        e.serial = dev.serial;
        e.info = dev;
        devices_.push_back(std::move(e));
    }
}

void DeviceDialog::draw(bool* p_open) {
    if (!ImGui::IsPopupOpen("Connect FTDI Device")) {
        ImGui::OpenPopup("Connect FTDI Device");
        refreshDevices();
    }

    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopupModal("Connect FTDI Device", p_open)) {

        // Device selection
        if (ImGui::CollapsingHeader("Device", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Combo for device list
            const char* preview = devices_.empty()
                ? "(none)"
                : devices_[device_idx_].label.c_str();
            if (ImGui::BeginCombo("FTDI Device", preview)) {
                for (int i = 0; i < static_cast<int>(devices_.size()); i++) {
                    bool selected = (i == device_idx_);
                    if (ImGui::Selectable(devices_[i].label.c_str(),
                                          selected)) {
                        device_idx_ = i;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("Refresh")) {
                refreshDevices();
            }

            // Device info panel — shown when a real device is selected.
            if (device_idx_ >= 0 &&
                device_idx_ < static_cast<int>(devices_.size()) &&
                !devices_[device_idx_].info.description.empty()) {
                const auto& info = devices_[device_idx_].info;
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextDisabled("Device Info");
                ImGui::Columns(2, "devinfo", false);
                ImGui::SetColumnWidth(0, 120.0f);
                if (!info.manufacturer.empty()) {
                    ImGui::TextDisabled("Manufacturer"); ImGui::NextColumn();
                    ImGui::Text("%s", info.manufacturer.c_str()); ImGui::NextColumn();
                }
                ImGui::TextDisabled("Product");      ImGui::NextColumn();
                ImGui::Text("%s", info.description.c_str()); ImGui::NextColumn();
                ImGui::TextDisabled("Serial");       ImGui::NextColumn();
                ImGui::Text("%s", info.serial.c_str()); ImGui::NextColumn();
                ImGui::TextDisabled("VID:PID");      ImGui::NextColumn();
                ImGui::Text("%04X:%04X", info.vendor_id, info.product_id);
                ImGui::Columns(1);
                ImGui::Separator();
            }
        }

        // Configuration
        if (ImGui::CollapsingHeader("Configuration",
                                     ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::InputText("Vendor ID (hex)", vid_buf_, sizeof(vid_buf_),
                             ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::InputText("Product ID (hex)", pid_buf_, sizeof(pid_buf_),
                             ImGuiInputTextFlags_CharsHexadecimal);

            const char* interfaces[] = {
                "Interface A (ADBUS)",
                "Interface B (BDBUS)"
            };
            ImGui::Combo("Interface", &interface_idx_, interfaces, 2);

            ImGui::InputInt("TCK Clock (Hz)", &clock_hz_, 1000000, 5000000);
            if (clock_hz_ < 100) clock_hz_ = 100;
            if (clock_hz_ > 30000000) clock_hz_ = 30000000;
        }

        ImGui::Separator();

        if (ImGui::Button("OK", ImVec2(120, 0))) {
            // Parse config
            config_.vendor_id = static_cast<uint16_t>(
                strtoul(vid_buf_, nullptr, 16));
            config_.product_id = static_cast<uint16_t>(
                strtoul(pid_buf_, nullptr, 16));
            if (config_.vendor_id == 0) config_.vendor_id = 0x0403;
            if (config_.product_id == 0) config_.product_id = 0x6010;
            config_.interface_channel = interface_idx_;
            config_.clock_freq_hz = static_cast<uint32_t>(clock_hz_);

            if (device_idx_ >= 0 &&
                device_idx_ < static_cast<int>(devices_.size())) {
                config_.serial = devices_[device_idx_].serial;
            }

            accepted_ = true;
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

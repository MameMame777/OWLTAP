#include "hardware_context.h"

#include <stdexcept>
#include <string>

namespace jtag::hardware {

HardwareContext::HardwareContext(gui::AppConfig cfg)
    : config_(std::move(cfg))
    , ftdi_()
    , tap_(ftdi_)
    , chain_(tap_) {}

HardwareContext::~HardwareContext() {
    close();
}

std::string HardwareContext::open() {
    if (ftdi_.isOpen()) return {};

    if (!ftdi_.open(config_.vendor_id,
                    config_.product_id,
                    config_.serial,
                    static_cast<FtdiInterface>(config_.interface_channel))) {
        return ftdi_.lastError();
    }

    if (!ftdi_.initMpsse(config_.clock_freq_hz)) {
        std::string err = ftdi_.lastError();
        ftdi_.close();
        return err;
    }

    return {};
}

void HardwareContext::close() {
    // Release lazily-constructed device objects before closing FTDI.
    resetDeviceObjects();
    ftdi_.close();
}

void HardwareContext::resetDeviceObjects() {
    scanners_.clear();
    pin_drivers_.clear();
    pl_configs_.clear();
}

Scanner& HardwareContext::scanner(int device_index) {
    auto it = scanners_.find(device_index);
    if (it == scanners_.end()) {
        const auto& devs = chain_.devices();
        if (device_index < 0 || device_index >= static_cast<int>(devs.size())) {
            throw std::out_of_range("scanner: device_index out of range");
        }
        auto [inserted, ok] = scanners_.emplace(
            device_index,
            std::make_unique<Scanner>(chain_, device_index));
        it = inserted;
    }
    return *it->second;
}

PinDriver& HardwareContext::pinDriver(int device_index) {
    auto it = pin_drivers_.find(device_index);
    if (it == pin_drivers_.end()) {
        const auto& devs = chain_.devices();
        if (device_index < 0 || device_index >= static_cast<int>(devs.size())) {
            throw std::out_of_range("pinDriver: device_index out of range");
        }
        auto [inserted, ok] = pin_drivers_.emplace(
            device_index,
            std::make_unique<PinDriver>(chain_, device_index));
        it = inserted;
    }
    return *it->second;
}

PlConfig& HardwareContext::plConfig(int device_index) {
    auto it = pl_configs_.find(device_index);
    if (it == pl_configs_.end()) {
        const auto& devs = chain_.devices();
        if (device_index < 0 || device_index >= static_cast<int>(devs.size())) {
            throw std::out_of_range("plConfig: device_index out of range");
        }
        auto [inserted, ok] = pl_configs_.emplace(
            device_index,
            std::make_unique<PlConfig>(chain_, device_index));
        it = inserted;
    }
    return *it->second;
}

}  // namespace jtag::hardware

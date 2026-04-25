#pragma once

#include <map>
#include <memory>
#include <string>

#include "src/boundary_scan/pin_driver.h"
#include "src/boundary_scan/scanner.h"
#include "src/config/pl_config.h"
#include "src/ftdi/ftdi_device.h"
#include "src/gui/app_config.h"
#include "src/jtag/jtag_chain.h"
#include "src/jtag/tap_controller.h"

namespace jtag::hardware {

// HardwareContext owns the full FTDI + JTAG stack and provides lazy-constructed
// per-device accessor objects (Scanner, PinDriver, PlConfig).
//
// This object must live on the heap (via unique_ptr) because TapController and
// JtagChain hold references into FtdiDevice / TapController members.  It is
// non-copyable and non-movable.
//
// All methods (except open/close/isOpen) must be called from the hardware
// worker thread only.
class HardwareContext {
public:
    explicit HardwareContext(gui::AppConfig cfg);
    ~HardwareContext();

    // Non-copyable, non-movable.
    HardwareContext(const HardwareContext&)            = delete;
    HardwareContext& operator=(const HardwareContext&) = delete;
    HardwareContext(HardwareContext&&)                 = delete;
    HardwareContext& operator=(HardwareContext&&)      = delete;

    // Open the FTDI device and initialise MPSSE.
    // Returns empty string on success, or a human-readable error on failure.
    std::string open();

    // Close the FTDI device and release all lazily-constructed objects.
    void close();

    bool isOpen() const { return ftdi_.isOpen(); }

    // Raw hardware accessors (worker thread only).
    FtdiDevice&   ftdi()  { return ftdi_; }
    TapController& tap()  { return tap_; }
    JtagChain&    chain() { return chain_; }

    // Config accessor (any thread: config is immutable after construction).
    const gui::AppConfig& config() const { return config_; }

    // Lazy-constructed per-device accessories.
    // Throws std::out_of_range if device_index is not present in the chain.
    Scanner&   scanner(int device_index);
    PinDriver& pinDriver(int device_index);
    PlConfig&  plConfig(int device_index);

    // Invalidate cached per-device objects (call after detectDevices()).
    void resetDeviceObjects();

private:
    gui::AppConfig                              config_;
    FtdiDevice                                  ftdi_;
    TapController                               tap_;
    JtagChain                                   chain_;
    std::map<int, std::unique_ptr<Scanner>>     scanners_;
    std::map<int, std::unique_ptr<PinDriver>>   pin_drivers_;
    std::map<int, std::unique_ptr<PlConfig>>    pl_configs_;
};

}  // namespace jtag::hardware

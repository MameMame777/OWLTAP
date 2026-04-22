#pragma once

#include <string>
#include <vector>

#include "src/boundary_scan/interconnect_test.h"
#include "src/boundary_scan/pin_driver.h"
#include "src/boundary_scan/scanner.h"

namespace jtag::gui {

/// Panel for loading and running board-level interconnect (.ict) tests.
class InterconnectPanel {
public:
    /// Draw the panel.
    /// scanners/drivers indexed by device_index in the JTAG chain.
    static void draw(const std::vector<jtag::Scanner*>& scanners,
                     const std::vector<jtag::PinDriver*>& drivers,
                     bool connected);

    static void setVisible(bool visible);
    static bool isVisible();

private:
    static bool visible_;
    static std::string ict_path_;
    static std::vector<jtag::NetDef> nets_;
    static std::string load_error_;
    static jtag::InterconnectResult last_result_;
    static bool has_result_;
    static std::string run_error_;
};

} // namespace jtag::gui

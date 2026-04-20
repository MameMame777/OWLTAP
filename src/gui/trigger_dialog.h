#pragma once

#include <string>
#include <vector>

#include "src/boundary_scan/scanner.h"
#include "src/capture/trigger.h"

namespace jtag::gui {

/// ImGui popup modal for configuring trigger conditions.
class TriggerDialog {
public:
    /// Draw the dialog popup. Set *p_open to false to close.
    static void draw(bool* p_open);

    /// Bind to a scanner and trigger engine. Must call before draw().
    static void bind(jtag::Scanner* scanner, jtag::TriggerEngine* trigger);

private:
    static jtag::Scanner* scanner_;
    static jtag::TriggerEngine* trigger_;

    static int mode_idx_;
    static int pin_idx_;
    static int type_idx_;
    static int edge_idx_;
    static int level_idx_;
    static float pretrigger_;

    static std::vector<std::string> pin_names_;
    static std::vector<jtag::TriggerCondition> conditions_;
};

} // namespace jtag::gui

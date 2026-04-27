#pragma once

#include <string>

namespace jtag::gui {

struct AppConfig;

class IlaGeneratorDialog {
public:
    static void draw(bool* p_open, AppConfig* config);
    static bool consumeStatusMessage(std::string& message);
};

} // namespace jtag::gui

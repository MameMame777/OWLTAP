#pragma once

#include <string>

namespace jtag::gui {

/// Dockable panel showing recent application debug messages.
class DebugLogPanel {
public:
    /// Draw the debug log panel.
    static void draw(const std::string& latest_status);

    /// Append one message, splitting multi-line text into separate entries.
    static void append(const std::string& message);

    /// Clear all log entries.
    static void clear();

private:
    static std::string formatEntry(const std::string& message_line);
};

} // namespace jtag::gui
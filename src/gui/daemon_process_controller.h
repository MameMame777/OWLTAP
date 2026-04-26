#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jtag::gui {

enum class DaemonState {
    kOff,       ///< Not running.
    kStarting,  ///< Process spawned; awaiting {"event":"ready"} on stdout.
    kRunning,   ///< Ready event received; MCP endpoint is live.
    kStopping,  ///< Stop requested; waiting for process to exit.
    kError,     ///< Process exited unexpectedly or sent an error event.
};

struct DaemonStatus {
    DaemonState state         = DaemonState::kOff;
    uint16_t    mcp_port      = 0;
    uint16_t    gui_port      = 0;  ///< 0 if GUI RPC not enabled or not yet known.
    std::string error_message;
};

/// Manages the lifecycle of a jtag_daemon child process.
///
/// Spawns the process, captures stdout JSON events for state tracking,
/// and pipes all output to an internal log buffer.
/// Call drainLog() from the GUI main thread each frame to forward buffered
/// lines to DebugLogPanel (which is not thread-safe).
class DaemonProcessController {
public:
    DaemonProcessController();
    ~DaemonProcessController();

    DaemonProcessController(const DaemonProcessController&) = delete;
    DaemonProcessController& operator=(const DaemonProcessController&) = delete;

    /// Start the daemon. Returns false if already running or start fails.
    /// @param exe_path    Full path to jtag_daemon.exe.
    /// @param config_path cfg.json path forwarded as --config.
    /// @param mcp_port    TCP port; 0 = OS-chosen dynamic port.
    bool start(const std::string& exe_path,
               const std::string& config_path,
               uint16_t mcp_port);

    /// Gracefully stop: sends CTRL_BREAK (Windows) or SIGTERM (POSIX),
    /// waits up to 3 s for clean exit, then force-terminates.
    /// Blocks until the process and reader threads have exited.
    void stop();

    /// Thread-safe snapshot of current daemon state.
    DaemonStatus status() const;

    /// Move pending log lines into 'out'. Call from GUI main thread each frame.
    /// Thread-safe; clears the internal buffer.
    void drainLog(std::vector<std::string>& out);

private:
    void readerThreadFn();
    void stderrThreadFn();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace jtag::gui

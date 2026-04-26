#include "daemon_process_controller.h"

// Include Windows headers before project headers to control macro scope.
// windows.h defines IN and OUT as empty SAL macros; we undef them immediately
// so they do not corrupt project enum values (e.g. bsdl PinDirection::IN).
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jtag::gui {

// ── Impl ─────────────────────────────────────────────────────────────────────

struct DaemonProcessController::Impl {
    mutable std::mutex    mu;
    DaemonStatus          status;
    std::vector<std::string> pending_log;

    std::thread reader_thread;
    std::thread stderr_thread;

#ifdef _WIN32
    HANDLE process_handle{INVALID_HANDLE_VALUE};
    HANDLE stdout_rd{INVALID_HANDLE_VALUE};
    HANDLE stderr_rd{INVALID_HANDLE_VALUE};
#else
    pid_t child_pid{-1};
    int   stdout_rd{-1};
    int   stderr_rd{-1};
#endif

    // ── Thread-safe helpers ────────────────────────────────────────────────

    void appendLog(const std::string& msg) {
        std::lock_guard<std::mutex> lk(mu);
        pending_log.push_back(msg);
    }

    void setReady(uint16_t mcp_p, uint16_t gui_p) {
        std::lock_guard<std::mutex> lk(mu);
        status.state    = DaemonState::kRunning;
        status.mcp_port = mcp_p;
        status.gui_port = gui_p;
    }

    void setError(const std::string& msg) {
        std::lock_guard<std::mutex> lk(mu);
        // Don't overwrite kStopping/kOff set by stop().
        if (status.state == DaemonState::kStarting ||
            status.state == DaemonState::kRunning) {
            status.state         = DaemonState::kError;
            status.error_message = msg;
        }
    }

    void setOff() {
        std::lock_guard<std::mutex> lk(mu);
        if (status.state == DaemonState::kStarting ||
            status.state == DaemonState::kRunning) {
            status.state    = DaemonState::kOff;
            status.mcp_port = 0;
        }
    }

    // ── Platform I/O ──────────────────────────────────────────────────────

#ifdef _WIN32
    /// Read one line from a Windows pipe handle.
    /// Accumulates bytes in 'buf'; returns false on EOF or error.
    static bool readLine(HANDLE h, std::string& buf, std::string& out) {
        while (true) {
            auto nl = buf.find('\n');
            if (nl != std::string::npos) {
                out = buf.substr(0, nl);
                if (!out.empty() && out.back() == '\r') out.pop_back();
                buf.erase(0, nl + 1);
                return true;
            }
            char tmp[512];
            DWORD n = 0;
            const BOOL ok = ReadFile(h, tmp, sizeof(tmp), &n, nullptr);
            if (!ok || n == 0) {
                // Flush remaining partial line on EOF.
                if (!buf.empty()) {
                    out = buf;
                    if (!out.empty() && out.back() == '\r') out.pop_back();
                    buf.clear();
                    return !out.empty();
                }
                return false;
            }
            buf.append(tmp, n);
        }
    }

    void closeHandles() {
        auto close_h = [](HANDLE& h) {
            if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
        };
        close_h(process_handle);
        close_h(stdout_rd);
        close_h(stderr_rd);
    }
#else
    static bool readLine(int fd, std::string& buf, std::string& out) {
        while (true) {
            auto nl = buf.find('\n');
            if (nl != std::string::npos) {
                out = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                return true;
            }
            char tmp[512];
            const ssize_t n = read(fd, tmp, sizeof(tmp));
            if (n <= 0) {
                if (!buf.empty()) {
                    out = buf;
                    buf.clear();
                    return !out.empty();
                }
                return false;
            }
            buf.append(tmp, static_cast<size_t>(n));
        }
    }

    void closeHandles() {
        auto close_fd = [](int& fd) {
            if (fd != -1) { close(fd); fd = -1; }
        };
        close_fd(stdout_rd);
        close_fd(stderr_rd);
        child_pid = -1;
    }
#endif
};

// ── Constructor / Destructor ─────────────────────────────────────────────────

DaemonProcessController::DaemonProcessController()
    : impl_(std::make_unique<Impl>()) {}

DaemonProcessController::~DaemonProcessController() {
    stop();
}

// ── start() ──────────────────────────────────────────────────────────────────

bool DaemonProcessController::start(const std::string& exe_path,
                                     const std::string& config_path,
                                     uint16_t mcp_port) {
    // Step 1: check current state.
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        const auto s = impl_->status.state;
        if (s != DaemonState::kOff && s != DaemonState::kError) return false;
        impl_->status.error_message.clear();
    }

#ifdef _WIN32
    // Step 2: create stdout and stderr pipes.
    // Write ends must be inheritable; read ends must not.
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, /*inherit=*/TRUE};

    HANDLE stdout_rd = INVALID_HANDLE_VALUE;
    HANDLE stdout_wr = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&stdout_rd, &stdout_wr, &sa, 0)) return false;
    SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE stderr_rd = INVALID_HANDLE_VALUE;
    HANDLE stderr_wr = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&stderr_rd, &stderr_wr, &sa, 0)) {
        CloseHandle(stdout_rd); CloseHandle(stdout_wr);
        return false;
    }
    SetHandleInformation(stderr_rd, HANDLE_FLAG_INHERIT, 0);

    // Step 3: build command line and start process.
    std::string cmdline = "\"" + exe_path + "\" --no-gui-port"
                          " --config \"" + config_path + "\""
                          " --mcp-port " + std::to_string(mcp_port);

    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput  = stdout_wr;
    si.hStdError   = stderr_wr;

    PROCESS_INFORMATION pi{};
    // CREATE_NEW_PROCESS_GROUP allows sending CTRL_BREAK to the daemon only.
    // CREATE_NO_WINDOW prevents an unwanted console window from appearing.
    BOOL ok = CreateProcessA(
        nullptr,
        cmdline.data(),    // writable buffer required
        nullptr, nullptr,
        /*bInheritHandles=*/TRUE,
        CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi);

    // Close write ends in parent — required for ReadFile to return EOF when
    // the child exits.
    CloseHandle(stdout_wr);
    CloseHandle(stderr_wr);

    if (!ok) {
        CloseHandle(stdout_rd);
        CloseHandle(stderr_rd);
        return false;
    }

    CloseHandle(pi.hThread);  // Not needed.

    // Step 4: atomically store handles and transition to kStarting.
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->process_handle = pi.hProcess;
        impl_->stdout_rd      = stdout_rd;
        impl_->stderr_rd      = stderr_rd;
        impl_->status.state   = DaemonState::kStarting;
        impl_->status.mcp_port = 0;
        impl_->pending_log.push_back("[daemon] Starting: " + cmdline);
    }

#else
    // POSIX: pipe + fork/exec.
    int out_fds[2], err_fds[2];
    if (pipe(out_fds) != 0) return false;
    if (pipe(err_fds) != 0) { close(out_fds[0]); close(out_fds[1]); return false; }

    const pid_t pid = fork();
    if (pid < 0) {
        close(out_fds[0]); close(out_fds[1]);
        close(err_fds[0]); close(err_fds[1]);
        return false;
    }
    if (pid == 0) {
        // Child: redirect stdout/stderr and exec daemon.
        dup2(out_fds[1], STDOUT_FILENO);
        dup2(err_fds[1], STDERR_FILENO);
        close(out_fds[0]); close(out_fds[1]);
        close(err_fds[0]); close(err_fds[1]);
        const std::string port_str = std::to_string(mcp_port);
        execl(exe_path.c_str(), exe_path.c_str(),
              "--no-gui-port",
              "--config", config_path.c_str(),
              "--mcp-port", port_str.c_str(),
              nullptr);
        _exit(127);  // exec failed
    }

    // Parent: close write ends.
    close(out_fds[1]);
    close(err_fds[1]);

    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->child_pid      = pid;
        impl_->stdout_rd      = out_fds[0];
        impl_->stderr_rd      = err_fds[0];
        impl_->status.state   = DaemonState::kStarting;
        impl_->status.mcp_port = 0;
        impl_->pending_log.push_back("[daemon] Starting: " + exe_path);
    }
#endif

    // Step 5: spawn reader threads (no mutex held).
    impl_->reader_thread = std::thread([this] { readerThreadFn(); });
    impl_->stderr_thread = std::thread([this] { stderrThreadFn(); });
    return true;
}

// ── stop() ───────────────────────────────────────────────────────────────────

void DaemonProcessController::stop() {
#ifdef _WIN32
    HANDLE proc;
    DWORD  pid;
#else
    pid_t  child_pid;
#endif
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        const auto s = impl_->status.state;
        if (s == DaemonState::kOff || s == DaemonState::kStopping) return;
        impl_->status.state = DaemonState::kStopping;
#ifdef _WIN32
        proc = impl_->process_handle;
        pid  = (proc != INVALID_HANDLE_VALUE) ? GetProcessId(proc) : 0;
#else
        child_pid = impl_->child_pid;
#endif
    }

#ifdef _WIN32
    if (proc != INVALID_HANDLE_VALUE && pid != 0) {
        // Graceful: CTRL_BREAK to the daemon's process group.
        if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid)) {
            TerminateProcess(proc, 1);
        } else {
            // Allow up to 3 s for graceful shutdown.
            if (WaitForSingleObject(proc, 3000) == WAIT_TIMEOUT) {
                TerminateProcess(proc, 1);
            }
        }
    }
#else
    if (child_pid > 0) {
        kill(child_pid, SIGTERM);
        // Reader threads will exit on pipe EOF when the child exits.
    }
#endif

    // Join reader threads — they exit when the pipes reach EOF after process exit.
    if (impl_->reader_thread.joinable()) impl_->reader_thread.join();
    if (impl_->stderr_thread.joinable()) impl_->stderr_thread.join();

    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->closeHandles();
        impl_->status.state   = DaemonState::kOff;
        impl_->status.mcp_port = 0;
    }
}

// ── status() / drainLog() ────────────────────────────────────────────────────

DaemonStatus DaemonProcessController::status() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->status;
}

void DaemonProcessController::drainLog(std::vector<std::string>& out) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->pending_log.empty()) {
        out.insert(out.end(),
                   std::make_move_iterator(impl_->pending_log.begin()),
                   std::make_move_iterator(impl_->pending_log.end()));
        impl_->pending_log.clear();
    }
}

// ── Reader threads ───────────────────────────────────────────────────────────

void DaemonProcessController::readerThreadFn() {
    std::string buf, line;
    while (Impl::readLine(impl_->stdout_rd, buf, line)) {
        impl_->appendLog("[daemon] " + line);
        // Parse JSON events from daemon stdout.
        try {
            auto j = nlohmann::json::parse(line);
            if (!j.contains("event")) continue;
            const std::string evt = j.at("event").get<std::string>();
            if (evt == "ready") {
                const uint16_t mcp_p = j.value("mcp_port", uint16_t(0));
                const uint16_t gui_p = j.value("gui_port", uint16_t(0));
                impl_->setReady(mcp_p, gui_p);
            } else if (evt == "error") {
                impl_->setError(j.value("message", "daemon error"));
            } else if (evt == "stopped") {
                impl_->setOff();
            }
        } catch (...) {
            // Not a JSON line (e.g. spurious text) — ignore.
        }
    }
    // Pipe EOF: the process has exited.
    impl_->setError("daemon exited unexpectedly");
}

void DaemonProcessController::stderrThreadFn() {
    std::string buf, line;
    while (Impl::readLine(impl_->stderr_rd, buf, line)) {
        impl_->appendLog("[daemon stderr] " + line);
    }
}

}  // namespace jtag::gui

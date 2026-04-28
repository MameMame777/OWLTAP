#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace jtag::gui {

/// Synchronous JSON-RPC 2.0 TCP client for the daemon GUI endpoint.
///
/// All public methods are thread-safe. The `call()` helpers block the caller
/// until a response arrives or the timeout expires.
///
/// Usage:
///   GuiDaemonClient c;
///   std::string err = c.connect(gui_port);
///   if (!err.empty()) { /* handle */ }
///   auto devs = c.detectDevices();
///   c.disconnect();
class GuiDaemonClient {
public:
    GuiDaemonClient();
    ~GuiDaemonClient();

    // Non-copyable.
    GuiDaemonClient(const GuiDaemonClient&)            = delete;
    GuiDaemonClient& operator=(const GuiDaemonClient&) = delete;

    /// Connect to 127.0.0.1:port. Returns "" on success, error string on fail.
    std::string connect(uint16_t port, int timeout_ms = 3000);

    /// Disconnect and stop reader thread.
    void disconnect();

    bool isConnected() const;

    // -----------------------------------------------------------------------
    // RPC methods — throw std::runtime_error on network error or RPC error.
    // -----------------------------------------------------------------------
    nlohmann::json daemonStatus(int timeout_ms = 2000);
    nlohmann::json detectDevices(int timeout_ms = 5000);
    nlohmann::json loadBsdl(int device_index, const std::string& bsdl_path,
                             int timeout_ms = 5000);
    nlohmann::json listPins(int device_index, int timeout_ms = 5000);
    nlohmann::json readPin(int device_index, const std::string& pin_name,
                           int timeout_ms = 5000);

    // -----------------------------------------------------------------------
    // Phase 4: capture and ILA RPC methods.
    // -----------------------------------------------------------------------

    // Start an async capture job.  Returns {"job_id": "..."}.
    // pin_filter: only decode/serialise these pins (empty = all pins).
    nlohmann::json captureStart(int device_index, int buffer_depth,
                                int interval_us,
                                const std::string& trigger_mode,
                                const std::vector<std::string>& pin_filter = {},
                                int timeout_ms = 5000);

    // Cancel a running capture job.  Returns {"cancelled": true|false}.
    nlohmann::json captureStop(const std::string& job_id,
                               int timeout_ms = 3000);

    // Poll a capture job; result contains samples when job is complete.
    nlohmann::json captureGetSamples(const std::string& job_id,
                                     int timeout_ms = 3000);

    // Generic job poll (works for any async hardware job).
    nlohmann::json jobPoll(const std::string& job_id, int timeout_ms = 2000);

    // Read ILA status registers from the daemon.
    // use_bscane=true → BscaneIlaTapBackend (Zynq PL Config TAP via USERn).
    // user_chain: BSCANE2 chain index 1..4 (USER1..USER4, default 1).
    nlohmann::json ilaStatus(int device_index, bool use_bscane = false,
                             int user_chain = 1, int timeout_ms = 5000);

    // -----------------------------------------------------------------------
    // Phase 5+: ILA high-level daemon operations.
    // -----------------------------------------------------------------------

    /// Probe ILA and return caps + optional signal_defs array.
    nlohmann::json ilaProbe(int device_index, bool use_bscane = false,
                            int user_chain = 1, int timeout_ms = 5000);

    /// Configure trigger registers and arm the ILA.
    nlohmann::json ilaArm(int device_index, bool use_bscane,
                          int user_chain,
                          uint32_t mask, uint32_t value,
                          uint32_t rise_mask, uint32_t fall_mask,
                          uint32_t mask2, uint32_t val2,
                          bool or_mode, int pre_samples,
                          int timeout_ms = 5000);

    /// Stop (disarm) the ILA.
    nlohmann::json ilaStop(int device_index, bool use_bscane = false,
                           int user_chain = 1, int timeout_ms = 3000);

    /// Force an immediate trigger.
    nlohmann::json ilaForce(int device_index, bool use_bscane = false,
                            int user_chain = 1, int timeout_ms = 3000);

    /// Reset the capture buffer.
    nlohmann::json ilaReset(int device_index, bool use_bscane = false,
                            int user_chain = 1, int timeout_ms = 3000);

    /// Read sample data.  Returns {ok, samples:[...], data_w:N}.
    nlohmann::json ilaReadSamples(int device_index, bool use_bscane = false,
                                  int user_chain = 1, int timeout_ms = 10000);

    // -----------------------------------------------------------------------
    // Phase 5: BSR sample-all and script execution.
    // -----------------------------------------------------------------------

    // Sample all observable pins via JTAG SAMPLE instruction.
    // Returns {"device_index": N, "pins": {...}, "raw_bsr": [byte0, ...]}
    nlohmann::json sampleBsr(int device_index, int timeout_ms = 3000);

    // Execute a script on the daemon.  Returns {"ok": bool, "output": "..."}.
    nlohmann::json runScript(int device_index, const std::string& script_text,
                             int timeout_ms = 30000);

    /// Program the PL from a .bit/.bin file on the daemon host.
    /// Returns {"ok": true} on success; throws std::runtime_error on failure.
    nlohmann::json programPl(int device_index, const std::string& bitstream_path,
                             int timeout_ms = 120000);

private:
    /// Send a JSON-RPC request and block until the matching response arrives.
    nlohmann::json call(const std::string& method,
                        const nlohmann::json& params,
                        int timeout_ms);

    /// Background thread that feeds received bytes into the framing parser.
    void readerLoop();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace jtag::gui

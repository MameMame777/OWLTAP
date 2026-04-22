#include "interconnect_test.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace jtag {
namespace {

std::string trimStr(const std::string& s) {
    size_t a = 0;
    while (a < s.size() &&
           std::isspace(static_cast<unsigned char>(s[a]))) {
        a++;
    }
    size_t b = s.size();
    while (b > a &&
           std::isspace(static_cast<unsigned char>(s[b - 1]))) {
        b--;
    }
    return s.substr(a, b - a);
}

std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

// Parse "devIndex:pin_name" → DevPin.  Returns false on parse error.
bool parseDevPin(const std::string& token, DevPin& dp, std::string& error) {
    const size_t colon = token.find(':');
    if (colon == std::string::npos) {
        error = "expected 'dev:pin' but got '" + token + "'";
        return false;
    }
    const std::string idx_str = token.substr(0, colon);
    const std::string pin_str = token.substr(colon + 1);
    if (idx_str.empty() || pin_str.empty()) {
        error = "malformed 'dev:pin' token '" + token + "'";
        return false;
    }
    try {
        dp.device_index = std::stoi(idx_str);
    } catch (...) {
        error = "invalid device index '" + idx_str + "'";
        return false;
    }
    dp.pin = toUpper(pin_str);
    return true;
}

} // namespace

std::vector<NetDef> parseIctFile(const std::string& path, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "cannot open file: " + path;
        return {};
    }

    std::vector<NetDef> nets;
    std::string line;
    int line_no = 0;
    while (std::getline(file, line)) {
        line_no++;
        // strip comment
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }
        line = trimStr(line);
        if (line.empty()) continue;

        std::istringstream ss(line);
        std::vector<std::string> tokens;
        std::string tok;
        while (ss >> tok) {
            tokens.push_back(tok);
        }

        // Minimum: name driver receiver
        if (tokens.size() < 3) {
            error = "line " + std::to_string(line_no) +
                    ": need at least: name driver receiver";
            return {};
        }

        NetDef nd;
        nd.name = tokens[0];
        std::string parse_err;
        if (!parseDevPin(tokens[1], nd.driver, parse_err)) {
            error = "line " + std::to_string(line_no) + ": " + parse_err;
            return {};
        }
        for (size_t i = 2; i < tokens.size(); i++) {
            DevPin dp;
            if (!parseDevPin(tokens[i], dp, parse_err)) {
                error = "line " + std::to_string(line_no) + ": " + parse_err;
                return {};
            }
            nd.receivers.push_back(dp);
        }
        nets.push_back(std::move(nd));
    }
    return nets;
}

InterconnectResult runInterconnectTest(
    const std::vector<NetDef>& nets,
    const std::vector<Scanner*>& scanners,
    const std::vector<PinDriver*>& drivers) {
    InterconnectResult result;

    for (const auto& net : nets) {
        NetResult nr;
        nr.name = net.name;

        for (int driven_val : {0, 1}) {
            NetTestStep step;
            step.driven_value = driven_val;
            step.pass = true;

            // Determine which scanner to use for observation.
            // We need to sample all unique receiver devices.
            // Build a map device_index → ScanResult.
            std::map<int, ScanResult> samples;

            // Drive the output pin
            const int drv_idx = net.driver.device_index;
            bool drive_ok = false;
            if (drv_idx < static_cast<int>(drivers.size()) &&
                drivers[drv_idx] != nullptr) {
                PinDriver& pd = *drivers[drv_idx];
                if (pd.extestAllowed()) {
                    pd.setPin(net.driver.pin, driven_val);
                    drive_ok = pd.applyOutputs();
                }
            }

            if (!drive_ok) {
                // Cannot drive — mark all receivers UNKNOWN and fail
                for (size_t r = 0; r < net.receivers.size(); r++) {
                    step.observed.push_back(PinState::UNKNOWN);
                }
                step.pass = false;
                nr.steps.push_back(step);
                nr.pass = false;
                continue;
            }

            // Sample each receiver device once
            for (const auto& recv : net.receivers) {
                const int rx_idx = recv.device_index;
                if (samples.find(rx_idx) == samples.end()) {
                    if (rx_idx < static_cast<int>(scanners.size()) &&
                        scanners[rx_idx] != nullptr) {
                        samples[rx_idx] = scanners[rx_idx]->sample();
                    }
                }
            }

            // Check each receiver
            const PinState expected =
                (driven_val == 0) ? PinState::LOW : PinState::HIGH;
            for (const auto& recv : net.receivers) {
                const int rx_idx = recv.device_index;
                PinState obs = PinState::UNKNOWN;
                auto it = samples.find(rx_idx);
                if (it != samples.end()) {
                    obs = it->second.getPin(recv.pin);
                }
                step.observed.push_back(obs);
                if (obs != expected) {
                    step.pass = false;
                    nr.pass = false;
                }
            }

            nr.steps.push_back(step);
        }

        if (nr.pass) {
            result.pass_count++;
        } else {
            result.fail_count++;
        }
        result.nets.push_back(std::move(nr));
    }
    return result;
}

std::string formatInterconnectReport(const InterconnectResult& result,
                                     const std::vector<NetDef>& nets) {
    std::ostringstream ss;
    ss << "=== Interconnect Test Report ===\n";
    ss << "Total: " << result.nets.size()
       << "  PASS: " << result.pass_count
       << "  FAIL: " << result.fail_count << "\n\n";

    for (size_t i = 0; i < result.nets.size() && i < nets.size(); i++) {
        const auto& nr = result.nets[i];
        const auto& nd = nets[i];
        ss << (nr.pass ? "[PASS] " : "[FAIL] ") << nr.name << "\n";
        for (const auto& step : nr.steps) {
            ss << "  drive=" << step.driven_value << " : ";
            for (size_t r = 0; r < step.observed.size() && r < nd.receivers.size(); r++) {
                const char* state = "?";
                switch (step.observed[r]) {
                    case PinState::LOW: state = "LOW"; break;
                    case PinState::HIGH: state = "HIGH"; break;
                    case PinState::UNKNOWN: state = "UNK"; break;
                }
                ss << nd.receivers[r].pin << "=" << state;
                if (r + 1 < nd.receivers.size()) ss << " ";
            }
            ss << (step.pass ? " OK" : " FAIL") << "\n";
        }
    }
    return ss.str();
}

} // namespace jtag

#include "script_engine.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace jtag::script {
namespace {

std::string trim(const std::string& input) {
    size_t start = 0;
    while (start < input.size() &&
           std::isspace(static_cast<unsigned char>(input[start]))) {
        start++;
    }

    size_t end = input.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        end--;
    }

    return input.substr(start, end - start);
}

std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

std::string toUpperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::toupper(ch));
                   });
    return value;
}

std::string stripComment(const std::string& line) {
    const size_t hash_pos = line.find('#');
    if (hash_pos != std::string::npos) {
        return trim(line.substr(0, hash_pos));
    }
    if (line.rfind("//", 0) == 0) {
        return {};
    }
    return trim(line);
}

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream stream(line);
    std::string token;
    while (stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

void appendOutput(std::string& output, const std::string& line) {
    output += line;
    output += '\n';
}

ScriptResult fail(std::string output, int line_number,
                  const std::string& message) {
    appendOutput(output, "line " + std::to_string(line_number) + ": ERROR: " +
                             message);
    return {false, line_number, std::move(output)};
}

bool parseDriveValue(const std::string& token, int& value, bool& high_z,
                     std::string& error) {
    const std::string lowered = toLowerAscii(token);
    if (lowered == "0" || lowered == "low") {
        value = 0;
        high_z = false;
        return true;
    }
    if (lowered == "1" || lowered == "high") {
        value = 1;
        high_z = false;
        return true;
    }
    if (lowered == "z" || lowered == "highz" || lowered == "high-z") {
        value = -1;
        high_z = true;
        return true;
    }
    error = "unsupported drive value '" + token + "'";
    return false;
}

bool parseExpectedValue(const std::string& token, jtag::PinState& value,
                       std::string& error) {
    const std::string lowered = toLowerAscii(token);
    if (lowered == "0" || lowered == "low") {
        value = jtag::PinState::LOW;
        return true;
    }
    if (lowered == "1" || lowered == "high") {
        value = jtag::PinState::HIGH;
        return true;
    }
    if (lowered == "unknown" || lowered == "?") {
        value = jtag::PinState::UNKNOWN;
        return true;
    }
    error = "unsupported expected value '" + token + "'";
    return false;
}

const char* pinStateName(jtag::PinState state) {
    switch (state) {
        case jtag::PinState::LOW:
            return "LOW";
        case jtag::PinState::HIGH:
            return "HIGH";
        case jtag::PinState::UNKNOWN:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool parseMilliseconds(const std::string& token, int& milliseconds,
                       std::string& error) {
    try {
        size_t consumed = 0;
        const int parsed = std::stoi(token, &consumed);
        if (consumed != token.size()) {
            error = "invalid integer '" + token + "'";
            return false;
        }
        if (parsed < 0) {
            error = "sleep duration must be >= 0";
            return false;
        }
        milliseconds = parsed;
        return true;
    } catch (...) {
        error = "invalid integer '" + token + "'";
        return false;
    }
}

} // namespace

ScriptResult ScriptEngine::run(const std::string& script_text,
                               ScriptHost& host) {
    std::istringstream stream(script_text);
    std::string line;
    std::string output;
    int line_number = 0;
    int total_expects = 0;
    int failed_expects = 0;

    while (std::getline(stream, line)) {
        line_number++;
        const std::string command_line = stripComment(line);
        if (command_line.empty()) {
            continue;
        }

        const auto tokens = tokenize(command_line);
        if (tokens.empty()) {
            continue;
        }

        const std::string command = toLowerAscii(tokens[0]);

        if (command == "sample") {
            if (tokens.size() != 1) {
                return fail(std::move(output), line_number,
                            "sample takes no arguments");
            }

            jtag::ScanResult result;
            std::string error;
            if (!host.sample(result, error)) {
                return fail(std::move(output), line_number, error);
            }
            appendOutput(output, "line " + std::to_string(line_number) +
                                     ": sample OK");
            continue;
        }

        if (command == "read") {
            if (tokens.size() != 2) {
                return fail(std::move(output), line_number,
                            "read requires: read <pin>");
            }

            const std::string pin_name = toUpperAscii(tokens[1]);
            jtag::ScanResult result;
            std::string error;
            if (!host.sample(result, error)) {
                return fail(std::move(output), line_number, error);
            }
            appendOutput(output,
                         "line " + std::to_string(line_number) + ": " +
                             pin_name + " = " + pinStateName(result.getPin(pin_name)));
            continue;
        }

        if (command == "set") {
            if (tokens.size() != 3) {
                return fail(std::move(output), line_number,
                            "set requires: set <pin> <0|1|low|high|z>");
            }

            const std::string pin_name = toUpperAscii(tokens[1]);
            int value = 0;
            bool high_z = false;
            std::string error;
            if (!parseDriveValue(tokens[2], value, high_z, error)) {
                return fail(std::move(output), line_number, error);
            }

            const bool ok = high_z
                ? host.setPinHighZ(pin_name, error)
                : host.setPin(pin_name, value, error);
            if (!ok) {
                return fail(std::move(output), line_number, error);
            }

            appendOutput(output,
                         "line " + std::to_string(line_number) + ": staged " +
                             pin_name + " = " + (high_z ? "HIGH-Z"
                                                          : (value == 0 ? "LOW"
                                                                        : "HIGH")));
            continue;
        }

        if (command == "highz" || command == "z") {
            if (tokens.size() != 2) {
                return fail(std::move(output), line_number,
                            "highz requires: highz <pin>");
            }

            const std::string pin_name = toUpperAscii(tokens[1]);
            std::string error;
            if (!host.setPinHighZ(pin_name, error)) {
                return fail(std::move(output), line_number, error);
            }
            appendOutput(output,
                         "line " + std::to_string(line_number) + ": staged " +
                             pin_name + " = HIGH-Z");
            continue;
        }

        if (command == "apply") {
            if (tokens.size() != 1) {
                return fail(std::move(output), line_number,
                            "apply takes no arguments");
            }

            std::string error;
            if (!host.applyOutputs(error)) {
                return fail(std::move(output), line_number, error);
            }
            appendOutput(output,
                         "line " + std::to_string(line_number) + ": apply OK");
            continue;
        }

        if (command == "expect") {
            if (tokens.size() != 3) {
                return fail(std::move(output), line_number,
                            "expect requires: expect <pin> <0|1|low|high|unknown>");
            }

            const std::string pin_name = toUpperAscii(tokens[1]);
            jtag::PinState expected = jtag::PinState::UNKNOWN;
            std::string error;
            if (!parseExpectedValue(tokens[2], expected, error)) {
                return fail(std::move(output), line_number, error);
            }

            jtag::ScanResult result;
            if (!host.sample(result, error)) {
                return fail(std::move(output), line_number, error);
            }

            const jtag::PinState actual = result.getPin(pin_name);
            total_expects++;
            if (actual != expected) {
                failed_expects++;
                return fail(std::move(output), line_number,
                            pin_name + " expected " + pinStateName(expected) +
                                " but observed " + pinStateName(actual));
            }

            appendOutput(output,
                         "line " + std::to_string(line_number) + ": expect " +
                             pin_name + " == " + pinStateName(expected) + " OK");
            continue;
        }

        if (command == "sleep") {
            if (tokens.size() != 2) {
                return fail(std::move(output), line_number,
                            "sleep requires: sleep <milliseconds>");
            }

            int milliseconds = 0;
            std::string error;
            if (!parseMilliseconds(tokens[1], milliseconds, error)) {
                return fail(std::move(output), line_number, error);
            }

            if (!host.sleepMs(milliseconds, error)) {
                return fail(std::move(output), line_number, error);
            }
            appendOutput(output,
                         "line " + std::to_string(line_number) + ": sleep " +
                             std::to_string(milliseconds) + " ms");
            continue;
        }

        if (command == "reset_safe") {
            if (tokens.size() != 1) {
                return fail(std::move(output), line_number,
                            "reset_safe takes no arguments");
            }

            host.resetToSafe();
            std::string error;
            if (!host.applyOutputs(error)) {
                return fail(std::move(output), line_number, error);
            }
            appendOutput(output,
                         "line " + std::to_string(line_number) +
                             ": reset_safe OK");
            continue;
        }

        return fail(std::move(output), line_number,
                    "unknown command '" + tokens[0] + "'");
    }

    appendOutput(output, "script completed successfully");
    return {true, 0, std::move(output), total_expects, failed_expects};
}

} // namespace jtag::script

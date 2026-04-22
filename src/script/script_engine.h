#pragma once

#include <string>

#include "src/boundary_scan/scanner.h"

namespace jtag::script {

class ScriptHost {
public:
    virtual ~ScriptHost() = default;

    virtual bool sample(jtag::ScanResult& result, std::string& error) = 0;
    virtual bool setPin(const std::string& pin_name, int value,
                        std::string& error) = 0;
    virtual bool setPinHighZ(const std::string& pin_name,
                             std::string& error) = 0;
    virtual bool applyOutputs(std::string& error) = 0;
    virtual void resetToSafe() = 0;
    virtual bool sleepMs(int milliseconds, std::string& error) = 0;
};

struct ScriptResult {
    bool success = false;
    int failed_line = 0;
    std::string output;
    int total_expects = 0;
    int failed_expects = 0;
};

class ScriptEngine {
public:
    static ScriptResult run(const std::string& script_text, ScriptHost& host);
};

} // namespace jtag::script

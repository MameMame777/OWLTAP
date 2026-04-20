#include <gtest/gtest.h>

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "src/script/script_engine.h"

namespace {

class FakeScriptHost : public jtag::script::ScriptHost {
public:
    struct Call {
        std::string name;
        std::string pin;
        int value = -2;
    };

    bool sample(jtag::ScanResult& result, std::string& error) override {
        calls.push_back({"sample", {}, -2});
        if (sample_should_fail) {
            error = sample_error;
            return false;
        }
        if (sample_results.empty()) {
            error = "no sample data configured";
            return false;
        }
        result = sample_results.front();
        sample_results.pop_front();
        return true;
    }

    bool setPin(const std::string& pin_name, int value,
                std::string& error) override {
        calls.push_back({"setPin", pin_name, value});
        if (set_should_fail) {
            error = set_error;
            return false;
        }
        return true;
    }

    bool setPinHighZ(const std::string& pin_name,
                     std::string& error) override {
        calls.push_back({"setPinHighZ", pin_name, -1});
        if (highz_should_fail) {
            error = highz_error;
            return false;
        }
        return true;
    }

    bool applyOutputs(std::string& error) override {
        calls.push_back({"applyOutputs", {}, -2});
        if (apply_should_fail) {
            error = apply_error;
            return false;
        }
        return true;
    }

    void resetToSafe() override {
        calls.push_back({"resetToSafe", {}, -2});
    }

    bool sleepMs(int milliseconds, std::string& error) override {
        calls.push_back({"sleepMs", {}, milliseconds});
        if (sleep_should_fail) {
            error = sleep_error;
            return false;
        }
        return true;
    }

    std::deque<jtag::ScanResult> sample_results;
    std::vector<Call> calls;
    bool sample_should_fail = false;
    bool set_should_fail = false;
    bool highz_should_fail = false;
    bool apply_should_fail = false;
    bool sleep_should_fail = false;
    std::string sample_error = "sample failed";
    std::string set_error = "set failed";
    std::string highz_error = "high-z failed";
    std::string apply_error = "apply failed";
    std::string sleep_error = "sleep failed";
};

jtag::ScanResult sampleWith(const std::string& pin_name, jtag::PinState state) {
    jtag::ScanResult result;
    result.pin_states[pin_name] = state;
    return result;
}

TEST(ScriptEngineTest, RunsHappyPathCommands) {
    FakeScriptHost host;
    host.sample_results.push_back(sampleWith("LED0", jtag::PinState::HIGH));
    host.sample_results.push_back(sampleWith("LED0", jtag::PinState::HIGH));

    const std::string script =
        "set led0 1\n"
        "apply\n"
        "read led0\n"
        "expect led0 high\n"
        "sleep 10\n"
        "reset_safe\n";

    const auto result = jtag::script::ScriptEngine::run(script, host);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.failed_line, 0);
    EXPECT_NE(result.output.find("LED0 = HIGH"), std::string::npos);
    EXPECT_NE(result.output.find("script completed successfully"),
              std::string::npos);
    ASSERT_EQ(host.calls.size(), 7u);
    EXPECT_EQ(host.calls[0].name, "setPin");
    EXPECT_EQ(host.calls[0].pin, "LED0");
    EXPECT_EQ(host.calls[0].value, 1);
    EXPECT_EQ(host.calls[1].name, "applyOutputs");
    EXPECT_EQ(host.calls[2].name, "sample");
    EXPECT_EQ(host.calls[3].name, "sample");
    EXPECT_EQ(host.calls[4].name, "sleepMs");
    EXPECT_EQ(host.calls[4].value, 10);
    EXPECT_EQ(host.calls[5].name, "resetToSafe");
    EXPECT_EQ(host.calls[6].name, "applyOutputs");
}

TEST(ScriptEngineTest, SupportsHighZCommand) {
    FakeScriptHost host;
    const auto result = jtag::script::ScriptEngine::run("highz led0\n", host);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(host.calls.size(), 1u);
    EXPECT_EQ(host.calls[0].name, "setPinHighZ");
    EXPECT_EQ(host.calls[0].pin, "LED0");
}

TEST(ScriptEngineTest, ReportsLineNumberForInvalidCommand) {
    FakeScriptHost host;
    const auto result = jtag::script::ScriptEngine::run(
        "set led0 1\n"
        "bogus thing\n",
        host);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.failed_line, 2);
    EXPECT_NE(result.output.find("line 2: ERROR: unknown command 'bogus'"),
              std::string::npos);
}

TEST(ScriptEngineTest, ReportsHostFailures) {
    FakeScriptHost host;
    host.apply_should_fail = true;
    host.apply_error = "driver apply failed";

    const auto result = jtag::script::ScriptEngine::run(
        "set led0 low\n"
        "apply\n",
        host);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.failed_line, 2);
    EXPECT_NE(result.output.find("driver apply failed"), std::string::npos);
}

TEST(ScriptEngineTest, ExpectFailsOnUnexpectedValue) {
    FakeScriptHost host;
    host.sample_results.push_back(sampleWith("LED0", jtag::PinState::LOW));

    const auto result = jtag::script::ScriptEngine::run("expect led0 1\n", host);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.failed_line, 1);
    EXPECT_NE(result.output.find("LED0 expected HIGH but observed LOW"),
              std::string::npos);
}

} // namespace
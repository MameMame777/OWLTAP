#include "test_suite.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace jtag::script {
namespace {

std::string baseName(const std::string& path) {
    const std::filesystem::path p(path);
    return p.filename().string();
}

std::string trim(const std::string& s) {
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

} // namespace

std::vector<std::string> TestSuiteRunner::parseSuiteFile(
    const std::string& suite_path, std::string& error) {
    std::ifstream file(suite_path);
    if (!file.is_open()) {
        error = "cannot open suite file: " + suite_path;
        return {};
    }

    const std::filesystem::path suite_dir =
        std::filesystem::path(suite_path).parent_path();

    std::vector<std::string> paths;
    std::string line;
    while (std::getline(file, line)) {
        // Strip comment
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        std::filesystem::path p(line);
        if (p.is_relative()) {
            p = suite_dir / p;
        }
        paths.push_back(p.string());
    }
    return paths;
}

TestSuiteResult TestSuiteRunner::run(
    const std::vector<std::string>& script_paths, ScriptHost& host) {
    TestSuiteResult suite;
    for (const auto& path : script_paths) {
        TestCaseResult tc;
        tc.name = baseName(path);
        tc.path = path;

        std::ifstream file(path);
        if (!file.is_open()) {
            tc.success = false;
            tc.output = "ERROR: cannot open file: " + path;
            suite.cases.push_back(std::move(tc));
            suite.fail_count++;
            continue;
        }

        std::ostringstream ss;
        ss << file.rdbuf();
        const std::string script_text = ss.str();

        ScriptResult res = ScriptEngine::run(script_text, host);
        tc.success = res.success;
        tc.failed_line = res.failed_line;
        tc.total_expects = res.total_expects;
        tc.failed_expects = res.failed_expects;
        tc.output = std::move(res.output);

        if (tc.success) {
            suite.pass_count++;
        } else {
            suite.fail_count++;
        }
        suite.cases.push_back(std::move(tc));
    }
    return suite;
}

std::string TestSuiteRunner::formatReport(const TestSuiteResult& result) {
    std::ostringstream ss;
    ss << "=== Test Suite Report ===\n";
    ss << "Total: " << result.cases.size()
       << "  PASS: " << result.pass_count
       << "  FAIL: " << result.fail_count << "\n\n";

    for (const auto& tc : result.cases) {
        ss << (tc.success ? "[PASS] " : "[FAIL] ") << tc.name;
        if (!tc.success && tc.failed_line > 0) {
            ss << " (line " << tc.failed_line << ")";
        }
        ss << "  expects: " << (tc.total_expects - tc.failed_expects)
           << "/" << tc.total_expects << " passed\n";
        if (!tc.success) {
            // Include last line of output (the error message)
            const size_t last_nl = tc.output.rfind('\n', tc.output.size() - 2);
            if (last_nl != std::string::npos) {
                ss << "         " << tc.output.substr(last_nl + 1);
            }
            ss << "\n";
        }
    }
    return ss.str();
}

} // namespace jtag::script

#pragma once

#include <string>
#include <vector>

#include "script_engine.h"

namespace jtag::script {

/// Result for a single script test case within a suite run.
struct TestCaseResult {
    std::string name;        // display name (basename of path)
    std::string path;        // full file path
    bool success = false;
    int failed_line = 0;
    int total_expects = 0;
    int failed_expects = 0;
    std::string output;
};

/// Aggregated result for a complete suite run.
struct TestSuiteResult {
    std::vector<TestCaseResult> cases;
    int pass_count = 0;
    int fail_count = 0;
};

/// Runs a list of script files sequentially and aggregates results.
class TestSuiteRunner {
public:
    /// Parse a .suite file (one script path per line; # comments ignored).
    /// Returns list of resolved paths.  Relative paths are resolved relative
    /// to suite_file_dir.
    static std::vector<std::string> parseSuiteFile(
        const std::string& suite_path, std::string& error);

    /// Execute all script files against the given host.
    static TestSuiteResult run(const std::vector<std::string>& script_paths,
                               ScriptHost& host);

    /// Format a plain-text report from a suite result.
    static std::string formatReport(const TestSuiteResult& result);
};

} // namespace jtag::script

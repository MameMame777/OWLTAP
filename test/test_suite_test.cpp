#include "gtest/gtest.h"

#include <deque>
#include <filesystem>
#include <fstream>
#include <string>

#include "src/script/test_suite.h"

namespace jtag::script {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string writeTempFile(const std::string& stem,
                                 const std::string& content) {
    const auto dir = std::filesystem::temp_directory_path();
    const auto path = (dir / (stem + ".tmp")).string();
    std::ofstream f(path, std::ios::trunc);
    f << content;
    return path;
}

// Minimal ScriptHost that succeeds all operations.
class StubHost : public ScriptHost {
public:
    bool sample(ScanResult& result, std::string& /*err*/) override {
        result = {};
        return true;
    }
    bool setPin(const std::string& /*pin*/, int /*v*/,
                std::string& /*err*/) override { return true; }
    bool setPinHighZ(const std::string& /*pin*/,
                     std::string& /*err*/) override { return true; }
    bool applyOutputs(std::string& /*err*/) override { return true; }
    void resetToSafe() override {}
    bool sleepMs(int /*ms*/, std::string& /*err*/) override { return true; }
};

// ---------------------------------------------------------------------------
// parseSuiteFile
// ---------------------------------------------------------------------------

TEST(TestSuiteParserTest, EmptyFileReturnsEmptyList) {
    const auto path = writeTempFile("suite_empty", "");
    std::string err;
    const auto paths = TestSuiteRunner::parseSuiteFile(path, err);
    EXPECT_TRUE(err.empty());
    EXPECT_TRUE(paths.empty());
}

TEST(TestSuiteParserTest, MissingFileReturnsError) {
    std::string err;
    const auto paths = TestSuiteRunner::parseSuiteFile(
        "/no/such/file.suite", err);
    EXPECT_FALSE(err.empty());
    EXPECT_TRUE(paths.empty());
}

TEST(TestSuiteParserTest, CommentsAndBlankLinesIgnored) {
    const auto path = writeTempFile("suite_comments",
        "# this is a comment\n"
        "\n"
        "   \n"
        "# another comment\n");
    std::string err;
    const auto paths = TestSuiteRunner::parseSuiteFile(path, err);
    EXPECT_TRUE(err.empty());
    EXPECT_TRUE(paths.empty());
}

TEST(TestSuiteParserTest, AbsolutePathRetainedAsIs) {
    const auto dir = std::filesystem::temp_directory_path();
    const auto abs_path = (dir / "abs_script.script").string();
    const auto suite_path = writeTempFile("suite_abs", abs_path + "\n");
    std::string err;
    const auto paths = TestSuiteRunner::parseSuiteFile(suite_path, err);
    ASSERT_TRUE(err.empty());
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], abs_path);
}

TEST(TestSuiteParserTest, RelativePathResolvedFromSuiteDir) {
    const auto suite_path = writeTempFile("suite_rel",
        "my_test.script\n");
    const auto suite_dir =
        std::filesystem::path(suite_path).parent_path();
    const auto expected = (suite_dir / "my_test.script").string();
    std::string err;
    const auto paths = TestSuiteRunner::parseSuiteFile(suite_path, err);
    ASSERT_TRUE(err.empty());
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], expected);
}

TEST(TestSuiteParserTest, InlineCommentStrippedFromPath) {
    const auto dir = std::filesystem::temp_directory_path();
    const auto abs = (dir / "inline_test.script").string();
    const auto suite_path = writeTempFile("suite_inline",
        abs + "  # inline comment\n");
    std::string err;
    const auto paths = TestSuiteRunner::parseSuiteFile(suite_path, err);
    ASSERT_TRUE(err.empty());
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], abs);
}

TEST(TestSuiteParserTest, MultiplePathsReturnedInOrder) {
    const auto dir = std::filesystem::temp_directory_path();
    const auto a = (dir / "a.script").string();
    const auto b = (dir / "b.script").string();
    const auto suite_path = writeTempFile("suite_multi",
        a + "\n" + b + "\n");
    std::string err;
    const auto paths = TestSuiteRunner::parseSuiteFile(suite_path, err);
    ASSERT_TRUE(err.empty());
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(paths[0], a);
    EXPECT_EQ(paths[1], b);
}

// ---------------------------------------------------------------------------
// TestSuiteRunner::run
// ---------------------------------------------------------------------------

TEST(TestSuiteRunnerTest, MissingScriptFileCountsAsFail) {
    StubHost host;
    const TestSuiteResult r =
        TestSuiteRunner::run({"/no/such/script.script"}, host);
    EXPECT_EQ(r.fail_count, 1);
    EXPECT_EQ(r.pass_count, 0);
    ASSERT_EQ(r.cases.size(), 1u);
    EXPECT_FALSE(r.cases[0].success);
}

TEST(TestSuiteRunnerTest, EmptyScriptPasses) {
    const auto path = writeTempFile("suite_run_pass", "# only comments\n");
    StubHost host;
    const TestSuiteResult r = TestSuiteRunner::run({path}, host);
    EXPECT_EQ(r.pass_count, 1);
    EXPECT_EQ(r.fail_count, 0);
    ASSERT_EQ(r.cases.size(), 1u);
    EXPECT_TRUE(r.cases[0].success);
}

TEST(TestSuiteRunnerTest, MultipleScriptsAggregated) {
    const auto p1 = writeTempFile("suite_run_s1", "# pass\n");
    const auto p2 = writeTempFile("suite_run_s2", "# pass\n");
    const auto bad = std::string("/no/such/x.script");
    StubHost host;
    const TestSuiteResult r = TestSuiteRunner::run({p1, p2, bad}, host);
    EXPECT_EQ(r.pass_count, 2);
    EXPECT_EQ(r.fail_count, 1);
    EXPECT_EQ(r.cases.size(), 3u);
}

TEST(TestSuiteRunnerTest, CaseNameIsBasenameOfPath) {
    const auto path = writeTempFile("named_case", "");
    StubHost host;
    const TestSuiteResult r = TestSuiteRunner::run({path}, host);
    ASSERT_EQ(r.cases.size(), 1u);
    // basename should be "named_case.tmp" (stem + extension from writeTempFile)
    EXPECT_NE(r.cases[0].name.find("named_case"), std::string::npos);
}

TEST(TestSuiteRunnerTest, EmptyScriptListReturnsZeroCounts) {
    StubHost host;
    const TestSuiteResult r = TestSuiteRunner::run({}, host);
    EXPECT_EQ(r.pass_count, 0);
    EXPECT_EQ(r.fail_count, 0);
    EXPECT_TRUE(r.cases.empty());
}

// ---------------------------------------------------------------------------
// TestSuiteRunner::formatReport
// ---------------------------------------------------------------------------

TEST(TestSuiteRunnerTest, FormatReportContainsSummaryLine) {
    TestSuiteResult sr;
    sr.pass_count = 2;
    sr.fail_count = 1;

    TestCaseResult pass_tc;
    pass_tc.name = "ok_test.script";
    pass_tc.path = "/tmp/ok_test.script";
    pass_tc.success = true;
    pass_tc.total_expects = 4;
    pass_tc.failed_expects = 0;
    sr.cases.push_back(pass_tc);

    TestCaseResult fail_tc;
    fail_tc.name = "bad_test.script";
    fail_tc.path = "/tmp/bad_test.script";
    fail_tc.success = false;
    fail_tc.failed_line = 7;
    fail_tc.total_expects = 2;
    fail_tc.failed_expects = 1;
    fail_tc.output = "line 7: EXPECT failed\n";
    sr.cases.push_back(fail_tc);

    const std::string report = TestSuiteRunner::formatReport(sr);
    EXPECT_NE(report.find("PASS: 2"), std::string::npos);
    EXPECT_NE(report.find("FAIL: 1"), std::string::npos);
    EXPECT_NE(report.find("ok_test.script"), std::string::npos);
    EXPECT_NE(report.find("bad_test.script"), std::string::npos);
}

} // namespace
} // namespace jtag::script

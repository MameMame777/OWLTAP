// Tests for HardwareJob: state transitions and thread-safe accessors.
#include <gtest/gtest.h>

#include <thread>

#include "src/hardware/hardware_job.h"

namespace jtag::hardware {
namespace {

TEST(HardwareJobTest, InitialStateIsQueued) {
    HardwareJob job("test-id-1", "test label");
    EXPECT_EQ(job.id(), "test-id-1");
    EXPECT_EQ(job.label(), "test label");
    EXPECT_EQ(job.state(), JobState::kQueued);
    EXPECT_TRUE(job.progress().is_null());
    EXPECT_TRUE(job.result().is_null());
    EXPECT_TRUE(job.error().empty());
    EXPECT_FALSE(job.isCancelRequested());
}

TEST(HardwareJobTest, StateTransitions) {
    HardwareJob job("id2");
    job.setState(JobState::kRunning);
    EXPECT_EQ(job.state(), JobState::kRunning);
    job.setState(JobState::kComplete);
    EXPECT_EQ(job.state(), JobState::kComplete);
}

TEST(HardwareJobTest, SetResultAndProgress) {
    HardwareJob job("id3");
    job.setProgress(nlohmann::json{{"pct", 50}});
    job.setResult(nlohmann::json{{"ok", true}});
    EXPECT_EQ(job.progress()["pct"], 50);
    EXPECT_EQ(job.result()["ok"], true);
}

TEST(HardwareJobTest, SetError) {
    HardwareJob job("id4");
    job.setError("hardware fault");
    EXPECT_EQ(job.error(), "hardware fault");
}

TEST(HardwareJobTest, RequestCancelIsAtomic) {
    HardwareJob job("id5");
    EXPECT_FALSE(job.isCancelRequested());
    job.requestCancel();
    EXPECT_TRUE(job.isCancelRequested());
}

TEST(HardwareJobTest, SnapshotCopiesAllFields) {
    HardwareJob job("id6", "snap-test");
    job.setState(JobState::kFailed);
    job.setError("boom");
    job.setProgress(nlohmann::json{{"p", 1}});
    job.setResult(nlohmann::json{{"r", 2}});

    const auto snap = job.snapshot();
    EXPECT_EQ(snap.id, "id6");
    EXPECT_EQ(snap.state, JobState::kFailed);
    EXPECT_EQ(snap.error, "boom");
    EXPECT_EQ(snap.progress["p"], 1);
    EXPECT_EQ(snap.result["r"], 2);
}

TEST(HardwareJobTest, SnapshotToJsonContainsRequiredKeys) {
    HardwareJob job("my-uuid");
    job.setState(JobState::kComplete);
    job.setResult(nlohmann::json{{"answer", 42}});
    const auto j = snapshotToJson(job.snapshot());
    EXPECT_EQ(j["job_id"], "my-uuid");
    EXPECT_EQ(j["state"], "complete");
    EXPECT_EQ(j["result"]["answer"], 42);
}

TEST(HardwareJobTest, JobStateName) {
    EXPECT_STREQ(jobStateName(JobState::kQueued),    "queued");
    EXPECT_STREQ(jobStateName(JobState::kRunning),   "running");
    EXPECT_STREQ(jobStateName(JobState::kComplete),  "complete");
    EXPECT_STREQ(jobStateName(JobState::kFailed),    "failed");
    EXPECT_STREQ(jobStateName(JobState::kCancelled), "cancelled");
}

// Concurrent read/write stress test: one writer, two readers.
TEST(HardwareJobTest, ConcurrentAccessDoesNotCrash) {
    HardwareJob job("concurrent-id");
    std::atomic<bool> done{false};

    auto writer = std::thread([&] {
        for (int i = 0; i < 1000; ++i) {
            job.setProgress(nlohmann::json{{"i", i}});
        }
        job.setState(JobState::kComplete);
        done.store(true);
    });

    auto reader = std::thread([&] {
        while (!done.load()) {
            volatile auto s = job.state();
            volatile auto p = job.progress();
            (void)s;
            (void)p;
        }
    });

    writer.join();
    reader.join();
    EXPECT_EQ(job.state(), JobState::kComplete);
}

}  // namespace
}  // namespace jtag::hardware

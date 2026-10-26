/**
 * @file test_filesystem_mount.cpp
 * @brief Native unit tests for the pure LittleFS mount decision (ADR-0074, §4 #34).
 *
 * The silent-reformat defect collapsed "mounted cleanly" and "had to reformat" into one true return, so
 * a wipe went unannounced. The field re-test then showed an erased partition reports the identical mount
 * failure as a corrupt one, so a reformat must further split into a real data-loss reformat (the
 * partition had a filesystem before) and a blank partition's expected first-time format (nothing to
 * lose, must stay silent). decideMountOutcome keeps all three distinct; these tests pin the distinctions
 * the glue relies on to decide whether to log loud + breadcrumb. The Arduino side (LittleFS, NVS) is
 * device-only.
 */
#include <unity.h>

#include "net/filesystem_mount.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

void test_clean_mount_is_mounted(void) {
    // A non-formatting mount that succeeds: nothing lost, nothing logged or counted. The other flags are
    // irrelevant once the first mount succeeded, so no format is attempted.
    TEST_ASSERT_TRUE(decideMountOutcome(/*firstMountOk=*/true, /*reformatMountOk=*/false,
                                        /*hadFilesystemBefore=*/false) == MountOutcome::Mounted);
    TEST_ASSERT_TRUE(decideMountOutcome(/*firstMountOk=*/true, /*reformatMountOk=*/true,
                                        /*hadFilesystemBefore=*/true) == MountOutcome::Mounted);
}

void test_reformat_of_a_seen_filesystem_is_reformatted(void) {
    // The non-formatting mount failed on a partition that previously held a filesystem: real data loss.
    const MountOutcome outcome = decideMountOutcome(/*firstMountOk=*/false, /*reformatMountOk=*/true,
                                                    /*hadFilesystemBefore=*/true);
    TEST_ASSERT_TRUE(outcome == MountOutcome::Reformatted);  // drives the loud [ERROR] + breadcrumb.
    TEST_ASSERT_FALSE(outcome == MountOutcome::Mounted);     // not a silent clean mount (the first defect).
    TEST_ASSERT_FALSE(outcome == MountOutcome::FirstFormat); // not a benign first format (the second defect).
}

void test_first_format_of_a_blank_partition_is_not_a_reformat(void) {
    // Same failed non-formatting mount, but the partition was never seen before: a blank/erased partition
    // formatting for the first time. It must NOT be Reformatted, or a fresh device would false-alarm
    // "data lost" and emit an [ERROR] on first boot (ADR-0074 decision 3).
    const MountOutcome outcome = decideMountOutcome(/*firstMountOk=*/false, /*reformatMountOk=*/true,
                                                    /*hadFilesystemBefore=*/false);
    TEST_ASSERT_TRUE(outcome == MountOutcome::FirstFormat);
    TEST_ASSERT_FALSE(outcome == MountOutcome::Reformatted);
}

void test_dead_filesystem_is_failed(void) {
    // Even the formatting mount failed: there is no filesystem, so the caller's [FATAL] must fire,
    // regardless of whether a filesystem was ever seen.
    TEST_ASSERT_TRUE(decideMountOutcome(/*firstMountOk=*/false, /*reformatMountOk=*/false,
                                        /*hadFilesystemBefore=*/true) == MountOutcome::Failed);
    TEST_ASSERT_TRUE(decideMountOutcome(/*firstMountOk=*/false, /*reformatMountOk=*/false,
                                        /*hadFilesystemBefore=*/false) == MountOutcome::Failed);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_clean_mount_is_mounted);
    RUN_TEST(test_reformat_of_a_seen_filesystem_is_reformatted);
    RUN_TEST(test_first_format_of_a_blank_partition_is_not_a_reformat);
    RUN_TEST(test_dead_filesystem_is_failed);
    return UNITY_END();
}

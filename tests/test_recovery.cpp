// Multi-instance recovery-slot tests — the "two running instances fight over
// one recovery autosave" SIGBUS fix. Each instance claims a per-slot OS file
// lock (kernel-released on death) and writes only its own snapshot; the
// startup scan offers only ORPHANED snapshots (owner provably dead), never a
// live instance's file.
//
// NOTE: the slot claim is a process-lifetime static, so the ordering inside
// this binary matters — the pre-seeded slot-0 orphan must exist BEFORE the
// first recovery API call, which is also exactly the scenario under test
// ("previous session crashed, new session starts").

#include "io/ProjectRecovery.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <filesystem>

#ifndef _WIN32
#include <sys/file.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#endif

namespace fs = std::filesystem;

namespace {

std::string g_base; // XDG_CONFIG_HOME sandbox for the whole binary

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream os(path, std::ios::out | std::ios::trunc);
    os << text;
}

std::string recDir() { return g_base + "/materializr/recovery"; }

// Pre-main setup: sandbox the config dir and seed a "crashed previous
// session" — a slot-0 snapshot (legacy filename) with no lock held.
struct Env {
    Env() {
        g_base = (fs::temp_directory_path() /
                  ("mzr_recovery_test_" + std::to_string(::getpid()))).string();
        fs::remove_all(g_base);
        fs::create_directories(recDir());
        ::setenv("XDG_CONFIG_HOME", g_base.c_str(), 1);
        writeFile(recDir() + "/autosave.materializr", "fake-snapshot-slot0");
        writeFile(recDir() + "/autosave.materializr.meta",
                  "MZRECOVERY 1\nSAVEDAT 1234\nBODIES 3\nSTEPS 7\n"
                  "PROJECT /tmp/original.materializr\n");
    }
    ~Env() { fs::remove_all(g_base); }
} g_env;

} // namespace

// The new instance must NOT claim the crashed session's slot (it holds the
// snapshot we want to offer) — it takes the next free one.
TEST(Recovery, ClaimAvoidsOrphanedSnapshotSlot) {
    const std::string own = materializr::projectRecoveryPath();
    EXPECT_NE(own.find("autosave-"), std::string::npos)
        << "claimed the orphan's slot 0: " << own;
    EXPECT_EQ(own.rfind(recDir(), 0), 0u) << own;
}

// The crashed session's snapshot is found, chosen, and its meta readable.
TEST(Recovery, OrphanIsOfferedWithMeta) {
    ASSERT_TRUE(materializr::hasProjectRecovery());
    const std::string cand = materializr::projectRecoveryRestorePath();
    EXPECT_NE(cand.find("autosave.materializr"), std::string::npos) << cand;
    EXPECT_NE(cand, materializr::projectRecoveryPath());

    materializr::ProjectRecoveryMeta meta;
    ASSERT_TRUE(materializr::readProjectRecoveryMeta(meta));
    EXPECT_EQ(meta.bodyCount, 3);
    EXPECT_EQ(meta.stepCount, 7);
    EXPECT_EQ(meta.projectPath, "/tmp/original.materializr");
}

#ifndef _WIN32
// A snapshot whose slot lock is HELD BY ANOTHER LIVE PROCESS must never be
// offered; the moment that process dies, it must be.
TEST(Recovery, LiveInstanceSnapshotIsSkippedUntilItDies) {
    // Child: claim slot 5's lock and idle (a stand-in second instance).
    int ready[2];
    ASSERT_EQ(::pipe(ready), 0);
    pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        int fd = ::open((recDir() + "/slot5.lock").c_str(),
                        O_CREAT | O_RDWR, 0600);
        if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) _exit(1);
        char ok = '1';
        (void)!::write(ready[1], &ok, 1);
        ::pause(); // hold the lock until killed
        _exit(0);
    }
    char ok = 0;
    ASSERT_EQ(::read(ready[0], &ok, 1), 1);
    ::close(ready[0]); ::close(ready[1]);

    // A newer snapshot in the live instance's slot...
    writeFile(recDir() + "/autosave-5.materializr", "fake-snapshot-slot5");

    // ...must be skipped: the (older) slot-0 orphan stays the candidate.
    ASSERT_TRUE(materializr::hasProjectRecovery());
    EXPECT_EQ(materializr::projectRecoveryRestorePath().find("autosave-5"),
              std::string::npos);

    // Kill the "instance"; its kernel-released lock makes slot 5 an orphan,
    // and being newest it becomes the candidate.
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    ASSERT_TRUE(materializr::hasProjectRecovery());
    EXPECT_NE(materializr::projectRecoveryRestorePath().find("autosave-5"),
              std::string::npos);
}
#endif

// Discard/consume deletes only the candidate; the next scan surfaces the
// remaining orphan, and clearing that too empties the queue.
TEST(Recovery, ClearCandidateConsumesOneOrphanAtATime) {
    ASSERT_TRUE(materializr::hasProjectRecovery());
    materializr::clearProjectRecoveryCandidate();
    // Slot-0 orphan should still be pending (if the fork test ran, slot 5 was
    // consumed first; either way exactly one orphan remains).
    ASSERT_TRUE(materializr::hasProjectRecovery());
    materializr::clearProjectRecoveryCandidate();
    EXPECT_FALSE(materializr::hasProjectRecovery());
    EXPECT_FALSE(fs::exists(recDir() + "/autosave.materializr"));
}

// clearProjectRecovery() (clean exit) touches only OUR slot's files.
TEST(Recovery, ClearOwnSlotOnly) {
    const std::string own = materializr::projectRecoveryPath();
    writeFile(own, "our-own-snapshot");
    writeFile(recDir() + "/autosave-9.materializr", "someone-elses");
    materializr::clearProjectRecovery();
    EXPECT_FALSE(fs::exists(own));
    EXPECT_TRUE(fs::exists(recDir() + "/autosave-9.materializr"));
    fs::remove(recDir() + "/autosave-9.materializr");
}

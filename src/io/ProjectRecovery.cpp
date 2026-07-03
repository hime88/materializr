#include "ProjectRecovery.h"
#include "ProjectIO.h"
#include "../core/Document.h"

#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace materializr {

namespace {
// Base config directory (mirrors SketchRecovery.cpp / Settings.cpp).
std::string configBaseDir() {
#ifdef _WIN32
    if (const char* up = std::getenv("USERPROFILE"); up && *up)
        return std::string(up) + "\\materializr";
    return "materializr";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/materializr";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.config/materializr";
    return ".materializr";
#endif
}

std::string recoveryDir() { return configBaseDir() + "/recovery"; }

// Slot N's snapshot filename. Slot 0 keeps the legacy name so recovery files
// written by older builds are still found (as slot-0 orphans) after updating.
std::string slotSnapshotPath(int slot) {
    if (slot == 0) return recoveryDir() + "/autosave.materializr";
    return recoveryDir() + "/autosave-" + std::to_string(slot) + ".materializr";
}
std::string slotLockPath(int slot) {
    return recoveryDir() + "/slot" + std::to_string(slot) + ".lock";
}

// ---- OS file locks -------------------------------------------------------
// The claim lock is held for the whole process lifetime and released by the
// KERNEL when the process dies (cleanly or not) — that's the liveness signal.
// A probe uses an independent open: per flock(2), a second file description
// in the SAME process is denied against our own held lock, so a probe never
// mistakes our own slot for an orphan. On Windows the exclusive-share
// CreateFile open gives the same semantics.
#ifdef _WIN32
using LockHandle = HANDLE;
const LockHandle kBadLock = INVALID_HANDLE_VALUE;
LockHandle tryLock(const std::string& path) {
    return CreateFileA(path.c_str(), GENERIC_WRITE, /*no sharing*/ 0, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
void releaseLock(LockHandle h) {
    if (h != kBadLock) CloseHandle(h);
}
#else
using LockHandle = int;
const LockHandle kBadLock = -1;
LockHandle tryLock(const std::string& path) {
    int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) return kBadLock;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return kBadLock;
    }
    return fd;
}
void releaseLock(LockHandle h) {
    if (h != kBadLock) ::close(h); // close releases the flock
}
#endif

// Claim this instance's slot (first call only; the lock handle is deliberately
// leaked so it lives exactly as long as the process).
int claimedSlot() {
    static int s_slot = [] {
        std::error_code ec;
        std::filesystem::create_directories(recoveryDir(), ec);
        // Pass 1: prefer a slot with NO leftover snapshot. A slot whose owner
        // crashed holds that session's snapshot — claiming it would make the
        // orphan scan treat the file as OURS (probe denied against our own
        // lock) and silently shadow the very recovery we should be offering.
        for (int pass = 0; pass < 2; ++pass) {
            for (int n = 0; n < 16; ++n) {
                if (pass == 0 && std::filesystem::exists(slotSnapshotPath(n), ec))
                    continue;
                LockHandle h = tryLock(slotLockPath(n));
                if (h != kBadLock) return n; // hold forever (kernel frees on exit)
            }
        }
        // 16 concurrent instances?! Fall back to slot 0 unlocked — behaves
        // like the old shared-path world, which is still better than nothing.
        std::fprintf(stderr, "[Recovery] no free instance slot; sharing 0\n");
        return 0;
    }();
    return s_slot;
}

std::string metaPathFor(const std::string& snapshotPath) {
    return snapshotPath + ".meta";
}

// The orphaned snapshot chosen by hasProjectRecovery() for this launch.
std::string s_candidatePath;
} // namespace

std::string projectRecoveryPath() { return slotSnapshotPath(claimedSlot()); }

bool writeProjectRecovery(const Document& doc, const ProjectHistory* history,
                          const std::string& projectPath, int bodyCount,
                          int stepCount) {
    const std::string path = projectRecoveryPath();
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);

    // Save to a temp file, then atomically rename — a crash mid-write must never
    // truncate the snapshot we'd restore from.
    const std::string tmp = path + ".tmp";
    auto res = ProjectIO::save(tmp, doc, history);
    if (!res.success) { std::filesystem::remove(tmp, ec); return false; }
    std::filesystem::rename(tmp, path, ec);
    if (ec) { // cross-device or race: fall back to a direct overwrite
        std::filesystem::copy_file(
            tmp, path, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
        if (ec) return false;
    }

    // Plain-text sidecar meta: the project's identity + when + counts, so the
    // restore prompt can describe what it found without parsing the snapshot.
    std::ofstream os(metaPathFor(path), std::ios::out | std::ios::trunc);
    if (os.is_open()) {
        os << "MZRECOVERY 1\n";
        os << "SAVEDAT " << static_cast<long long>(std::time(nullptr)) << "\n";
        os << "BODIES " << bodyCount << "\n";
        os << "STEPS " << stepCount << "\n";
        os << "PROJECT " << projectPath << "\n"; // rest-of-line; may be empty
    }
    return true;
}

bool hasProjectRecovery() {
    // Ensure our own slot is claimed FIRST so the scan below can never treat
    // our own snapshot path as an orphan candidate.
    (void)claimedSlot();

    s_candidatePath.clear();
    std::error_code ec;
    std::filesystem::file_time_type bestTime{};
    for (int n = 0; n < 16; ++n) {
        const std::string snap = slotSnapshotPath(n);
        if (!std::filesystem::exists(snap, ec)) continue;
        // Liveness probe: acquirable lock = the owning instance is dead (or
        // the file predates slot locks — same conclusion: nobody owns it).
        LockHandle h = tryLock(slotLockPath(n));
        if (h == kBadLock) continue; // owner alive (possibly us) — not ours to offer
        releaseLock(h);
        auto t = std::filesystem::last_write_time(snap, ec);
        if (ec) t = std::filesystem::file_time_type{};
        if (s_candidatePath.empty() || t > bestTime) {
            s_candidatePath = snap;
            bestTime = t;
        }
    }
    return !s_candidatePath.empty();
}

std::string projectRecoveryRestorePath() { return s_candidatePath; }

bool readProjectRecoveryMeta(ProjectRecoveryMeta& meta) {
    meta = ProjectRecoveryMeta{};
    if (s_candidatePath.empty()) return false;
    std::ifstream is(metaPathFor(s_candidatePath));
    if (is.is_open()) {
        std::string line;
        while (std::getline(is, line)) {
            std::istringstream s(line);
            std::string tok;
            s >> tok;
            if      (tok == "SAVEDAT") s >> meta.savedAtUnix;
            else if (tok == "BODIES")  s >> meta.bodyCount;
            else if (tok == "STEPS")   s >> meta.stepCount;
            else if (tok == "PROJECT") {
                std::string rest;
                std::getline(s, rest);
                if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
                meta.projectPath = rest;
            }
        }
    }
    meta.valid = true; // the snapshot exists; the meta is best-effort
    return true;
}

void clearProjectRecovery() {
    std::error_code ec;
    std::filesystem::remove(projectRecoveryPath(), ec);
    std::filesystem::remove(projectRecoveryPath() + ".tmp", ec);
    std::filesystem::remove(metaPathFor(projectRecoveryPath()), ec);
}

void clearProjectRecoveryCandidate() {
    if (s_candidatePath.empty()) return;
    std::error_code ec;
    std::filesystem::remove(s_candidatePath, ec);
    std::filesystem::remove(s_candidatePath + ".tmp", ec);
    std::filesystem::remove(metaPathFor(s_candidatePath), ec);
    s_candidatePath.clear();
}

} // namespace materializr

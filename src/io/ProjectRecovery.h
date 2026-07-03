#pragma once
#include <string>

class Document;

namespace materializr {

struct ProjectHistory; // ProjectIO.h

// Crash/hang recovery for the WHOLE project. SketchRecovery only guards the
// in-progress (uncommitted) sketch; this guards the committed model — bodies
// and the full operation history — including an UNSAVED project that has no
// .materializr path yet (the case that loses the most work). The active project
// is periodically snapshotted to a sidecar, independent of the user's own save
// file, so a crash or a hang never costs more than a few seconds of committed
// work. The snapshot is deleted on a clean exit, so one surviving to the next
// launch means "the last session ended unexpectedly with unsaved work" → offer
// to restore it.
struct ProjectRecoveryMeta {
    bool        valid = false;
    std::string projectPath;  // the project's own save path ("" = never saved)
    long long   savedAtUnix = 0;
    int         bodyCount = 0;
    int         stepCount = 0;
};

// Absolute path of the recovery snapshot (~/.config/materializr/recovery/...).
// MULTI-INSTANCE SAFETY: every running instance claims its own recovery SLOT
// (an OS file lock on recovery/slot<N>.lock, held for the process lifetime and
// auto-released by the kernel on crash). projectRecoveryPath() is THIS
// instance's slot file — slot 0 keeps the legacy "autosave.materializr" name,
// later slots get "autosave-<N>.materializr". Two instances therefore never
// write (or truncate) each other's snapshot: the old single shared path let
// instance A's rename/overwrite yank the file out from under instance B
// (SIGBUS) and made the restore prompt offer the WRONG session's work.
std::string projectRecoveryPath();

// Snapshot the document (+ optional history) to the recovery sidecar, written to
// a temp file then atomically renamed so a crash mid-write never leaves a
// truncated snapshot. `projectPath` is the project's own save path ("" if
// unsaved). Best-effort; returns success.
bool writeProjectRecovery(const Document& doc, const ProjectHistory* history,
                          const std::string& projectPath, int bodyCount,
                          int stepCount);

// Startup scan: true if some ORPHANED recovery snapshot exists — one whose
// owning instance is provably dead (its slot lock is acquirable). A snapshot
// belonging to a live instance (or to us) is never offered. When several
// orphans exist the newest is chosen; the rest surface on later launches.
// Remembers the chosen snapshot for the calls below.
bool hasProjectRecovery();

// Path of the orphaned snapshot chosen by hasProjectRecovery() ("" if none).
// This is what the restore loads — NOT projectRecoveryPath(), which is our
// own (live) slot.
std::string projectRecoveryRestorePath();

// Read the chosen orphan's sidecar metadata (for the restore prompt).
bool readProjectRecoveryMeta(ProjectRecoveryMeta& meta);

// Delete THIS instance's snapshot + meta (clean exit).
void clearProjectRecovery();

// Delete the chosen ORPHANED snapshot + meta (user discard, failed restore,
// or right after a successful restore so it isn't offered again).
void clearProjectRecoveryCandidate();

} // namespace materializr

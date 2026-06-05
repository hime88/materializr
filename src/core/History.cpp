#include "History.h"
#include "EventBus.h"
#include "Events.h"
#include <cstdio>

History::History() = default;

bool History::pushOperation(std::unique_ptr<Operation> op, Document& doc) {
    if (!op) {
        return false;
    }

    // Thread-last reflow: threads are a finishing pass. An op that touches
    // a threaded body is inserted BEFORE the trailing Thread steps so its
    // boolean runs against clean geometry (OCCT cannot classify cuts along
    // helical groove fields — established empirically at length), and the
    // threads re-cut parametrically afterwards.
    {
        int at = reflowInsertionIndex(*op);
        if (at >= 0) {
            std::fprintf(stderr, "[History] reflow: inserting '%s' before "
                                 "thread step %d\n",
                         op->name().c_str(), at);
            if (insertStepAndReplay(at, std::move(op), doc)) return true;
            // Reflow declined or the op failed against clean geometry — the
            // op was consumed either way.
            return false;
        }
    }

    // Execute the operation
    if (!op->execute(doc)) {
        return false;
    }
    op->rememberGoodParams();

    // Clear any redo stack (operations beyond current index)
    if (m_currentIndex + 1 < static_cast<int>(m_operations.size())) {
        m_operations.erase(m_operations.begin() + m_currentIndex + 1, m_operations.end());
    }

    // Push the operation
    m_operations.push_back(std::move(op));
    m_currentIndex = static_cast<int>(m_operations.size()) - 1;

    if (m_eventBus) m_eventBus->publish(materializr::HistoryStepEvent{m_currentIndex, false});
    return true;
}

void History::pushExecuted(std::unique_ptr<Operation> op) {
    if (!op) return;
    op->rememberGoodParams(); // already-applied params are by definition good
    if (m_currentIndex + 1 < static_cast<int>(m_operations.size())) {
        m_operations.erase(m_operations.begin() + m_currentIndex + 1, m_operations.end());
    }
    m_operations.push_back(std::move(op));
    m_currentIndex = static_cast<int>(m_operations.size()) - 1;
    if (m_eventBus) m_eventBus->publish(materializr::HistoryStepEvent{m_currentIndex, false});
}

bool History::canUndo() const {
    return m_currentIndex >= 0;
}

bool History::canRedo() const {
    return m_currentIndex < static_cast<int>(m_operations.size()) - 1;
}

bool History::undo(Document& doc) {
    if (!canUndo()) {
        std::fprintf(stderr, "[History] undo: nothing to undo (currentIndex=%d)\n",
                     m_currentIndex);
        return false;
    }

    Operation* op = m_operations[m_currentIndex].get();
    std::fprintf(stderr, "[History] undo step %d '%s' (type=%s reloaded=%d enabled=%d)\n",
                 m_currentIndex, op->name().c_str(), op->typeId().c_str(),
                 op->isReloaded() ? 1 : 0, op->isEnabled() ? 1 : 0);
    if (!op->undo(doc)) {
        std::fprintf(stderr, "[History] undo FAILED at step %d '%s' — op->undo() "
                             "returned false; staying at this step\n",
                     m_currentIndex, op->name().c_str());
        return false;
    }

    m_currentIndex--;
    // Manual undo means the user is steering the applied range themselves —
    // drop any pending auto-recovery so a later edit doesn't surprise-redo
    // steps they deliberately walked back past.
    m_failedReplayAt = -1;
    if (m_eventBus) m_eventBus->publish(materializr::HistoryStepEvent{m_currentIndex, true});
    return true;
}

bool History::redo(Document& doc) {
    if (!canRedo()) {
        return false;
    }

    m_currentIndex++;
    Operation* op = m_operations[m_currentIndex].get();
    if (!op->execute(doc)) {
        m_failedReplayAt = m_currentIndex;
        m_currentIndex--;
        return false;
    }

    op->rememberGoodParams();
    if (m_failedReplayAt >= 0 && m_currentIndex >= m_failedReplayAt) {
        m_failedReplayAt = -1; // the previously-failed step recomputed fine
    }
    if (m_eventBus) m_eventBus->publish(materializr::HistoryStepEvent{m_currentIndex, false});
    return true;
}

int History::stepCount() const {
    return static_cast<int>(m_operations.size());
}

int History::currentStep() const {
    return m_currentIndex;
}

const Operation* History::getStep(int index) const {
    if (index < 0 || index >= static_cast<int>(m_operations.size())) {
        return nullptr;
    }
    return m_operations[index].get();
}

bool History::editStep(int index, Document& doc) {
    if (index < 0 || index >= static_cast<int>(m_operations.size())) {
        return false;
    }

    int limit = m_currentIndex;
    if (m_breakpoint >= 0 && m_breakpoint < limit) {
        limit = m_breakpoint;
    }

    if (index > limit) {
        // The step isn't currently applied — e.g. it was suspended by an
        // earlier failed recompute (fillet grew, chamfer edge vanished) and
        // the user is editing ITS parameters to fix it. Roll FORWARD to it,
        // executing the intervening steps, instead of refusing.
        for (int i = limit + 1; i <= index; ++i) {
            Operation* op = m_operations[i].get();
            if (op->isEnabled()) {
                if (!op->execute(doc)) {
                    m_failedReplayAt = i;
                    return false;
                }
                op->rememberGoodParams();
            }
            m_currentIndex = i;
        }
        if (m_failedReplayAt >= 0 && m_currentIndex >= m_failedReplayAt) {
            m_failedReplayAt = -1;
        }
        return true;
    }

    // Rebuild IN PLACE rather than clearing the document: clearing would also
    // wipe bodies that aren't operations (the base/imported bodies) and reset
    // body ids, so dependent ops (which reference body ids) would fail. Instead
    // roll back to just before `index` by undoing the applied ops in reverse,
    // then re-execute from `index` forward so the edited parameters take effect.
    for (int i = limit; i >= index; --i) {
        Operation* op = m_operations[i].get();
        if (op->isEnabled()) op->undo(doc);
    }
    bool editRejected = false;
    for (int i = index; i <= limit; ++i) {
        Operation* op = m_operations[i].get();
        if (op->isEnabled()) {
            if (!op->execute(doc)) {
                // If the EDITED step itself rejects its new values (e.g. a
                // fillet radius its host geometry can't carry), restore the
                // parameters from its last successful execute and reapply —
                // the model stays as it was instead of stranding this step
                // and everything above it (where the next Ctrl+Z would hit
                // the step below: "undo deleted the whole body"). The UI
                // re-reads the op, so the value visibly snaps back.
                const std::string& good = op->lastGoodParams();
                if (i == index && !good.empty() &&
                    op->deserializeParams(good) && op->execute(doc)) {
                    editRejected = true;
                    continue;
                }
                m_currentIndex = i - 1;
                m_failedReplayAt = i;
                return false;
            }
            op->rememberGoodParams();
        }
    }
    if (editRejected) return false; // model intact, but the edit didn't apply

    // If a PREVIOUS edit knocked steps out (failed recompute left a suspended
    // tail), retry them now that the upstream geometry changed again — this is
    // what makes "grow the fillet too far, chamfer dies, shrink the fillet
    // back" bring the chamfer back automatically. Stops at the first step
    // that still fails (keeping the flag) without failing THIS edit.
    if (m_failedReplayAt >= 0) {
        int cap = static_cast<int>(m_operations.size()) - 1;
        if (m_breakpoint >= 0 && m_breakpoint < cap) cap = m_breakpoint;
        bool cleared = true;
        for (int i = limit + 1; i <= cap; ++i) {
            Operation* op = m_operations[i].get();
            if (op->isEnabled()) {
                if (!op->execute(doc)) {
                    m_failedReplayAt = i;
                    cleared = false;
                    break;
                }
                op->rememberGoodParams();
            }
            m_currentIndex = i;
        }
        if (cleared) m_failedReplayAt = -1;
    }

    // Note: we deliberately don't publish HistoryStepEvent here. editStep
    // is initiated from explicit user UI (HistoryPanel's Apply Changes),
    // which publishes its own SketchEditedEvent when appropriate — better
    // signal-to-noise than a generic step event that also fires for
    // unrelated history shuffles (push/pull preview undos, etc).
    return true;
}

bool History::removeStep(int index, Document& doc) {
    int count = static_cast<int>(m_operations.size());
    if (index < 0 || index >= count) return false;

    // If the step is beyond the current state (in the redo region) it has no
    // effect on the document right now — just drop it.
    if (index > m_currentIndex) {
        m_operations.erase(m_operations.begin() + index);
        if (m_breakpoint > index) m_breakpoint--;
        else if (m_breakpoint == index) m_breakpoint = -1;
        return true;
    }

    // Roll the document back to just before `index` by undoing applied ops.
    for (int i = m_currentIndex; i >= index; --i) {
        Operation* op = m_operations[i].get();
        if (op->isEnabled()) op->undo(doc);
    }

    // Detach the op so we can restore it if a later dependent op fails.
    std::unique_ptr<Operation> removed = std::move(m_operations[index]);
    m_operations.erase(m_operations.begin() + index);

    int savedCurrent = m_currentIndex;
    int savedBreak = m_breakpoint;
    m_currentIndex--; // one fewer applied op
    if (m_breakpoint > index) m_breakpoint--;
    else if (m_breakpoint == index) m_breakpoint = -1;

    // Re-execute the remaining ops from `index` forward.
    for (int i = index; i <= m_currentIndex; ++i) {
        Operation* op = m_operations[i].get();
        if (op->isEnabled()) {
            if (!op->execute(doc)) {
                // Conflict: a later op depended on the removed one. Roll back the
                // partial rebuild, reinsert the removed op, and replay the
                // original chain so the model is left exactly as it was.
                for (int j = i - 1; j >= index; --j) {
                    Operation* o = m_operations[j].get();
                    if (o->isEnabled()) o->undo(doc);
                }
                m_operations.insert(m_operations.begin() + index, std::move(removed));
                m_currentIndex = savedCurrent;
                m_breakpoint = savedBreak;
                for (int j = index; j <= m_currentIndex; ++j) {
                    Operation* o = m_operations[j].get();
                    if (o->isEnabled()) o->execute(doc);
                }
                return false;
            }
        }
    }

    if (m_eventBus) m_eventBus->publish(materializr::HistoryStepEvent{m_currentIndex, true});
    return true;
}

void History::setBreakpoint(int index) {
    m_breakpoint = index;
}

int History::getBreakpoint() const {
    return m_breakpoint;
}

bool History::replayAll(Document& doc) {
    doc.clear();

    int limit = static_cast<int>(m_operations.size()) - 1;
    if (m_breakpoint >= 0 && m_breakpoint < limit) {
        limit = m_breakpoint;
    }

    for (int i = 0; i <= limit; ++i) {
        Operation* op = m_operations[i].get();
        if (op->isEnabled()) {
            if (!op->execute(doc)) {
                m_currentIndex = i - 1;
                return false;
            }
            op->rememberGoodParams();
        }
    }

    m_currentIndex = limit;
    return true;
}

void History::clear() {
    m_operations.clear();
    m_currentIndex = -1;
    m_breakpoint = -1;
    m_failedReplayAt = -1;
}

const std::vector<std::unique_ptr<Operation>>& History::operations() const {
    return m_operations;
}

int History::reflowInsertionIndex(const Operation& op) const {
    if (op.typeId() == "thread") return -1; // stacking threads is fine as-is
    std::vector<int> planned = op.plannedBodyIds();
    if (planned.empty()) return -1;

    int limit = m_currentIndex;
    if (m_breakpoint >= 0 && m_breakpoint < limit) limit = m_breakpoint;

    auto touchesPlanned = [&](const Operation* s) {
        OperationDiff d = s->captureDiff();
        for (const auto& [id, shp] : d.modifiedBefore)
            for (int p : planned) if (p == id) return true;
        for (int id : d.created)
            for (int p : planned) if (p == id) return true;
        for (const auto& [id, shp] : d.deletedBefore)
            for (int p : planned) if (p == id) return true;
        return false;
    };

    // Walk down from the top, SKIPPING steps unrelated to the op's bodies
    // (real histories always have sketches / other-body work above the
    // thread). Reorder beneath the deepest thread that touched a planned
    // body; stop cold at any NON-thread step touching a planned body — the
    // op may depend on its result, so we can't jump past it.
    int insertAt = -1;
    for (int i = limit; i >= 0; --i) {
        const Operation* s = m_operations[i].get();
        if (!s->isEnabled()) continue;
        bool touches = touchesPlanned(s);
        if (s->typeId() == "thread") {
            if (touches) insertAt = i;
            continue;
        }
        if (touches) break;
    }
    return insertAt;
}

bool History::insertStepAndReplay(int index, std::unique_ptr<Operation> op,
                                  Document& doc) {
    int limit = m_currentIndex;
    if (m_breakpoint >= 0 && m_breakpoint < limit) limit = m_breakpoint;
    if (index < 0 || index > limit) return false;

    // Only steps that TOUCH the op's bodies are rolled back and replayed.
    // Unrelated steps stay applied — critically, a reloaded project's baked
    // full-document snapshot steps must NOT re-execute, or their snapshots
    // resurrect the pre-op world and silently erase the inserted op's result
    // (Steve: "it didn't work, but it didn't crash").
    std::vector<int> planned = op->plannedBodyIds();
    auto touches = [&](const Operation* s) {
        OperationDiff d = s->captureDiff();
        for (const auto& [id, shp] : d.modifiedBefore)
            for (int p : planned) if (p == id) return true;
        for (int id : d.created)
            for (int p : planned) if (p == id) return true;
        for (const auto& [id, shp] : d.deletedBefore)
            for (int p : planned) if (p == id) return true;
        return false;
    };
    std::vector<int> touched; // indices in [index..limit], ascending
    for (int i = index; i <= limit; ++i) {
        Operation* s = m_operations[i].get();
        if (!s->isEnabled() || !touches(s)) continue;
        if (s->isReloaded()) {
            // A baked reload snapshot on this body can't recompute — replaying
            // it would clobber the op's result. Decline the whole reflow; the
            // caller falls back to the direct path (fast failure, no hang).
            std::fprintf(stderr, "[History] reflow declined: step %d '%s' is "
                                 "a baked reload snapshot on the same body\n",
                         i, s->name().c_str());
            return false;
        }
        touched.push_back(i);
    }

    // Roll back the touched steps (deepest last).
    for (int k = static_cast<int>(touched.size()) - 1; k >= 0; --k) {
        m_operations[touched[k]]->undo(doc);
    }

    // The new op runs against clean geometry.
    if (!op->execute(doc)) {
        for (int i : touched) {
            Operation* s = m_operations[i].get();
            if (s->execute(doc)) s->rememberGoodParams();
        }
        return false;
    }
    op->rememberGoodParams();

    // Splice it in and replay the touched steps on the new geometry.
    m_operations.insert(m_operations.begin() + index, std::move(op));
    if (m_breakpoint >= index) m_breakpoint++;
    m_currentIndex = limit + 1;
    for (int i : touched) {
        Operation* s = m_operations[i + 1].get(); // shifted by the insert
        if (!s->execute(doc)) {
            // E.g. the thread's cylindrical span was consumed. The user's op
            // DID apply; the step suspends with the explainer banner.
            std::fprintf(stderr, "[History] reflow: step %d '%s' could not "
                                 "re-execute\n",
                         i + 1, s->name().c_str());
            m_failedReplayAt = i + 1;
            break;
        }
        s->rememberGoodParams();
    }

    // Finishing passes propagate to bodies the inserted op CREATED — a split
    // lengthwise through a bolt must leave threads on BOTH halves, not just
    // the half that kept the original body id ("half of it is smooth").
    // Clone each displaced thread for each created body and append the
    // clones as real (undoable, saveable) steps.
    if (m_failedReplayAt < 0) {
        OperationDiff nd = m_operations[index]->captureDiff();
        for (int createdId : nd.created) {
            for (int i : touched) {
                Operation* s = m_operations[i + 1].get();
                if (s->typeId() != "thread") continue;
                std::unique_ptr<Operation> clone = s->cloneForBody(createdId);
                if (!clone) continue;
                if (!clone->execute(doc)) {
                    std::fprintf(stderr, "[History] reflow: thread clone for "
                                         "body %d failed (span may not "
                                         "exist on this piece)\n", createdId);
                    continue;
                }
                clone->rememberGoodParams();
                m_operations.insert(m_operations.begin() + m_currentIndex + 1,
                                    std::move(clone));
                m_currentIndex++;
            }
        }
    }
    if (m_eventBus)
        m_eventBus->publish(materializr::HistoryStepEvent{m_currentIndex, false});
    return true;
}

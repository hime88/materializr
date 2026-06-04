#include "ReplayOp.h"
#include <imgui.h>
#include <map>
#include <set>

ReplayOp::ReplayOp(std::string typeId, std::string name, std::string description,
                   BodyState before, BodyState after, bool fromReload)
    : m_typeId(std::move(typeId))
    , m_name(std::move(name))
    , m_description(std::move(description))
    , m_before(std::move(before))
    , m_after(std::move(after))
    , m_fromReload(fromReload) {}

void ReplayOp::restore(Document& doc, const BodyState& state,
                       bool removeUnlisted) {
    std::set<int> wanted;
    for (const auto& [id, shape] : state) {
        doc.putBody(id, shape);
        wanted.insert(id);
    }
    // Project-reload snapshots cover the whole document — anything not listed
    // shouldn't exist in this state (e.g. a body created by a later step that
    // we're undoing past). Batch in-session snapshots only cover the affected
    // bodies, so removing unlisted ones would delete the rest of the scene.
    if (!removeUnlisted) return;
    for (int id : doc.getAllBodyIds()) {
        if (!wanted.count(id)) doc.removeBody(id);
    }
}

bool ReplayOp::execute(Document& doc) {
    restore(doc, m_after, /*removeUnlisted=*/m_fromReload);
    return true;
}

bool ReplayOp::undo(Document& doc) {
    restore(doc, m_before, /*removeUnlisted=*/m_fromReload);
    return true;
}

OperationDiff ReplayOp::captureDiff() const {
    OperationDiff d;
    std::map<int, const TopoDS_Shape*> before;
    for (const auto& [id, s] : m_before) before[id] = &s;
    std::set<int> afterIds;
    for (const auto& [id, s] : m_after) {
        afterIds.insert(id);
        auto it = before.find(id);
        if (it == before.end()) {
            d.created.push_back(id);
        } else if (!s.IsEqual(*it->second)) {
            // Same body, different shape/placement (e.g. a rotate-body
            // revolve or a batched multi-body transform). Untouched bodies
            // that merely ride along in a full-document snapshot compare
            // IsEqual and are skipped, so the diff stays minimal.
            d.modifiedBefore.push_back({id, *it->second});
        }
    }
    for (const auto& [id, s] : m_before) {
        if (!afterIds.count(id)) d.deletedBefore.push_back({id, s});
    }
    return d;
}

void ReplayOp::renderProperties() {
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", m_description.c_str());
    if (m_fromReload) {
        ImGui::Spacing();
        ImGui::TextWrapped("Loaded from a saved project. Undo/redo work, but the "
                           "parameters of a reloaded step can't be edited.");
    } else {
        ImGui::Spacing();
        ImGui::TextWrapped("Batched transform — undo/redo restores the whole "
                           "set at once.");
    }
}

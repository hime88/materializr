#include "InteractiveOpController.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../core/SelectionManager.h"
#include "../core/Operation.h"
#include <imgui.h>

namespace materializr {

bool InteractiveOpController::begin(const IopContext& ctx) {
    int body = onBegin(ctx);
    if (body < 0) return false;
    try {
        m_snapshot = ctx.doc.getBody(body);
    } catch (...) { return false; }
    if (m_snapshot.IsNull()) return false;
    m_bodyId = body;
    m_active = true;
    m_commitRequested = false;
    update(ctx);
    return true;
}

void InteractiveOpController::update(const IopContext& ctx) {
    if (!m_active || m_bodyId < 0) return;
    if (!wantsLivePreview(ctx)) {
        // Live preview suppressed (recomputing it per change would freeze the
        // UI). Keep the snapshot shown and mark preview "ok" so Confirm still
        // computes + pushes the op once. pushOperation re-runs execute() and
        // refuses on failure, so a heavy commit that fails just does nothing.
        ctx.doc.updateBody(m_bodyId, m_snapshot);
        ctx.markMeshesDirty();
        m_previewOk = true;
        return;
    }
    // Reset to the snapshot, then run a fresh op against it so the live
    // preview tracks the current values exactly without compounding edits.
    ctx.doc.updateBody(m_bodyId, m_snapshot);
    ctx.markMeshesDirty();
    m_previewOk = false;
    try {
        std::unique_ptr<Operation> op = buildOp(ctx);
        if (op && op->execute(ctx.doc)) {
            ctx.markMeshesDirty();
            m_previewOk = true;
        } else {
            ctx.doc.updateBody(m_bodyId, m_snapshot);
        }
    } catch (...) {
        ctx.doc.updateBody(m_bodyId, m_snapshot);
    }
}

void InteractiveOpController::commit(const IopContext& ctx) {
    if (!m_active) return;
    // Roll back the preview; History::pushOperation re-runs the op cleanly
    // against the snapshot.
    ctx.doc.updateBody(m_bodyId, m_snapshot);
    if (!m_previewOk) {
        cancel(ctx);
        return;
    }
    std::unique_ptr<Operation> op = buildOp(ctx);
    if (op) ctx.history.pushOperation(std::move(op), ctx.doc);
    ctx.selection.clear();
    ctx.markMeshesDirty();
    cleanup();
}

void InteractiveOpController::cancel(const IopContext& ctx) {
    if (m_bodyId >= 0 && !m_snapshot.IsNull()) {
        ctx.doc.updateBody(m_bodyId, m_snapshot);
    }
    ctx.markMeshesDirty();
    cleanup();
}

void InteractiveOpController::cleanup() {
    m_active = false;
    m_commitRequested = false;
    m_previewOk = false;
    m_bodyId = -1;
    m_snapshot.Nullify();
    onCleanup();
}

void InteractiveOpController::renderPanel(const IopContext& ctx) {
    if (!m_active) return;

    // Same pinned top-right anchor + flag set every hand-written panel
    // used — known stable, no hover flicker.
    const float w = panelWidth();
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x +
                                       ImGui::GetWindowWidth() - w - 20.0f,
                                   ImGui::GetWindowPos().y + 50.0f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    char id[64];
    std::snprintf(id, sizeof(id), "##iop_%s", title());
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", title());
    ImGui::Separator();

    bool changed = false;
    panelBody(ctx, changed);
    if (changed) update(ctx);

    ImGui::Spacing();
    bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    bool esc   = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    bool doCommit = m_commitRequested || enter ||
                    ImGui::Button("Confirm (Enter)", ImVec2(120, 0));
    if (!doCommit) ImGui::SameLine();
    bool doCancel = !doCommit &&
                    (esc || ImGui::Button("Cancel (Esc)", ImVec2(120, 0)));
    ImGui::End();

    if (doCommit) commit(ctx);
    else if (doCancel) cancel(ctx);
}

} // namespace materializr

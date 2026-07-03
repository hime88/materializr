#include "ui_scale.h"
#include "../touch_mode.h"
#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../plugin/InteractiveTool.h"
#include "../plugin/PluginRegistry.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../core/SelectionManager.h"
#include "../core/NumParse.h"
#include "../modeling/PushPullOp.h"
#include "../modeling/Sketch.h"
#include <TopoDS.hxx>
#include <imgui.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace {

struct PushPullTarget {
    int sketchId;
    int regionIndex;
    int sourceBodyId;
    TopoDS_Face profile;
};

class PushPullTool : public materializr::InteractiveTool {
public:
    void begin(materializr::PluginContext& ctx) override {
        for (const auto& e : ctx.selection().getSelection()) {
            if (e.type == SelectionType::SketchRegion) {
                auto sketch = ctx.document().getSketch(e.sketchId);
                if (!sketch) continue;
                auto regions = sketch->buildRegions();
                if (e.subShapeIndex < 0 || e.subShapeIndex >= static_cast<int>(regions.size())) continue;
                PushPullTarget t;
                t.sketchId = e.sketchId;
                t.regionIndex = e.subShapeIndex;
                // Detached sketch = independent of its former host; don't
                // fuse into the stale source body (see beginPushPull).
                t.sourceBodyId = sketch->isDetachedFromBody()
                                     ? -1
                                     : sketch->getSourceBody();
                t.profile = regions[e.subShapeIndex].face;
                if (t.profile.IsNull()) continue;
                m_targets.push_back(t);
            } else if (e.type == SelectionType::Face && !e.shape.IsNull()) {
                PushPullTarget t;
                t.sketchId = -1;
                t.regionIndex = -1;
                t.sourceBodyId = e.bodyId;
                t.profile = TopoDS::Face(e.shape);
                if (t.profile.IsNull()) continue;
                m_targets.push_back(t);
            }
        }

        if (m_targets.empty()) { m_done = true; return; }

        m_distance = 5.0f;
        std::snprintf(m_inputBuf, sizeof(m_inputBuf), "%.1f", m_distance);
        m_inputFocus = true;

        updatePreview(ctx);
    }

    bool update(materializr::PluginContext&) override { return !m_done; }

    void commit(materializr::PluginContext& ctx) override {
        // The last preview push IS the committed result — clear the flag so a
        // later cancel() can never undo it.
        m_previewPushed = false;
        m_done = true;
        m_targets.clear();
        ctx.selection().clear();
        ctx.markMeshesDirty();
    }

    void cancel(materializr::PluginContext& ctx) override {
        if (m_previewPushed && ctx.history().canUndo()) {
            ctx.history().undo(ctx.document());
            m_previewPushed = false;
        }
        m_done = true;
        m_targets.clear();
        ctx.markMeshesDirty();
    }

    bool handleInput(materializr::PluginContext& ctx, const materializr::ToolInputEvent& event) override {
        if (event.type == materializr::ToolInputEvent::KeyPress) {
            if (event.key == 525) { commit(ctx); return true; }
            if (event.key == 526) { cancel(ctx); return true; }
        }
        return false;
    }

    void renderOverlay(materializr::PluginContext& ctx) override {
        ImGui::SetCursorPos(ImVec2(10, 30));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.85f, 1.0f, 1.0f));
        ImGui::Text(materializr::touchMode()
            ? "PUSH/PULL - Positive = extrude, Negative = cut. Drag the arrow, then Confirm / Cancel."
            : "PUSH/PULL - Positive = extrude, Negative = cut. Enter to confirm, Escape to cancel.");
        ImGui::PopStyleColor();

        ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 260,
                                        ImGui::GetWindowPos().y + 50));
        ImGui::SetNextWindowSize(materializr::uiSz(240, 0));
        ImGui::Begin("##PushPullInput", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("Distance (mm)");
        ImGui::Separator();

        if (m_inputFocus) {
            ImGui::SetKeyboardFocusHere();
            m_inputFocus = false;
        }

        // parseFinite: reject garbage / non-finite input ("1e999" -> inf
        // used to flow straight into the extrude) — the previous value stays.
        if (ImGui::InputText("##dist", m_inputBuf, sizeof(m_inputBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            (void)materializr::parseFinite(m_inputBuf, m_distance);
            updatePreview(ctx);
            commit(ctx);
        } else {
            float parsed = m_distance;
            if (materializr::parseFinite(m_inputBuf, parsed) &&
                std::abs(parsed - m_distance) > 0.01f) {
                m_distance = parsed;
                updatePreview(ctx);
            }
        }

        ImGui::SameLine();
        ImGui::Text("mm");

        if (ImGui::SliderFloat("##ppslider", &m_distance, -50.0f, 50.0f, "%.1f mm")) {
            std::snprintf(m_inputBuf, sizeof(m_inputBuf), "%.1f", m_distance);
            updatePreview(ctx);
        }

        ImGui::Spacing();
        if (ImGui::Button(materializr::btnConfirm(), ImVec2(110, 0))) { commit(ctx); }
        ImGui::SameLine();
        if (ImGui::Button(materializr::btnCancel(), ImVec2(110, 0))) { cancel(ctx); }

        ImGui::End();
    }

    std::string name() const override { return "Push/Pull"; }

private:
    void updatePreview(materializr::PluginContext& ctx) {
        if (m_previewPushed && ctx.history().canUndo()) {
            ctx.history().undo(ctx.document());
            m_previewPushed = false;
        }

        auto op = std::make_unique<PushPullOp>();
        std::vector<PushPullOp::Target> targets;
        for (const auto& t : m_targets) {
            PushPullOp::Target ot;
            ot.profile = t.profile;
            ot.sourceBodyId = t.sourceBodyId;
            targets.push_back(ot);
        }
        op->setTargets(std::move(targets));
        op->setDistance(static_cast<double>(m_distance));
        // Cascade plumbing: stamp each target's sketch+region (when present)
        // so a later constraint edit can re-derive its profile from the
        // updated sketch. Face-driven targets keep -1 (no cascade).
        for (size_t i = 0; i < m_targets.size(); ++i) {
            if (m_targets[i].sketchId >= 0) {
                op->setSketchSource(static_cast<int>(i),
                                    m_targets[i].sketchId,
                                    m_targets[i].regionIndex);
            }
        }
        if (ctx.history().pushOperation(std::move(op), ctx.document())) {
            m_previewPushed = true;
        }
        ctx.markMeshesDirty();
    }

    std::vector<PushPullTarget> m_targets;
    float m_distance = 5.0f;
    char m_inputBuf[32] = "5.0";
    bool m_inputFocus = true;
    bool m_previewPushed = false;
    bool m_done = false;
};

} // anonymous namespace

REGISTER_PLUGIN(PushPull, [](materializr::PluginContext& ctx) {
    // Both face and sketch-region push/pull are handled by the Application's
    // interactive arrow gizmo (default 0, click-drag to extrude/cut, with a live
    // measurement). That path needs viewport drag input, which plugin tools
    // don't receive, so there is no toolbar button here. We keep a Command
    // Palette entry as a fallback (it uses the typed popup).
    ctx.registerCommand({"Push/Pull", "",
        [](materializr::PluginContext& ctx) {
            materializr::PluginRegistry::instance().activateTool(
                std::make_unique<PushPullTool>(), ctx);
        }});
})

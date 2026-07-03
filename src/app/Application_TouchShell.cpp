// "im-touch" tablet shell (docs/im-touch-ui-plan.md — Phase 0 skeleton with
// the Phase 1 theme/widgets/icons applied).
//
// Replaces the desktop menu bar / dockspace / status bar with fixed chrome
// when Settings → im-touch UI is on: a top app bar (project, undo/redo,
// Focus, overflow menu), a left tool rail (catalogue lands in Phase 3), a
// right side panel (Items/History content lands in Phase 2), and a floating
// FULL pill. The 3D viewport is pinned into the remaining center rect by
// renderViewport() via m_touchVp* (set here, read there, every frame).
//
// All shell windows are ##-named + NoSavedSettings so they never touch
// imgui.ini — toggling back to the desktop shell restores its saved layout
// untouched.

#include "app/Application.h"
#include "app/Window.h"
#include "core/History.h"
#include "core/SelectionManager.h"
#include "modeling/SketchTool.h"   // SketchToolMode for the select-mode gate
#include "plugin/PluginContext.h"
#include "ui/HistoryPanel.h"
#include "ui/ItemsPanel.h"
#include "ui/Toolbar.h"       // ToolAction for the starter rail entries
#include "ui/TouchIcons.h"
#include "ui/TouchTheme.h"
#include "ui/TouchWidgets.h"
#include "touch_mode.h"
#include "ui_scale.h"

#include <imgui.h>
#include <string>

namespace {

constexpr ImGuiWindowFlags kShellWin =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;

} // namespace

namespace materializr {

// The ⋯/☰ menu shared by both shell variants: the full desktop menus,
// flattened one level, via the shared item lists (renderFileMenuItems & co.)
// so the shells and the desktop bar cannot drift. Caller does OpenPopup
// ("##TouchOverflow") on its trigger button.
void Application::renderTouchOverflowPopup() {
    if (!ImGui::BeginPopup("##TouchOverflow")) return;
    if (ImGui::BeginMenu(MZ_ICON_OPEN "  File")) {
        renderFileMenuItems(false);   // Settings is exposed at the bottom instead
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(MZ_ICON_UNDO "  Edit")) {
        renderEditMenuItems();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(MZ_ICON_EXTRUDE "  Tools")) {
        renderToolsMenuItems();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(MZ_ICON_FOCUS "  View")) {
        renderViewMenuItems();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(MZ_ICON_ABOUT "  Help")) {
        renderHelpMenuItems();
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(MZ_ICON_SETTINGS "  Settings...")) {
        m_settingsOrbitButton = m_orbitButton;
        m_settingsPanButton   = m_panButton;
        m_showSettings = true;
    }
    ImGui::EndPopup();
}

void Application::renderTouchShell() {
    if (m_imTouchLite) {
        renderTouchShellLite();
        return;
    }

    touchui::Scope style; // TouchTheme push/pop around the whole shell

    const float s = uiScale();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 wp = vp->WorkPos;
    const ImVec2 ws = vp->WorkSize;

    const float topH   = 60.0f * s;
    const float railW  = m_leftPanelHidden  ? 0.0f : 92.0f * s;
    const float rightW = m_rightPanelHidden ? 0.0f : 320.0f * s;

    // ── Top app bar ─────────────────────────────────────────────────────────
    ImGui::SetNextWindowPos(wp);
    ImGui::SetNextWindowSize(ImVec2(ws.x, topH));
    if (ImGui::Begin("##TouchTopBar", nullptr, kShellWin)) {
        const float pad = 14.0f * s;
        const float bh  = 44.0f * s;
        const float cy  = (topH - bh) * 0.5f; // vertical center for controls

        // ⋯ menu (top-left, nav-drawer style), then logo chip + name + /project.
        {
            const float bh0 = 44.0f * s;
            const float menuW = bh0 + 12.0f * s;    // ⋯ button + gap
            ImGui::SetCursorPos(ImVec2(pad, (topH - bh0) * 0.5f));
            if (touchui::iconButton("overflow", MZ_ICON_MORE, bh0))
                ImGui::OpenPopup("##TouchOverflow");
            renderTouchOverflowPopup();

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 win = ImGui::GetWindowPos();
            const float chip = 30.0f * s;
            const float lx = pad + menuW;
            const ImVec2 c0(win.x + lx, win.y + (topH - chip) * 0.5f);
            dl->AddRectFilled(c0, ImVec2(c0.x + chip, c0.y + chip),
                              ImGui::GetColorU32(touchui::accentFill()),
                              9.0f * s);
            ImGui::SetCursorPos(ImVec2(lx + chip + 10.0f * s,
                                       (topH - ImGui::GetTextLineHeight()) * 0.5f));
            ImGui::TextColored(touchui::textPrimary(), "Materializr");
            std::string pn = "New project";
            if (!m_currentProjectPath.empty()) {
                pn = m_currentProjectPath;
                auto slash = pn.find_last_of("/\\");
                if (slash != std::string::npos) pn = pn.substr(slash + 1);
            }
            ImGui::SameLine();
            ImGui::TextColored(touchui::textDim(), "/ %s", pn.c_str());
        }

        // Right-aligned controls: [Finish, Discard,] Undo, Redo, [Keyboard,]
        // Focus, ⋯. Finish/Discard appear in sketch mode — the two actions
        // that must never be hunted for.
        const float sp = 8.0f * s;
        const bool showKb = materializr::touchMode();
        // Square icon buttons in the right cluster: undo, redo, [keyboard].
        // (The ⋯ overflow moved to the top-left.)
        const int nSquare = showKb ? 3 : 2;
        auto pillW = [&](const char* label) {
            return bh + ImGui::CalcTextSize(label).x + 27.0f * s;
        };
        // Multi-Select toggle (the touch Ctrl stand-in): shown for 3D selection
        // and in sketch Select/move mode, hidden in the sketch draw tools where
        // adding to a selection is meaningless. Its old home was the bottom-left
        // viewport bar, where it overlapped the FULL pill.
        const bool showMulti = !m_inSketchMode ||
            (m_sketchTool && m_sketchTool->getMode() == SketchToolMode::Select);
        const float focusW = pillW("Focus");
        float total = bh * nSquare + focusW + sp * nSquare;
        if (m_inSketchMode)
            total += pillW("Finish") + pillW("Exit") + sp * 2;
        if (showMulti)
            total += pillW("Multi") + sp;
        float x = ws.x - pad - total;
        ImGui::SetCursorPos(ImVec2(x, cy));

        if (showMulti) {
            if (touchui::pillButton("multi", MZ_ICON_SELECT, "Multi",
                                    m_multiSelectToggle))
                m_multiSelectToggle = !m_multiSelectToggle;
            ImGui::SameLine(0.0f, sp);
            ImGui::SetCursorPosY(cy);
        }

        if (m_inSketchMode) {
            // Context-smart: a running draw tool finishes / cancels its own
            // shape; with no tool mid-placement, Finish/Exit act on the sketch.
            const bool toolRunning = m_sketchTool && m_sketchTool->isPlacing();
            if (touchui::pillButton("finish", MZ_ICON_FINISH, "Finish", true)) {
                if (toolRunning)
                    recordSketchMutation([&]{ m_sketchTool->onConfirm(); });
                else
                    handleToolAction(static_cast<int>(ToolAction::FinishSketch));
            }
            ImGui::SameLine(0.0f, sp);
            ImGui::SetCursorPosY(cy);
            if (touchui::pillButton("exit", MZ_ICON_DISCARD, "Exit")) {
                if (toolRunning)
                    m_sketchTool->onCancel();       // discard the in-progress shape
                else
                    handleToolAction(static_cast<int>(ToolAction::ExitSketchDiscard));
            }
            ImGui::SameLine(0.0f, sp);
            ImGui::SetCursorPosY(cy);
        }

        const bool histLocked = anyInteractivePreviewActive();
        ImGui::BeginDisabled(histLocked || !touchCanUndo());
        if (touchui::iconButton("undo", MZ_ICON_UNDO, bh)) touchUndo();
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, sp);
        ImGui::SetCursorPosY(cy);
        ImGui::BeginDisabled(histLocked || !m_history->canRedo());
        if (touchui::iconButton("redo", MZ_ICON_REDO, bh)) redoWithCascade();
        ImGui::EndDisabled();

        // Soft-keyboard toggle (the desktop menu bar's right-aligned item;
        // there's no menu bar here). Touch mode only, same flag.
        if (showKb) {
            ImGui::SameLine(0.0f, sp);
            ImGui::SetCursorPosY(cy);
            if (touchui::iconButton("kb", MZ_ICON_KEYBOARD, bh))
                m_softKeyboardForced = !m_softKeyboardForced;
        }

        // Focus: viewport + rail only (right panel hidden).
        ImGui::SameLine(0.0f, sp);
        ImGui::SetCursorPosY(cy);
        if (touchui::pillButton("focus", MZ_ICON_FOCUS, "Focus",
                                m_rightPanelHidden)) {
            m_rightPanelHidden = !m_rightPanelHidden;
            saveAppSettings();
        }
        // (The ⋯ overflow menu now lives at the top-left of this bar.)
    }
    ImGui::End();

    // ── Left tool rail — the selection-context tool catalogue. ──────────────
    if (railW > 0.0f) {
        ImGui::SetNextWindowPos(ImVec2(wp.x, wp.y + topH));
        ImGui::SetNextWindowSize(ImVec2(railW, ws.y - topH));
        if (ImGui::Begin("##TouchRail", nullptr,
                         kShellWin & ~ImGuiWindowFlags_NoScrollbar)) {
            ImGui::SetCursorPosX(10.0f * s);
            touchui::sectionHeader("Tools");
            if (m_toolbar) {
                for (const auto& tool : m_toolbar->railTools()) {
                    if (touchui::railButton(tool.label, tool.icon, tool.label,
                                            tool.active))
                        handleToolAction(static_cast<int>(tool.action));
                }
            }
        }
        ImGui::End();
    }

    // ── Right side panel (Phase 2: Items | History content) ─────────────────
    if (rightW > 0.0f) {
        ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x - rightW, wp.y + topH));
        ImGui::SetNextWindowSize(ImVec2(rightW, ws.y - topH));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, touchui::panelBg());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(14.0f * s, 12.0f * s));
        if (ImGui::Begin("##TouchRight", nullptr, kShellWin)) {
            static const char* kTabs[] = { "Items", "History" };
            const int tab = touchui::segmented("rightTabs", kTabs, 2,
                                               m_touchRightTab);
            if (tab != m_touchRightTab) {
                m_touchRightTab = tab;
                saveAppSettings();
            }
            // Scrolling body below the pinned switcher. The panels' content
            // renderers are the same code the desktop docks host — identical
            // behavior, different container.
            if (ImGui::BeginChild("##touchRightBody", ImVec2(0, 0), false)) {
                if (m_touchRightTab == 0) {
                    if (m_itemsPanel && m_itemsPanel->renderContent()) {
                        m_hoveredBodyId = -1;
                        m_meshesDirty = true;
                    }
                } else {
                    if (m_historyPanel && m_historyPanel->renderContent())
                        m_meshesDirty = true;
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    // ── FULL pill — floats bottom-left so it stays reachable when the rail is
    //    hidden. Toggles chrome-less (viewport-only) mode and back. ──────────
    {
        const float m = 14.0f * s;
        ImGui::SetNextWindowPos(ImVec2(wp.x + railW + m, wp.y + ws.y - m),
                                ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        if (ImGui::Begin("##TouchFull", nullptr,
                         kShellWin | ImGuiWindowFlags_AlwaysAutoResize)) {
            const bool full = m_leftPanelHidden && m_rightPanelHidden;
            if (touchui::pillButton("full", full ? MZ_ICON_FULL_EXIT : MZ_ICON_FULL,
                                    full ? "Exit" : "Full")) {
                const bool newHidden = !full;
                m_leftPanelHidden  = newHidden;
                m_rightPanelHidden = newHidden;
                saveAppSettings();
            }
        }
        ImGui::End();
    }

    // Center rect for renderViewport()'s pin.
    m_touchVpX = wp.x + railW;
    m_touchVpY = wp.y + topH;
    m_touchVpW = ws.x - railW - rightW;
    m_touchVpH = ws.y - topH;
}

// im-touch-lite: near-zero chrome. The viewport fills the whole work rect;
// everything else floats over it — project/selection chip (top-left), undo +
// keyboard + menu (top-right), the contextual tool catalogue as a centered
// bottom bar, a "+" create FAB (bottom-right), and an fps readout.
void Application::renderTouchShellLite() {
    touchui::Scope style;

    const float s = uiScale();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 wp = vp->WorkPos;
    const ImVec2 ws = vp->WorkSize;
    const float m = 12.0f * s; // float margin from the work-rect edges

    // Viewport underneath everything.
    m_touchVpX = wp.x;
    m_touchVpY = wp.y;
    m_touchVpW = ws.x;
    m_touchVpH = ws.y;

    const ImGuiWindowFlags kFloat = kShellWin | ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * s);

    // ── Project / selection chip (top-left) ─────────────────────────────────
    ImGui::SetNextWindowPos(ImVec2(wp.x + m, wp.y + m));
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("##LiteChip", nullptr, kFloat)) {
        // ⋯ menu at the far left (moved off the top-right cluster).
        if (touchui::iconButton("menu", MZ_ICON_MENU_BARS, 30.0f * s))
            ImGui::OpenPopup("##TouchOverflow");
        renderTouchOverflowPopup();
        ImGui::SameLine(0.0f, 8.0f * s);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float chip = 18.0f * s;
        const ImVec2 c0 = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(c0, ImVec2(c0.x + chip, c0.y + chip),
                          ImGui::GetColorU32(touchui::accentFill()), 5.0f * s);
        ImGui::Dummy(ImVec2(chip, chip));
        ImGui::SameLine();

        std::string pn = "New project";
        if (!m_currentProjectPath.empty()) {
            pn = m_currentProjectPath;
            auto slash = pn.find_last_of("/\\");
            if (slash != std::string::npos) pn = pn.substr(slash + 1);
        }
        // Selection summary: "· Face (2)" of the primary type, mirroring the
        // mockup's "mug.mzr · Face (1)".
        std::string sel;
        if (m_selection && m_selection->hasSelection()) {
            const SelectionType t = m_selection->primaryType();
            int n = 0;
            for (const auto& e : m_selection->getSelection())
                if (e.type == t) ++n;
            static const char* kNames[] = { "None", "Body", "Face", "Edge",
                                            "Vertex", "Sketch", "Region",
                                            "Plane", "Axis" };
            const int ti = static_cast<int>(t);
            if (ti > 0 && ti < 9) {
                sel = std::string("  ·  ") + kNames[ti] +
                      " (" + std::to_string(n) + ")";
            }
        }
        ImGui::TextColored(touchui::textPrimary(), "%s", pn.c_str());
        if (!sel.empty()) {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextColored(touchui::textDim(), "%s", sel.c_str());
        }
    }
    ImGui::End();

    // ── Undo / keyboard / menu (top-right) ──────────────────────────────────
    ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x - m, wp.y + m),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("##LiteTopRight", nullptr, kFloat)) {
        const float bh = 44.0f * s;
        // Multi-Select (moved off the bottom-left viewport bar): 3D selection and
        // sketch Select/move mode only.
        const bool showMulti = !m_inSketchMode ||
            (m_sketchTool && m_sketchTool->getMode() == SketchToolMode::Select);
        if (showMulti) {
            if (touchui::pillButton("multi", MZ_ICON_SELECT, "Multi",
                                    m_multiSelectToggle))
                m_multiSelectToggle = !m_multiSelectToggle;
            ImGui::SameLine(0.0f, 8.0f * s);
        }
        const bool histLocked = anyInteractivePreviewActive();
        ImGui::BeginDisabled(histLocked || !touchCanUndo());
        if (touchui::iconButton("undo", MZ_ICON_UNDO, bh)) touchUndo();
        ImGui::EndDisabled();
        if (materializr::touchMode()) {
            ImGui::SameLine(0.0f, 8.0f * s);
            if (touchui::iconButton("kb", MZ_ICON_KEYBOARD, bh))
                m_softKeyboardForced = !m_softKeyboardForced;
        }
        // (The ⋯ menu moved to the top-left chip.)
    }
    ImGui::End();

    // ── Contextual tool bar (bottom-center) — the same catalogue the full
    //    shell's rail uses, horizontal. Sketch mode appends Finish/Discard. ──
    ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x * 0.5f, wp.y + ws.y - m),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, touchui::panelBg());
    if (ImGui::Begin("##LiteToolBar", nullptr, kFloat)) {
        if (m_toolbar) {
            bool first = true;
            for (const auto& tool : m_toolbar->railTools()) {
                if (!first) ImGui::SameLine(0.0f, 4.0f * s);
                first = false;
                if (touchui::railButton(tool.label, tool.icon, tool.label,
                                        tool.active, 64.0f * s))
                    handleToolAction(static_cast<int>(tool.action));
            }
            if (m_inSketchMode) {
                const float pillY = ImGui::GetCursorPosY() - 62.0f * s +
                                    (62.0f - 44.0f) * 0.5f * s;
                const bool toolRunning = m_sketchTool && m_sketchTool->isPlacing();
                ImGui::SameLine(0.0f, 12.0f * s);
                ImGui::SetCursorPosY(pillY);
                if (touchui::pillButton("finish", MZ_ICON_FINISH, "Finish", true)) {
                    if (toolRunning)
                        recordSketchMutation([&]{ m_sketchTool->onConfirm(); });
                    else
                        handleToolAction(static_cast<int>(ToolAction::FinishSketch));
                }
                ImGui::SameLine(0.0f, 8.0f * s);
                ImGui::SetCursorPosY(pillY);
                if (touchui::pillButton("exit", MZ_ICON_DISCARD, "Exit")) {
                    if (toolRunning)
                        m_sketchTool->onCancel();
                    else
                        handleToolAction(static_cast<int>(ToolAction::ExitSketchDiscard));
                }
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();

    // ── "+" create FAB (bottom-right) ───────────────────────────────────────
    ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x - m, wp.y + ws.y - m),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("##LiteFab", nullptr, kFloat)) {
        if (touchui::fab("create", MZ_ICON_ADD))
            ImGui::OpenPopup("##LiteCreate");
        if (ImGui::BeginPopup("##LiteCreate")) {
            if (ImGui::MenuItem(MZ_ICON_SKETCH "  New Sketch"))
                handleToolAction(static_cast<int>(ToolAction::StartSketch));
            if (m_pluginContext) {
                ImGui::Separator();
                if (ImGui::MenuItem(MZ_ICON_EXTRUDE "  Box"))
                    m_pluginContext->requestInteractiveOp("PrimitiveBox");
                if (ImGui::MenuItem(MZ_ICON_CIRCLE "  Cylinder"))
                    m_pluginContext->requestInteractiveOp("PrimitiveCylinder");
                if (ImGui::MenuItem(MZ_ICON_CIRCLE "  Sphere"))
                    m_pluginContext->requestInteractiveOp("PrimitiveSphere");
                if (ImGui::MenuItem(MZ_ICON_POLYGON "  Cone"))
                    m_pluginContext->requestInteractiveOp("PrimitiveCone");
                if (ImGui::MenuItem(MZ_ICON_CIRCLE "  Torus"))
                    m_pluginContext->requestInteractiveOp("PrimitiveTorus");
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();

    // ── fps readout (bottom-left, like the mockup's "60 fps") ───────────────
    ImGui::SetNextWindowPos(ImVec2(wp.x + m, wp.y + ws.y - m),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("##LiteFps", nullptr, kFloat)) {
        ImGui::TextColored(touchui::textDim(), "%.0f fps",
                           ImGui::GetIO().Framerate);
    }
    ImGui::End();

    ImGui::PopStyleVar(); // WindowRounding
}

} // namespace materializr

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
#include "core/Document.h"
#include "core/History.h"
#include "core/Operation.h"
#include "core/SelectionManager.h"
#include "modeling/SketchEditOp.h"      // lite timeline: Apply cascade targets
#include "modeling/SketchTransformOp.h"
#include "modeling/SketchTool.h"   // SketchToolMode for the select-mode gate
#include "plugin/PluginContext.h"
#include "ui/HistoryPanel.h"
#include "ui/ItemsPanel.h"
#include "ui/PropertiesPanel.h"
#include "ui/Toolbar.h"       // ToolAction for the starter rail entries
#include "ui/TouchIcons.h"
#include "ui/TouchTheme.h"
#include "ui/TouchWidgets.h"
#include "ui/LogoTexture.h"
#include "gl_common.h"
#include "touch_mode.h"
#include "ui_scale.h"

#include <cfloat> // FLT_MAX (lite tool bar height constraint)
#include <imgui.h>
#include <cstdint>
#include <set>
#include <string>

namespace {

constexpr ImGuiWindowFlags kShellWin =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;

// The Materializr logo (embedded RGBA, LogoTexture.h) as a lazily-uploaded
// GL texture for the top-bar chip. Uploaded once; lives with the GL context.
ImTextureID logoTexture() {
    static GLuint tex = 0;
    if (!tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, materializr::kLogoTexW,
                     materializr::kLogoTexH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     materializr::kLogoTexRGBA);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    return (ImTextureID)(intptr_t)tex;
}

// Icon for a history step's box in the lite timeline, by op typeId. Reloaded
// steps (ReplayOp) report the ORIGINAL op's typeId, so they map the same.
const char* liteStepIcon(const std::string& t) {
    if (t == "extrude")                    return MZ_ICON_EXTRUDE;
    if (t == "pushpull" || t == "moveface" || t == "move_hole")
                                           return MZ_ICON_PUSHPULL;
    if (t == "revolve")                    return MZ_ICON_LATHE;
    if (t == "loft" || t == "sweep")       return MZ_ICON_SPLINE;
    if (t == "fillet")                     return MZ_ICON_FILLET;
    if (t == "chamfer")                    return MZ_ICON_CHAMFER;
    if (t == "shell")                      return MZ_ICON_SHELL;
    if (t == "boolean")                    return MZ_ICON_SUBTRACT;
    if (t == "split_body")                 return MZ_ICON_TRIM;
    if (t == "delete")                     return MZ_ICON_DELETE;
    if (t == "defeature")                  return MZ_ICON_REPAIR;
    if (t == "mirror")                     return MZ_ICON_MIRROR;
    if (t == "pattern")                    return MZ_ICON_PATTERN;
    if (t == "copy" || t == "duplicate_sketch")
                                           return MZ_ICON_COPY;
    if (t == "scale_face" || t == "resize_cylindrical" || t == "taper")
                                           return MZ_ICON_SCALE;
    if (t == "primitive")                  return MZ_ICON_PRIMITIVE;
    if (t == "thread")                     return MZ_ICON_ROTATE;
    if (t == "construction_plane" || t == "construction_axis")
                                           return MZ_ICON_AXES;
    if (t == "sketchedit" || t == "combine_sketches" || t == "project_sketch")
                                           return MZ_ICON_SKETCH;
    if (t == "transform" || t == "axis_transform" || t == "plane_transform" ||
        t == "sketchtransform" || t == "align")
                                           return MZ_ICON_MOVE;
    return MZ_ICON_EDIT;
}

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

    // Hover tooltip on the previous item — honours the same "toolbar
    // tooltips" setting the classic toolbar uses. (Hover exists on desktop
    // im-touch and on a tablet with a mouse; bare-finger use never sees it.)
    auto tip = [&](const char* text) {
        if (!m_showToolbarTooltips || !text) return;
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };

    const float topH   = 60.0f * s;
    const float railW  = m_leftPanelHidden  ? 0.0f : m_touchRailW * s; // user-resizable
    const float rightW = m_rightPanelHidden ? 0.0f : m_touchRightW * s; // user-resizable

    // ── Top app bar ─────────────────────────────────────────────────────────
    // The fixed bars are edge-flush strips — opt out of the theme's global
    // WindowRounding (their corners would notch against the viewport). The
    // pop right after Begin keeps rounding intact for anything opened inside
    // (modals, popups pick up style at their own Begin).
    auto beginFlushBar = [](const char* name, ImGuiWindowFlags flags) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        const bool open = ImGui::Begin(name, nullptr, flags);
        ImGui::PopStyleVar();
        return open;
    };

    ImGui::SetNextWindowPos(wp);
    ImGui::SetNextWindowSize(ImVec2(ws.x, topH));
    if (beginFlushBar("##TouchTopBar", kShellWin)) {
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
            tip("Menu: file, edit, view, help and settings");
            renderTouchOverflowPopup();

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 win = ImGui::GetWindowPos();
            const float chip = 30.0f * s;
            const float lx = pad + menuW;
            const ImVec2 c0(win.x + lx, win.y + (topH - chip) * 0.5f);
            dl->AddImageRounded(logoTexture(), c0,
                                ImVec2(c0.x + chip, c0.y + chip),
                                ImVec2(0, 0), ImVec2(1, 1),
                                IM_COL32_WHITE, 7.0f * s);
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
        // Multi-Select toggle (the touch Ctrl stand-in): shown for 3D selection
        // and in sketch Select/move mode, hidden in the sketch draw tools where
        // adding to a selection is meaningless. Its old home was the bottom-left
        // viewport bar, where it overlapped the FULL pill.
        const bool showMulti = !m_inSketchMode ||
            (m_sketchTool && m_sketchTool->getMode() == SketchToolMode::Select);
        // Context-clear labels so nobody discards a whole sketch by reflex: while
        // a draw tool is running the buttons act on its SHAPE (Finish / Cancel);
        // with no tool running (e.g. Select/move) they act on the SKETCH, and say
        // so — "Finish Sketch" / "Discard Sketch".
        const bool toolRunning = m_inSketchMode && m_sketchTool &&
                                 m_sketchTool->isPlacing();
        const char* finishLbl = toolRunning ? "Finish" : "Finish Sketch";
        const char* exitLbl   = toolRunning ? "Cancel" : "Discard Sketch";
        // Right-align the cluster with EXACT widths (touchui::pillButtonWidth
        // shares pillButton's sizing) — the previous estimate overshot per
        // pill, leaving an awkward gap against the right edge.
        // Focus is a 3-position cycle: full UI -> side panel hidden ->
        // viewport only (which retired the old bottom-left FULL pill). The
        // label/icon reflect the CURRENT state; width uses the current label.
        const int focusState = m_leftPanelHidden ? 2 : (m_rightPanelHidden ? 1 : 0);
        const char* focusIcon = focusState == 2 ? MZ_ICON_FULL_EXIT : MZ_ICON_FOCUS;
        const char* focusLbl  = focusState == 2 ? "Full" : "Focus";
        float total = bh * nSquare + sp * nSquare +
                      touchui::pillButtonWidth(focusIcon, focusLbl);
        if (m_inSketchMode)
            total += touchui::pillButtonWidth(MZ_ICON_FINISH, finishLbl) +
                     touchui::pillButtonWidth(MZ_ICON_DISCARD, exitLbl) + sp * 2;
        if (showMulti)
            total += touchui::pillButtonWidth(MZ_ICON_SELECT, "Multi") + sp;
        float x = ws.x - pad - total;
        ImGui::SetCursorPos(ImVec2(x, cy));

        if (showMulti) {
            if (touchui::pillButton("multi", MZ_ICON_SELECT, "Multi",
                                    m_multiSelectToggle))
                m_multiSelectToggle = !m_multiSelectToggle;
            tip("Multi-select: add taps to the current selection\n"
                "(the touch equivalent of holding Ctrl)");
            ImGui::SameLine(0.0f, sp);
            ImGui::SetCursorPosY(cy);
        }

        if (m_inSketchMode) {
            if (touchui::pillButton("finish", MZ_ICON_FINISH, finishLbl, true)) {
                if (toolRunning)
                    recordSketchMutation([&]{ m_sketchTool->onConfirm(); });
                else
                    handleToolAction(static_cast<int>(ToolAction::FinishSketch));
            }
            tip(toolRunning
                    ? "Finish the current shape, keeping the points placed"
                    : "Leave sketch mode, keeping the sketch");
            ImGui::SameLine(0.0f, sp);
            ImGui::SetCursorPosY(cy);
            if (touchui::pillButton("exit", MZ_ICON_DISCARD, exitLbl)) {
                if (toolRunning)
                    m_sketchTool->onCancel();       // discard the in-progress shape
                else
                    // Whole-sketch discard is destructive — confirm first so a
                    // misclick can't throw the sketch away.
                    ImGui::OpenPopup("Discard sketch?");
            }
            tip(toolRunning
                    ? "Cancel the in-progress shape"
                    : "Throw the sketch away and leave (asks to confirm)");
            if (ImGui::BeginPopupModal("Discard sketch?", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(
                    "Leave the sketch and throw away its changes?");
                ImGui::Spacing();
                const float bw = 150.0f * s;
                if (ImGui::Button("Discard Sketch", ImVec2(bw, 44.0f * s))) {
                    ImGui::CloseCurrentPopup();
                    handleToolAction(static_cast<int>(ToolAction::ExitSketchDiscard));
                }
                ImGui::SameLine();
                if (ImGui::Button("Keep Editing", ImVec2(bw, 44.0f * s)))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::SameLine(0.0f, sp);
            ImGui::SetCursorPosY(cy);
        }

        const bool histLocked = anyInteractivePreviewActive();
        ImGui::BeginDisabled(histLocked || !touchCanUndo());
        if (touchui::iconButton("undo", MZ_ICON_UNDO, bh)) touchUndo();
        ImGui::EndDisabled();
        tip("Undo (in a sketch: backs out the in-progress shape first)");
        ImGui::SameLine(0.0f, sp);
        ImGui::SetCursorPosY(cy);
        ImGui::BeginDisabled(histLocked || !m_history->canRedo());
        if (touchui::iconButton("redo", MZ_ICON_REDO, bh)) redoWithCascade();
        ImGui::EndDisabled();
        tip("Redo");

        // Soft-keyboard toggle (the desktop menu bar's right-aligned item;
        // there's no menu bar here). Touch mode only, same flag.
        if (showKb) {
            ImGui::SameLine(0.0f, sp);
            ImGui::SetCursorPosY(cy);
            if (touchui::iconButton("kb", MZ_ICON_KEYBOARD, bh))
                m_softKeyboardForced = !m_softKeyboardForced;
            tip("Toggle the on-screen keyboard");
        }

        // Focus cycle: 0 = everything, 1 = side panel hidden, 2 = viewport
        // only (rail hidden too). One button, three positions.
        ImGui::SameLine(0.0f, sp);
        ImGui::SetCursorPosY(cy);
        if (touchui::pillButton("focus", focusIcon, focusLbl,
                                focusState != 0)) {
            const int next = (focusState + 1) % 3;
            m_rightPanelHidden = next >= 1;
            m_leftPanelHidden  = next == 2;
            saveAppSettings();
        }
        tip(focusState == 0 ? "Focus: hide the side panel (tap again for viewport only)"
            : focusState == 1 ? "Focus: hide the tool rail too (viewport only)"
                              : "Bring the panels back");
        // (The ⋯ overflow menu now lives at the top-left of this bar.)
    }
    ImGui::End();

    // ── Left tool rail — the selection-context tool catalogue. ──────────────
    if (railW > 0.0f) {
        ImGui::SetNextWindowPos(ImVec2(wp.x, wp.y + topH));
        ImGui::SetNextWindowSize(ImVec2(railW, ws.y - topH));
        if (beginFlushBar("##TouchRail",
                          kShellWin & ~ImGuiWindowFlags_NoScrollbar)) {
            ImGui::SetCursorPosX(10.0f * s);
            touchui::sectionHeader("Tools");

            // Grouped popups for the create tools the contextual rail omits —
            // one tap away (not buried in the ⋯ menu). On a touch screen they
            // get roomier rows (bigger padding + row gap) for finger targets.
            const bool nothingSel = !m_inSketchMode &&
                (!m_selection || !m_selection->hasSelection());
            const bool touchPad = materializr::touchMode();
            auto pushPopupPad = [&] {
                if (!touchPad) return;
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                    ImVec2(16.0f * s, 13.0f * s));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(12.0f * s, 14.0f * s));
            };
            auto popPopupPad = [&] { if (touchPad) ImGui::PopStyleVar(2); };
            auto constructGroup = [&] {
                if (touchui::railButton("constructGroup", MZ_ICON_FOCUS,
                                        "Construct", false))
                    ImGui::OpenPopup("##railConstruct");
                tip("Create a construction plane or axis derived from the selection");
                pushPopupPad();
                if (ImGui::BeginPopup("##railConstruct")) {
                    renderConstructionMenuItems();
                    ImGui::EndPopup();
                }
                popPopupPad();
            };

            // With nothing selected the create tools lead (Sketch on… at the
            // very top) and railTools' Measure lands at the bottom; with a
            // selection the contextual tools lead and Construct follows.
            if (nothingSel) {
                if (touchui::railButton("sketchOnGroup", MZ_ICON_SKETCH,
                                        "Sketch on...", false))
                    ImGui::OpenPopup("##railSketchOn");
                tip("Start a sketch on a world plane (XY / XZ / YZ)");
                pushPopupPad();
                if (ImGui::BeginPopup("##railSketchOn")) {
                    if (ImGui::MenuItem("XY plane"))
                        handleToolAction(static_cast<int>(ToolAction::StartSketchXY));
                    if (ImGui::MenuItem("XZ plane"))
                        handleToolAction(static_cast<int>(ToolAction::StartSketchXZ));
                    if (ImGui::MenuItem("YZ plane"))
                        handleToolAction(static_cast<int>(ToolAction::StartSketchYZ));
                    ImGui::EndPopup();
                }
                popPopupPad();
                if (touchui::railButton("primGroup", MZ_ICON_PRIMITIVE,
                                        "Primitive", false))
                    ImGui::OpenPopup("##railPrimitive");
                tip("Add a primitive solid: box, cylinder, sphere, cone or torus");
                pushPopupPad();
                if (ImGui::BeginPopup("##railPrimitive")) {
                    if (m_pluginContext) {
                        if (ImGui::MenuItem("Box"))
                            m_pluginContext->requestInteractiveOp("PrimitiveBox");
                        if (ImGui::MenuItem("Cylinder"))
                            m_pluginContext->requestInteractiveOp("PrimitiveCylinder");
                        if (ImGui::MenuItem("Sphere"))
                            m_pluginContext->requestInteractiveOp("PrimitiveSphere");
                        if (ImGui::MenuItem("Cone"))
                            m_pluginContext->requestInteractiveOp("PrimitiveCone");
                        if (ImGui::MenuItem("Torus"))
                            m_pluginContext->requestInteractiveOp("PrimitiveTorus");
                    }
                    ImGui::EndPopup();
                }
                popPopupPad();
                constructGroup();
            }

            if (m_toolbar) {
                for (const auto& tool : m_toolbar->railTools()) {
                    if (touchui::railButton(tool.label, tool.icon, tool.label,
                                            tool.active))
                        handleToolAction(static_cast<int>(tool.action));
                    tip(tool.tip);
                }
            }

            // Construction stays reachable with a selection too (its options
            // derive from the selection) — after the contextual tools.
            if (!m_inSketchMode && !nothingSel)
                constructGroup();
        }
        ImGui::End();
    }

    // ── Right side panel (Items | History & Properties) ─────────────────────
    if (rightW > 0.0f) {
        ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x - rightW, wp.y + topH));
        ImGui::SetNextWindowSize(ImVec2(rightW, ws.y - topH));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, touchui::panelBg());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(14.0f * s, 12.0f * s));
        if (beginFlushBar("##TouchRight", kShellWin)) {
            // Left-edge drag splitter: the panel is resizable. Screen-space
            // strip along the window's left edge; drag left = wider (the
            // panel is right-anchored). Width persists via m_touchRightW.
            {
                const ImVec2 winPos = ImGui::GetWindowPos();
                const ImVec2 keep = ImGui::GetCursorPos();
                ImGui::SetCursorScreenPos(winPos);
                ImGui::InvisibleButton("##rightResize",
                                       ImVec2(10.0f * s, ws.y - topH));
                const bool active = ImGui::IsItemActive();
                if (ImGui::IsItemHovered() || active)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (active) {
                    m_touchRightW -= ImGui::GetIO().MouseDelta.x / s;
                    if (m_touchRightW < 200.0f) m_touchRightW = 200.0f;
                    if (m_touchRightW > 520.0f) m_touchRightW = 520.0f;
                }
                if (ImGui::IsItemDeactivated()) saveAppSettings();
                // Grip hint: hairline normally, accent while grabbed/hovered.
                ImGui::GetWindowDrawList()->AddRectFilled(
                    winPos, ImVec2(winPos.x + 3.0f * s, winPos.y + ws.y - topH),
                    ImGui::GetColorU32(active ? touchui::accentDeep()
                                              : touchui::hairline()));
                ImGui::SetCursorPos(keep);
            }

            // Properties lives inside the History tab (below the steps) but
            // the tab just says "History" — that's where people expect it,
            // and the short label keeps the switcher clean.
            static const char* kTabs[] = { "Items", "History" };
            if (m_touchRightTab > 1) m_touchRightTab = 1; // migrate old 3-tab value
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
                    // History on top, Properties beneath — one tab hosts both
                    // (the step list and the editor for the selected step /
                    // selection live together). Properties rarely holds much,
                    // so it gets the bottom third at most.
                    const float histH = ImGui::GetContentRegionAvail().y * 0.667f;
                    if (ImGui::BeginChild("##histHalf", ImVec2(0, histH), false)) {
                        if (m_historyPanel) {
                            // Undo/redo live in the shell's top bar; the panel
                            // shows its step counter beside the label instead.
                            m_historyPanel->setShowUndoRedo(false);
                            if (m_historyPanel->renderContent())
                                m_meshesDirty = true;
                        }
                    }
                    ImGui::EndChild();
                    ImGui::Separator();
                    touchui::sectionHeader("Properties");
                    if (ImGui::BeginChild("##propsHalf", ImVec2(0, 0), false)) {
                        if (m_propertiesPanel && m_propertiesPanel->renderContent())
                            m_meshesDirty = true;
                    }
                    ImGui::EndChild();
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    // (The old bottom-left FULL pill folded into the Focus cycle above.)

    // ── Edge tabs: semicircular grips on the panel/viewport boundaries.
    //    Tap = pop the panel out / back in; drag = resize it (no more hunting
    //    for the hairline splitter). Drawn after the panels so they sit on top.
    {
        const float tabW = 16.0f * s, tabH = 72.0f * s;
        const float midY = wp.y + topH + (ws.y - topH) * 0.5f;
        // side: -1 = tab bulges rightward (left rail edge), +1 = leftward.
        auto edgeTab = [&](const char* id, float edgeX, int side,
                           bool* hiddenVar, float* widthVar, float minW,
                           float maxW, bool* dragged) {
            // Anchor by the window's LEFT edge (pivot 0) so the flat side lands
            // exactly on the panel boundary. Two gotchas the earlier versions
            // hit: (1) right-edge anchoring (pivot 1) offset the whole tab by
            // the window's real width; (2) tabW (~25px) is below the default
            // WindowMinSize (32), so ImGui clamped the window wider than tabW
            // and GetWindowPos then disagreed with the requested pos. Fix: push
            // WindowMinSize 0 AND derive geometry from our own winLeft, never
            // GetWindowPos — so cx = winLeft(+tabW) is exactly edgeX.
            const float winLeft = (side < 0) ? edgeX : edgeX - tabW;
            ImGui::SetNextWindowPos(ImVec2(winLeft, midY), ImGuiCond_Always,
                                    ImVec2(0.0f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(tabW, tabH));
            ImGui::SetNextWindowBgAlpha(0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            if (ImGui::Begin(id, nullptr, kShellWin)) {
                const ImVec2 p(winLeft, midY - tabH * 0.5f); // our exact rect
                ImGui::SetCursorScreenPos(p);
                ImGui::InvisibleButton("##grip", ImVec2(tabW, tabH));
                const bool hov = ImGui::IsItemHovered();
                const bool act = ImGui::IsItemActive();
                if (hov || act)
                    ImGui::SetMouseCursor(*hiddenVar ? ImGuiMouseCursor_Hand
                                                     : ImGuiMouseCursor_ResizeEW);
                // Drag = resize (visible panel only); a few px of slop
                // separates a tap from a drag.
                if (act && !*hiddenVar && ImGui::IsMouseDragging(0, 4.0f)) {
                    *dragged = true;
                    *widthVar += (side < 0 ? 1.0f : -1.0f) *
                                 ImGui::GetIO().MouseDelta.x / s;
                    if (*widthVar < minW) *widthVar = minW;
                    if (*widthVar > maxW) *widthVar = maxW;
                }
                if (ImGui::IsItemDeactivated()) {
                    if (*dragged) {
                        saveAppSettings();          // resize ended
                    } else {
                        *hiddenVar = !*hiddenVar;   // tap: pop out / back in
                        saveAppSettings();
                    }
                    *dragged = false;
                }

                ImDrawList* dl = ImGui::GetWindowDrawList();
                // Semicircle bulging INTO the viewport.
                const float cx = side < 0 ? p.x : p.x + tabW;
                const ImVec2 c(cx, p.y + tabH * 0.5f);
                const float pi = 3.1415926f;
                dl->PathArcTo(c, tabW - 1.0f * s,
                              side < 0 ? -pi * 0.5f : pi * 0.5f,
                              side < 0 ? pi * 0.5f : pi * 1.5f, 24);
                dl->PathFillConvex(ImGui::GetColorU32(
                    (hov || act) ? touchui::rowBg() : touchui::panelBg()));
                // Chevron points where the panel edge will MOVE on tap:
                // hidden -> panel pops toward the viewport; visible -> away.
                const float bulge = side < 0 ? 1.0f : -1.0f; // toward viewport
                const float dir = *hiddenVar ? bulge : -bulge;
                const float chw = 4.0f * s, chh = 5.0f * s;
                const ImVec2 tipPt(c.x + bulge * 6.0f * s + dir * chw * 0.5f, c.y);
                dl->AddLine(ImVec2(tipPt.x - dir * chw, c.y - chh), tipPt,
                            ImGui::GetColorU32(touchui::textDim()), 2.0f * s);
                dl->AddLine(ImVec2(tipPt.x - dir * chw, c.y + chh), tipPt,
                            ImGui::GetColorU32(touchui::textDim()), 2.0f * s);
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
        };
        edgeTab("##railTab", wp.x + railW, -1, &m_leftPanelHidden,
                &m_touchRailW, 64.0f, 160.0f, &m_railTabDragged);
        edgeTab("##rightTab", wp.x + ws.x - rightW, +1, &m_rightPanelHidden,
                &m_touchRightW, 200.0f, 520.0f, &m_rightTabDragged);
    }

    // Center rect for renderViewport()'s pin.
    m_touchVpX = wp.x + railW;
    m_touchVpY = wp.y + topH;
    m_touchVpW = ws.x - railW - rightW;
    m_touchVpH = ws.y - topH;
}

// im-touch-lite: near-zero chrome. The viewport fills the whole work rect;
// everything else floats over it — project/selection chip (top-left), undo +
// keyboard + menu (top-right), the contextual tool catalogue on the left
// edge, the Fusion-style history timeline (bottom-center), a "+" create FAB
// (bottom-right), and an fps readout.
void Application::renderTouchShellLite() {
    touchui::Scope style;

    const float s = uiScale();
    auto tip = [&](const char* text) {   // same policy as the full shell
        if (!m_showToolbarTooltips || !text) return;
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 wp = vp->WorkPos;
    const ImVec2 ws = vp->WorkSize;
    const float m = 12.0f * s; // float margin from the work-rect edges

    // Viewport underneath everything.
    m_touchVpX = wp.x;
    m_touchVpY = wp.y;
    m_touchVpW = ws.x;
    m_touchVpH = ws.y;

    // These overlays float ON TOP of the full-screen viewport window, which is
    // NoBringToFrontOnFocus (pinned to the back). They must NOT share that flag:
    // if they do, z-order falls to ImGui's persistent creation order — which is
    // fine when the app LAUNCHES straight into lite (overlays created early), but
    // when the user TOGGLES to lite at runtime the viewport was created first and
    // stays in front, burying every overlay (the "invisible lite shell" bug).
    // Dropping the flag makes them ordinary foreground windows that come to front
    // on appearance, so they render above the back-pinned viewport every time.
    const ImGuiWindowFlags kFloat =
        (kShellWin & ~ImGuiWindowFlags_NoBringToFrontOnFocus) |
        ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * s);

    // ── Project / selection chip (top-left) ─────────────────────────────────
    ImGui::SetNextWindowPos(ImVec2(wp.x + m, wp.y + m));
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("##LiteChip", nullptr, kFloat)) {
        // ⋯ menu at the far left (moved off the top-right cluster).
        if (touchui::iconButton("menu", MZ_ICON_MENU_BARS, 30.0f * s))
            ImGui::OpenPopup("##TouchOverflow");
        tip("Menu: file, edit, view, help and settings");
        renderTouchOverflowPopup();
        ImGui::SameLine(0.0f, 8.0f * s);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float chip = 18.0f * s;
        const ImVec2 c0 = ImGui::GetCursorScreenPos();
        dl->AddImageRounded(logoTexture(), c0,
                            ImVec2(c0.x + chip, c0.y + chip),
                            ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, 4.0f * s);
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
            tip("Multi-select: add taps to the current selection\n"
                "(the touch equivalent of holding Ctrl)");
            ImGui::SameLine(0.0f, 8.0f * s);
        }
        const bool histLocked = anyInteractivePreviewActive();
        ImGui::BeginDisabled(histLocked || !touchCanUndo());
        if (touchui::iconButton("undo", MZ_ICON_UNDO, bh)) touchUndo();
        ImGui::EndDisabled();
        tip("Undo (in a sketch: backs out the in-progress shape first)");
        if (materializr::touchMode()) {
            ImGui::SameLine(0.0f, 8.0f * s);
            if (touchui::iconButton("kb", MZ_ICON_KEYBOARD, bh))
                m_softKeyboardForced = !m_softKeyboardForced;
            tip("Toggle the on-screen keyboard");
        }
        // Model-tree toggle (the transparent Bodies/Sketches/Construction
        // overlay on the right edge).
        ImGui::SameLine(0.0f, 8.0f * s);
        if (touchui::iconButton("tree", MZ_ICON_ITEMS, bh)) {
            m_imTouchLiteTree = !m_imTouchLiteTree;
            saveAppSettings();
        }
        // History-timeline toggle (the Fusion-style step boxes at the bottom).
        ImGui::SameLine(0.0f, 8.0f * s);
        if (touchui::iconButton("hist", MZ_ICON_HISTORY, bh)) {
            m_imTouchLiteTimeline = !m_imTouchLiteTimeline;
            saveAppSettings();
        }
        // (The ⋯ menu moved to the top-left chip.)
    }
    ImGui::End();

    // ── Transparent model tree (right edge) — the structure the full shell's
    //    Items panel shows, display-focused: visibility checkbox + name +
    //    tap-to-select. Deep actions (rename, folders, export) live in the
    //    full shell; this stays a lite-only overlay.
    if (m_imTouchLiteTree && m_document) {
        ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x - m, wp.y + ws.y * 0.5f),
                                ImGuiCond_Always, ImVec2(1.0f, 0.5f));
        const float treeW = 250.0f * s;
        ImGui::SetNextWindowSizeConstraints(ImVec2(treeW, 0),
                                            ImVec2(treeW, ws.y - 2.0f * m));
        ImGui::SetNextWindowBgAlpha(0.35f);
        if (ImGui::Begin("##LiteTree", nullptr,
                         kFloat & ~ImGuiWindowFlags_NoScrollbar)) {
            // Selected ids per kind, collected once.
            std::set<int> selB, selS, selP, selA;
            if (m_selection)
                for (const auto& e : m_selection->getSelection()) {
                    if (e.type == SelectionType::Body   && e.bodyId   >= 0) selB.insert(e.bodyId);
                    if (e.type == SelectionType::Sketch && e.sketchId >= 0) selS.insert(e.sketchId);
                    if (e.type == SelectionType::Plane  && e.planeId  >= 0) selP.insert(e.planeId);
                    if (e.type == SelectionType::Axis   && e.axisId   >= 0) selA.insert(e.axisId);
                }
            // Plain tap = single-select; with the Multi toggle armed, bodies
            // toggle in/out of the selection (same semantics as the Items
            // panel's Ctrl+click). Body picks are navigation-only, exactly
            // like the Items panel's rows; other kinds select plainly.
            auto pick = [&](SelectionEntry entry, bool multiOk) {
                if (!m_selection) return;
                if (multiOk && m_multiSelectToggle) m_selection->toggleSelection(entry);
                else                                m_selection->select(entry);
                if (entry.type == SelectionType::Body)
                    m_selection->setNavigationOnly(true);
            };

            bool any = false;
            const auto bodyIds = m_document->getAllBodyIds();
            if (!bodyIds.empty()) {
                any = true;
                touchui::sectionHeader("Bodies");
                for (int id : bodyIds) {
                    ImGui::PushID(id);
                    bool visible = m_document->isBodyVisible(id);
                    auto act = touchui::listRow("body", &visible,
                                                m_document->getBodyName(id).c_str(),
                                                selB.count(id) > 0,
                                                /*withOverflow=*/false);
                    if (act.toggled) m_document->setBodyVisible(id, visible);
                    if (act.clicked) {
                        SelectionEntry e;
                        e.type = SelectionType::Body;
                        e.bodyId = id;
                        // Parity with ItemsPanel::makeEntry — downstream code
                        // (highlight outline, ops) expects body entries to
                        // carry the shape.
                        try { e.shape = m_document->getBody(id); } catch (...) {}
                        pick(e, /*multiOk=*/true);
                    }
                    ImGui::PopID();
                }
            }
            const auto sketchIds = m_document->getAllSketchIds();
            if (!sketchIds.empty()) {
                any = true;
                touchui::sectionHeader("Sketches");
                for (int id : sketchIds) {
                    ImGui::PushID(id);
                    bool visible = m_document->isSketchVisible(id);
                    auto act = touchui::listRow("sketch", &visible,
                                                m_document->getSketchName(id).c_str(),
                                                selS.count(id) > 0,
                                                /*withOverflow=*/false);
                    if (act.toggled) m_document->setSketchVisible(id, visible);
                    if (act.clicked) {
                        SelectionEntry e;
                        e.type = SelectionType::Sketch;
                        e.sketchId = id;
                        pick(e, /*multiOk=*/false);
                    }
                    ImGui::PopID();
                }
            }
            const auto planeIds = m_document->getAllPlaneIds();
            const auto axisIds  = m_document->getAllAxisIds();
            if (!planeIds.empty() || !axisIds.empty()) {
                any = true;
                touchui::sectionHeader("Construction");
                for (int id : planeIds) {
                    ImGui::PushID(id + 100000); // avoid plane/axis id collisions
                    const auto* p = m_document->getPlane(id);
                    std::string label = p ? p->name
                                          : std::string("Plane ") + std::to_string(id);
                    bool visible = m_document->isPlaneVisible(id);
                    auto act = touchui::listRow("plane", &visible, label.c_str(),
                                                selP.count(id) > 0,
                                                /*withOverflow=*/false);
                    if (act.toggled) m_document->setPlaneVisible(id, visible);
                    if (act.clicked) {
                        SelectionEntry e;
                        e.type = SelectionType::Plane;
                        e.planeId = id;
                        pick(e, /*multiOk=*/false);
                    }
                    ImGui::PopID();
                }
                for (int id : axisIds) {
                    ImGui::PushID(id + 200000);
                    const auto* a = m_document->getAxis(id);
                    std::string label = a ? a->name
                                          : std::string("Axis ") + std::to_string(id);
                    bool visible = m_document->isAxisVisible(id);
                    auto act = touchui::listRow("axis", &visible, label.c_str(),
                                                selA.count(id) > 0,
                                                /*withOverflow=*/false);
                    if (act.toggled) m_document->setAxisVisible(id, visible);
                    if (act.clicked) {
                        SelectionEntry e;
                        e.type = SelectionType::Axis;
                        e.axisId = id;
                        pick(e, /*multiOk=*/false);
                    }
                    ImGui::PopID();
                }
            }
            if (!any)
                ImGui::TextColored(touchui::textDim(), "Nothing here yet");
        }
        ImGui::End();
    }

    // ── Contextual tool bar — the same catalogue the full shell's rail uses,
    //    floating on the LEFT edge, vertically centered. Sketch mode appends
    //    Finish/Exit pills below the tools. Tall catalogues (sketch mode on a
    //    landscape tablet) can exceed the work rect, so cap the height and
    //    let the bar scroll rather than run off-screen.
    ImGui::SetNextWindowPos(ImVec2(wp.x + m, wp.y + ws.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.0f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                        ImVec2(FLT_MAX, ws.y - 2.0f * m));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, touchui::panelBg());
    if (ImGui::Begin("##LiteToolBar", nullptr,
                     kFloat & ~ImGuiWindowFlags_NoScrollbar)) {
        if (m_toolbar) {
            for (const auto& tool : m_toolbar->railTools()) {
                if (touchui::railButton(tool.label, tool.icon, tool.label,
                                        tool.active, 64.0f * s))
                    handleToolAction(static_cast<int>(tool.action));
                tip(tool.tip);
            }
            if (m_inSketchMode) {
                const bool toolRunning = m_sketchTool && m_sketchTool->isPlacing();
                const char* finishLbl = toolRunning ? "Finish" : "Finish Sketch";
                const char* exitLbl   = toolRunning ? "Cancel" : "Discard Sketch";
                ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
                if (touchui::pillButton("finish", MZ_ICON_FINISH, finishLbl, true)) {
                    if (toolRunning)
                        recordSketchMutation([&]{ m_sketchTool->onConfirm(); });
                    else
                        handleToolAction(static_cast<int>(ToolAction::FinishSketch));
                }
                tip(toolRunning
                        ? "Finish the current shape, keeping the points placed"
                        : "Leave sketch mode, keeping the sketch");
                if (touchui::pillButton("exit", MZ_ICON_DISCARD, exitLbl)) {
                    if (toolRunning)
                        m_sketchTool->onCancel();
                    else
                        ImGui::OpenPopup("Discard sketch?"); // confirm — destructive
                }
                tip(toolRunning
                        ? "Cancel the in-progress shape"
                        : "Throw the sketch away and leave (asks to confirm)");
                if (ImGui::BeginPopupModal("Discard sketch?", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextUnformatted(
                        "Leave the sketch and throw away its changes?");
                    ImGui::Spacing();
                    const float bw = 150.0f * s;
                    if (ImGui::Button("Discard Sketch", ImVec2(bw, 44.0f * s))) {
                        ImGui::CloseCurrentPopup();
                        handleToolAction(static_cast<int>(ToolAction::ExitSketchDiscard));
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Keep Editing", ImVec2(bw, 44.0f * s)))
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
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
        tip("Create: a sketch or a primitive solid");
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

    // ── History timeline (bottom-center) — Fusion-360-style boxes, one per
    //    history step, oldest → newest. Tap a box for its properties popup:
    //    edit the op's parameters (Apply replays downstream steps, same code
    //    path as the desktop History panel), roll the model back/forward to
    //    it, toggle it, or delete it. Hidden while a sketch is open: rolling
    //    the host body back under a live sketch is forbidden (see
    //    History::setUndoFloor).
    const int liteSteps = m_history ? m_history->stepCount() : 0;
    if (m_imTouchLiteTimeline && !m_inSketchMode && m_document && m_history) {
        ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x * 0.5f, wp.y + ws.y - m),
                                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        // Keep clear of the fps readout (left) and the create FAB (right).
        const float inset = 96.0f * s;
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(0, 0), ImVec2(ws.x - 2.0f * (m + inset), FLT_MAX));
        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, touchui::panelBg());
        if (ImGui::Begin("##LiteTimeline", nullptr,
                         (kFloat & ~ImGuiWindowFlags_NoScrollbar) |
                             ImGuiWindowFlags_HorizontalScrollbar)) {
            // Empty history: keep the bar visible with a hint instead of
            // vanishing — otherwise toggling the clock button in a fresh
            // project looks like it does nothing.
            if (liteSteps == 0)
                ImGui::TextColored(touchui::textDim(),
                                   "History: no steps yet");
            const int curr = m_history->currentStep();
            const int failedAt = m_history->lastReplayFailure();
            const bool histLocked = anyInteractivePreviewActive();
            const ImU32 amber = ImGui::GetColorU32(ImVec4(0.95f, 0.75f, 0.3f, 1.0f));
            const ImU32 red   = ImGui::GetColorU32(ImVec4(1.0f, 0.45f, 0.35f, 1.0f));

            // Auto-scroll the current step into view whenever history mutates
            // (new op, undo/redo, edit) — not on user scrolls.
            static unsigned s_seenRev = ~0u;
            const bool historyMoved = (s_seenRev != m_history->revision());

            bool wantOpen = false;
            for (int i = 0; i < liteSteps; ++i) {
                const Operation* op = m_history->getStep(i);
                if (!op) continue;
                if (i > 0) ImGui::SameLine(0.0f, 6.0f * s);
                ImGui::PushID(i);
                ImU32 tint = 0;
                if (i == failedAt)          tint = red;
                else if (op->isReloaded())  tint = amber;
                const bool dim = (i > curr) || !op->isEnabled();
                if (touchui::timelineBox("step", liteStepIcon(op->typeId()),
                                         i == curr, i == m_liteHistoryEdit,
                                         dim, tint)) {
                    m_liteHistoryEdit = (m_liteHistoryEdit == i) ? -1 : i;
                    wantOpen = (m_liteHistoryEdit == i);
                    // Drive the viewport's orange edited-element highlight.
                    if (m_historyPanel)
                        m_historyPanel->setEditingStep(m_liteHistoryEdit);
                }
                if (i == curr && historyMoved) ImGui::SetScrollHereX(0.5f);
                ImGui::PopID();
            }
            s_seenRev = m_history->revision();

            if (wantOpen) ImGui::OpenPopup("##LiteStepProps");
            ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f * s, 0),
                                                ImVec2(360.0f * s, 460.0f * s));
            if (ImGui::BeginPopup("##LiteStepProps")) {
                const Operation* op =
                    (m_liteHistoryEdit >= 0 && m_liteHistoryEdit < liteSteps)
                        ? m_history->getStep(m_liteHistoryEdit)
                        : nullptr;
                if (!op) {
                    ImGui::CloseCurrentPopup();
                } else {
                    const int i = m_liteHistoryEdit;
                    std::string detail = op->description();
                    if (detail.empty()) detail = op->name();
                    ImGui::TextColored(touchui::textPrimary(), "%d. %s",
                                       i + 1, detail.c_str());
                    if (!op->isEnabled())
                        ImGui::TextColored(touchui::textDim(), "Disabled");
                    if (i > curr)
                        ImGui::TextColored(touchui::textDim(),
                                           "Undone \xE2\x80\x94 Go Here replays it.");
                    if (i == failedAt) {
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                            "Couldn't recompute after an upstream change. Edit "
                            "its parameters, fix the step before it, or delete it.");
                        ImGui::PopTextWrapPos();
                    }
                    ImGui::Separator();

                    if (op->isReloaded()) {
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f),
                            "Restored from an older save \xE2\x80\x94 no editable "
                            "parameters. Undo/redo still work.");
                        ImGui::PopTextWrapPos();
                    } else {
                        // The op's own parameter editor — identical widgets to
                        // the desktop History panel's Properties section.
                        ImGui::BeginChild("##props", ImVec2(0.0f, 200.0f * s),
                                          true);
                        const_cast<Operation*>(op)->renderProperties();
                        ImGui::EndChild();
                        ImGui::BeginDisabled(histLocked);
                        if (ImGui::Button("Apply Changes",
                                          ImVec2(-1.0f, 44.0f * s))) {
                            // Same sequence as HistoryPanel: carry inline
                            // sketch-dimension edits into later snapshots
                            // FIRST, then a transactional replay, then cascade
                            // so bodies built from the sketch follow.
                            m_history->propagateSketchValueEdits(i, *m_document);
                            const bool applied = m_history->editStep(
                                i, *m_document, /*transactional=*/true);
                            m_meshesDirty = true;
                            if (applied) {
                                if (auto* se =
                                        dynamic_cast<const SketchEditOp*>(op)) {
                                    auto tgt = se->getTarget();
                                    int sid = tgt ? m_document->findSketchId(
                                                        tgt.get())
                                                  : -1;
                                    if (sid >= 0) cascadeFromSketchEdit(sid);
                                } else if (auto* st = dynamic_cast<
                                               const SketchTransformOp*>(op)) {
                                    if (st->getSketchId() >= 0)
                                        cascadeFromSketchEdit(st->getSketchId());
                                }
                            }
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::Separator();

                    const float bw = 104.0f * s;
                    ImGui::BeginDisabled(histLocked || i == curr);
                    if (ImGui::Button("Go Here", ImVec2(bw, 44.0f * s))) {
                        // Roll the model to this step (Fusion's marker drag).
                        // Progress guard: a failed replay mid-walk must not
                        // spin forever.
                        while (m_history->currentStep() > i) {
                            const int before = m_history->currentStep();
                            undoWithCascade();
                            if (m_history->currentStep() == before) break;
                        }
                        while (m_history->currentStep() < i) {
                            const int before = m_history->currentStep();
                            redoWithCascade();
                            if (m_history->currentStep() == before) break;
                        }
                        m_meshesDirty = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(histLocked);
                    if (ImGui::Button(op->isEnabled() ? "Disable" : "Enable",
                                      ImVec2(bw, 44.0f * s))) {
                        // In-place toggle — preserves base bodies the op
                        // modifies (replayAll's doc.clear() would drop them).
                        m_history->setStepEnabled(i, !op->isEnabled(),
                                                  *m_document);
                        m_meshesDirty = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Delete", ImVec2(bw, 44.0f * s))) {
                        if (m_history->removeStep(i, *m_document)) {
                            m_liteHistoryEdit = -1;
                            if (m_historyPanel)
                                m_historyPanel->setEditingStep(-1);
                            ImGui::CloseCurrentPopup();
                        } else {
                            showToast(
                                "Can't delete: a later operation depends on it.");
                        }
                        m_meshesDirty = true;
                    }
                    ImGui::EndDisabled();
                }
                ImGui::EndPopup();
            } else if (m_liteHistoryEdit >= 0) {
                // Popup dismissed by tapping elsewhere — drop the edit state
                // (and the viewport highlight) with it.
                m_liteHistoryEdit = -1;
                if (m_historyPanel) m_historyPanel->setEditingStep(-1);
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

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

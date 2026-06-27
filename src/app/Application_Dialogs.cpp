#include "ui/UiTheme.h"
#include "ui_scale.h"
#include "touch_mode.h"
#include "gl_common.h"
#include "url_open.h"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <glm/gtc/matrix_transform.hpp>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "app/Application.h"
#include "app/Window.h"
#include "viewport/Viewport.h"
#include "viewport/Grid.h"
#include "viewport/ShapeRenderer.h"
#include "viewport/SketchRenderer.h"
#include "viewport/ViewCube.h"
#include "viewport/Picker.h"
#include "viewport/Gizmo.h"
#include "viewport/SelectionHighlight.h"
#include "viewport/BoxSelect.h"
#include "viewport/EdgeRenderer.h"
#include "viewport/BackgroundRenderer.h"
#include "core/Document.h"
#include "core/History.h"
#include "core/SelectionManager.h"
#include "ui/Toolbar.h"
#include "ui/HistoryPanel.h"
#include "ui/ItemsPanel.h"
#include "ui/StatusBar.h"
#include "ui/ThemeManager.h"
#include "ui/PropertiesPanel.h"
#include "ui/AboutDialog.h"
#include "ui/ShortcutsPanel.h"
#include "ui/HelpPanel.h"
#include "ui/UpdateChecker.h"
#include "modeling/Sketch.h"
#include "modeling/SketchSolver.h"
#include "modeling/SketchTool.h"
#include "modeling/TextSketchOp.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/ReplayOp.h"
#include "modeling/PrimitiveOp.h"
#include "modeling/ThreadOp.h"
#include <chrono>
#include <future>
#include "modeling/PushPullOp.h"
#include "modeling/TransformOp.h"
#include "modeling/SketchTransformOp.h"
#include "modeling/PlaneTransformOp.h"
#include "modeling/MirrorOp.h"
#include "modeling/RevolveOp.h"
#include "modeling/FilletOp.h"
#include "modeling/ChamferOp.h"
#include "modeling/DeleteOp.h"
#include "modeling/SketchEditOp.h"
#include "io/StepIO.h"
#include "io/StlExport.h"
#include "io/FileDialogs.h"
#include "io/ProjectIO.h"
#include "io/Settings.h"
#include "core/EventBus.h"
#include "plugin/PluginContext.h"
#include "plugin/PluginRegistry.h"

namespace materializr { namespace force_link { void linkAll(); } }

// Mouse-button index → display name (used by the Interactions panel).
static const char* mouseButtonName(int b) {
    switch (b) {
        case 0: return "Left";
        case 1: return "Right";
        case 2: return "Middle";
        default: return "?";
    }
}

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <gp_Ax3.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Plane.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Mat.hxx>
#include <gp_XYZ.hxx>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Implementations split out of Application.cpp — the small modal/popup
// renderers that don't share state with the main 3D viewport.
namespace materializr {

void Application::renderSettings() {
    if (!m_showSettings) return;
    ImGui::SetNextWindowSize(uiSz(420, 420), ImGuiCond_Appearing);
    if (ImGui::Begin("Settings", &m_showSettings)) {
        bool changed = false; // any change persists the settings file

        // Tabs share the same `changed` flag; the Apply/Close row below the
        // tab bar stays visible regardless of which tab is open. A scrolling
        // child holds the tab contents so the buttons stay pinned at the
        // bottom even when a tab's controls overflow.
        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        if (ImGui::BeginChild("SettingsBody", ImVec2(0, -footer))) {
            if (ImGui::BeginTabBar("SettingsTabs")) {

                // ── General ───────────────────────────────────────────────
                if (ImGui::BeginTabItem("General")) {
                    ImGui::SeparatorText("Interaction");
                    // Touch mode: large UI + touch-gesture input. The whole UI
                    // scale and input model branch on this, baked at startup, so
                    // it takes full effect on the next launch.
                    if (ImGui::Checkbox("Touch mode (large UI + touch gestures)", &m_touchMode)) {
                        changed = true;
                    }
                    ImGui::TextWrapped("On: finger-sized UI, long-press menus, on-screen "
                                       "toggles, trackpad navigation. Off: the desktop "
                                       "mouse/keyboard layout — use it with an attached "
                                       "mouse/keyboard. Takes full effect on restart.");
                    if (materializr::touchMode() != m_touchMode) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                            "Restart Materializr to apply the new mode.");
                    }

                    ImGui::Spacing();
                    ImGui::SeparatorText("Appearance");
                    // Theme selector (the View menu mirrors this).
                    if (m_themeManager->renderSelector()) {
                        m_themeManager->apply();
                        changed = true;
                    }

                    ImGui::Spacing();
                    ImGui::SeparatorText("Panels");
                    ImGui::TextWrapped("Show or hide the docked panels to free up "
                                       "screen space. Re-enable any of them here.");
                    if (ImGui::Checkbox("Tools",        &m_showTools))        changed = true;
                    if (ImGui::Checkbox("Interactions", &m_showInteractions)) changed = true;
                    if (ImGui::Checkbox("History",      &m_showHistory))      changed = true;
                    if (ImGui::Checkbox("Items",        &m_showItems))        changed = true;
                    if (ImGui::Checkbox("Properties",   &m_showProperties))   changed = true;

                    ImGui::Spacing();
                    ImGui::SeparatorText("Toolbar tooltips");
                    if (ImGui::Checkbox("Show toolbar tooltips", &m_showToolbarTooltips)) {
                        changed = true;
                    }
                    ImGui::TextWrapped("Hover any toolbar button for a short description of what it does. "
                                       "Turn off if you already know the tools and find the pop-ups distracting.");

                    ImGui::Spacing();
                    ImGui::SeparatorText("Autosave");
                    if (ImGui::Checkbox("Autosave saved projects", &m_autosaveEnabled)) changed = true;
                    ImGui::TextWrapped("Periodically re-saves the project once it has been "
                                       "saved to a file at least once.");
                    ImGui::BeginDisabled(!m_autosaveEnabled);
                    int interval = static_cast<int>(m_autosaveIntervalSec);
                    if (ImGui::SliderInt("Interval (s)", &interval, 15, 600, "%d s")) {
                        m_autosaveIntervalSec = static_cast<float>(interval);
                        changed = true;
                    }
                    ImGui::EndDisabled();
                    if (m_autosaveEnabled && m_currentProjectPath.empty()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                            "Save the project once to start autosaving.");
                    }

                    ImGui::Spacing();
                    ImGui::SeparatorText("Session");
                    if (ImGui::Checkbox("Open last project on launch", &m_autoOpenLastProject)) {
                        changed = true;
                    }
                    ImGui::TextWrapped("If on, Materializr reopens the project you had open the last "
                                       "time you quit. Using File → Close Project before quitting "
                                       "makes the next launch start empty instead.");

                    ImGui::Spacing();
                    if (ImGui::Checkbox("Check for updates on launch", &m_checkForUpdatesOnLaunch)) {
                        changed = true;
                    }
                    ImGui::TextWrapped("If on, Materializr asks GitHub for the latest release at "
                                       "startup and pops a small dialog when a newer build is "
                                       "available. Turn off for offline or portable use; you can "
                                       "still check manually via Help → Check for Updates.");
                    ImGui::EndTabItem();
                }

                // ── Sketch helpers (tooltips) ─────────────────────────────
                // Inference assistance now lives on a live Full/Reduced/Off
                // toggle in the sketch toolbar (no longer a persisted setting);
                // constraints are always on the right-click "Add Constraint"
                // menu. This tab keeps the toolbar-tooltip toggle.
                if (ImGui::BeginTabItem("Sketch")) {
                    ImGui::SeparatorText("Drawing inferences");
                    ImGui::TextWrapped(
                        "As you draw, coloured ghost guides show alignment "
                        "(perpendicular, parallel, on-axis, on-midpoint, etc.) "
                        "and the cursor snaps. Full adds hover-to-charge "
                        "references (dwell on a point / midpoint / face vertex "
                        "to align from it). Reduced is the classic guides only, "
                        "no hover-charging. Off is grid + endpoint only. "
                        "Constraints live on the sketch right-click \"Add "
                        "Constraint\" menu.");

                    ImGui::Spacing();
                    {
                        // The combo edits the live sketch tool inference level,
                        // which currentSettings() then reads back when the file
                        // saves below — so the user's last-set level survives a
                        // relaunch regardless of whether they used this combo or
                        // the toolbar's live cycle button.
                        using IL = SketchTool::InferenceLevel;
                        int cur = m_sketchTool
                            ? static_cast<int>(m_sketchTool->getInferenceLevel()) : 0;
                        const char* labels[] = { "Full", "Reduced", "Off", "Max (touch)" };
                        if (ImGui::Combo("Inference level", &cur, labels, 4)) {
                            if (m_sketchTool) {
                                IL next = (cur == 1) ? IL::Reduced
                                        : (cur == 2) ? IL::Off
                                        : (cur == 3) ? IL::Max
                                                     : IL::Full;
                                m_sketchTool->setInferenceLevel(next);
                            }
                            changed = true;
                        }
                        ImGui::SetItemTooltip("Max widens snap/alignment catch ranges for fingertips — "
                                              "stronger than Full. Full and below behave the same on every device.");
                    }

                    ImGui::Spacing();
                    {
                        // Angle-snap increment for the line tool. Maps the combo
                        // index to a degree value; 0 = off (free angles only).
                        static const int kDegs[] = { 0, 5, 15, 30, 45, 90 };
                        const char* degLabels[] = {
                            "Off", "5°", "15°", "30°", "45°", "90°" };
                        int curDeg = m_sketchTool ? m_sketchTool->getAngleSnapDeg() : 15;
                        int idx = 2; // default 15°
                        for (int i = 0; i < 6; ++i) if (kDegs[i] == curDeg) idx = i;
                        if (ImGui::Combo("Angle snap", &idx, degLabels, 6)) {
                            if (m_sketchTool) m_sketchTool->setAngleSnapDeg(kDegs[idx]);
                            changed = true;
                        }
                        ImGui::SetItemTooltip(
                            "While drawing a line, snap its direction to multiples "
                            "of this angle from the start point. Lower = more "
                            "snap rays (15° is the classic CAD default); higher "
                            "= only the cardinal angles; Off = free angles.");
                    }

                    ImGui::Spacing();
                    ImGui::SeparatorText("Appearance");
                    if (ImGui::SliderFloat("Grid opacity", &m_sketchGridOpacity,
                                           0.0f, 1.0f, "%.2f")) {
                        changed = true;
                    }
                    ImGui::SetItemTooltip(
                        "Opacity of the sketch-plane grid. Lower it if the grid "
                        "competes with your sketch lines; 0 hides it.");

                    if (ImGui::SliderFloat("Grid thickness", &m_sketchGridThickness,
                                           0.1f, 2.0f, "%.2fx")) {
                        changed = true;
                    }
                    ImGui::SetItemTooltip(
                        "Sketch grid line width, as a multiplier of the default "
                        "(1.0x). Raise it for a bolder grid, lower it for a finer "
                        "one. Only affects the sketch-plane grid, not the ground.");

                    ImGui::Spacing();
                    if (ImGui::Checkbox("Show level toggle in sketch toolbar",
                                        &m_showInferenceToolbarToggle)) {
                        changed = true;
                    }
                    ImGui::TextWrapped("Off hides the live Full / Reduced / Off "
                                       "cycle button from the sketch toolbar — "
                                       "use this combo instead. On (default) "
                                       "keeps the per-session button visible.");

                    ImGui::EndTabItem();
                }

                // ── Navigation (camera + touch) ───────────────────────────
                if (ImGui::BeginTabItem("Navigation")) {
                    ImGui::SeparatorText("Mouse");
                    ImGui::TextWrapped("Choose which mouse button orbits and which pans. "
                                       "Zoom is always the scroll wheel.");
                    ImGui::Spacing();

                    const char* buttons[] = { "Left", "Middle", "Right" };
                    // Map button index <-> combo index (Left=0, Middle=2, Right=1 in ImGui).
                    auto toCombo = [](int b) { return b == 0 ? 0 : (b == 2 ? 1 : 2); };
                    auto fromCombo = [](int c) { return c == 0 ? 0 : (c == 1 ? 2 : 1); };

                    // Trackpad preset: both orbit and pan on the left button (with Shift
                    // toggling between them). Mirrors what most laptops without a middle
                    // mouse button can reach. Editing the combos manually disables the box.
                    bool trackpad = (m_settingsOrbitButton == 0 && m_settingsPanButton == 0);
                    if (ImGui::Checkbox("Trackpad mode (left-drag = orbit, Shift+left = pan)",
                                        &trackpad)) {
                        if (trackpad) { m_settingsOrbitButton = 0; m_settingsPanButton = 0; }
                        else          { m_settingsOrbitButton = 2; m_settingsPanButton = 1; }
                    }

                    int orbitC = toCombo(m_settingsOrbitButton);
                    if (ImGui::Combo("Orbit", &orbitC, buttons, 3)) m_settingsOrbitButton = fromCombo(orbitC);
                    int panC = toCombo(m_settingsPanButton);
                    if (ImGui::Combo("Pan", &panC, buttons, 3)) m_settingsPanButton = fromCombo(panC);

                    if (!trackpad && (m_settingsOrbitButton == 0 || m_settingsPanButton == 0)) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                            "Note: Left is also used to select; assigning it here may conflict.");
                    }
                    ImGui::TextDisabled("Orbit/Pan buttons take effect on Apply.");

                    ImGui::Spacing();
                    ImGui::SeparatorText("Double-click");
                    // Trackpads double-tap slower than a mouse; a too-short
                    // window splits a slow body double-click into two single
                    // clicks, which cycle-selects past the body. Applied live.
                    if (ImGui::SliderFloat("Double-click speed", &m_doubleClickTime,
                                           0.20f, 0.75f, "%.2f s")) {
                        if (m_doubleClickTime < 0.20f) m_doubleClickTime = 0.20f;
                        if (m_doubleClickTime > 0.75f) m_doubleClickTime = 0.75f;
                        ImGui::GetIO().MouseDoubleClickTime = m_doubleClickTime;
                        changed = true;
                    }
                    ImGui::SetItemTooltip("Max time between two clicks to count as a double-click. "
                                          "Raise it if double-clicking to select a body feels too fast "
                                          "on a trackpad. Default 0.30 s.");

                    ImGui::Spacing();
                    ImGui::SeparatorText("Mouse sensitivity");
                    // One uniform multiplier on orbit / pan / zoom input deltas
                    // — so a trackpad that's already slow at the OS level
                    // doesn't whip the camera around. Applied live (the camera
                    // multiplies it onto each delta on the next mouse event).
                    {
                        float sens = m_viewport->getCamera().getMouseSensitivity();
                        if (ImGui::SliderFloat("Mouse sensitivity", &sens,
                                               0.10f, 3.00f, "%.2fx")) {
                            m_viewport->getCamera().setMouseSensitivity(sens);
                            changed = true;
                        }
                        ImGui::TextWrapped("Scales orbit, pan, and zoom uniformly. "
                                           "Lower it if a trackpad feels too fast "
                                           "compared to desktop cursor speed; "
                                           "raise it for a snappier feel with a mouse. "
                                           "1.00x is the default baseline.");
                    }

                    ImGui::Spacing();
                    ImGui::SeparatorText("Orbit behaviour");
                    // Level (turntable) orbit toggle — applied live.
                    bool level = m_viewport->getCamera().isLevelOrbit();
                    if (ImGui::Checkbox("Level orbit (keep horizon flat)", &level)) {
                        m_viewport->getCamera().setLevelOrbit(level);
                        changed = true;
                    }
                    ImGui::TextWrapped("On: orbiting is a level turntable. Off: free trackball "
                                       "that can tumble in any direction.");

                    // Invert the cube-drag → orbit direction.
                    if (ImGui::Checkbox("Invert ViewCube drag direction", &m_invertCubeDrag)) {
                        changed = true;
                    }

                    if (materializr::touchMode()) {
                        ImGui::Spacing();
                        ImGui::SeparatorText("Touch sensitivity");
                        ImGui::TextWrapped("Scale how far the camera moves per gesture "
                                           "(1.00x = default).");
                        // ##suffix keeps the visible label ("Orbit"/"Pan") but
                        // gives a unique ID: the Orbit/Pan mouse-button Combos
                        // above share this tab's ID scope, so a bare "Orbit"/"Pan"
                        // slider collided with them (ImGui id-conflict warning).
                        // Zoom had no matching Combo, which is why it never warned.
                        if (ImGui::SliderFloat("Orbit##touchSens", &m_touchOrbitSens, 0.25f, 3.0f, "%.2fx")) changed = true;
                        if (ImGui::SliderFloat("Pan##touchSens",   &m_touchPanSens,   0.25f, 3.0f, "%.2fx")) changed = true;
                        if (ImGui::SliderFloat("Zoom##touchSens",  &m_touchZoomSens,  0.25f, 3.0f, "%.2fx")) changed = true;
                    }
                    ImGui::EndTabItem();
                }

                // ── Rendering ─────────────────────────────────────────────
                if (ImGui::BeginTabItem("Rendering")) {
                    ImGui::SeparatorText("Lighting");
                    // Lighting — tame the harsh single-direction shadows.
                    ImGui::TextWrapped("Lighting controls how evenly the model is lit.");
                    if (ImGui::SliderFloat("Ambient", &m_lightAmbient, 0.0f, 1.0f, "%.2f")) {
                        applyRenderingSettings();
                        changed = true;
                    }
                    ImGui::SetItemTooltip("Higher values brighten shadowed faces for more uniform lighting.");
                    if (ImGui::Checkbox("Headlight (light follows camera)", &m_lightHeadlight)) {
                        applyRenderingSettings();
                        changed = true;
                    }
                    ImGui::SetItemTooltip("The face you're looking at is always lit; removes large cast shadows.");
                    if (ImGui::Checkbox("Fill light (soften opposite side)", &m_lightFill)) {
                        applyRenderingSettings();
                        changed = true;
                    }

                    ImGui::Spacing();
                    ImGui::SeparatorText("Quality");
                    // Anti-aliasing.
                    const char* aaItems[] = { "Off", "2x", "4x", "8x" };
                    auto samplesToIdx = [](int s) { return s >= 8 ? 3 : (s >= 4 ? 2 : (s >= 2 ? 1 : 0)); };
                    const int idxToSamples[] = { 0, 2, 4, 8 };
                    int aaIdx = samplesToIdx(m_msaaSamples);
                    if (ImGui::Combo("Anti-aliasing", &aaIdx, aaItems, 4)) {
                        m_msaaSamples = idxToSamples[aaIdx];
                        applyRenderingSettings();
                        changed = true;
                    }
                    ImGui::SetItemTooltip("Multisampling (MSAA) smooths jagged edges in the viewport.");

                    // Mesh quality — denser tessellation for smoother curved surfaces.
                    const char* mqItems[] = { "Low", "Medium", "High", "Ultra" };
                    if (ImGui::Combo("Mesh quality", &m_meshQuality, mqItems, 4)) {
                        if (m_meshQuality < 0) m_meshQuality = 0;
                        if (m_meshQuality > 3) m_meshQuality = 3;
                        m_meshesDirty = true; // re-tessellate at the new density
                        changed = true;
                    }
                    ImGui::SetItemTooltip("Higher quality uses more polygons, smoothing curves and holes.");

                    ImGui::Spacing();
                    ImGui::SeparatorText("Selection");
                    // Selection line width — how boldly picked edges/bodies are outlined.
                    if (ImGui::SliderFloat("Selection line width", &m_selectionLineWidth, 1.0f, 10.0f, "%.1f px")) {
                        if (m_selectionLineWidth < 1.0f) m_selectionLineWidth = 1.0f;
                        if (m_selectionLineWidth > 10.0f) m_selectionLineWidth = 10.0f;
                        applyRenderingSettings();
                        changed = true;
                    }
                    ImGui::SetItemTooltip("Thickness of the highlight drawn over selected edges and bodies. "
                                          "Increase to make selected edges easier to see.");

                    ImGui::SeparatorText("Sketch");
                    // Sketch line width — how boldly sketch geometry reads over the grid.
                    if (ImGui::SliderFloat("Sketch line width", &m_sketchLineWidth, 1.0f, 6.0f, "%.1f px")) {
                        if (m_sketchLineWidth < 1.0f) m_sketchLineWidth = 1.0f;
                        if (m_sketchLineWidth > 6.0f) m_sketchLineWidth = 6.0f;
                        applyRenderingSettings();
                        changed = true;
                    }
                    ImGui::SetItemTooltip("Thickness of sketch lines, circles and arcs (and the vertex dots). "
                                          "Increase if sketch geometry is too thin or blends into the grid.");
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (ImGui::Button("Apply", ImVec2(90, 0))) {
            m_orbitButton = m_settingsOrbitButton;
            m_panButton = m_settingsPanButton;
            changed = true;
            m_showSettings = false; // Apply commits + dismisses; matches expectation
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(90, 0))) {
            m_showSettings = false;
        }

        // Import/Export the whole preference set as JSON, for backup or sharing
        // between machines. The file browsers render on the next frames; import
        // applies live and persists, so dismiss the dialog to avoid showing
        // stale staged values over freshly-imported ones.
        ImGui::SameLine();
        if (ImGui::Button("Import...", ImVec2(90, 0))) {
            importSettings();
            m_showSettings = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Export...", ImVec2(90, 0))) {
            exportSettings();
        }

        if (changed) saveAppSettings();
    }
    ImGui::End();
}

void Application::renderMirrorPopup() {
    if (m_showMirrorPopup) {
        ImGui::OpenPopup("MirrorPopup");
        m_showMirrorPopup = false;
    }
    if (ImGui::BeginPopup("MirrorPopup")) {
        ImGui::TextColored(materializr::accentText(), "Mirror across");
        ImGui::Separator();

        // Mirror the body across the plane on its own bounding box for the chosen
        // axis (the copy lands flush beside the original).
        auto mirrorAxis = [&](int axis) {
            try {
                const TopoDS_Shape& shape = m_document->getBody(m_mirrorBodyId);
                Bnd_Box bb; BRepBndLib::Add(shape, bb);
                if (bb.IsVoid()) return;
                double x1, y1, z1, x2, y2, z2; bb.Get(x1, y1, z1, x2, y2, z2);
                double cx = (x1 + x2) * 0.5, cy = (y1 + y2) * 0.5, cz = (z1 + z2) * 0.5;
                gp_Pnt pt; gp_Dir dir;
                if (axis == 0)      { pt = gp_Pnt(x1, cy, cz); dir = gp_Dir(1, 0, 0); }
                else if (axis == 1) { pt = gp_Pnt(cx, y1, cz); dir = gp_Dir(0, 1, 0); }
                else                { pt = gp_Pnt(cx, cy, z1); dir = gp_Dir(0, 0, 1); }
                auto op = std::make_unique<MirrorOp>();
                op->setBody(m_mirrorBodyId);
                op->setPlane(MirrorPlane::Custom);
                op->setCustomPlane(gp_Ax2(pt, dir));
                op->setKeepOriginal(true);
                if (m_history->pushOperation(std::move(op), *m_document)) m_meshesDirty = true;
            } catch (...) {}
        };

        if (ImGui::Button("X axis", ImVec2(150, 0))) { mirrorAxis(0); ImGui::CloseCurrentPopup(); }
        if (ImGui::Button("Y axis", ImVec2(150, 0))) { mirrorAxis(1); ImGui::CloseCurrentPopup(); }
        if (ImGui::Button("Z axis", ImVec2(150, 0))) { mirrorAxis(2); ImGui::CloseCurrentPopup(); }
        ImGui::Separator();
        if (ImGui::Button("Across a face…", ImVec2(150, 0))) {
            m_mirrorPickFace = true; // next planar face click defines the mirror plane
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Application::renderUpdatePopup() {
    if (!m_showUpdatePopup) return;

    ImGui::OpenPopup("Check for Updates");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(uiSz(400, 220), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Check for Updates", &m_showUpdatePopup,
                               ImGuiWindowFlags_NoResize)) {
        // First open: run the network check.
        if (!m_updateChecked) {
            auto r = UpdateChecker::check("materializr-cad", "materializr");
            m_updateCurrent     = r.current;
            m_updateLatest      = r.latest;
            m_updateAvailable   = r.updateAvailable;
            m_updateReleaseUrl  = r.releasePageUrl;
            m_updateMessage     = r.ok ? "" : r.errorMessage;
            m_updateChecked     = true;
        }

        ImGui::Text("Current version: %s", m_updateCurrent.c_str());
        if (!m_updateLatest.empty()) {
            ImGui::Text("Latest release:  %s", m_updateLatest.c_str());
        }
        ImGui::Spacing();

        if (!m_updateMessage.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                               "Couldn't reach GitHub: %s", m_updateMessage.c_str());
        } else if (m_updateAvailable) {
            ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f),
                               "A newer release is available.");
            ImGui::TextWrapped("Download the new build from the release page; the "
                               "installer or portable zip will replace this one.");
            ImGui::Spacing();
            if (ImGui::Button("Open Release Page", ImVec2(180, 0))) {
                // m_updateReleaseUrl is the GitHub API's html_url — server
                // controlled — so open it via the shell-free helper, pinned to
                // github.com (a tampered response can neither inject a shell
                // command nor redirect the user elsewhere).
                materializr::openUrl(m_updateReleaseUrl, "https://github.com/");
            }
        } else {
            ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f),
                               "You are running the latest release.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            m_showUpdatePopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Re-check", ImVec2(100, 0))) {
            m_updateChecked = false;
        }
        ImGui::EndPopup();
    } else {
        m_showUpdatePopup = false;
    }
}

// Multi-body Rotate type-in panel. Visible only when the Rotate gizmo is the
// active mode AND 2+ bodies are selected — the case where the live gizmo path
// gets pathologically slow on big selections. The user can dial in an exact
// per-axis rotation and click Apply to commit it in a single frame, instead of
// dragging through many laggy preview frames.
void Application::renderMultiTransformPanel() {
    // Track whether the panel's display conditions are currently met. When the
    // conditions transition from "not met" to "met", reopen the panel so the
    // user can dismiss it once and still get it back next time they enter the
    // state — without having to dig through menus.
    bool conditionsMet = m_gizmo && m_gizmo->getMode() == GizmoMode::Rotate &&
                         m_selection && m_selection->selectedBodyCount() >= 2;
    if (conditionsMet && !m_multiTransformConditionsMet) {
        m_multiTransformPanelOpen = true;
    }
    m_multiTransformConditionsMet = conditionsMet;
    if (!conditionsMet || !m_multiTransformPanelOpen) return;

    int n = m_selection->selectedBodyCount();
    char title[64];
    std::snprintf(title, sizeof(title), "Rotate %d Bodies###MultiTransform", n);

    ImGui::SetNextWindowSize(uiSz(360, 0), ImGuiCond_FirstUseEver);
    // The window-titlebar X also closes the panel — same state as the Close
    // button below, so either way auto-reopen logic above takes effect.
    if (!ImGui::Begin(title, &m_multiTransformPanelOpen)) { ImGui::End(); return; }

    ImGui::TextWrapped("Type exact angles instead of dragging the gizmo — useful "
                       "when the selection is too large for a smooth live drag. "
                       "Rotation is composed X → Y → Z around the selection centroid.");
    ImGui::Spacing();

    const char* axisLabels[3] = { "X", "Y", "Z" };
    const ImVec4 axisColors[3] = {
        ImVec4(1.00f, 0.35f, 0.35f, 1.0f),  // red    (X)
        ImVec4(0.35f, 1.00f, 0.35f, 1.0f),  // green  (Y)
        ImVec4(0.40f, 0.55f, 1.00f, 1.0f),  // blue   (Z)
    };

    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        ImGui::TextColored(axisColors[i], "%s", axisLabels[i]);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        ImGui::SliderFloat("##slider", &m_multiRotate[i], -180.0f, 180.0f, "%.1f°");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputFloat("##input", &m_multiRotate[i], 0.0f, 0.0f, "%.3f");
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::Spacing();
    bool anyNonZero = std::abs(m_multiRotate[0]) > 1e-3f ||
                      std::abs(m_multiRotate[1]) > 1e-3f ||
                      std::abs(m_multiRotate[2]) > 1e-3f;

    ImGui::BeginDisabled(!anyNonZero);
    if (ImGui::Button("Apply", ImVec2(100, 0))) {
        applyMultiBodyRotation();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(100, 0))) {
        m_multiRotate[0] = m_multiRotate[1] = m_multiRotate[2] = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(100, 0))) {
        m_multiTransformPanelOpen = false;
    }

    ImGui::End();
}

void Application::applyMultiBodyRotation() {
    if (!m_selection || !m_document || !m_history) return;

    // Snapshot every selected body's current state.
    std::vector<std::pair<int, TopoDS_Shape>> bodies;
    for (const auto& sel : m_selection->getSelection()) {
        if (sel.type != SelectionType::Body) continue;
        try {
            bodies.push_back({sel.bodyId, m_document->getBody(sel.bodyId)});
        } catch (...) {}
    }
    if (bodies.size() < 2) return;

    // Pivot = selection centroid (matches the gizmo's drag behaviour).
    glm::vec3 pivot(0.0f);
    int np = 0;
    for (auto& [id, orig] : bodies) {
        try {
            Bnd_Box bb; BRepBndLib::Add(orig, bb);
            if (bb.IsVoid()) continue;
            double x0,y0,z0,x1,y1,z1; bb.Get(x0,y0,z0,x1,y1,z1);
            pivot += glm::vec3((x0+x1)*0.5f, (y0+y1)*0.5f, (z0+z1)*0.5f);
            ++np;
        } catch (...) {}
    }
    if (np > 0) pivot /= static_cast<float>(np);

    // Compose X → Y → Z rotation about the pivot. Each rotation is a discrete
    // gp_Trsf; multiplication composes them in OCCT.
    const double d2r = M_PI / 180.0;
    gp_Pnt p(pivot.x, pivot.y, pivot.z);
    gp_Trsf trsf;
    if (std::abs(m_multiRotate[0]) > 1e-3f) {
        gp_Trsf rx; rx.SetRotation(gp_Ax1(p, gp_Dir(1, 0, 0)), m_multiRotate[0] * d2r);
        trsf = rx * trsf;
    }
    if (std::abs(m_multiRotate[1]) > 1e-3f) {
        gp_Trsf ry; ry.SetRotation(gp_Ax1(p, gp_Dir(0, 1, 0)), m_multiRotate[1] * d2r);
        trsf = ry * trsf;
    }
    if (std::abs(m_multiRotate[2]) > 1e-3f) {
        gp_Trsf rz; rz.SetRotation(gp_Ax1(p, gp_Dir(0, 0, 1)), m_multiRotate[2] * d2r);
        trsf = rz * trsf;
    }

    // Apply and capture before/after snapshots for a single ReplayOp commit.
    ReplayOp::BodyState beforeState, afterState;
    for (auto& [id, orig] : bodies) {
        beforeState.push_back({id, orig});
        try {
            BRepBuilderAPI_Transform xf(orig, trsf, /*copy=*/true);
            if (xf.IsDone()) {
                m_document->updateBody(id, xf.Shape());
                afterState.push_back({id, xf.Shape()});
            }
        } catch (...) {}
    }

    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "Rotate %d bodies by X %.2f° Y %.2f° Z %.2f° around centroid",
                  static_cast<int>(bodies.size()),
                  m_multiRotate[0], m_multiRotate[1], m_multiRotate[2]);
    auto op = std::make_unique<ReplayOp>(
        "multirotate",
        std::string("Rotate (") + std::to_string(bodies.size()) + " bodies)",
        std::string(buf),
        std::move(beforeState), std::move(afterState),
        /*fromReload=*/false);
    m_history->pushExecuted(std::move(op));
    m_meshesDirty = true;

    // Zero the sliders so the next Apply is relative to the new orientation.
    m_multiRotate[0] = m_multiRotate[1] = m_multiRotate[2] = 0.0f;
}

void Application::renderResizeCylindricalPanel() {
    if (!m_resizeCylActive) return;

    // Anchor only on first appearance — afterwards the user can drag the
    // popup somewhere more convenient and it stays there. Re-positioning
    // every frame fought any drag attempt + NoMove also prevented dragging.
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 280,
                                    ImGui::GetWindowPos().y + 50),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(uiSz(260, 0), ImGuiCond_Appearing);
    ImGui::Begin("Edit Diameter", nullptr,
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize);

    bool both = m_resizeCylEditBottom && m_resizeCylEditTop;
    const char* roleSuffix = m_resizeCylIsHole ? " (hole)" : " (outer)";
    const char* heading = both
        ? (m_resizeCylIsHole ? "Edit Diameter (hole)" : "Edit Diameter (outer)")
        : m_resizeCylEditBottom
            ? (m_resizeCylIsHole ? "Edit Bottom Diameter (hole)" : "Edit Bottom Diameter (outer)")
            : (m_resizeCylIsHole ? "Edit Top Diameter (hole)"    : "Edit Top Diameter (outer)");
    (void)roleSuffix; // kept for grep
    ImGui::TextColored(materializr::accentText(), "%s", heading);
    ImGui::Separator();

    if (both) {
        ImGui::Text("Original: %.2f mm", m_resizeCylOriginalTopR * 2.0);
    } else if (m_resizeCylEditBottom) {
        ImGui::Text("Original: %.2f mm",     m_resizeCylOriginalBottomR * 2.0);
        ImGui::TextDisabled("Top stays at %.2f mm — drag this end to make a cone.",
                            m_resizeCylOriginalTopR * 2.0);
    } else {
        ImGui::Text("Original: %.2f mm",     m_resizeCylOriginalTopR * 2.0);
        ImGui::TextDisabled("Bottom stays at %.2f mm — drag this end to make a cone.",
                            m_resizeCylOriginalBottomR * 2.0);
    }

    if (m_resizeCylInputFocus) {
        ImGui::SetKeyboardFocusHere();
        m_resizeCylInputFocus = false;
    }

    // Drive one buffer; mirror into the other when face-editing both ends.
    char*    buf = m_resizeCylEditBottom ? m_resizeCylBotBuf : m_resizeCylTopBuf;
    double*  val = m_resizeCylEditBottom ? &m_resizeCylNewBottomDiameter
                                         : &m_resizeCylNewTopDiameter;

    ImGui::SetNextItemWidth(140);
    bool entered = ImGui::InputText("##rcyldia", buf, 32,
                                    ImGuiInputTextFlags_EnterReturnsTrue |
                                    ImGuiInputTextFlags_CharsDecimal);

    double parsed = std::atof(buf);
    bool changed = std::abs(parsed - *val) > 0.001;
    if (changed) {
        *val = parsed;
        if (both) {
            m_resizeCylNewBottomDiameter = parsed;
            m_resizeCylNewTopDiameter    = parsed;
            std::snprintf(m_resizeCylEditBottom ? m_resizeCylTopBuf : m_resizeCylBotBuf,
                          32, "%.2f", parsed);
        }
        updateResizeCylindrical();
    }

    if (entered) {
        commitResizeCylindrical();
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    ImGui::Text("mm");

    if (m_resizeCylPreviewFailed) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.35f, 1.0f),
                           "Invalid diameter for this feature —\n"
                           "a hole can't exceed the surrounding wall.");
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(m_resizeCylPreviewFailed);
    if (ImGui::Button(materializr::btnConfirm(), ImVec2(115, 0))) commitResizeCylindrical();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(materializr::btnCancel(),    ImVec2(115, 0))) cancelResizeCylindrical();

    ImGui::End();
}

void Application::renderScalePanel() {
    // Shown only while the Scale gizmo is active with a body selected.
    if (m_inSketchMode || !m_selection->hasSelectedBodies()) return;
    if (m_gizmo->getMode() != GizmoMode::Scale) return;

    // mm mode only makes sense for a single body — multi-body scale needs a
    // dimensionless factor. Force Percent if more than one body is in the
    // selection so the popup stays coherent.
    const bool singleBody = (m_selection->selectedBodyCount() == 1);
    if (!singleBody) m_scaleUnitMode = ScaleUnitMode::Percent;

    // Resolve the (single-body) bbox now so mm-mode fields can pre-fill from
    // the live dimensions. user-Z-up remap: user X = world X, user Y = world
    // Z, user Z = world Y (matches the Properties size readout and the
    // move-gizmo Z-bottom label).
    const int userToWorld[3] = {0, 2, 1};
    double worldMins[3] = {0, 0, 0};
    double userExtents[3] = {0, 0, 0};
    int targetBodyId = -1;
    bool haveBbox = false;
    if (singleBody) {
        targetBodyId = m_selection->getSelection()[0].bodyId;
        try {
            const TopoDS_Shape& shape = m_document->getBody(targetBodyId);
            if (!shape.IsNull()) {
                Bnd_Box bb;
                BRepBndLib::AddOptimal(shape, bb, Standard_False, Standard_False);
                if (!bb.IsVoid()) {
                    Standard_Real x1, y1, z1, x2, y2, z2;
                    bb.Get(x1, y1, z1, x2, y2, z2);
                    worldMins[0] = x1; worldMins[1] = y1; worldMins[2] = z1;
                    const double worldExtents[3] = {
                        x2 - x1, y2 - y1, z2 - z1,
                    };
                    for (int i = 0; i < 3; ++i)
                        userExtents[i] = worldExtents[userToWorld[i]];
                    haveBbox = true;
                }
            }
        } catch (...) {}
    }

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 250,
                                    ImGui::GetWindowPos().y + 50));
    ImGui::SetNextWindowSize(uiSz(230, 0));
    ImGui::Begin("##ScalePanel", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize);

    const bool mm = (m_scaleUnitMode == ScaleUnitMode::Millimeter);
    ImGui::TextColored(materializr::accentText(),
                       mm ? "Scale (target mm)" : "Scale (%% of current)");
    ImGui::Separator();

    // Unit toggle. mm disabled when multi-body so we don't mislead — there's
    // no single "current dimension" to type a target against.
    ImGui::BeginDisabled(!singleBody);
    if (ImGui::RadioButton("%", !mm))  m_scaleUnitMode = ScaleUnitMode::Percent;
    ImGui::SameLine();
    if (ImGui::RadioButton("mm", mm))  m_scaleUnitMode = ScaleUnitMode::Millimeter;
    ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 20.0f);
    ImGui::Checkbox("Uniform", &m_scaleUniform);

    ImGui::Spacing();

    // Per-axis edit row. % mode: m_scalePct in [0,inf]; field is a percent.
    // mm mode: m_scaleMmEdit[i].buf reflects the current bbox extent on the
    // user-X/Y/Z axis (Z-up); typing a new value reflects the absolute
    // target dimension. Uniform mirrors percent value (% mode) or ratio
    // (mm mode) so the body keeps proportional in both modes.
    const char* axisLabels[3] = {"X", "Y", "Z"};
    const ImVec4 axisColors[3] = {
        ImVec4(1.00f, 0.35f, 0.35f, 1.0f),
        ImVec4(0.35f, 1.00f, 0.35f, 1.0f),
        ImVec4(0.40f, 0.55f, 1.00f, 1.0f),
    };

    if (!mm) {
        for (int i = 0; i < 3; ++i) {
            ImGui::PushID(i);
            ImGui::TextColored(axisColors[i], "%s", axisLabels[i]);
            ImGui::SameLine(28);
            ImGui::SetNextItemWidth(95.0f);
            if (ImGui::InputFloat("##pct", &m_scalePct[i], 0.0f, 0.0f, "%.1f")) {
                if (m_scaleUniform) {
                    m_scalePct[0] = m_scalePct[1] = m_scalePct[2] = m_scalePct[i];
                }
            }
            ImGui::SameLine(); ImGui::Text("%%");
            ImGui::PopID();
        }
    } else {
        // mm mode — show current dim, target on commit applies a per-axis
        // ratio anchored at bbox-min so growth happens along +axis only
        // (predictable, matches the old body-dim-editor anchor).
        for (int i = 0; i < 3; ++i) {
            auto& edit = m_scaleMmEdit[i];
            if (haveBbox && (!edit.focused || edit.bodyId != targetBodyId)) {
                std::snprintf(edit.buf, sizeof(edit.buf), "%.3f", userExtents[i]);
            }
            ImGui::PushID(100 + i);
            ImGui::TextColored(axisColors[i], "%s", axisLabels[i]);
            ImGui::SameLine(28);
            ImGui::SetNextItemWidth(95.0f);
            ImGui::InputText("##mm", edit.buf, sizeof(edit.buf),
                             ImGuiInputTextFlags_CharsDecimal |
                             ImGuiInputTextFlags_AutoSelectAll);
            if (ImGui::IsItemActivated()) {
                edit.focused = true;
                edit.bodyId = targetBodyId;
                edit.initialExtent = userExtents[i];
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) edit.focused = false;
            ImGui::SameLine(); ImGui::Text("mm");
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("Apply", ImVec2(95, 0))) {
        if (m_selection->hasSelectedBodies()) {
            int bodyId = m_selection->getSelection()[0].bodyId;
            float sx = 1.0f, sy = 1.0f, sz = 1.0f;       // per-WORLD-axis ratios
            double cx = 0, cy = 0, cz = 0;               // pivot in world coords

            try {
                const TopoDS_Shape& shape = m_document->getBody(bodyId);
                Bnd_Box bb;
                BRepBndLib::AddOptimal(shape, bb, Standard_False, Standard_False);
                if (!bb.IsVoid()) {
                    Standard_Real x1, y1, z1, x2, y2, z2;
                    bb.Get(x1, y1, z1, x2, y2, z2);

                    if (!mm) {
                        // % mode: existing centre-pivot scale.
                        cx = (x1 + x2) * 0.5; cy = (y1 + y2) * 0.5; cz = (z1 + z2) * 0.5;
                        sx = m_scalePct[0] / 100.0f;
                        sy = (m_scaleUniform ? m_scalePct[0] : m_scalePct[1]) / 100.0f;
                        sz = (m_scaleUniform ? m_scalePct[0] : m_scalePct[2]) / 100.0f;
                    } else if (haveBbox) {
                        // mm mode: target dims → ratios on each USER axis,
                        // then route to WORLD axes via userToWorld. Pivot at
                        // bbox-MIN so the body grows along +axis only.
                        double targetUser[3];
                        for (int i = 0; i < 3; ++i)
                            targetUser[i] = std::atof(m_scaleMmEdit[i].buf);
                        // Uniform mode: derive ratio from whichever axis the
                        // user most recently changed (focused), or from X by
                        // default. Mirror that ratio to the other axes.
                        int driver = 0;
                        for (int i = 0; i < 3; ++i)
                            if (m_scaleMmEdit[i].focused) { driver = i; break; }
                        if (m_scaleUniform &&
                            userExtents[driver] > 1e-6 &&
                            targetUser[driver] > 0.0) {
                            const double r = targetUser[driver] / userExtents[driver];
                            for (int i = 0; i < 3; ++i)
                                targetUser[i] = userExtents[i] * r;
                        }
                        double userRatio[3] = {1, 1, 1};
                        for (int i = 0; i < 3; ++i) {
                            if (userExtents[i] > 1e-6 && targetUser[i] > 0.0)
                                userRatio[i] = targetUser[i] / userExtents[i];
                        }
                        // user → world remap. Default per-world-axis ratio
                        // stays 1; only the axes that map from a user slot
                        // get overwritten (all three do).
                        float worldRatio[3] = {1.0f, 1.0f, 1.0f};
                        for (int i = 0; i < 3; ++i)
                            worldRatio[userToWorld[i]] = static_cast<float>(userRatio[i]);
                        sx = worldRatio[0]; sy = worldRatio[1]; sz = worldRatio[2];
                        cx = x1; cy = y1; cz = z1; // pivot at bbox-min
                    }
                }
            } catch (...) {}

            if (sx > 0.001f && sy > 0.001f && sz > 0.001f &&
                (std::abs(sx - 1) > 1e-4f ||
                 std::abs(sy - 1) > 1e-4f ||
                 std::abs(sz - 1) > 1e-4f)) {
                auto op = std::make_unique<TransformOp>();
                op->setBodyId(bodyId);
                op->setType(TransformType::Scale);
                op->setCenter(cx, cy, cz);
                op->setScaleXYZ(sx, sy, sz);
                if (m_history->pushOperation(std::move(op), *m_document)) m_meshesDirty = true;
            }
            // Reset % fields after Apply. mm-mode fields reseed naturally
            // from the new bbox next frame.
            m_scalePct[0] = m_scalePct[1] = m_scalePct[2] = 100.0f;
            for (int i = 0; i < 3; ++i) m_scaleMmEdit[i].focused = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(95, 0))) {
        m_scalePct[0] = m_scalePct[1] = m_scalePct[2] = 100.0f;
        for (int i = 0; i < 3; ++i) m_scaleMmEdit[i].focused = false;
    }
    ImGui::End();
}

void Application::renderInteractionsPanel() {
    // A quick-reference of the viewport interactions, docked above Items. The
    // camera rows reflect the live mouse bindings chosen in File > Settings.
    // No collapse handle — Settings > Panels owns show/hide now, so the per-window
    // minimize is just wasted title-bar space.
    ImGui::Begin("Interactions", nullptr, ImGuiWindowFlags_NoCollapse);
    // Action label in a fixed left column, keys to its right. Keeping the action
    // first (it's short) means a long key string extends rightward instead of
    // overlapping the label.
    // Action label, then the binding. Desktop aligns the binding in a fixed
    // column (120 px); touch lays it out ragged (binding right after the label
    // with default spacing) because the 2x font makes a fixed column either
    // overlap the label or force the panel wide — ragged never overlaps and
    // needs the least width.
    const bool touchRagged = materializr::touchMode();
    auto row = [touchRagged](const char* action, const char* keys) {
        ImGui::TextUnformatted(action);
        if (touchRagged) ImGui::SameLine();
        else             ImGui::SameLine(120.0f);
        ImGui::TextColored(materializr::accentText(), "%s", keys);
    };
    if (materializr::touchMode()) {
    // Touch gesture reference. The mouse/keyboard legend is nonsense on a bare
    // tablet ("Scroll wheel", "Ctrl+Click", "W/E/R"), so show the actual finger
    // gestures. Labels/values kept short so the panel can stay narrow. With a
    // mouse/keyboard attached, turn off touch mode for the desktop bindings.
    ImGui::SeparatorText("Camera");
    row("Orbit", "1-finger drag");
    row("Pan", "2-finger drag");
    row("Zoom", "Pinch");
    row("Reset view", "View menu");
    ImGui::SeparatorText("Select");
    row("Face", "Tap");
    row("Context menu", "Long-press");
    row("Body", "Hold ▸ Body");
    row("Add to sel.", "Multi-Select");
    ImGui::SeparatorText("Sketch");
    row("Draw", "Tap or drag");
    row("Finish / cancel", "Buttons");
    row("Dimension", "Tap + type");
    ImGui::SeparatorText("General");
    row("Nav lock", "Move toggle");
    row("Undo / redo", "Buttons");
    } else {
    char orbitKeys[32], panKeys[32];
    std::snprintf(orbitKeys, sizeof(orbitKeys), "%s-drag", mouseButtonName(m_orbitButton));
    std::snprintf(panKeys, sizeof(panKeys), "%s-drag", mouseButtonName(m_panButton));
    ImGui::SeparatorText("Camera");
    row("Orbit", orbitKeys);
    row("Pan", panKeys);
    row("Zoom", "Scroll wheel");
    row("Reset view", "Home");
    ImGui::SeparatorText("Select");
    row("Select face", "Click");
    row("Select body", "Double-click");
    row("Select behind", "Slow double-click");
    row("Add to selection", "Ctrl+Click");
    {
        // In trackpad / left-camera mode a plain drag orbits, so box-select is
        // Alt+Drag; with the default bindings Left is free, so it's a plain drag.
        const bool leftIsCamera = (m_orbitButton == ImGuiMouseButton_Left ||
                                   m_panButton  == ImGuiMouseButton_Left);
        row("Box-select", leftIsCamera ? "Alt+Drag" : "Drag empty space");
    }
    row("Delete selected", "Del");
    ImGui::SeparatorText("Transform (body)");
    row("Move / Rotate / Scale", "W / E / R");
    ImGui::SeparatorText("Sketch");
    row("Start", "Pick a base plane / face");
    row("Finish / Cancel", "Enter / Esc");
    row("Dimension", "Type value + Enter");
    ImGui::SeparatorText("General");
    row("Undo / Redo", "Ctrl+Z / Ctrl+Y");
    }
    ImGui::End();
}

void Application::renderSketchPatternPopup() {
    if (!m_sketchPatternActive) return;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 280,
                                    ImGui::GetWindowPos().y + 50),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(uiSz(280, 0), ImGuiCond_Appearing);
    const char* title = (m_sketchPatternKind == PatternKind::Linear)
                            ? "Sketch Linear Pattern"
                            : "Sketch Circular Pattern";
    ImGui::Begin(title, nullptr,
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize);

    if (m_sketchPatternKind == PatternKind::Linear) {
        ImGui::TextDisabled("Copies along the sketch +X axis.");
    } else {
        ImGui::TextDisabled("Rotates copies around the (x, y) origin in sketch coords.");
    }
    ImGui::Separator();

    if (m_sketchPatternFocusInput) {
        ImGui::SetKeyboardFocusHere();
        m_sketchPatternFocusInput = false;
    }
    bool changed = false;
    ImGui::Text("Copies"); ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputText("##spcount", m_sketchPatternCountBuf,
                     sizeof(m_sketchPatternCountBuf),
                     ImGuiInputTextFlags_CharsDecimal);
    int newCount = std::max(2, std::atoi(m_sketchPatternCountBuf));
    if (newCount != m_sketchPatternCount) { m_sketchPatternCount = newCount; changed = true; }

    if (m_sketchPatternKind == PatternKind::Linear) {
        ImGui::Text("Spacing"); ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::InputText("##spdist", m_sketchPatternDistanceBuf,
                         sizeof(m_sketchPatternDistanceBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(); ImGui::Text("mm");
        float newDist = static_cast<float>(std::atof(m_sketchPatternDistanceBuf));
        if (std::abs(newDist - m_sketchPatternDistance) > 1e-4f) {
            m_sketchPatternDistance = newDist; changed = true;
        }
        if (ImGui::SliderFloat("##spdistslider", &m_sketchPatternDistance,
                               0.1f, 100.0f, "%.2f mm")) {
            std::snprintf(m_sketchPatternDistanceBuf,
                          sizeof(m_sketchPatternDistanceBuf),
                          "%.2f", m_sketchPatternDistance);
            changed = true;
        }
    } else {
        ImGui::Text("Sweep"); ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::InputText("##spangle", m_sketchPatternAngleBuf,
                         sizeof(m_sketchPatternAngleBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(); ImGui::Text("°");
        float newAng = static_cast<float>(std::atof(m_sketchPatternAngleBuf));
        if (std::abs(newAng - m_sketchPatternAngle) > 1e-3f) {
            m_sketchPatternAngle = newAng; changed = true;
        }
        if (ImGui::SliderFloat("##spangslider", &m_sketchPatternAngle,
                               5.0f, 360.0f, "%.1f°")) {
            std::snprintf(m_sketchPatternAngleBuf,
                          sizeof(m_sketchPatternAngleBuf),
                          "%.1f", m_sketchPatternAngle);
            changed = true;
        }

        ImGui::Separator();
        ImGui::TextColored(materializr::accentText(), "Origin");
        ImGui::Text("(%.2f, %.2f) sketch coords",
                    m_sketchPatternOriginX, m_sketchPatternOriginY);
        if (m_sketchPatternPickingOrigin) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                               "Click a point in the sketch… (Esc to cancel)");
            if (ImGui::Button("Cancel picking", ImVec2(-1, 0))) {
                m_sketchPatternPickingOrigin = false;
            }
        } else {
            if (ImGui::Button("Pick origin in sketch", ImVec2(-1, 0))) {
                m_sketchPatternPickingOrigin = true;
            }
            ImGui::TextDisabled("Click in the sketch — snaps to the grid.");
        }
    }

    ImGui::Separator();
    bool apply  = ImGui::Button("Apply",  ImVec2(120, 0));
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel", ImVec2(120, 0));
    bool esc    = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    if (changed) updateSketchPattern();
    if (apply) {
        commitSketchPattern();
    } else if (cancel || esc) {
        cancelSketchPattern();
    }
    ImGui::End();
}

void Application::renderPatternPanel() {
    if (!m_patternActive) return;

    // Same anchor-then-drag pattern as Edit Diameter — first appearance only,
    // then the user can move the popup somewhere convenient.
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 280,
                                    ImGui::GetWindowPos().y + 50),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(uiSz(280, 0), ImGuiCond_Appearing);
    const char* title = (m_patternKind == PatternKind::Linear)
                            ? "Linear Pattern"
                            : "Circular Pattern";
    ImGui::Begin(title, nullptr,
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize);

    bool axisChanged = false;
    if (m_patternKind == PatternKind::Linear) {
        // ---- Direction radio buttons (X / Y / Z) ----
        ImGui::TextColored(materializr::accentText(), "Direction");
        const char* labels[] = { "X", "Y", "Z" };
        for (int i = 0; i < 3; ++i) {
            if (i > 0) ImGui::SameLine();
            if (ImGui::RadioButton(labels[i], m_patternAxisIdx == i)) {
                m_patternAxisIdx = i;
                axisChanged = true;
            }
        }
    } else {
        // ---- Rotation axis combo: construction axes + world X/Y/Z ----
        // Mirrors the Revolve axis picker so any construction axis the user
        // made can drive the pattern (the headline of "axes feed patterns").
        ImGui::TextColored(materializr::accentText(), "Rotation axis");
        std::vector<int> axisIds = m_document->getAllAxisIds();
        std::string current;
        const char* userLabels[3] = {"X (user)", "Y (user)", "Z (user, floor-up)"};
        if (m_patternAxisId >= 0) {
            current = m_document->getAxisName(m_patternAxisId) +
                      " (id " + std::to_string(m_patternAxisId) + ")";
        } else {
            current = userLabels[std::clamp(m_patternAxisIdx, 0, 2)];
        }
        if (ImGui::BeginCombo("##patAxisCombo", current.c_str())) {
            for (int aid : axisIds) {
                const auto* a = m_document->getAxis(aid);
                if (!a) continue;
                std::string label = a->name + "  (id " + std::to_string(aid) + ")";
                if (ImGui::Selectable(label.c_str(), m_patternAxisId == aid)) {
                    m_patternAxisId = aid;
                    axisChanged = true;
                }
            }
            if (!axisIds.empty()) ImGui::Separator();
            for (int i = 0; i < 3; ++i) {
                bool sel = (m_patternAxisId < 0 && m_patternAxisIdx == i);
                if (ImGui::Selectable(userLabels[i], sel)) {
                    m_patternAxisId = -1;
                    m_patternAxisIdx = i;
                    axisChanged = true;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Separator();

    // ---- Count ----
    ImGui::Text("Copies"); ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (m_patternInputFocus) {
        ImGui::SetKeyboardFocusHere();
        m_patternInputFocus = false;
    }
    bool countEnter = ImGui::InputText("##patcount", m_patternCountBuf,
                                       sizeof(m_patternCountBuf),
                                       ImGuiInputTextFlags_EnterReturnsTrue |
                                       ImGuiInputTextFlags_CharsDecimal);
    int parsedCount = std::max(2, std::atoi(m_patternCountBuf));
    bool countChanged = parsedCount != m_patternCount;
    if (countChanged) m_patternCount = parsedCount;

    // ---- Distance (linear) or Angle (radial) ----
    bool distChanged = false;
    if (m_patternKind == PatternKind::Linear) {
        ImGui::Text("Spacing"); ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::InputText("##patdist", m_patternDistanceBuf,
                         sizeof(m_patternDistanceBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(); ImGui::Text("mm");
        float parsed = static_cast<float>(std::atof(m_patternDistanceBuf));
        if (std::abs(parsed - m_patternDistance) > 1e-4f) {
            m_patternDistance = parsed; distChanged = true;
        }
        // Slider that mirrors the text field — quick sweep without retyping.
        if (ImGui::SliderFloat("##patdistslider", &m_patternDistance, 0.1f, 100.0f, "%.2f mm")) {
            std::snprintf(m_patternDistanceBuf, sizeof(m_patternDistanceBuf),
                          "%.2f", m_patternDistance);
            distChanged = true;
        }
    } else {
        ImGui::Text("Sweep"); ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::InputText("##patangle", m_patternAngleBuf,
                         sizeof(m_patternAngleBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(); ImGui::Text("°");
        float parsed = static_cast<float>(std::atof(m_patternAngleBuf));
        if (std::abs(parsed - m_patternAngle) > 1e-3f) {
            m_patternAngle = parsed; distChanged = true;
        }
        if (ImGui::SliderFloat("##patangleslider", &m_patternAngle, 5.0f, 360.0f, "%.1f°")) {
            std::snprintf(m_patternAngleBuf, sizeof(m_patternAngleBuf),
                          "%.1f", m_patternAngle);
            distChanged = true;
        }

        // ---- Axis origin (radial only) ----
        ImGui::Separator();
        ImGui::TextColored(materializr::accentText(), "Axis origin");
        if (m_patternAxisId >= 0) {
            // A construction axis defines its own origin — copies orbit its
            // centreline, so the manual origin picker doesn't apply.
            if (const auto* a = m_document->getAxis(m_patternAxisId)) {
                ImGui::Text("(%.2f, %.2f, %.2f)", a->origin.X(), a->origin.Y(), a->origin.Z());
            }
            ImGui::TextDisabled("From the selected construction axis.");
        } else {
            ImGui::Text("(%.2f, %.2f, %.2f)", m_patternOriginX, m_patternOriginY, m_patternOriginZ);
            if (m_patternPickingOrigin) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                                   "Pick a point in the viewport… (Esc to cancel)");
                if (ImGui::Button("Cancel picking", ImVec2(-1, 0))) {
                    m_patternPickingOrigin = false;
                }
            } else {
                if (ImGui::Button("Pick axis origin in viewport", ImVec2(-1, 0))) {
                    m_patternPickingOrigin = true;
                }
                ImGui::TextDisabled("Click a point in the viewport — snaps to the grid.");
            }
        }
    }

    // ---- Apply / Cancel ----
    ImGui::Separator();
    bool applyClicked  = ImGui::Button("Apply", ImVec2(120, 0));
    ImGui::SameLine();
    bool cancelClicked = ImGui::Button("Cancel", ImVec2(120, 0));
    bool escPressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    if (axisChanged || countChanged || distChanged) updatePattern();
    if (countEnter || applyClicked) {
        commitPattern();
    } else if (cancelClicked || escPressed) {
        cancelPattern();
    }

    ImGui::End();
}


void Application::renderThreadPanel() {
    // Worker-thread completion: poll the future each frame; when the cut
    // lands, push the real op with the precomputed result (instant) and tear
    // the popup down. A modal keeps input blocked meanwhile so the window
    // stays responsive instead of "not responding".
    if (m_threadComputing) {
        if (m_threadFuture.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready) {
            TopoDS_Shape result = m_threadFuture.get();
            m_threadComputing = false;
            if (!result.IsNull()) {
                auto op = makeThreadOpFromState();
                op->setPrecomputedResult(result);
                if (!m_history->pushOperation(std::move(op), *m_document)) {
                    std::fprintf(stderr, "[Thread] push failed unexpectedly\n");
                }
            } else {
                std::fprintf(stderr, "[Thread] failed — pitch/depth may be too "
                                     "large for the face (or > 300 turns)\n");
            }
            m_threadActive = false;
            m_threadBodyId = -1;
            m_selection->clear();
            m_meshesDirty = true;
        } else {
            ImGui::OpenPopup("Cutting thread…");
            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal("Cutting thread…", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoMove)) {
                int dots = static_cast<int>(ImGui::GetTime() * 2.0) % 4;
                ImGui::Text("Sweeping the helical groove%.*s", dots, "...");
                ImGui::Spacing();
                drawIndeterminateBar();
                ImGui::Spacing();
                ImGui::TextDisabled("A few seconds for typical threads.");
                ImGui::EndPopup();
            }
            return; // suppress the parameter popup while computing
        }
    }
    if (!m_threadActive) return;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 280,
                                    ImGui::GetWindowPos().y + 50),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(uiSz(280, 0), ImGuiCond_Appearing);
    ImGui::Begin("Thread", nullptr,
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::TextColored(materializr::accentText(), "%s thread",
                       m_threadIsHole ? "Internal" : "External");
    ImGui::Text("Diameter %.2f mm, length %.2f mm",
                m_threadRadius * 2.0, m_threadLength);
    ImGui::Separator();

    ImGui::Text("Pitch"); ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputText("##thrPitch", m_threadPitchBuf, sizeof(m_threadPitchBuf),
                         ImGuiInputTextFlags_CharsDecimal)) {
        float v = static_cast<float>(std::atof(m_threadPitchBuf));
        if (v >= 0.1f) m_threadPitch = v;
    }
    ImGui::SameLine(); ImGui::Text("mm");

    ImGui::Text("Depth"); ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputText("##thrDepth", m_threadDepthBuf, sizeof(m_threadDepthBuf),
                         ImGuiInputTextFlags_CharsDecimal)) {
        float v = static_cast<float>(std::atof(m_threadDepthBuf));
        if (v >= 0.05f) m_threadDepth = v;
    }
    ImGui::SameLine(); ImGui::Text("mm");
    // Depth beyond ~0.65·pitch merges grooves into floating helical fins;
    // beyond ~45% of the radius it eats the core. Clamp + say so.
    {
        float maxDepth = static_cast<float>(
            std::min(0.65 * m_threadPitch, 0.45 * m_threadRadius));
        if (m_threadDepth > maxDepth) {
            m_threadDepth = maxDepth;
            std::snprintf(m_threadDepthBuf, sizeof(m_threadDepthBuf), "%.2f",
                          m_threadDepth);
        }
        ImGui::TextDisabled("Depth caps at 0.65 \xC3\x97 pitch.");
    }

    ImGui::Checkbox("Right-handed", &m_threadRightHanded);

    double turns = m_threadLength / std::max(0.1f, m_threadPitch);
    ImGui::TextDisabled("%.0f turns over the face", turns);
    if (turns > 300.0) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                           "Too many turns (max 300) — raise the pitch.");
    }
    ImGui::TextDisabled("Computed on Apply — may take a few seconds.");
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f),
                       "Apply threads LAST.");
    ImGui::TextWrapped("Make all other cuts (holes, slots, splits, chamfers) "
                       "first — modeling operations on threaded bodies are "
                       "refused. To change a threaded part: delete the "
                       "Thread step in History, edit, then re-thread.");

    ImGui::Separator();
    bool applyClicked  = ImGui::Button("Apply", ImVec2(120, 0));
    ImGui::SameLine();
    bool cancelClicked = ImGui::Button("Cancel", ImVec2(120, 0));
    bool escPressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    if (applyClicked && turns <= 300.0) {
        commitThread();
    } else if (cancelClicked || escPressed) {
        cancelThread();
    }

    ImGui::End();
}

void Application::renderLoftPanel() {
    if (!m_loftActive) return;

    // Anchor near the top-right on first open; subsequent frames let the user
    // drag the popup somewhere convenient (same pattern as Pattern panel).
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 280,
                                    ImGui::GetWindowPos().y + 50),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(uiSz(280, 0), ImGuiCond_Appearing);
    ImGui::Begin("Loft", nullptr,
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize);

    bool changed = false;
    if (ImGui::Checkbox("Solid (off = surface shell)", &m_loftSolid)) changed = true;
    ImGui::SetItemTooltip("On: ThruSections caps the ends and produces a solid "
                          "body. Off: open shell — useful when one profile is "
                          "open or you want a swept surface.");
    if (ImGui::Checkbox("Ruled surface (off = smooth)", &m_loftRuled)) changed = true;
    ImGui::SetItemTooltip("Ruled draws straight-line ribs between matching "
                          "vertices on the two profiles. Smooth interpolates a "
                          "curved surface — usually nicer between similar "
                          "profiles, less predictable between dissimilar ones.");
    if (ImGui::Checkbox("Reverse profile B vertex order", &m_loftReverseB)) changed = true;
    ImGui::SetItemTooltip("Re-pairs vertices between the two profiles. Use this "
                          "if the loft pinches to an apex or twists — usually "
                          "means the wires' start vertices weren't lined up.");

    ImGui::Separator();
    bool applyClicked  = ImGui::Button("Apply", ImVec2(120, 0));
    ImGui::SameLine();
    bool cancelClicked = ImGui::Button("Cancel", ImVec2(120, 0));
    bool escPressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    if (changed) updateLoft();
    if (applyClicked) {
        commitLoft();
    } else if (cancelClicked || escPressed) {
        cancelLoft();
    }

    ImGui::End();
}

void Application::renderSketchMovePanel() {
    // Only when Move gizmo is active on a single standalone sketch — counted
    // as the number of DISTINCT parent sketch ids across Sketch and
    // SketchRegion entries (a region clicked inside is the user pointing at
    // its sketch). No bodies — those route through the multi-transform path.
    int distinctSketches = 0;
    if (m_selection) {
        std::vector<int> seen;
        for (const auto& e : m_selection->getSelection()) {
            if ((e.type == SelectionType::Sketch ||
                 e.type == SelectionType::SketchRegion) && e.sketchId >= 0) {
                bool dup = false;
                for (int x : seen) if (x == e.sketchId) { dup = true; break; }
                if (!dup) { seen.push_back(e.sketchId); ++distinctSketches; }
            }
        }
    }
    // Only appears when the user has explicitly armed the sketch gizmo (via
    // Move in the Tools panel) for the currently-selected sketch — matches
    // the visibility rule for the gizmo itself. Without this gate the panel
    // pops up as soon as you select a sketch, because the gizmo's mode
    // persists across selections and defaults to Translate.
    bool conditionsMet = m_gizmo && m_gizmo->getMode() == GizmoMode::Translate &&
                         m_selection && distinctSketches == 1 &&
                         m_selection->selectedBodyCount() == 0 &&
                         !m_inSketchMode &&
                         m_sketchGizmoArmed;
    if (conditionsMet && !m_sketchMoveConditionsMet) {
        m_sketchMovePanelOpen = true;
        m_sketchMove[0] = m_sketchMove[1] = m_sketchMove[2] = 0.0f;
        for (int i = 0; i < 3; ++i)
            std::snprintf(m_sketchMoveBuf[i], sizeof(m_sketchMoveBuf[i]), "0");
    }
    m_sketchMoveConditionsMet = conditionsMet;
    if (!conditionsMet || !m_sketchMovePanelOpen) return;

    ImGui::SetNextWindowSize(uiSz(340, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Move Sketch###SketchMove", &m_sketchMovePanelOpen)) {
        ImGui::End(); return;
    }

    ImGui::TextWrapped("Type an exact offset to translate the sketch's plane "
                       "by - useful when you want the second \"construction "
                       "plane\" at a precise distance instead of dragging the "
                       "gizmo. Apply nudges the sketch by the typed values "
                       "(from its current pose) and resets the fields.");
    ImGui::Spacing();

    // Buffer-based inputs: more reliable than ImGui::InputFloat for our case.
    // The buffer is the source of truth each frame; atof + writeback into
    // m_sketchMove keeps the value live without needing an Enter-press.
    const char* axisLabels[3] = { "X", "Y", "Z" };
    const ImVec4 axisColors[3] = {
        ImVec4(1.00f, 0.35f, 0.35f, 1.0f),
        ImVec4(0.35f, 1.00f, 0.35f, 1.0f),
        ImVec4(0.40f, 0.55f, 1.00f, 1.0f),
    };
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        ImGui::TextColored(axisColors[i], "%s", axisLabels[i]);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::InputText("##input", m_sketchMoveBuf[i], sizeof(m_sketchMoveBuf[i]),
                         ImGuiInputTextFlags_CharsDecimal |
                         ImGuiInputTextFlags_CharsNoBlank |
                         ImGuiInputTextFlags_AutoSelectAll);
        ImGui::SameLine(); ImGui::Text("mm");
        m_sketchMove[i] = static_cast<float>(std::atof(m_sketchMoveBuf[i]));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130);
        if (ImGui::SliderFloat("##slider", &m_sketchMove[i], -100.0f, 100.0f, "%.2f")) {
            std::snprintf(m_sketchMoveBuf[i], sizeof(m_sketchMoveBuf[i]),
                          "%.3f", m_sketchMove[i]);
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    bool anyNonZero = std::abs(m_sketchMove[0]) > 1e-4f ||
                      std::abs(m_sketchMove[1]) > 1e-4f ||
                      std::abs(m_sketchMove[2]) > 1e-4f;
    ImGui::BeginDisabled(!anyNonZero);
    if (ImGui::Button("Apply", ImVec2(100, 0))) applySketchMove();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(100, 0))) {
        m_sketchMove[0] = m_sketchMove[1] = m_sketchMove[2] = 0.0f;
        for (int i = 0; i < 3; ++i)
            std::snprintf(m_sketchMoveBuf[i], sizeof(m_sketchMoveBuf[i]), "0");
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(100, 0))) m_sketchMovePanelOpen = false;

    ImGui::End();
}

void Application::applySketchMove() {
    if (!m_selection || !m_document || !m_history) {
        std::fprintf(stderr, "[SketchMove] missing selection/document/history\n");
        return;
    }
    int sketchId = -1;
    for (const auto& e : m_selection->getSelection()) {
        if ((e.type == SelectionType::Sketch ||
             e.type == SelectionType::SketchRegion) && e.sketchId >= 0) {
            sketchId = e.sketchId; break;
        }
    }
    if (sketchId < 0) {
        std::fprintf(stderr, "[SketchMove] no sketch in selection\n");
        return;
    }

    gp_Trsf trsf;
    trsf.SetTranslation(gp_Vec(m_sketchMove[0], m_sketchMove[1], m_sketchMove[2]));
    auto op = std::make_unique<materializr::SketchTransformOp>();
    op->setSketch(sketchId);
    op->setTransform(trsf);
    bool ok = m_history->pushOperation(std::move(op), *m_document);
    std::fprintf(stderr, "[SketchMove] apply sketch=%d delta=(%.3f, %.3f, %.3f) -> %s\n",
                 sketchId, m_sketchMove[0], m_sketchMove[1], m_sketchMove[2],
                 ok ? "ok" : "FAILED");

    m_sketchMove[0] = m_sketchMove[1] = m_sketchMove[2] = 0.0f;
    for (int i = 0; i < 3; ++i)
        std::snprintf(m_sketchMoveBuf[i], sizeof(m_sketchMoveBuf[i]), "0");
    m_meshesDirty = true;
}

void Application::renderSnapWidget() {
    // Tucked just under the ViewCube. We borrow the ViewCube's window-anchor
    // arithmetic (top-right of the viewport window) so the widget sits in a
    // consistent spot regardless of dock layout.
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 ws = ImGui::GetWindowSize();
    const float pad     = 10.0f;
    // Square shrunk ~25 % from its original 38 px (font stays the same — it
    // was over-padded before). +20 px nudge right keeps it tucked under the
    // ViewCube's accessory arc.
    const float size    = 28.0f;
    const float widgetR = 38.0f;
    const float xNudge  = 20.0f;
    ImVec2 widgetPos(wp.x + ws.x - pad - widgetR - 26.0f - size * 0.5f + xNudge,
                     wp.y + pad + widgetR * 2.0f + 96.0f);
    ImVec2 widgetEnd(widgetPos.x + size, widgetPos.y + size);

    // Manual hit-test — same pattern the ViewCube uses to anchor in a corner
    // without polluting the parent window's layout cursor (which is what
    // ImGui's boundary-extension assert was complaining about when we used
    // SetCursorScreenPos + InvisibleButton).
    ImGui::PushID("snap-corner-widget");
    bool hovered = ImGui::IsMouseHoveringRect(widgetPos, widgetEnd) &&
                   ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                                          ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    bool clicked      = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    bool rightClicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    if (hovered) {
        ImGui::BeginTooltip();
        ImGui::Text("Snap step: %.3g mm   |   %s", m_sketchGridStep,
                    m_snapToGrid ? "Snap ON" : "Snap off");
        ImGui::TextDisabled("Click: open snap settings");
        ImGui::TextDisabled("Right-click: toggle snap");
        ImGui::EndTooltip();
    }
    if (clicked) ImGui::OpenPopup("SnapSettings");
    if (rightClicked) {
        m_snapToGrid = !m_snapToGrid;
        if (m_toolbar) m_toolbar->setSnapToGrid(m_snapToGrid);
        saveAppSettings();
    }
    // Latch this frame's hover state so the next frame's viewport input
    // handlers know to skip picker / sketch-tool clicks over the widget.
    // OR with the popup-open check so dragging through the popup also stays
    // out of the picker.
    m_snapWidgetHovered = hovered || ImGui::IsPopupOpen("SnapSettings");

    // Draw the square + step label + blue border when snap is on.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = hovered ? IM_COL32(60, 70, 90, 230) : IM_COL32(30, 35, 45, 210);
    dl->AddRectFilled(widgetPos, widgetEnd, bg, 5.0f);
    if (m_snapToGrid) {
        dl->AddRect(widgetPos, widgetEnd, IM_COL32(60, 140, 255, 255), 5.0f, 0, 2.5f);
    } else {
        dl->AddRect(widgetPos, widgetEnd, IM_COL32(120, 120, 130, 200), 5.0f, 0, 1.0f);
    }
    char buf[8];
    if      (m_sketchGridStep < 0.3f)  std::snprintf(buf, sizeof(buf), "0.1");
    else if (m_sketchGridStep < 0.75f) std::snprintf(buf, sizeof(buf), "0.5");
    else if (m_sketchGridStep < 5.0f)  std::snprintf(buf, sizeof(buf), "1");
    else                               std::snprintf(buf, sizeof(buf), "10");
    ImGui::PushFont(nullptr);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    ImVec2 tp(widgetPos.x + (size - ts.x) * 0.5f,
              widgetPos.y + (size - ts.y) * 0.5f);
    dl->AddText(tp, IM_COL32(240, 240, 245, 255), buf);
    ImGui::PopFont();

    // Settings popup — checkbox + radio buttons. Each change saves to the
    // settings file immediately so the choice survives the next launch.
    if (ImGui::BeginPopup("SnapSettings")) {
        ImGui::TextColored(materializr::accentText(), "Snap & Grid");
        ImGui::Separator();
        bool snap = m_snapToGrid;
        if (ImGui::Checkbox("Snap to grid", &snap)) {
            m_snapToGrid = snap;
            if (m_toolbar) m_toolbar->setSnapToGrid(m_snapToGrid);
            saveAppSettings();
        }
        ImGui::Spacing();
        ImGui::Text("Step (mm)");
        const float steps[] = { 0.1f, 0.5f, 1.0f, 10.0f };
        const char* labels[] = { "0.1", "0.5", "1", "10" };
        for (int i = 0; i < 4; ++i) {
            if (i > 0) ImGui::SameLine();
            bool active = std::abs(m_sketchGridStep - steps[i]) < 1e-4f;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
            if (ImGui::Button(labels[i], ImVec2(46, 26))) {
                m_sketchGridStep = steps[i];
                if (m_toolbar) m_toolbar->setGridStep(m_sketchGridStep);
                saveAppSettings();
                // Picking a step is the task — close the popup so drawing
                // resumes immediately (no click-away-to-dismiss).
                ImGui::CloseCurrentPopup();
            }
            if (active) ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Settings persist across launches.");
        ImGui::EndPopup();
    }

    ImGui::PopID();
}

void Application::renderConstructionPlanePanel() {
    if (!m_planeOpActive) return;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 280,
                                    ImGui::GetWindowPos().y + 50),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(uiSz(280, 0), ImGuiCond_Appearing);
    ImGui::Begin("Construction Plane", nullptr,
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::TextColored(materializr::accentText(), "Alignment");
    bool kindChanged = false;
    auto kindRadio = [&](const char* label, int idx, bool enabled = true) {
        if (!enabled) ImGui::BeginDisabled();
        if (idx > 0) ImGui::SameLine();
        if (ImGui::RadioButton(label, m_planeOpKindIdx == idx)) {
            m_planeOpKindIdx = idx;
            kindChanged = true;
        }
        if (!enabled) ImGui::EndDisabled();
    };
    kindRadio("XY", 0);
    kindRadio("XZ", 1);
    kindRadio("YZ", 2);
    if (m_planeOpHaveFace) {
        if (ImGui::RadioButton("Parallel to selected face", m_planeOpKindIdx == 3)) {
            m_planeOpKindIdx = 3;
            kindChanged = true;
        }
    } else {
        ImGui::TextDisabled("(Select a planar face to enable Parallel-to-face.)");
    }

    // Derived modes — each enabled only when its required selection exists.
    if (m_planeOpHaveTwoPlanes) {
        if (ImGui::RadioButton("Midplane (between 2 planes/faces)", m_planeOpKindIdx == 4)) {
            m_planeOpKindIdx = 4; kindChanged = true;
        }
    }
    if (m_planeOpHaveAxis) {
        if (ImGui::RadioButton("Normal to selected axis/edge", m_planeOpKindIdx == 5)) {
            m_planeOpKindIdx = 5; kindChanged = true;
        }
    }
    if (m_planeOpHaveCylinder) {
        if (ImGui::RadioButton("Tangent to selected cylinder", m_planeOpKindIdx == 6)) {
            m_planeOpKindIdx = 6; kindChanged = true;
        }
        if (ImGui::RadioButton("Through cylinder axis (longitudinal)", m_planeOpKindIdx == 7)) {
            m_planeOpKindIdx = 7; kindChanged = true;
        }
    }
    if (!m_planeOpHaveTwoPlanes && !m_planeOpHaveAxis && !m_planeOpHaveCylinder) {
        ImGui::TextDisabled("(Select 2 planes/faces, an axis/edge, or a cylinder\n"
                            " for Midplane / Normal-to-axis / Tangent.)");
    }

    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "Offset");
    // Sync the slider/field with the live preview plane's distance from
    // world origin along its current normal, so the value reflects gizmo
    // drags and rotations rather than only the popup-input history. Skip
    // when the user is actively typing the field (else editing would jump
    // mid-keystroke).
    // Only sync for the standard / parallel modes: the derived modes (4/5/6)
    // measure their offset relative to a computed base position, so reading
    // back the plane's absolute distance-from-origin would fight them.
    if (m_planeOpKindIdx <= 3) {
        auto ids = m_document->getAllPlaneIds();
        if (!ids.empty()) {
            const auto* p = m_document->getPlane(ids.back());
            if (p) {
                const gp_Dir& nd = p->plane.Position().Direction();
                const gp_Pnt& o  = p->plane.Position().Location();
                double along = nd.X() * o.X() + nd.Y() * o.Y() + nd.Z() * o.Z();
                if (!ImGui::IsItemActive() &&
                    std::abs(along - m_planeOpOffset) > 1e-4) {
                    m_planeOpOffset = along;
                    std::snprintf(m_planeOpOffsetBuf, sizeof(m_planeOpOffsetBuf),
                                  "%.2f", m_planeOpOffset);
                }
            }
        }
    }
    ImGui::Text("Distance"); ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    bool offsetChanged = false;
    if (ImGui::InputText("##planeoffset", m_planeOpOffsetBuf, sizeof(m_planeOpOffsetBuf),
                         ImGuiInputTextFlags_CharsDecimal)) {
        double parsed = std::atof(m_planeOpOffsetBuf);
        if (std::abs(parsed - m_planeOpOffset) > 1e-4) {
            m_planeOpOffset = parsed;
            offsetChanged = true;
        }
    }
    ImGui::SameLine(); ImGui::Text("mm");
    float offsetF = static_cast<float>(m_planeOpOffset);
    if (ImGui::SliderFloat("##planeoffsetslider", &offsetF, -100.0f, 100.0f, "%.2f mm")) {
        m_planeOpOffset = offsetF;
        std::snprintf(m_planeOpOffsetBuf, sizeof(m_planeOpOffsetBuf), "%.2f", m_planeOpOffset);
        offsetChanged = true;
    }
    ImGui::TextDisabled("Pushes the plane along its normal. Negative for the "
                        "opposite side.");

    // Gizmo for the preview plane appears automatically (the popup auto-
    // selects the just-pushed plane). Switch modes with W/E or click the
    // plane after committing.
    ImGui::TextDisabled("W = Move gizmo, E = Rotate gizmo.");

    // Type-an-exact-angle rotation. Applies on top of whatever the popup
    // base orientation + gizmo edits produced; lets the user dial in a
    // precise angle (e.g. 23.5°) that's awkward to land with the snap.
    // Axis labels use Z-up convention (user X = world X, user Y = world Z,
    // user Z = world Y), so picking "Z" rotates around the up axis.
    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "Rotate by");
    ImGui::SetNextItemWidth(80);
    // Enter in the field is equivalent to clicking Apply — same shortcut the
    // sketch dim popup uses, so the user can dial in 23.5°, press Enter,
    // and move on without reaching for the mouse.
    bool rotEnter = ImGui::InputText("##planeRotDeg", m_planeOpRotBuf,
                                     sizeof(m_planeOpRotBuf),
                                     ImGuiInputTextFlags_CharsDecimal |
                                     ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(); ImGui::Text("\xC2\xB0 around");
    ImGui::SameLine();
    if (ImGui::RadioButton("X##planeRotX", m_planeOpRotAxisIdx == 0)) m_planeOpRotAxisIdx = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Y##planeRotY", m_planeOpRotAxisIdx == 2)) m_planeOpRotAxisIdx = 2;
    ImGui::SameLine();
    if (ImGui::RadioButton("Z##planeRotZ", m_planeOpRotAxisIdx == 1)) m_planeOpRotAxisIdx = 1;
    ImGui::SameLine();
    bool rotApply = ImGui::SmallButton("Apply##planeRotApply");
    if (rotApply || rotEnter) {
        float deg = static_cast<float>(std::atof(m_planeOpRotBuf));
        if (std::abs(deg) > 1e-4f) {
            // Find the most-recently-added plane (auto-selected on push)
            // and rotate it around its CURRENT origin by `deg`° about the
            // chosen world axis. Stays additive: typing 10°, Apply, then
            // 10°, Apply nets 20° total.
            auto ids = m_document->getAllPlaneIds();
            if (!ids.empty()) {
                int pid = ids.back();
                const auto* entry = m_document->getPlane(pid);
                if (entry) {
                    gp_Pln pln = entry->plane;
                    gp_Pnt o = pln.Position().Location();
                    gp_Dir ax;
                    // user X = world X; user Z = world Y (up); user Y = world Z.
                    if      (m_planeOpRotAxisIdx == 0) ax = gp_Dir(1, 0, 0);
                    else if (m_planeOpRotAxisIdx == 1) ax = gp_Dir(0, 1, 0);
                    else                                ax = gp_Dir(0, 0, 1);
                    gp_Trsf t;
                    t.SetRotation(gp_Ax1(o, ax), deg * M_PI / 180.0);
                    pln.Transform(t);
                    m_document->setPlane(pid, pln);
                    m_meshesDirty = true;
                }
            }
        }
        // Reset for the next stacked rotation.
        m_planeOpRotDeg = 0.0f;
        std::snprintf(m_planeOpRotBuf, sizeof(m_planeOpRotBuf), "0.0");
    }

    ImGui::Separator();
    bool applyClicked  = ImGui::Button("Apply", ImVec2(120, 0));
    ImGui::SameLine();
    bool cancelClicked = ImGui::Button("Cancel", ImVec2(120, 0));
    bool escPressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    if (kindChanged || offsetChanged) updateConstructionPlane();
    if (applyClicked) {
        commitConstructionPlane();
    } else if (cancelClicked || escPressed) {
        cancelConstructionPlane();
    }

    ImGui::End();
}

void Application::beginRevolve() {
    if (m_revolveActive) return;
    m_revolveSketchId = -1;
    m_revolveAxisId   = -1;
    m_revolveBodyId   = -1;
    m_revolveBodyIds.clear();
    // First-of-kind capture for sketch / axis (popup operates on one of
    // each); collect EVERY body so Rotate Body can multi-rotate them
    // around a single axis. Sweep Sketch only uses the primary body for
    // boolean targeting.
    // ANY entry that names a body — Body / Face / Edge / Vertex — counts
    // its parent body as a rotate target. The user's "ctrl-click two
    // bodies, click Revolve" flow often lands one as Body and the other
    // as Face (depending on where they clicked); treating both as body
    // targets matches the visible selection.
    for (const auto& e : m_selection->getSelection()) {
        if ((e.type == SelectionType::Sketch ||
             e.type == SelectionType::SketchRegion) &&
            e.sketchId >= 0 && m_revolveSketchId < 0) {
            m_revolveSketchId = e.sketchId;
        } else if (e.type == SelectionType::Axis && e.axisId >= 0 &&
                   m_revolveAxisId < 0) {
            m_revolveAxisId = e.axisId;
        } else if (e.bodyId >= 0 &&
                   (e.type == SelectionType::Body ||
                    e.type == SelectionType::Face ||
                    e.type == SelectionType::Edge ||
                    e.type == SelectionType::Vertex)) {
            if (m_revolveBodyId < 0) m_revolveBodyId = e.bodyId;
            bool dup = false;
            for (int bid : m_revolveBodyIds)
                if (bid == e.bodyId) { dup = true; break; }
            if (!dup) m_revolveBodyIds.push_back(e.bodyId);
        }
    }
    // Default the What mode from the selection: a captured sketch profile
    // means the user wants to revolve it (Sweep); bodies-only means rotate
    // in place. Previously this kept its last value (default Rotate Body),
    // so revolving a selected profile silently committed a rotate-snapshot
    // step ("Batched transform") instead of a real, re-editable RevolveOp.
    m_revolveWhatIdx = (m_revolveSketchId >= 0) ? 1 : 0;
    // Reset to a neutral start angle every time the popup opens so the
    // user isn't surprised by the last session's value sticking around
    // (and so live preview begins at "no rotation" and tracks the slider
    // outwards from there).
    m_revolveAngle = 0.0f;
    m_revolveActive = true;
    std::snprintf(m_revolveAngleBuf, sizeof(m_revolveAngleBuf), "%.1f",
                  m_revolveAngle);
    std::fprintf(stderr, "[Revolve] begin: captured %d bodies (primary=%d), "
                         "sketch=%d, axis=%d\n",
                 (int)m_revolveBodyIds.size(), m_revolveBodyId,
                 m_revolveSketchId, m_revolveAxisId);
    for (size_t i = 0; i < m_revolveBodyIds.size(); ++i)
        std::fprintf(stderr, "[Revolve]   body[%zu] = id %d (%s)\n",
                     i, m_revolveBodyIds[i],
                     m_document->getBodyName(m_revolveBodyIds[i]).c_str());
    revolveLiveBegin();
}

void Application::renderRevolvePopup() {
    if (!m_revolveActive) return;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 320,
                                    ImGui::GetWindowPos().y + 50),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(uiSz(320, 0), ImGuiCond_Appearing);
    // The mode is chosen by the selection (sketch → Lathe, body → Revolve) in
    // beginRevolve(); the window title reflects it. No in-popup mode switch —
    // to change mode you change the selection.
    const bool lathe = (m_revolveWhatIdx == 1);
    ImGui::Begin(lathe ? "Lathe###RevolvePopup" : "Revolve###RevolvePopup", nullptr,
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize);

    // Selection summary so the popup reflects what the click captured.
    // Both flows want a body shown (Rotate Body operates on it; Sweep
    // boolean modes target it). Sketch only matters in Sweep mode.
    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "Selection");
    const int bodyCount = static_cast<int>(m_revolveBodyIds.size());
    if (bodyCount == 1) {
        ImGui::Text("• Body: %s (id %d)",
                    m_document->getBodyName(m_revolveBodyId).c_str(),
                    m_revolveBodyId);
    } else if (bodyCount > 1) {
        ImGui::Text("• %d bodies — rotate together around the axis", bodyCount);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.35f, 1.0f), "• Body: none");
    }
    if (m_revolveWhatIdx == 1) {
        if (m_revolveSketchId >= 0) {
            ImGui::Text("• Sketch: %s (id %d)",
                        m_document->getSketchName(m_revolveSketchId).c_str(),
                        m_revolveSketchId);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.35f, 1.0f),
                               "• Sketch: none — select one and re-open.");
        }
    }

    // Axis picker — combo box listing every construction axis in the
    // document plus the canonical user-Z-up world axes at the bottom.
    // Solves the "I can't pick the axis I just made" report.
    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "Axis");
    std::vector<int> axisIds = m_document->getAllAxisIds();
    std::string current;
    if (m_revolveAxisId >= 0) {
        current = m_document->getAxisName(m_revolveAxisId) +
                  " (id " + std::to_string(m_revolveAxisId) + ")";
    } else {
        const char* worldLabels[3] = {"World X", "World Y (Z-up depth)", "World Z (Z-up up)"};
        current = worldLabels[std::clamp(m_revolveWorldAxisIdx, 0, 2)];
    }
    if (ImGui::BeginCombo("##revAxisCombo", current.c_str())) {
        for (int aid : axisIds) {
            const auto* a = m_document->getAxis(aid);
            if (!a) continue;
            std::string label = a->name + "  (id " + std::to_string(aid) + ")";
            bool sel = (m_revolveAxisId == aid);
            if (ImGui::Selectable(label.c_str(), sel)) m_revolveAxisId = aid;
        }
        if (!axisIds.empty()) ImGui::Separator();
        // Z-up: the popup's "X / Y / Z" maps to world X / Z / Y so the
        // labels line up with the sketch / move-gizmo conventions
        // elsewhere in the app.
        const char* userLabels[3] = {"X (user)", "Y (user)", "Z (user, floor-up)"};
        for (int i = 0; i < 3; ++i) {
            bool sel = (m_revolveAxisId < 0 && m_revolveWorldAxisIdx == i);
            if (ImGui::Selectable(userLabels[i], sel)) {
                m_revolveAxisId = -1;
                m_revolveWorldAxisIdx = i;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "Angle");
    ImGui::SetNextItemWidth(100);
    bool angleChanged = false;
    if (ImGui::InputText("##revAng", m_revolveAngleBuf, sizeof(m_revolveAngleBuf),
                         ImGuiInputTextFlags_CharsDecimal)) {
        m_revolveAngle = static_cast<float>(std::atof(m_revolveAngleBuf));
        angleChanged = true;
    }
    ImGui::SameLine(); ImGui::Text("°");
    if (ImGui::SliderFloat("##revAngSld", &m_revolveAngle,
                            (m_revolveWhatIdx == 0 ? -360.0f : 0.1f),
                            360.0f, "%.1f°")) {
        std::snprintf(m_revolveAngleBuf, sizeof(m_revolveAngleBuf),
                      "%.1f", m_revolveAngle);
        angleChanged = true;
    }
    // Live preview: every angle change in Rotate Body mode applies a
    // fresh-from-snapshot rotation so the user sees the result without
    // having to Apply. Threshold guards against float jitter triggering a
    // pointless rebuild every frame the slider's parked.
    if (angleChanged && m_revolveWhatIdx == 0) {
        std::fprintf(stderr, "[Revolve] angle changed to %.2f  liveActive=%d "
                             "lastApplied=%.2f  bodies=%zu\n",
                     m_revolveAngle,
                     int(m_revolveLiveActive),
                     m_revolveLastAppliedAngle,
                     m_revolveBodyIds.size());
    }
    if (m_revolveWhatIdx == 0 && m_revolveLiveActive &&
        std::abs(m_revolveAngle - m_revolveLastAppliedAngle) > 0.05f) {
        revolveLiveApply(m_revolveAngle);
    }

    // Boolean mode applies only to Sweep Sketch — Rotate Body is always
    // an in-place transform, no mode choice.
    if (m_revolveWhatIdx == 1) {
        ImGui::Separator();
        ImGui::TextColored(materializr::accentText(), "Mode");
        const char* modes[] = {"New Body", "Union", "Cut", "Intersect"};
        for (int i = 0; i < 4; ++i) {
            if (i > 0) ImGui::SameLine();
            if (ImGui::RadioButton(modes[i], m_revolveModeIdx == i))
                m_revolveModeIdx = i;
        }
        if (m_revolveModeIdx != 0 && m_revolveBodyId < 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.35f, 1.0f),
                               "Boolean modes need a target body in the selection.");
        }
    }

    // Apply-enabled validation depends on the chosen What.
    const bool canApply = (m_revolveWhatIdx == 0)
                              ? !m_revolveBodyIds.empty()
                              : (m_revolveSketchId >= 0);

    ImGui::Separator();
    ImGui::BeginDisabled(!canApply);
    bool applyClicked  = ImGui::Button("Apply", ImVec2(130, 0));
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool cancelClicked = ImGui::Button("Cancel", ImVec2(130, 0));
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancelClicked = true;

    if (applyClicked && canApply) {
        // Restore original first so TransformOp::execute can capture the
        // pre-rotation state as its previousShape. Without this the real
        // op would compute "previous = already-rotated", and undo would
        // bring us back to the live-preview state instead of true origin.
        revolveLiveRestore();
        applyRevolve();
        m_revolveActive = false;
    } else if (cancelClicked) {
        revolveLiveRestore();
        m_revolveActive = false;
    }

    ImGui::End();
}

// ─── Rotate Plane About Axis popup ─────────────────────────────────────────
// Tilts / hinges an existing construction plane about a chosen line by a
// typed angle, with live preview. Hinge candidates + the snapshot are seeded
// by beginRotatePlaneAboutAxis(); this just drives the UI. Writes through
// Document::setPlane (no history op — matches the plane gizmo). Apply leaves
// the current pose; Cancel / Escape restores the snapshot.
void Application::renderRotatePlaneAboutAxisPopup() {
    if (!m_rotPlaneActive) return;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 340,
                                    ImGui::GetWindowPos().y + 50),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(uiSz(340, 0), ImGuiCond_Appearing);
    ImGui::Begin("Rotate Plane About Axis", nullptr,
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize);

    if (m_document && m_rotPlaneId >= 0) {
        ImGui::Text("Plane: %s", m_document->getPlaneName(m_rotPlaneId).c_str());
    }

    // Hinge picker — the lines computed at open time.
    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "Hinge");
    const char* curLabel =
        (m_rotPlaneHingeIdx >= 0 &&
         m_rotPlaneHingeIdx < static_cast<int>(m_rotPlaneHingeLabels.size()))
            ? m_rotPlaneHingeLabels[m_rotPlaneHingeIdx].c_str()
            : "(none)";
    if (ImGui::BeginCombo("##rotPlaneHinge", curLabel)) {
        for (int i = 0; i < static_cast<int>(m_rotPlaneHingeLabels.size()); ++i) {
            bool sel = (m_rotPlaneHingeIdx == i);
            if (ImGui::Selectable(m_rotPlaneHingeLabels[i].c_str(), sel)) {
                m_rotPlaneHingeIdx = i;
                applyRotatePlanePreview();   // re-preview about the new hinge
            }
        }
        ImGui::EndCombo();
    }

    // Angle — typed entry + slider, both live-preview on change.
    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "Angle");
    ImGui::SetNextItemWidth(100);
    bool angleChanged = false;
    if (ImGui::InputText("##rotPlaneAng", m_rotPlaneAngleBuf, sizeof(m_rotPlaneAngleBuf),
                         ImGuiInputTextFlags_CharsDecimal)) {
        m_rotPlaneAngle = static_cast<float>(std::atof(m_rotPlaneAngleBuf));
        angleChanged = true;
    }
    ImGui::SameLine(); ImGui::Text("°");
    if (ImGui::SliderFloat("##rotPlaneAngSld", &m_rotPlaneAngle, -180.0f, 180.0f, "%.1f°")) {
        std::snprintf(m_rotPlaneAngleBuf, sizeof(m_rotPlaneAngleBuf), "%.1f", m_rotPlaneAngle);
        angleChanged = true;
    }
    if (angleChanged) applyRotatePlanePreview();

    ImGui::Separator();
    bool applyClicked  = ImGui::Button("Apply", ImVec2(150, 0));
    ImGui::SameLine();
    bool cancelClicked = ImGui::Button("Cancel", ImVec2(150, 0));
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancelClicked = true;

    if (applyClicked) {
        // Make sure the final pose is in the document, then record an
        // undoable PlaneTransformOp (before = snapshot, after = current) so
        // Ctrl+Z reverts the rotation. Skip the push for a no-op (angle 0 /
        // unchanged) so we don't litter history with empty steps.
        applyRotatePlanePreview();
        const auto* pe = (m_document && m_rotPlaneId >= 0)
                             ? m_document->getPlane(m_rotPlaneId) : nullptr;
        if (m_history && pe && std::abs(m_rotPlaneAngle) > 1e-4f) {
            std::vector<PlaneTransformOp::Entry> entries{
                {m_rotPlaneId, m_rotPlaneOriginal, pe->plane}};
            m_history->pushExecuted(
                std::make_unique<PlaneTransformOp>("Rotate Plane", std::move(entries)));
        }
        markDirty();
        m_rotPlaneActive = false;
        m_rotPlaneId = -1;
        m_rotPlaneHinges.clear();
        m_rotPlaneHingeLabels.clear();
    } else if (cancelClicked) {
        cancelRotatePlaneAboutAxis();
    }

    ImGui::End();
}

// ─── Revolve live-preview helpers (Rotate Body mode) ───────────────────────

void Application::revolveLiveBegin() {
    if (m_revolveLiveActive) {
        std::fprintf(stderr, "[Revolve] revolveLiveBegin: ALREADY ACTIVE — "
                             "stale state from a previous popup? Force-restore "
                             "and rebegin to clear it.\n");
        revolveLiveRestore();
    }
    if (m_revolveWhatIdx != 0) {
        std::fprintf(stderr, "[Revolve] revolveLiveBegin: skipped — what=%d\n",
                     m_revolveWhatIdx);
        return;
    }
    if (m_revolveBodyIds.empty()) {
        std::fprintf(stderr, "[Revolve] revolveLiveBegin: skipped — no bodies\n");
        return;
    }
    m_revolveOrigBodyId = m_revolveBodyId;
    m_revolveLastAppliedAngle = m_revolveAngle;
    m_revolveLiveActive = true;
    std::fprintf(stderr, "[Revolve] revolveLiveBegin: ACTIVATED  bodies=%zu  "
                         "seed lastAppliedAngle=%.2f\n",
                 m_revolveBodyIds.size(), m_revolveLastAppliedAngle);
}

void Application::revolveLiveApply(float angle) {
    // GPU-matrix preview path: we no longer need m_revolveOrigBodyId or
    // m_revolveOrigShape — those were guards from the old single-body
    // geometric-rebuild preview that updateBody'd into the document. The
    // current path just sets a model-matrix uniform per slot, so the only
    // precondition is "we have bodies to preview and we've called Begin".
    if (!m_revolveLiveActive) return;
    if (m_revolveBodyIds.empty()) return;
    // Resolve the axis once per call — the user might have switched
    // between Construction Axis and a canonical world axis mid-preview.
    gp_Pnt axisOrigin(0, 0, 0);
    gp_Dir axisDir(0, 0, 1);
    if (m_revolveAxisId >= 0) {
        const auto* a = m_document->getAxis(m_revolveAxisId);
        if (a) { axisOrigin = a->origin; axisDir = a->direction; }
    } else {
        switch (m_revolveWorldAxisIdx) {
            case 0: axisDir = gp_Dir(1, 0, 0); break;
            case 1: axisDir = gp_Dir(0, 0, 1); break;
            case 2: axisDir = gp_Dir(0, 1, 0); break;
        }
    }

    // GPU-only preview: build a model matrix for the rotation and push it
    // to the renderer's slot for this body. No geometry rebuild, no
    // re-tessellation, no edge re-sampling — orders of magnitude cheaper
    // than the previous BRepBuilderAPI_Transform + updateBody path on
    // complex bodies (the live-edit lag the user reported). Apply() will
    // do the real geometric transform once through TransformOp.
    glm::vec3 pivot((float)axisOrigin.X(), (float)axisOrigin.Y(),
                    (float)axisOrigin.Z());
    glm::vec3 axis((float)axisDir.X(), (float)axisDir.Y(), (float)axisDir.Z());
    glm::mat4 m(1.0f);
    m = glm::translate(m, pivot);
    m = glm::rotate(m, glm::radians(angle), axis);
    m = glm::translate(m, -pivot);
    // Apply to every body in the multi-selection. Each body's mesh slot
    // gets the same model matrix so they all rotate as a rigid group
    // around the chosen axis — the natural reading of "revolve these
    // around the axis together".
    int hits = 0, misses = 0;
    for (int bid : m_revolveBodyIds) {
        bool found = false;
        if (m_shapeRenderer) {
            int slot = m_shapeRenderer->findSlotByBody(bid);
            if (slot >= 0) { m_shapeRenderer->setModelMatrix(slot, m); found = true; }
        }
        if (m_edgeRenderer) {
            int slot = m_edgeRenderer->findSlotByBody(bid);
            if (slot >= 0) m_edgeRenderer->setModelMatrix(slot, m);
        }
        if (found) ++hits; else ++misses;
    }
    std::fprintf(stderr, "[Revolve] live-apply: angle=%.2f hits=%d misses=%d  "
                         "axis dir=(%.3f,%.3f,%.3f) origin=(%.2f,%.2f,%.2f)\n",
                 angle, hits, misses,
                 axisDir.X(), axisDir.Y(), axisDir.Z(),
                 axisOrigin.X(), axisOrigin.Y(), axisOrigin.Z());
    m_revolveLastAppliedAngle = angle;
}

void Application::revolveLiveRestore() {
    if (!m_revolveLiveActive) return;
    // Reset every previewed body's model matrix to identity. Geometry
    // never changed, so this is the only step needed — slots stay,
    // meshes stay, just the transform uniform goes back to identity.
    glm::mat4 id(1.0f);
    for (int bid : m_revolveBodyIds) {
        if (m_shapeRenderer) {
            int slot = m_shapeRenderer->findSlotByBody(bid);
            if (slot >= 0) m_shapeRenderer->setModelMatrix(slot, id);
        }
        if (m_edgeRenderer) {
            int slot = m_edgeRenderer->findSlotByBody(bid);
            if (slot >= 0) m_edgeRenderer->setModelMatrix(slot, id);
        }
    }
    m_revolveOrigShape.Nullify();
    m_revolveOrigBodyId = -1;
    m_revolveLastAppliedAngle = 0.0f;
    m_revolveLiveActive = false;
}

void Application::applyRevolve() {
    if (!m_history || !m_document) return;

    // Resolve the axis once — both flows use the same picker.
    gp_Pnt axisOrigin(0, 0, 0);
    gp_Dir axisDir(0, 0, 1);
    if (m_revolveAxisId >= 0) {
        const auto* a = m_document->getAxis(m_revolveAxisId);
        if (a) {
            axisOrigin = a->origin;
            axisDir = a->direction;
        }
    } else {
        switch (m_revolveWorldAxisIdx) {
            case 0: axisDir = gp_Dir(1, 0, 0); break;
            case 1: axisDir = gp_Dir(0, 0, 1); break; // user Y = world Z
            case 2: axisDir = gp_Dir(0, 1, 0); break; // user Z = world Y up
        }
    }

    if (m_revolveWhatIdx == 0) {
        if (m_revolveBodyIds.empty()) {
            std::fprintf(stderr, "[Revolve] apply skipped: no bodies captured\n");
            return;
        }
        // Bundle all bodies' rotations into one ReplayOp so the user undoes
        // the entire revolve in a single Ctrl+Z. Snapshot each body before
        // applying the transform, run the transform directly via OCCT
        // (skipping per-body TransformOp ops), snapshot the new shape, then
        // wrap before/after into a single ReplayOp the history pushes
        // executed. The single op is enough — multi-body undo replays all
        // bodies' before-states at once.
        gp_Trsf trsf;
        trsf.SetRotation(gp_Ax1(axisOrigin, axisDir),
                         static_cast<double>(m_revolveAngle) * M_PI / 180.0);

        ReplayOp::BodyState before;
        ReplayOp::BodyState after;
        int rotated = 0;
        for (int bid : m_revolveBodyIds) {
            TopoDS_Shape src;
            try { src = m_document->getBody(bid); } catch (...) { continue; }
            if (src.IsNull()) continue;
            before.push_back({bid, src});
            try {
                BRepBuilderAPI_Transform xf(src, trsf, /*copy=*/true);
                xf.Build();
                if (!xf.IsDone() || xf.Shape().IsNull()) {
                    // Roll the before-entry off — couldn't transform this
                    // body, don't pretend we did.
                    before.pop_back();
                    std::fprintf(stderr, "[Revolve]   body %d: transform FAILED\n",
                                 bid);
                    continue;
                }
                m_document->updateBody(bid, xf.Shape());
                after.push_back({bid, xf.Shape()});
                ++rotated;
            } catch (...) {
                before.pop_back();
                std::fprintf(stderr, "[Revolve]   body %d: THREW\n", bid);
            }
        }

        if (rotated > 0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "Revolve %d bodies by %.1f\xC2\xB0", rotated,
                          m_revolveAngle);
            auto op = std::make_unique<ReplayOp>("revolve_rotate",
                                                  "Revolve",
                                                  buf,
                                                  std::move(before),
                                                  std::move(after),
                                                  /*fromReload=*/false);
            // pushExecuted: state's already been applied directly to the
            // document above; we just want history to know it happened so
            // Ctrl+Z can roll it back via the captured before-state.
            m_history->pushExecuted(std::move(op));
            m_meshesDirty = true;
            std::fprintf(stderr, "[Revolve] applied: %.1f° dir(%.3f,%.3f,%.3f) "
                                 "origin(%.2f,%.2f,%.2f) over %d bodies "
                                 "(single ReplayOp)\n",
                         m_revolveAngle, axisDir.X(), axisDir.Y(), axisDir.Z(),
                         axisOrigin.X(), axisOrigin.Y(), axisOrigin.Z(),
                         rotated);
        }
        return;
    }

    // Sweep Sketch path — original RevolveOp flow.
    if (m_revolveSketchId < 0) return;
    auto sk = m_document->getSketch(m_revolveSketchId);
    if (!sk) return;
    auto regions = sk->buildRegions();
    // Pick the outermost region (largest outer bbox) — its face carries any
    // inner boundaries as holes. Matches RevolveOp::rebuildProfileFromSketch
    // so reloads re-derive the same profile.
    int bestIdx = -1;
    double bestDiag = -1.0;
    for (size_t i = 0; i < regions.size(); ++i) {
        if (regions[i].face.IsNull() || regions[i].outerWire.IsNull()) continue;
        Bnd_Box bb;
        BRepBndLib::Add(regions[i].outerWire, bb);
        if (bb.IsVoid()) continue;
        double x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        double dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
        double diag = dx * dx + dy * dy + dz * dz;
        if (diag > bestDiag) { bestDiag = diag; bestIdx = static_cast<int>(i); }
    }
    if (bestIdx < 0) {
        std::fprintf(stderr, "[Revolve] sketch has no closed region to revolve\n");
        return;
    }

    auto op = std::make_unique<RevolveOp>();
    op->setProfile(regions[bestIdx].face);
    op->setSketchSource(m_revolveSketchId); // for reload profile re-derivation
    op->setAxis(gp_Ax1(axisOrigin, axisDir));
    op->setAngle(static_cast<double>(m_revolveAngle));
    RevolveMode mode = RevolveMode::NewBody;
    switch (m_revolveModeIdx) {
        case 0: mode = RevolveMode::NewBody;   break;
        case 1: mode = RevolveMode::Union;     break;
        case 2: mode = RevolveMode::Subtract;  break;
        case 3: mode = RevolveMode::Intersect; break;
    }
    op->setMode(mode);
    if (mode != RevolveMode::NewBody) op->setTargetBody(m_revolveBodyId);

    if (m_history->pushOperation(std::move(op), *m_document)) {
        m_meshesDirty = true;
        std::fprintf(stdout, "[Revolve] sweep-sketch applied: angle=%.1f° mode=%d\n",
                     m_revolveAngle, m_revolveModeIdx);
    } else {
        std::fprintf(stderr, "[Revolve] op execute failed\n");
    }
}

void Application::renderConstructionAxisPanel() {
    if (!m_axisOpActive) return;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 280,
                                    ImGui::GetWindowPos().y + 50),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(uiSz(280, 0), ImGuiCond_Appearing);
    ImGui::Begin("Construction Axis", nullptr,
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::TextColored(materializr::accentText(), "Direction");
    bool kindChanged = false;
    auto kindRadio = [&](const char* label, int idx) {
        if (idx > 0) ImGui::SameLine();
        if (ImGui::RadioButton(label, m_axisOpKindIdx == idx)) {
            m_axisOpKindIdx = idx;
            kindChanged = true;
        }
    };
    // User-Z-up labels: "X" world-X, "Y" world-Z (depth in user terms),
    // "Z" world-Y (up). Matches the plane popup's user-axis convention so
    // an "X axis" through (0,0,0) reads as the red world arrow.
    kindRadio("X", 0);
    kindRadio("Y", 1);
    kindRadio("Z", 2);
    ImGui::TextDisabled("Labels are user-Z-up: Z is the floor-up axis.");

    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "Origin (mm)");
    bool originChanged = false;
    const char* axisLetters[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        ImGui::Text("%s", axisLetters[i]); ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputText("##axisOrig", m_axisOpOriginBuf[i],
                             sizeof(m_axisOpOriginBuf[i]),
                             ImGuiInputTextFlags_CharsDecimal)) {
            double parsed = std::atof(m_axisOpOriginBuf[i]);
            if (std::abs(parsed - m_axisOpOrigin[i]) > 1e-4) {
                m_axisOpOrigin[i] = parsed;
                originChanged = true;
            }
        }
        ImGui::PopID();
        if (i < 2) ImGui::SameLine();
    }
    ImGui::TextDisabled("Point the axis passes through. Drag the gizmo "
                        "later (after Apply) to fine-tune.");

    ImGui::Separator();
    bool applyClicked  = ImGui::Button("Apply", ImVec2(120, 0));
    ImGui::SameLine();
    bool cancelClicked = ImGui::Button("Cancel", ImVec2(120, 0));
    bool escPressed    = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    if (kindChanged || originChanged) updateConstructionAxis();
    if (applyClicked) {
        commitConstructionAxis();
    } else if (cancelClicked || escPressed) {
        cancelConstructionAxis();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Section View — render-only clip plane for inspecting interiors (thread
// profiles, wall thickness) without destructive booleans.

gp_Pln Application::sectionBasePlane() const {
    gp_Pln pl;
    bool fromDatum = false;
    if (m_sectionPlaneId >= 0) {
        if (const PlaneEntry* pe = m_document->getPlane(m_sectionPlaneId)) {
            pl = pe->plane;
            fromDatum = true;
        }
    }
    if (!fromDatum) {
        switch (m_sectionWorldPlane) {
            case 0:  pl = gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)); break; // XY
            case 1:  pl = gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0)); break; // XZ
            default: pl = gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)); break; // YZ
        }
    }
    if (m_sectionFlip) {
        const gp_Ax3& ax = pl.Position();
        pl = gp_Pln(gp_Ax3(ax.Location(), ax.Direction().Reversed()));
    }
    return pl;
}

void Application::renderSectionPanel() {
    if (!m_sectionEnabled) return;

    // Top-centre, like the other tool popups. The first version pinned this
    // top-RIGHT — squarely behind the Items/Properties panels, so the plane
    // picker existed but was never seen.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 60.0f),
        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    bool open = true;
    if (ImGui::Begin("Section View", &open,
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings)) {
        static const char* kWorldNames[3] = {
            "World XY (front)", "World XZ (ground)", "World YZ (side)"};

        // The selected construction plane may have been deleted — fall back
        // to a world plane rather than sectioning by a stale gp_Pln.
        std::string current;
        if (m_sectionPlaneId >= 0) {
            const PlaneEntry* pe = m_document->getPlane(m_sectionPlaneId);
            if (pe) current = pe->name;
            else { m_sectionPlaneId = -1; m_sectionDirty = true; }
        }
        if (m_sectionPlaneId < 0) current = kWorldNames[m_sectionWorldPlane];

        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::BeginCombo("Plane", current.c_str())) {
            for (int w = 0; w < 3; ++w) {
                bool sel = m_sectionPlaneId < 0 && m_sectionWorldPlane == w;
                if (ImGui::Selectable(kWorldNames[w], sel)) {
                    m_sectionPlaneId = -1;
                    m_sectionWorldPlane = w;
                    m_sectionDirty = true;
                }
            }
            auto planeIds = m_document->getAllPlaneIds();
            if (!planeIds.empty()) ImGui::Separator();
            for (int id : planeIds) {
                const PlaneEntry* pe = m_document->getPlane(id);
                if (!pe) continue;
                ImGui::PushID(id);
                if (ImGui::Selectable(pe->name.c_str(),
                                      m_sectionPlaneId == id)) {
                    m_sectionPlaneId = id;
                    m_sectionDirty = true;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }

        // Offset range adapts to the model so large parts can be fully
        // traversed — the old fixed ±100 mm couldn't reach the far side of a
        // bigger body. Use the largest bounding-box dimension of the visible
        // bodies (floored at 100 mm so small parts keep a usable range), cached
        // and refreshed every ~0.5 s so the bbox walk isn't run every frame.
        static double s_secNextCheck = 0.0;
        static float  s_secRange     = 100.0f;
        double secNow = ImGui::GetTime();
        if (secNow >= s_secNextCheck) {
            try {
                Bnd_Box bb;
                bool any = false;
                for (int id : m_document->getAllBodyIds()) {
                    if (!m_document->isBodyVisible(id)) continue;
                    BRepBndLib::Add(m_document->getBody(id), bb);
                    any = true;
                }
                if (any && !bb.IsVoid()) {
                    double xmn, ymn, zmn, xmx, ymx, zmx;
                    bb.Get(xmn, ymn, zmn, xmx, ymx, zmx);
                    double ext = std::max({xmx - xmn, ymx - ymn, zmx - zmn});
                    s_secRange = std::max(100.0f, static_cast<float>(ext));
                }
            } catch (...) {}
            s_secNextCheck = secNow + 0.5;
        }
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::SliderFloat("Offset (mm)", &m_sectionOffset,
                               -s_secRange, s_secRange, "%.1f"))
            m_sectionDirty = true; // Ctrl+click still types exact values past the range
        if (ImGui::Checkbox("Flip side", &m_sectionFlip))
            m_sectionDirty = true;

        ImGui::TextDisabled("View-only: bodies are not modified.");
        ImGui::Separator();
        if (ImGui::Button("Exit Section View", ImVec2(200, 0)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            open = false;
        }
    }
    ImGui::End();
    if (!open) m_sectionEnabled = false;
}

void Application::renderTextToolPanel() {
    if (!m_inSketchMode || !m_sketchTool ||
        m_sketchTool->getMode() != SketchToolMode::Text)
        return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 60.0f),
        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    bool open = true;
    if (ImGui::Begin("Text", &open,
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings)) {
        // The string buffer lives here; the tool consumes the committed
        // std::string. Re-seeded each time the window (re)appears.
        static char buf[128];
        if (ImGui::IsWindowAppearing()) {
            std::snprintf(buf, sizeof(buf), "%s",
                          m_sketchTool->getTextString().c_str());
        }
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputText("##textString", buf, sizeof(buf)))
            m_sketchTool->setTextString(buf);

        // Bundled fonts only — deterministic across machines, unlike a
        // system-font scan.
        static const char* kFontNames[] = {"Mono (JetBrains)",
                                           "Sans (DejaVu)",
                                           "Serif (DejaVu)",
                                           "Times (Liberation)",
                                           "Arial (Liberation)",
                                           "Ubuntu",
                                           "Comic (Neue)",
                                           "Stencil (Black Ops)",
                                           "Impact (Anton)",
                                           "Script (Pacifico)"};
        static const char* kFontFiles[] = {"JetBrainsMono-Regular.ttf",
                                           "DejaVuSans.ttf",
                                           "DejaVuSerif.ttf",
                                           "LiberationSerif-Regular.ttf",
                                           "LiberationSans-Regular.ttf",
                                           "Ubuntu-Regular.ttf",
                                           "ComicNeue-Regular.ttf",
                                           "BlackOpsOne-Regular.ttf",
                                           "Anton-Regular.ttf",
                                           "Pacifico-Regular.ttf"};
        static_assert(IM_ARRAYSIZE(kFontNames) == IM_ARRAYSIZE(kFontFiles),
                      "font name/file lists must stay in lockstep");
        static int fontIdx = 0;
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("Font", &fontIdx, kFontNames, IM_ARRAYSIZE(kFontNames))) {
            std::string p = resolveBundledFont(kFontFiles[fontIdx]);
            if (p.empty()) {
                std::fprintf(stderr, "[Text] bundled font missing: %s\n",
                             kFontFiles[fontIdx]);
            }
            m_sketchTool->setTextFontPath(p);
        }

        float h = m_sketchTool->getTextHeight();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderFloat("Height (mm)", &h, 1.0f, 50.0f, "%.1f",
                               ImGuiSliderFlags_Logarithmic)) {
            // Snap the height to the sketch grid increment when snap-to-grid is
            // on, so text sizes land on the same lattice as everything else.
            if (m_snapToGrid && m_sketchGridStep > 0.0f)
                h = std::round(h / m_sketchGridStep) * m_sketchGridStep;
            if (h < 1.0f) h = 1.0f; // keep within the slider's lower bound
            m_sketchTool->setTextHeight(h);
        }

        // 90° steps about the click anchor. The default is seeded from the
        // camera so text usually starts upright; these fix the rest.
        int ang = m_sketchTool->getTextAngle();
        if (ImGui::Button("Rotate left"))
            m_sketchTool->setTextAngle(ang + 90);
        ImGui::SameLine();
        if (ImGui::Button("Rotate right"))
            m_sketchTool->setTextAngle(ang - 90);
        ImGui::SameLine();
        ImGui::TextDisabled("%d deg", m_sketchTool->getTextAngle());

        // Keep the placement-preview extents current. Re-measured only when
        // string / font / height actually change (font load isn't free).
        {
            static std::string lastKey;
            std::string key = m_sketchTool->getTextString() + "|" +
                              m_sketchTool->getTextFontPath() + "|" +
                              std::to_string(m_sketchTool->getTextHeight());
            if (key != lastKey) {
                glm::vec2 mn, mx;
                if (TextSketch::measure(m_sketchTool->getTextString(),
                                        m_sketchTool->getTextFontPath(),
                                        m_sketchTool->getTextHeight(),
                                        mn, mx)) {
                    m_sketchTool->setTextPreviewBox(mn, mx);
                    // Also capture the actual glyph contours for a live preview.
                    std::vector<std::vector<glm::vec2>> loops;
                    TextSketch::outline(m_sketchTool->getTextString(),
                                        m_sketchTool->getTextFontPath(),
                                        m_sketchTool->getTextHeight(), loops);
                    m_sketchTool->setTextPreviewLoops(std::move(loops));
                } else {
                    m_sketchTool->clearTextPreviewBox();
                }
                lastKey = key;
            }
        }

        if (materializr::touchMode()) {
            // Touch has no hover: drag in the sketch to slide the preview anchor
            // (the Move toggle frees the camera; two-finger still pans/zooms),
            // then commit with Place. Remove-last walks back through stamps.
            ImGui::TextDisabled("Drag in the sketch to position.");
            if (ImGui::Button("Place Here"))
                recordSketchMutation([&]{ m_sketchTool->commitStamp(); });
            if (m_sketchTool->hasLastStamp()) {
                ImGui::SameLine();
                if (ImGui::Button("Remove Last Placement"))
                    recordSketchMutation([&]{ m_sketchTool->undoLastStamp(); });
            }
        } else {
            ImGui::TextDisabled("Click in the sketch to place.");
            if (m_sketchTool->hasLastStamp())
                ImGui::TextDisabled("Backspace removes the last placement.");
        }
        if (m_sketchTool->getTextFontPath().empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                               "Font file not found - cannot place text.");
        }

    }
    ImGui::End();
    if (!open) m_sketchTool->setMode(SketchToolMode::Select);
}

void Application::renderSvgToolPanel() {
    if (!m_inSketchMode || !m_sketchTool ||
        m_sketchTool->getMode() != SketchToolMode::Svg)
        return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 60.0f),
        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    bool open = true;
    if (ImGui::Begin("Import SVG", &open,
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings)) {
        const auto& svg = m_sketchTool->getSvgPaths();
        ImGui::TextDisabled("%d path(s) ready.",
                            static_cast<int>(svg.loops.size()));

        float w = m_sketchTool->getSvgWidth();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderFloat("Width (mm)", &w, 1.0f, 300.0f, "%.1f",
                               ImGuiSliderFlags_Logarithmic))
            m_sketchTool->setSvgWidth(w);

        int ang = m_sketchTool->getTextAngle();
        if (ImGui::Button("Rotate left"))
            m_sketchTool->setTextAngle(ang + 90);
        ImGui::SameLine();
        if (ImGui::Button("Rotate right"))
            m_sketchTool->setTextAngle(ang - 90);
        ImGui::SameLine();
        ImGui::TextDisabled("%d deg", m_sketchTool->getTextAngle());

        // Preview box: artwork extents centred on the cursor anchor.
        {
            glm::vec2 size = svg.size();
            float rawW = (size.x > 1e-6f) ? size.x : size.y;
            if (rawW > 1e-6f) {
                float scale = m_sketchTool->getSvgWidth() / rawW;
                glm::vec2 half = 0.5f * size * scale;
                m_sketchTool->setTextPreviewBox(-half, half);
            }
        }

        if (materializr::touchMode()) {
            // Touch has no hover: drag in the sketch to slide the preview anchor
            // (the Move toggle frees the camera; two-finger still pans/zooms),
            // then commit with Place. Remove-last walks back through stamps.
            ImGui::TextDisabled("Drag in the sketch to position.");
            if (ImGui::Button("Place Here"))
                recordSketchMutation([&]{ m_sketchTool->commitStamp(); });
            if (m_sketchTool->hasLastStamp()) {
                ImGui::SameLine();
                if (ImGui::Button("Remove Last Placement"))
                    recordSketchMutation([&]{ m_sketchTool->undoLastStamp(); });
            }
        } else {
            ImGui::TextDisabled("Click in the sketch to place.");
            if (m_sketchTool->hasLastStamp())
                ImGui::TextDisabled("Backspace removes the last placement.");
        }
    }
    ImGui::End();
    if (!open) m_sketchTool->setMode(SketchToolMode::Select);
}

// ─── Primitive popup ─────────────────────────────────────────────────────────
void Application::renderPrimitivePopup() {
    if (!m_primitivePopupActive) return;

    const char* titles[] = {
        "New Box", "New Cylinder", "New Sphere", "New Cone", "New Torus"};
    const int   k = m_primitivePopupKind;
    if (k < 0 || k > 4) { m_primitivePopupActive = false; return; }

    // Same pinned top-right placement the IOp panels use, so the popup never
    // hides the viewport's body / selection.
    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 260,
               ImGui::GetWindowPos().y + 50),
        ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(uiSz(260, 0), ImGuiCond_Appearing);
    ImGui::Begin(titles[k], nullptr,
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::TextColored(materializr::accentText(), "Dimensions");
    switch (k) {
    case 0: // Box
        ImGui::InputDouble("Width (X)",  &m_primitivePopupExtents[0],
                           0.1, 1.0, "%.3f");
        ImGui::InputDouble("Depth (Y)",  &m_primitivePopupExtents[1],
                           0.1, 1.0, "%.3f");
        ImGui::InputDouble("Height (Z)", &m_primitivePopupExtents[2],
                           0.1, 1.0, "%.3f");
        break;
    case 1: // Cylinder
        ImGui::InputDouble("Radius", &m_primitivePopupRadius, 0.1, 1.0, "%.3f");
        ImGui::InputDouble("Height", &m_primitivePopupHeight, 0.1, 1.0, "%.3f");
        break;
    case 2: // Sphere
        ImGui::InputDouble("Radius", &m_primitivePopupRadius, 0.1, 1.0, "%.3f");
        break;
    case 3: // Cone
        ImGui::InputDouble("Bottom radius", &m_primitivePopupRadius,
                           0.1, 1.0, "%.3f");
        ImGui::InputDouble("Top radius",    &m_primitivePopupTopRadius,
                           0.1, 1.0, "%.3f");
        ImGui::InputDouble("Height",        &m_primitivePopupHeight,
                           0.1, 1.0, "%.3f");
        break;
    case 4: // Torus
        ImGui::InputDouble("Major radius",  &m_primitivePopupRadius,
                           0.1, 1.0, "%.3f");
        ImGui::InputDouble("Minor radius",  &m_primitivePopupMinorRadius,
                           0.1, 1.0, "%.3f");
        ImGui::TextDisabled("Major must exceed minor — equal radii are a "
                            "degenerate self-touching torus.");
        break;
    }

    ImGui::Spacing();
    ImGui::TextColored(materializr::accentText(), "Origin (mm)");
    ImGui::InputDouble("X", &m_primitivePopupOrigin[0], 0.1, 1.0, "%.3f");
    ImGui::InputDouble("Y", &m_primitivePopupOrigin[1], 0.1, 1.0, "%.3f");
    ImGui::InputDouble("Z", &m_primitivePopupOrigin[2], 0.1, 1.0, "%.3f");
    ImGui::TextDisabled("Box origin = corner; the rest use it as the axis "
                        "base / centre.");

    // Geometric validity check — must mirror the bounds in PrimitiveOp::
    // execute(). If the user typed a degenerate combination (zero/negative
    // extent, torus minor ≥ major, etc.) we grey out Create and explain
    // WHY so they aren't left clicking a dead button. (Steve: a major <
    // minor torus crashed the app instead of being rejected up-front.)
    const char* invalidReason = nullptr;
    bool ok = true;
    switch (k) {
    case 0:
        if (m_primitivePopupExtents[0] <= 0.0 ||
            m_primitivePopupExtents[1] <= 0.0 ||
            m_primitivePopupExtents[2] <= 0.0) {
            ok = false;
            invalidReason = "All three extents must be > 0.";
        }
        break;
    case 1:
        if (m_primitivePopupRadius <= 0.0 || m_primitivePopupHeight <= 0.0) {
            ok = false;
            invalidReason = "Radius and height must both be > 0.";
        }
        break;
    case 2:
        if (m_primitivePopupRadius <= 0.0) {
            ok = false; invalidReason = "Radius must be > 0.";
        }
        break;
    case 3:
        if (m_primitivePopupRadius < 0.0 || m_primitivePopupTopRadius < 0.0 ||
            m_primitivePopupHeight <= 0.0 ||
            (m_primitivePopupRadius <= 0.0 &&
             m_primitivePopupTopRadius <= 0.0)) {
            ok = false;
            invalidReason = "Height must be > 0 and at least one radius "
                            "must be positive (the other may be 0 for a tip).";
        }
        break;
    case 4:
        if (m_primitivePopupRadius <= 0.0 || m_primitivePopupMinorRadius <= 0.0) {
            ok = false;
            invalidReason = "Major and minor radii must both be > 0.";
        } else if (m_primitivePopupRadius <= m_primitivePopupMinorRadius) {
            ok = false;
            invalidReason = "Major radius must exceed minor "
                            "(equal = horn torus with zero-diameter hole; "
                            "smaller = self-intersecting spindle torus).";
        }
        break;
    }
    if (!ok && invalidReason) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                           "%s", invalidReason);
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    if (!ok) ImGui::BeginDisabled();
    if (ImGui::Button(materializr::btnCreate(), ImVec2(110, 0)) ||
        (ok && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
        commitPrimitivePopup();
    }
    if (!ok) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(materializr::btnCancel(), ImVec2(110, 0)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        cancelPrimitivePopup();
    }
    ImGui::End();
}

} // namespace materializr

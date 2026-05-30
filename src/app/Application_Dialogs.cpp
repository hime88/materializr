#include "gl_common.h"

#include <cstdlib>
#include <filesystem>
#include <map>

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
#include "viewport/PlaneRenderer.h"
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
#include "modeling/ExtrudeOp.h"
#include "modeling/ReplayOp.h"
#include "modeling/PushPullOp.h"
#include "modeling/TransformOp.h"
#include "modeling/MirrorOp.h"
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
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>
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
    ImGui::SetNextWindowSize(ImVec2(420, 420), ImGuiCond_Appearing);
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
                    ImGui::SeparatorText("Appearance");
                    // Theme selector (the View menu mirrors this).
                    if (m_themeManager->renderSelector()) {
                        m_themeManager->apply();
                        changed = true;
                    }

                    ImGui::Spacing();
                    ImGui::SeparatorText("Interface");
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

                // ── Camera ────────────────────────────────────────────────
                if (ImGui::BeginTabItem("Camera")) {
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
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Mirror across");
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
    ImGui::SetNextWindowSize(ImVec2(400, 220), ImGuiCond_Appearing);

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
                // openInBrowser lives in AboutDialog.cpp — duplicate the tiny
                // platform helper inline so this file doesn't add a dependency.
#ifdef _WIN32
                ShellExecuteA(nullptr, "open", m_updateReleaseUrl.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
#else
                std::string cmd = std::string("xdg-open ") + "\"" +
                                  m_updateReleaseUrl + "\" >/dev/null 2>&1 &";
                [[maybe_unused]] int rc = std::system(cmd.c_str());
#endif
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

    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
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
    ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_Appearing);
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
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", heading);
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

    ImGui::Spacing();
    if (ImGui::Button("Confirm (Enter)", ImVec2(115, 0))) commitResizeCylindrical();
    ImGui::SameLine();
    if (ImGui::Button("Cancel (Esc)",    ImVec2(115, 0))) cancelResizeCylindrical();

    ImGui::End();
}

void Application::renderShellPanel() {
    if (!m_shellActive) return;

    // Same pinned top-right anchor + flag set as the push/pull and resize
    // popups — known stable, no hover flicker.
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 260,
                                    ImGui::GetWindowPos().y + 50));
    ImGui::SetNextWindowSize(ImVec2(240, 0));
    ImGui::Begin("##ShellPanel", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "Shell");
    ImGui::TextDisabled("Hollows the body and removes the picked face.");
    ImGui::Separator();

    if (m_shellInputFocus) {
        ImGui::SetKeyboardFocusHere();
        m_shellInputFocus = false;
    }

    ImGui::SetNextItemWidth(140);
    if (ImGui::InputText("##shellThickness", m_shellInputBuf,
                         sizeof(m_shellInputBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue |
                         ImGuiInputTextFlags_CharsDecimal)) {
        m_shellThickness = static_cast<float>(std::atof(m_shellInputBuf));
        commitInteractiveShell();
        ImGui::End();
        return;
    } else {
        float parsed = static_cast<float>(std::atof(m_shellInputBuf));
        if (std::abs(parsed - m_shellThickness) > 0.001f) {
            m_shellThickness = parsed;
            updateInteractiveShell();
        }
    }
    ImGui::SameLine();
    ImGui::Text("mm");

    // Slider as a quick scrub.
    if (ImGui::SliderFloat("##shellSlider", &m_shellThickness, 0.1f, 20.0f, "%.2f mm")) {
        std::snprintf(m_shellInputBuf, sizeof(m_shellInputBuf), "%.2f", m_shellThickness);
        updateInteractiveShell();
    }

    ImGui::Spacing();
    if (ImGui::Button("Confirm (Enter)", ImVec2(110, 0))) commitInteractiveShell();
    ImGui::SameLine();
    if (ImGui::Button("Cancel (Esc)",    ImVec2(110, 0))) cancelInteractiveShell();

    ImGui::End();
}

void Application::renderScalePanel() {
    // Shown only while the Scale gizmo is active with a body selected.
    if (m_inSketchMode || !m_selection->hasSelectedBodies()) return;
    if (m_gizmo->getMode() != GizmoMode::Scale) return;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 230,
                                    ImGui::GetWindowPos().y + 50));
    ImGui::SetNextWindowSize(ImVec2(210, 0));
    ImGui::Begin("##ScalePanel", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "Scale (%% of current)");
    ImGui::Separator();
    ImGui::Checkbox("Uniform", &m_scaleUniform);

    // Always show X/Y/Z. Typed decimals are allowed; with Uniform on, editing one
    // field mirrors it to the others. No +/- step buttons so the row stays narrow.
    auto box = [&](const char* label, int i) {
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputFloat(label, &m_scalePct[i], 0.0f, 0.0f, "%.1f")) {
            if (m_scaleUniform) { m_scalePct[0] = m_scalePct[1] = m_scalePct[2] = m_scalePct[i]; }
        }
    };
    box("X %", 0);
    box("Y %", 1);
    box("Z %", 2);

    ImGui::Spacing();
    if (ImGui::Button("Apply", ImVec2(90, 0))) {
        if (m_selection->hasSelectedBodies()) {
            int bodyId = m_selection->getSelection()[0].bodyId;
            float sx = m_scalePct[0] / 100.0f;
            float sy = (m_scaleUniform ? m_scalePct[0] : m_scalePct[1]) / 100.0f;
            float sz = (m_scaleUniform ? m_scalePct[0] : m_scalePct[2]) / 100.0f;
            if (sx > 0.001f && sy > 0.001f && sz > 0.001f &&
                (std::abs(sx - 1) > 1e-4f || std::abs(sy - 1) > 1e-4f || std::abs(sz - 1) > 1e-4f)) {
                try {
                    const TopoDS_Shape& shape = m_document->getBody(bodyId);
                    Bnd_Box bb; BRepBndLib::Add(shape, bb);
                    double x1, y1, z1, x2, y2, z2; bb.Get(x1, y1, z1, x2, y2, z2);
                    auto op = std::make_unique<TransformOp>();
                    op->setBodyId(bodyId);
                    op->setType(TransformType::Scale);
                    op->setCenter((x1 + x2) * 0.5, (y1 + y2) * 0.5, (z1 + z2) * 0.5);
                    op->setScaleXYZ(sx, sy, sz);
                    if (m_history->pushOperation(std::move(op), *m_document)) m_meshesDirty = true;
                } catch (...) {}
            }
            m_scalePct[0] = m_scalePct[1] = m_scalePct[2] = 100.0f;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(90, 0))) {
        m_scalePct[0] = m_scalePct[1] = m_scalePct[2] = 100.0f;
    }
    ImGui::End();
}

void Application::renderInteractionsPanel() {
    // A quick-reference of the viewport interactions, docked above Items. The
    // camera rows reflect the live mouse bindings chosen in File > Settings.
    ImGui::Begin("Interactions");
    // Action label in a fixed left column, keys to its right. Keeping the action
    // first (it's short) means a long key string extends rightward instead of
    // overlapping the label.
    auto row = [](const char* action, const char* keys) {
        ImGui::TextUnformatted(action);
        ImGui::SameLine(120.0f);
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", keys);
    };
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
    row("Add to selection", "Ctrl+Click");
    row("Delete selected", "Del");
    ImGui::SeparatorText("Transform (body)");
    row("Move / Rotate / Scale", "W / E / R");
    ImGui::SeparatorText("Sketch");
    row("Start", "Pick a base plane / face");
    row("Finish / Cancel", "Enter / Esc");
    row("Dimension", "Type value + Enter");
    ImGui::SeparatorText("General");
    row("Undo / Redo", "Ctrl+Z / Ctrl+Y");
    ImGui::End();
}

void Application::renderSketchPatternPopup() {
    if (!m_sketchPatternActive) return;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 280,
                                    ImGui::GetWindowPos().y + 50),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_Appearing);
    const char* title = (m_sketchPatternKind == PatternKind::Linear)
                            ? "Sketch Linear Pattern"
                            : "Sketch Radial Pattern";
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
    ImGui::Text("Copies"); ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputText("##spcount", m_sketchPatternCountBuf,
                     sizeof(m_sketchPatternCountBuf),
                     ImGuiInputTextFlags_CharsDecimal);
    m_sketchPatternCount = std::max(2, std::atoi(m_sketchPatternCountBuf));

    if (m_sketchPatternKind == PatternKind::Linear) {
        ImGui::Text("Spacing"); ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::InputText("##spdist", m_sketchPatternDistanceBuf,
                         sizeof(m_sketchPatternDistanceBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(); ImGui::Text("mm");
        m_sketchPatternDistance = static_cast<float>(std::atof(m_sketchPatternDistanceBuf));
    } else {
        ImGui::Text("Sweep"); ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::InputText("##spangle", m_sketchPatternAngleBuf,
                         sizeof(m_sketchPatternAngleBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(); ImGui::Text("°");
        m_sketchPatternAngle = static_cast<float>(std::atof(m_sketchPatternAngleBuf));

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "Origin (sketch X, Y)");
        ImGui::SetNextItemWidth(110);
        ImGui::InputText("X##spox", m_sketchPatternOXBuf, sizeof(m_sketchPatternOXBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::InputText("Y##spoy", m_sketchPatternOYBuf, sizeof(m_sketchPatternOYBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        m_sketchPatternOriginX = static_cast<float>(std::atof(m_sketchPatternOXBuf));
        m_sketchPatternOriginY = static_cast<float>(std::atof(m_sketchPatternOYBuf));
    }

    ImGui::Separator();
    bool apply  = ImGui::Button("Apply",  ImVec2(120, 0));
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel", ImVec2(120, 0));
    bool esc    = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    if (apply) {
        applySketchPattern(); // also sets m_sketchPatternActive = false
    } else if (cancel || esc) {
        m_sketchPatternActive = false;
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
    ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_Appearing);
    const char* title = (m_patternKind == PatternKind::Linear)
                            ? "Linear Pattern"
                            : "Radial Pattern";
    ImGui::Begin(title, nullptr,
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize);

    // ---- Axis radio buttons (X / Y / Z) ----
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s",
                       m_patternKind == PatternKind::Linear ? "Direction" : "Rotation axis");
    const char* labels[] = { "X", "Y", "Z" };
    bool axisChanged = false;
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::RadioButton(labels[i], m_patternAxisIdx == i)) {
            m_patternAxisIdx = i;
            axisChanged = true;
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
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "Axis origin");
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

} // namespace materializr

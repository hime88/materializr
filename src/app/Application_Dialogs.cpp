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
#include "ui/CommandPalette.h"
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
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Appearing);
    if (ImGui::Begin("Settings", &m_showSettings)) {
        bool changed = false; // any change persists the settings file

        ImGui::SeparatorText("Appearance");
        // Theme selector (the existing one in the View menu mirrors this).
        if (m_themeManager->renderSelector()) {
            m_themeManager->apply();
            changed = true;
        }

        ImGui::SeparatorText("Mouse — Camera");
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
        ImGui::Separator();
        if (ImGui::Button("Apply", ImVec2(90, 0))) {
            m_orbitButton = m_settingsOrbitButton;
            m_panButton = m_settingsPanButton;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(90, 0))) {
            m_showSettings = false;
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
    row("Command palette", "Ctrl+K");
    ImGui::End();
}

} // namespace materializr

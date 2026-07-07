// Layout-shared chrome: everything every interface layout (classic / modern /
// im-touch) renders from the SAME code so the layouts can't drift apart in
// fundamentals — the menu item lists (incl. plugin menu contributions), the
// dockspace host, the overflow popup, and the shared undo helpers. See
// LayoutCommon.h for the keep-in-lockstep contract.

#include "app/Application.h"
#include "app/Window.h"
#include "ui/MeasureTool.h"
#include "app/layout/LayoutCommon.h"
#include "core/Document.h"
#include "core/History.h"
#include "core/SelectionManager.h"
#include "modeling/Sketch.h"
#include "modeling/SketchTool.h"
#include "modeling/SketchTransformOp.h"
#include "plugin/PluginContext.h"
#include "plugin/PluginRegistry.h"
#include "ui/AboutDialog.h"
#include "io/Timelapse.h"   // File > Export > Timelapse GIF
#include "ui/HelpPanel.h"
#include "ui/LogoTexture.h"
#include "ui/ShortcutsPanel.h"
#include "ui/ThemeManager.h"
#include "ui/Toolbar.h"
#include "ui/TouchIcons.h"
#include "viewport/Viewport.h"
#include "gl_common.h"
#include "touch_mode.h"

#include <imgui.h>
#include <imgui_internal.h> // dock-node tab-bar policy (per-node LocalFlags)

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <TopoDS.hxx>

#include <cstdint>
#include <string>
#include <vector>

namespace materializr {

namespace layoutui {

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

} // namespace layoutui

void Application::renderDockspace() {
    // Host the dockspace in a window inset above the status bar so docked panels
    // (e.g. the Tools window's bottom Delete button) aren't covered by the
    // full-width status bar overlay. Reuse the original DockSpaceOverViewport
    // dockspace id (0x08BD597D) so the saved imgui.ini layout still binds.
    const float statusBarHeight = 24.0f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // Transparent host + a pass-through central node so the OpenGL scene shows
    // through. This matters now that the host is submitted EVERY frame (incl.
    // modern/im-touch): there the viewport is undocked, leaving the central
    // node empty — an opaque host/node would paint dark over the 3D view.
    // Docked classic windows cover the host anyway, so it looks identical there.
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("DockHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(0x08BD597Du, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);

    // Per-node tab-bar policy. The viewport's tab bar is permanently OFF
    // (NoTabBar = no tab AND no re-show triangle) — it's the whole app, never
    // something to label or hide. Every panel ALWAYS shows its tab (a label +
    // drag handle) and loses the "Hide tab bar" menu, so panel visibility is
    // owned solely by Settings > Panels. Applied to LocalFlags each frame so it
    // overrides whatever the saved imgui.ini had (e.g. the central node and the
    // Interactions node both shipped with HiddenTabBar=1).
    auto setNodeFlags = [](const char* win, ImGuiDockNodeFlags set,
                           ImGuiDockNodeFlags clear) {
        if (ImGuiWindow* w = ImGui::FindWindowByName(win))
            if (w->DockNode) {
                w->DockNode->LocalFlags |= set;
                w->DockNode->LocalFlags &= ~clear;
            }
    };
    setNodeFlags("Viewport", ImGuiDockNodeFlags_NoTabBar, 0);
    const ImGuiDockNodeFlags kPanelSet   = ImGuiDockNodeFlags_NoWindowMenuButton;
    const ImGuiDockNodeFlags kPanelClear = ImGuiDockNodeFlags_HiddenTabBar |
                                           ImGuiDockNodeFlags_AutoHideTabBar;
    setNodeFlags("Tools",        kPanelSet, kPanelClear);
    setNodeFlags("Interactions", kPanelSet, kPanelClear);
    setNodeFlags("Items",        kPanelSet, kPanelClear);
    setNodeFlags("History",      kPanelSet, kPanelClear);
    setNodeFlags("Properties",   kPanelSet, kPanelClear);
    ImGui::End();
}

bool Application::touchCanUndo() const {
    if (m_inSketchMode) {
        // An in-progress shape can always be cancelled; otherwise only committed
        // sketch edits (steps after the sketch's entry) are undoable here.
        if (m_sketchTool && m_sketchTool->isPlacing()) return true;
        return m_history->canUndo() &&
               m_history->currentStep() > m_sketchEntryHistoryStep;
    }
    return m_history->canUndo();
}

void Application::touchUndo() {
    if (m_inSketchMode) {
        // Mid-placement Undo backs out the in-progress shape first — the editor
        // convention, and what the sketch's own Ctrl+Z does.
        if (m_sketchTool && m_sketchTool->isPlacing()) {
            m_sketchTool->onCancel();
            m_meshesDirty = true;
            return;
        }
        // Undo committed sketch edits, but never past the sketch's entry into
        // history: rolling the host body back under a live sketch crashes.
        if (m_history->canUndo() &&
            m_history->currentStep() > m_sketchEntryHistoryStep) {
            undoWithCascade();                 // undoes + re-cascades the body
            if (m_activeSketch) m_activeSketch->pruneOrphanPoints();
        }
        return;
    }
    if (m_history->canUndo()) undoWithCascade();
}

// The four menu bodies, shared by classic's menu bar and the modern/im-touch
// overflow popup — one item list each, so the layouts cannot drift.
void Application::renderFileMenuItems(bool withSettings) {
    if (ImGui::MenuItem("Open Project...", "Ctrl+O")) loadProject();
    // Open Recent — persisted, most-recent-first. Greyed when empty.
    if (ImGui::BeginMenu("Open Recent", !m_recentProjects.empty())) {
        // Snapshot: openRecentProject() mutates m_recentProjects.
        std::vector<AppSettings::RecentProject> snapshot = m_recentProjects;
        for (size_t i = 0; i < snapshot.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::MenuItem(snapshot[i].name.c_str()))
                openRecentProject(snapshot[i]);
            if (ImGui::IsItemHovered() && !snapshot[i].ref.empty())
                ImGui::SetTooltip("%s", snapshot[i].ref.c_str());
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear Recent")) {
            m_recentProjects.clear();
            saveAppSettings();
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Save Project", "Ctrl+S")) saveProjectQuick();
    if (ImGui::MenuItem("Save Project As...")) saveProject();
    if (ImGui::MenuItem("New Project")) closeProject();
    ImGui::Separator();

    // Build Import submenu from IOFormat contributions
    auto& formats = PluginRegistry::instance().ioFormats();
    bool hasImporters = false;
    for (auto& fmt : formats) { if (fmt.canImport) { hasImporters = true; break; } }
    if (hasImporters && ImGui::BeginMenu("Import")) {
        for (size_t i = 0; i < formats.size(); ++i) {
            auto& fmt = formats[i];
            if (!fmt.canImport || !fmt.importFn) continue;
            ImGui::PushID(static_cast<int>(i));
            std::string label = fmt.name + "...";
            if (ImGui::MenuItem(label.c_str())) {
                fmt.importFn(*m_pluginContext, "");
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }

    // Build Export submenu from IOFormat contributions
    bool hasExporters = false;
    for (auto& fmt : formats) { if (fmt.canExport) { hasExporters = true; break; } }
    if (hasExporters && ImGui::BeginMenu("Export")) {
        for (size_t i = 0; i < formats.size(); ++i) {
            auto& fmt = formats[i];
            if (!fmt.canExport || !fmt.exportFn) continue;
            ImGui::PushID(static_cast<int>(i) + 1000);
            std::string label = fmt.name + "...";
            if (ImGui::MenuItem(label.c_str())) {
                fmt.exportFn(*m_pluginContext, "");
            }
            ImGui::PopID();
        }
        // Timelapse GIF — the desktop surface for the recording the im-touch
        // timelapse button owns (frames captured per committed step).
        ImGui::Separator();
        const int tlFrames = m_timelapse ? m_timelapse->frameCount() : 0;
        ImGui::BeginDisabled(tlFrames < 2);
        if (ImGui::MenuItem("Timelapse GIF (full length)..."))
            exportTimelapse(0);
        if (ImGui::MenuItem("Timelapse GIF (30 seconds)..."))
            exportTimelapse(30);
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }

    if (withSettings) {
        ImGui::Separator();
        if (ImGui::MenuItem("Settings...")) {
            // Stage the current bindings so the dialog can Cancel cleanly.
            m_settingsOrbitButton = m_orbitButton;
            m_settingsPanButton = m_panButton;
            m_showSettings = true;
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Exit", "Alt+F4")) m_window->requestClose(true);
}

void Application::renderEditMenuItems() {
    // Disabled while a legacy preview is live: those previews
    // undo/re-push their op per frame, and an outside undo pops the
    // preview op so the preview's NEXT cycle pops the user's last
    // COMMITTED op instead — which then gets erased for good when
    // the preview pushes over the redo tail. (How "pull, confirm,
    // pull the other way" ate the first body.)
    const bool histLocked = anyInteractivePreviewActive();
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false,
                        !histLocked && m_history->canUndo())) {
        undoWithCascade();
    }
    if (ImGui::MenuItem("Redo", "Ctrl+Y", false,
                        !histLocked && m_history->canRedo())) {
        redoWithCascade();
    }
}

void Application::renderConstructionMenuItems() {
    // Detect which plane/axis derivations the current selection supports —
    // mirrors Toolbar::renderAddPlaneMenu / renderAddAxisMenu (keep in sync).
    int planarFaces = 0, planeCount = 0, vertexCount = 0;
    bool haveCyl = false, straightEdge = false, haveAxis = false;
    if (m_selection) {
        for (const auto& e : m_selection->getSelection()) {
            if (e.type == SelectionType::Plane)  { ++planeCount;  continue; }
            if (e.type == SelectionType::Axis)   { haveAxis = true; continue; }
            if (e.type == SelectionType::Vertex) { ++vertexCount; continue; }
            if (e.shape.IsNull()) continue;
            try {
                if (e.type == SelectionType::Face) {
                    Handle(Geom_Surface) srf = BRep_Tool::Surface(TopoDS::Face(e.shape));
                    if (!srf.IsNull()) {
                        if (srf->IsKind(STANDARD_TYPE(Geom_Plane))) ++planarFaces;
                        else if (!Handle(Geom_CylindricalSurface)::DownCast(srf).IsNull())
                            haveCyl = true;
                    }
                } else if (e.type == SelectionType::Edge) {
                    BRepAdaptor_Curve ad(TopoDS::Edge(e.shape));
                    if (ad.GetType() == GeomAbs_Line) straightEdge = true;
                }
            } catch (...) {}
        }
    }
    const bool midplane   = (planarFaces >= 2) || (planeCount >= 2);
    const bool twoVerts   = (vertexCount >= 2);
    const bool faceNormal = (planarFaces >= 1);
    const bool anyPlane = m_pluginContext &&
                          (midplane || haveCyl || haveAxis || straightEdge);
    const bool anyAxis  = m_pluginContext &&
                          (haveCyl || straightEdge || twoVerts || faceNormal || midplane);

    // Plane ▸ and Axis ▸ are always present so the catalogue is discoverable.
    // Each leads with the BASE "New …" creator (the world-plane/-axis popup —
    // always available, selection or not), then the modes derived FROM the
    // selection; with nothing suitable selected the derived section explains
    // what to pick instead of vanishing.
    if (ImGui::BeginMenu("Plane")) {
        if (m_pluginContext && ImGui::MenuItem("New Plane..."))
            m_pluginContext->requestInteractiveOp("ConstructionPlane");
        ImGui::Separator();
        if (!anyPlane) {
            ImGui::MenuItem("Select what to derive from:", nullptr, false, false);
            ImGui::MenuItem("2 flat faces/planes  - midplane", nullptr, false, false);
            ImGui::MenuItem("a cylinder  - tangent / normal", nullptr, false, false);
            ImGui::MenuItem("an edge or axis  - normal plane", nullptr, false, false);
        } else {
            if (midplane && ImGui::MenuItem("Midplane (between the 2 selected)"))
                m_pluginContext->requestInteractiveOp("Midplane");
            if (haveCyl) {
                if (ImGui::MenuItem("Tangent to cylinder"))
                    m_pluginContext->requestInteractiveOp("TangentPlane");
                if (ImGui::MenuItem("Perpendicular to cylinder axis"))
                    m_pluginContext->requestInteractiveOp("PlaneNormalToAxis");
                if (ImGui::MenuItem("Through cylinder axis (longitudinal)"))
                    m_pluginContext->requestInteractiveOp("PlaneThroughAxis");
            } else if (haveAxis || straightEdge) {
                if (ImGui::MenuItem(straightEdge ? "Normal to edge" : "Normal to axis"))
                    m_pluginContext->requestInteractiveOp("PlaneNormalToAxis");
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Axis")) {
        if (m_pluginContext && ImGui::MenuItem("New Axis..."))
            m_pluginContext->requestInteractiveOp("ConstructionAxis");
        ImGui::Separator();
        if (!anyAxis) {
            ImGui::MenuItem("Select what to derive from:", nullptr, false, false);
            ImGui::MenuItem("a cylinder or straight edge", nullptr, false, false);
            ImGui::MenuItem("2 vertices / a flat face / 2 planes", nullptr, false, false);
        } else {
            if (haveCyl && ImGui::MenuItem("From cylinder axis"))
                m_pluginContext->requestInteractiveOp("AxisFromCylinder");
            if (straightEdge && ImGui::MenuItem("Along edge"))
                m_pluginContext->requestInteractiveOp("AxisAlongEdge");
            if (twoVerts && ImGui::MenuItem("Through two vertices"))
                m_pluginContext->requestInteractiveOp("AxisTwoPoints");
            if (faceNormal && ImGui::MenuItem("Normal to face"))
                m_pluginContext->requestInteractiveOp("AxisNormalToFace");
            if (midplane && ImGui::MenuItem("Intersection of two planes"))
                m_pluginContext->requestInteractiveOp("AxisTwoPlanes");
        }
        ImGui::EndMenu();
    }
}

void Application::renderViewMenuItems() {
    if (ImGui::MenuItem("Reset Camera", "Home")) m_viewport->getCamera().reset();
    // The F shortcut's menu twin — and the only way to frame on touch.
    if (ImGui::MenuItem("Frame Selection", "F")) frameSelection();
    // Measure lives here now — one home for it across layouts instead of a
    // toolbar/rail button duplicated per context. Drops the user at the
    // measure mode picker (Object / Edge / Point-to-Point).
    if (ImGui::MenuItem("Measure...")) {
        if (m_measureTool) m_measureTool->setMode(MeasureMode::PickMode);
    }
    if (ImGui::MenuItem("Section View", nullptr, &m_sectionEnabled)) {
        m_sectionDirty = true;
        if (m_sectionEnabled) {
            // Aim the plane through the middle of the visible
            // bodies so enabling it visibly halves the scene —
            // a zero-offset plane at the world origin can sit
            // entirely outside (or under) everything.
            try {
                Bnd_Box bb;
                for (int id : m_document->getAllBodyIds())
                    if (m_document->isBodyVisible(id))
                        BRepBndLib::Add(m_document->getBody(id), bb);
                if (!bb.IsVoid()) {
                    double x0, y0, z0, x1, y1, z1;
                    bb.Get(x0, y0, z0, x1, y1, z1);
                    gp_Pnt c(0.5 * (x0 + x1), 0.5 * (y0 + y1),
                             0.5 * (z0 + z1));
                    gp_Pln pl = sectionBasePlane();
                    m_sectionOffset = static_cast<float>(
                        gp_Vec(pl.Location(), c)
                            .Dot(gp_Vec(pl.Axis().Direction())));
                }
            } catch (...) {}
        }
    }
    ImGui::Separator();
    // Collapse the docked side panels to give the 3D view the whole
    // window — a fallback for small screens (and a quick "maximize
    // canvas" anywhere). The panels keep their docked widths and snap
    // back on toggle. F9 on a keyboard; touch gets edge tabs. This menu
    // item hides/shows BOTH columns at once; the checkmark = both hidden.
    bool bothHidden = m_leftPanelHidden && m_rightPanelHidden;
    if (ImGui::MenuItem("Hide Panels", "F9", bothHidden)) {
        bool hide = !bothHidden;
        m_leftPanelHidden = m_rightPanelHidden = hide;
        saveAppSettings();
    }
    ImGui::Separator();
    if (m_themeManager->renderSelector()) {
        m_themeManager->apply();
    }
}

void Application::renderHelpMenuItems() {
    if (ImGui::MenuItem("User Guide")) m_helpPanel->setVisible(true);
    if (ImGui::MenuItem("Keyboard Shortcuts")) m_shortcutsPanel->setVisible(true);
    ImGui::Separator();
    if (ImGui::MenuItem("Check for Updates...")) {
        m_showUpdatePopup = true;
        m_updateChecked = false; // run the network call when the popup opens
    }
    // Plugin-contributed Help items (e.g. the Tutorial's "Getting
    // Started"). Lets a plugin add a launcher without Application
    // knowing about it. See renderPluginMenuItems.
    renderPluginMenuItems("Help");
    ImGui::Separator();
    if (ImGui::MenuItem("About Materializr...")) m_aboutDialog->setVisible(true);
}

void Application::renderPluginMenuItems(const char* menuName) {
    // Render every plugin MenuContribution whose path is "<menuName> > Label"
    // as a MenuItem in the current menu. Keeps the contribution type generic
    // (a plugin says where it wants to live) without Application hardcoding it.
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t");
        size_t b = s.find_last_not_of(" \t");
        return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
    };
    for (auto& m : PluginRegistry::instance().menuContributions()) {
        auto gt = m.path.find('>');
        if (gt == std::string::npos) continue;
        if (trim(m.path.substr(0, gt)) != menuName) continue;
        std::string label = trim(m.path.substr(gt + 1));
        const bool enabled = !m.enabled || m.enabled(*m_pluginContext);
        const char* sc = m.shortcut.empty() ? nullptr : m.shortcut.c_str();
        if (ImGui::MenuItem(label.c_str(), sc, false, enabled) && m.action)
            m.action(*m_pluginContext);
    }
}

// The ⋯/☰ menu shared by the modern and im-touch layouts: the full desktop
// menus, flattened one level, via the shared item lists (renderFileMenuItems
// & co.) so these layouts and classic's menu bar cannot drift. Caller does
// OpenPopup("##TouchOverflow") on its trigger button.
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

void Application::renderRailPolygonSidesPopup(bool clicked) {
    // Same side-count popout as the classic sketch toolbar (Toolbar.cpp): pick a
    // named polygon, which sets the tool's side count and starts placement — so
    // every layout drives the identical polygon flow. `clicked` is this frame's
    // rail-button result; the popup body renders every frame while open.
    if (clicked) ImGui::OpenPopup("##railPolySides");
    if (ImGui::BeginPopup("##railPolySides")) {
        struct PolyChoice { const char* name; int sides; };
        static const PolyChoice choices[] = {
            {"Triangle (3)", 3}, {"Square (4)", 4}, {"Pentagon (5)", 5},
            {"Hexagon (6)", 6}, {"Heptagon (7)", 7}, {"Octagon (8)", 8}};
        for (const auto& c : choices)
            if (ImGui::MenuItem(c.name)) {
                m_toolbar->setRequestedPolygonSides(c.sides);
                handleToolAction(static_cast<int>(ToolAction::Polygon));
            }
        ImGui::EndPopup();
    }
}

} // namespace materializr

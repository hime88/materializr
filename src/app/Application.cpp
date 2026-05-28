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
#include <TopoDS.hxx>
#include <stdexcept>
#include <cstdio>

namespace materializr {

Application::Application() {
    m_window = std::make_unique<Window>(1600, 900, "Materializr");
    m_viewport = std::make_unique<Viewport>();
    m_grid = std::make_unique<Grid>();
    m_shapeRenderer = std::make_unique<ShapeRenderer>();
    m_sketchRenderer = std::make_unique<SketchRenderer>();
    m_edgeRenderer = std::make_unique<EdgeRenderer>();
    m_planeRenderer = std::make_unique<PlaneRenderer>();
    m_backgroundRenderer = std::make_unique<BackgroundRenderer>();
    m_document = std::make_unique<Document>();
    m_history = std::make_unique<History>();
    m_selection = std::make_unique<SelectionManager>();
    m_eventBus = std::make_unique<EventBus>();
    m_pluginContext = std::make_unique<PluginContext>();

    m_toolbar = std::make_unique<Toolbar>();
    m_historyPanel = std::make_unique<HistoryPanel>();
    m_itemsPanel = std::make_unique<ItemsPanel>();
    m_commandPalette = std::make_unique<CommandPalette>();

    m_sketchTool = std::make_unique<SketchTool>();
    m_viewCube = std::make_unique<ViewCube>();
    m_picker = std::make_unique<Picker>();
    m_gizmo = std::make_unique<Gizmo>();
    m_selectionHighlight = std::make_unique<SelectionHighlight>();
    m_boxSelect = std::make_unique<BoxSelect>();
    m_statusBar = std::make_unique<StatusBar>();
    m_themeManager = std::make_unique<ThemeManager>();
    m_propertiesPanel = std::make_unique<PropertiesPanel>();
    m_aboutDialog = std::make_unique<AboutDialog>();
    m_shortcutsPanel = std::make_unique<ShortcutsPanel>();
    m_helpPanel = std::make_unique<HelpPanel>();

    // Wire up references
    m_toolbar->setSelectionManager(m_selection.get());
    m_toolbar->setPluginContext(m_pluginContext.get());
    m_historyPanel->setHistory(m_history.get());
    m_historyPanel->setDocument(m_document.get());
    m_itemsPanel->setDocument(m_document.get());
    m_itemsPanel->setSelectionManager(m_selection.get());
    m_itemsPanel->setHistory(m_history.get());
    m_statusBar->setDocument(m_document.get());
    m_statusBar->setSelectionManager(m_selection.get());
    m_propertiesPanel->setHistory(m_history.get());
    m_propertiesPanel->setDocument(m_document.get());
    m_propertiesPanel->setSelectionManager(m_selection.get());

    initImGui();
    loadAppSettings(); // restore persisted preferences before the theme is applied
    m_themeManager->apply();
    initRenderers();
    setupCommands();

    // Wire EventBus into core services
    m_document->setEventBus(m_eventBus.get());
    m_history->setEventBus(m_eventBus.get());
    m_selection->setEventBus(m_eventBus.get());

    // Plugin system
    m_pluginContext->_bind(m_document.get(), m_history.get(), m_selection.get(),
                          m_eventBus.get(), &m_viewport->getCamera(), &m_meshesDirty);
    materializr::force_link::linkAll();
    PluginRegistry::instance().initAll(*m_pluginContext);

    // Register plugin commands in the command palette
    for (auto& cmd : PluginRegistry::instance().commandContributions()) {
        auto* ctxPtr = m_pluginContext.get();
        m_commandPalette->addCommand(cmd.name, cmd.shortcut, [&cmd, ctxPtr]() {
            if (cmd.action) cmd.action(*ctxPtr);
        });
    }
}

Application::~Application() {
    PluginRegistry::instance().shutdownAll();
    m_backgroundRenderer.reset();
    m_planeRenderer.reset();
    m_edgeRenderer.reset();
    m_sketchRenderer.reset();
    m_shapeRenderer.reset();
    m_grid.reset();
    m_viewport.reset();
    shutdownImGui();
}

static const char* s_defaultLayout = R"([Window][WindowOverViewport_11111111]
Pos=0,19
Size=1600,881
Collapsed=0

[Window][Debug##Default]
Pos=60,60
Size=400,400
Collapsed=0

[Window][Viewport]
Pos=177,19
Size=1122,881
Collapsed=0
DockId=0x00000001,0

[Window][Tools]
Pos=0,19
Size=175,881
Collapsed=0
DockId=0x00000003,0

[Window][Interactions]
Pos=1301,19
Size=299,175
Collapsed=0
DockId=0x00000007,0

[Window][Items]
Pos=1301,197
Size=299,339
Collapsed=0
DockId=0x00000008,0

[Window][History]
Pos=1301,538
Size=299,362
Collapsed=0
DockId=0x00000006,1

[Window][Properties]
Pos=1301,538
Size=299,362
Collapsed=0
DockId=0x00000006,0

[Docking][Data]
DockSpace       ID=0x08BD597D Window=0x1BBC0F80 Pos=0,19 Size=1600,881 Split=X
  DockNode      ID=0x00000003 Parent=0x08BD597D SizeRef=175,900 Selected=0x18A5FDB9
  DockNode      ID=0x00000004 Parent=0x08BD597D SizeRef=1423,900 Split=X
    DockNode    ID=0x00000001 Parent=0x00000004 SizeRef=1122,900 CentralNode=1 Selected=0xC450F867
    DockNode    ID=0x00000002 Parent=0x00000004 SizeRef=299,900 Split=Y Selected=0x933ECD57
      DockNode  ID=0x00000005 Parent=0x00000002 SizeRef=148,528 Split=Y Selected=0x933ECD57
        DockNode ID=0x00000007 Parent=0x00000005 SizeRef=148,175 HiddenTabBar=1
        DockNode ID=0x00000008 Parent=0x00000005 SizeRef=148,348 Selected=0x933ECD57
      DockNode  ID=0x00000006 Parent=0x00000002 SizeRef=148,370 Selected=0x8C72BEA8
)";

// Where to read/write imgui.ini. On Linux we keep the relative "imgui.ini" path
// (the AppImage runs from a user-writable cwd, which is the existing behaviour
// the user prefers). On Windows the exe usually launches from Program Files,
// which is read-only without admin — the write would silently fail and ImGui
// would fall back to its tiny "everything stacked at (0,0)" defaults. Anchor
// the file under %APPDATA% there so it's both writable and per-user.
static std::string s_imguiIniPath;
static const char* computeImguiIniPath() {
#ifdef _WIN32
    std::string base;
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata) {
        base = std::string(appdata) + "\\materializr";
    } else if (const char* up = std::getenv("USERPROFILE"); up && *up) {
        base = std::string(up) + "\\materializr";
    } else {
        s_imguiIniPath = "imgui.ini";
        return s_imguiIniPath.c_str();
    }
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    s_imguiIniPath = base + "\\imgui.ini";
#else
    s_imguiIniPath = "imgui.ini";
#endif
    return s_imguiIniPath.c_str();
}

void Application::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // Write the default layout when imgui.ini is missing, or migrate a layout
    // saved before the Interactions panel existed (detected by its window name)
    // so the panel actually appears docked above Items rather than not at all.
    const char* iniPath = computeImguiIniPath();
    {
        bool needDefault = true;
        if (std::FILE* f = std::fopen(iniPath, "r")) {
            std::string ini;
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) ini.append(buf, n);
            std::fclose(f);
            if (ini.find("[Window][Interactions]") != std::string::npos) needDefault = false;
        }
        if (needDefault) {
            if (std::FILE* f = std::fopen(iniPath, "w")) {
                std::fputs(s_defaultLayout, f);
                std::fclose(f);
            }
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    // Point ImGui at the chosen ini path BEFORE the first NewFrame so it loads
    // the default layout we just wrote. The string is owned by s_imguiIniPath
    // and stays alive for the program's lifetime.
    io.IniFilename = iniPath;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.08f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.14f, 0.18f, 1.0f);

    ImGui_ImplGlfw_InitForOpenGL(m_window->handle(), true);
    ImGui_ImplOpenGL3_Init("#version 330");
}

void Application::shutdownImGui() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void Application::initRenderers() {
    if (!m_grid->initialize()) {
        std::fprintf(stderr, "Failed to initialize grid renderer\n");
        return;
    }
    if (!m_shapeRenderer->initialize()) {
        std::fprintf(stderr, "Failed to initialize shape renderer\n");
        return;
    }
    if (!m_sketchRenderer->initialize()) {
        std::fprintf(stderr, "Failed to initialize sketch renderer\n");
        return;
    }

    if (!m_backgroundRenderer->initialize()) {
        std::fprintf(stderr, "Failed to initialize background renderer\n");
    }
    if (!m_selectionHighlight->initialize()) {
        std::fprintf(stderr, "Failed to initialize selection highlight\n");
    }
    if (!m_boxSelect->initialize()) {
        std::fprintf(stderr, "Failed to initialize box select\n");
    }
    if (!m_gizmo->initialize()) {
        std::fprintf(stderr, "Failed to initialize gizmo\n");
    }
    if (!m_edgeRenderer->initialize()) {
        std::fprintf(stderr, "Failed to initialize edge renderer\n");
    }
    if (!m_planeRenderer->initialize()) {
        std::fprintf(stderr, "Failed to initialize plane renderer\n");
    }

    // Create a demo box so there's something to see (a 20 mm cube).
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
    m_document->addBody(box, "Demo Box");
    m_meshesDirty = true;

    // Frame it so the larger cube isn't clipped by the default camera distance.
    try {
        Bnd_Box bbox;
        BRepBndLib::Add(box, bbox);
        double x0, y0, z0, x1, y1, z1;
        bbox.Get(x0, y0, z0, x1, y1, z1);
        m_viewport->getCamera().zoomToFit(
            glm::vec3(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(z0)),
            glm::vec3(static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(z1)));
    } catch (...) {}

    m_renderersReady = true;
}

void Application::setupCommands() {
    // Commands are now registered by plugins via PluginRegistry.
    // Plugin commands are added to the command palette after initAll().
}

void Application::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void Application::endFrame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

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
    ImGui::Begin("DockHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(0x08BD597Du, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    ImGui::End();
}

void Application::renderMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Project...", "Ctrl+O")) loadProject();
            if (ImGui::MenuItem("Save Project", "Ctrl+S")) saveProjectQuick();
            if (ImGui::MenuItem("Save Project As...")) saveProject();
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
                ImGui::EndMenu();
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Settings...")) {
                // Stage the current bindings so the dialog can Cancel cleanly.
                m_settingsOrbitButton = m_orbitButton;
                m_settingsPanButton = m_panButton;
                m_showSettings = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) glfwSetWindowShouldClose(m_window->handle(), true);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_history->canUndo())) {
                m_history->undo(*m_document);
                m_meshesDirty = true;
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_history->canRedo())) {
                m_history->redo(*m_document);
                m_meshesDirty = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Reset Camera", "Home")) m_viewport->getCamera().reset();
            if (ImGui::MenuItem("Command Palette", "Ctrl+K")) m_commandPalette->toggle();
            ImGui::Separator();
            if (m_themeManager->renderSelector()) {
                m_themeManager->apply();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("User Guide")) m_helpPanel->setVisible(true);
            if (ImGui::MenuItem("Keyboard Shortcuts")) m_shortcutsPanel->setVisible(true);
            ImGui::Separator();
            if (ImGui::MenuItem("Check for Updates...")) {
                m_showUpdatePopup = true;
                m_updateChecked = false; // run the network call when the popup opens
            }
            ImGui::Separator();
            if (ImGui::MenuItem("About Materializr...")) m_aboutDialog->setVisible(true);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void Application::loadAppSettings() {
    AppSettings s = SettingsIO::load(SettingsIO::defaultPath());
    m_themeManager->setTheme(s.theme == 1 ? Theme::Light : Theme::Dark);
    m_orbitButton = s.orbitButton;
    m_panButton = s.panButton;
    m_settingsOrbitButton = s.orbitButton;
    m_settingsPanButton = s.panButton;
    m_viewport->getCamera().setLevelOrbit(s.levelOrbit);
    m_autosaveEnabled = s.autosaveEnabled;
    m_autosaveIntervalSec = static_cast<float>(s.autosaveIntervalSec);
    m_invertCubeDrag = s.invertCubeDrag;
}

void Application::saveAppSettings() {
    AppSettings s;
    s.theme = (m_themeManager->getTheme() == Theme::Light) ? 1 : 0;
    s.orbitButton = m_orbitButton;
    s.panButton = m_panButton;
    s.levelOrbit = m_viewport->getCamera().isLevelOrbit();
    s.autosaveEnabled = m_autosaveEnabled;
    s.autosaveIntervalSec = static_cast<int>(m_autosaveIntervalSec);
    s.invertCubeDrag = m_invertCubeDrag;
    SettingsIO::save(SettingsIO::defaultPath(), s);
}



void Application::handleToolAction(int action) {
    ToolAction a = static_cast<ToolAction>(action);
    switch (a) {
        case ToolAction::StartSketch: enterSketchMode(); break;
        // Each plane is built so that alignCameraToActiveSketch (camera at +normal,
        // up = plane YDirection) reproduces the matching ViewCube view exactly, in
        // this Y-up world: XY = Top, XZ = Front, YZ = Right. gp_Ax3(origin, normal,
        // xDir); YDirection (the camera up) = normal × xDir.
        case ToolAction::StartSketchXY: // Top: camera +Y, up -Z
            enterSketchOnPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0), gp_Dir(1, 0, 0)))); break;
        case ToolAction::StartSketchXZ: // Front: camera +Z, up +Y
            enterSketchOnPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)))); break;
        case ToolAction::StartSketchYZ: // Right: camera +X, up +Y
            enterSketchOnPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0), gp_Dir(0, 0, -1)))); break;
        case ToolAction::SketchOnFace: {
            const auto& sel = m_selection->getSelection();
            for (const auto& entry : sel) {
                if (entry.type == SelectionType::Face && !entry.shape.IsNull()) {
                    enterSketchOnFace(TopoDS::Face(entry.shape));
                    if (m_activeSketch) m_activeSketch->setSourceBody(entry.bodyId);
                    break;
                }
            }
            break;
        }
        case ToolAction::FinishSketch: {
            if (m_inSketchMode) {
                recordSketchMutation([&]{ m_sketchTool->onConfirm(); });
                exitSketchMode();
            }
            break;
        }
        case ToolAction::EditSketch: {
            const auto& sel = m_selection->getSelection();
            // Accept a selected whole sketch OR a selected sketch region (edit the
            // region's parent sketch) so "Edit Sketch" works from the region tools.
            for (const auto& entry : sel) {
                if ((entry.type == SelectionType::Sketch ||
                     entry.type == SelectionType::SketchRegion) && entry.sketchId >= 0) {
                    editSketch(entry.sketchId);
                    break;
                }
            }
            break;
        }
        case ToolAction::ExtrudeSketch: {
            const auto& sel = m_selection->getSelection();
            for (const auto& entry : sel) {
                if (entry.type == SelectionType::Sketch && entry.sketchId >= 0) {
                    extrudeSketchById(entry.sketchId);
                    break;
                }
            }
            break;
        }
        case ToolAction::PushPull: {
            beginPushPull();
            break;
        }
        case ToolAction::LookAtSketch: {
            alignCameraToActiveSketch();
            break;
        }
        case ToolAction::SelectSketch:
            if (m_inSketchMode) m_sketchTool->setMode(SketchToolMode::Select);
            break;
        case ToolAction::Line:
            if (m_inSketchMode) m_sketchTool->setMode(SketchToolMode::Line);
            break;
        case ToolAction::Circle:
            if (m_inSketchMode) m_sketchTool->setMode(SketchToolMode::Circle);
            break;
        case ToolAction::Rectangle:
            if (m_inSketchMode) m_sketchTool->setMode(SketchToolMode::Rectangle);
            break;
        case ToolAction::Arc:
            if (m_inSketchMode) m_sketchTool->setMode(SketchToolMode::Arc);
            break;
        case ToolAction::Spline:
            if (m_inSketchMode) m_sketchTool->setMode(SketchToolMode::Spline);
            break;
        case ToolAction::Polygon:
            if (m_inSketchMode) m_sketchTool->setMode(SketchToolMode::Polygon);
            break;
        case ToolAction::Trim:
            if (m_inSketchMode) m_sketchTool->setMode(SketchToolMode::Trim);
            break;

        // --- Sketch element transforms (operate on the Select-mode selection) ---
        case ToolAction::SketchCopy:
        case ToolAction::SketchMirror:
        case ToolAction::SketchRotate: {
            if (!m_inSketchMode || !m_activeSketch || !m_sketchTool) break;

            // Operate on the current sketch-element selection, or — if nothing
            // is selected (the common case when the user hasn't switched into
            // Select mode yet) — on the whole sketch.
            std::set<int> involved;
            std::set<int> selLines;
            if (m_sketchTool->hasElementSelection()) {
                involved.insert(m_sketchTool->getSelectedPoints().begin(),
                                m_sketchTool->getSelectedPoints().end());
                selLines = m_sketchTool->getSelectedLines();
                for (int lid : selLines) {
                    for (const auto& l : m_activeSketch->getLines()) {
                        if (l.id == lid) {
                            involved.insert(l.startPointId);
                            involved.insert(l.endPointId);
                            break;
                        }
                    }
                }
            } else {
                // No selection → entire sketch.
                for (const auto& p : m_activeSketch->getPoints()) involved.insert(p.id);
                for (const auto& l : m_activeSketch->getLines())  selLines.insert(l.id);
            }
            if (involved.empty()) break;
            // Centroid of the involved points — used as the mirror/rotate pivot
            // so the transform happens "in place" near the selection rather than
            // about the sketch origin.
            glm::vec2 c{0.0f};
            int n = 0;
            for (int id : involved) {
                if (auto* p = m_activeSketch->getPoint(id)) { c += p->pos; ++n; }
            }
            if (n > 0) c /= static_cast<float>(n);

            if (a == ToolAction::SketchRotate) {
                // Enter interactive rotate mode: spin the affected points around
                // the centroid based on cursor angle, commit on left-click.
                m_sketchRotating = true;
                m_sketchRotateBefore = std::make_shared<Sketch>(*m_activeSketch);
                m_sketchRotateCenter = c;
                m_sketchRotateAnchor = m_sketchTool->getCurrentPos();
                m_sketchRotateOriginals.clear();
                for (int id : involved) {
                    if (auto* p = m_activeSketch->getPoint(id))
                        m_sketchRotateOriginals.push_back({id, p->pos});
                }
                // Don't push a history op yet; that happens on commit.
                break;
            }

            // Copy / Mirror: create new points (transformed copies) and new
            // lines connecting them, then SELECT the new ones and switch to
            // Select mode so the user can immediately drag them to position.
            auto before = std::make_shared<Sketch>(*m_activeSketch);
            std::unordered_map<int, int> remap;
            for (int oldId : involved) {
                auto* p = m_activeSketch->getPoint(oldId);
                if (!p) continue;
                glm::vec2 np = p->pos;
                if (a == ToolAction::SketchCopy) {
                    // Land the duplicate exactly on the original; the user can
                    // immediately drag the now-selected copy to place it.
                    np = p->pos;
                } else { // Mirror across vertical line through centroid (flips X).
                    np = glm::vec2(2.0f * c.x - p->pos.x, p->pos.y);
                }
                int newId = m_activeSketch->addPoint(np);
                remap[oldId] = newId;
            }
            std::set<int> newLineIds;
            for (int lid : selLines) {
                for (const auto& l : m_activeSketch->getLines()) {
                    if (l.id != lid) continue;
                    auto sIt = remap.find(l.startPointId);
                    auto eIt = remap.find(l.endPointId);
                    if (sIt != remap.end() && eIt != remap.end()) {
                        int newLine = m_activeSketch->addLine(sIt->second, eIt->second);
                        newLineIds.insert(newLine);
                    }
                    break;
                }
            }
            // Record + select the duplicates.
            std::set<int> newPointIds;
            for (auto& kv : remap) newPointIds.insert(kv.second);
            m_sketchTool->setSelection(newPointIds, newLineIds);
            m_sketchTool->setMode(SketchToolMode::Select);

            auto after = std::make_shared<Sketch>(*m_activeSketch);
            if (before->getPoints().size() != after->getPoints().size() ||
                before->getLines().size()  != after->getLines().size()) {
                auto op = std::make_unique<SketchEditOp>(m_activeSketch, before, after);
                m_history->pushExecuted(std::move(op));
            }
            break;
        }

        case ToolAction::ResetCamera: m_viewport->getCamera().reset(); break;

        case ToolAction::Move: {
            if (!m_selection->hasSelectedBodies()) break;
            m_gizmo->setMode(GizmoMode::Translate);
            break;
        }
        case ToolAction::Rotate: {
            if (!m_selection->hasSelectedBodies()) break;
            m_gizmo->setMode(GizmoMode::Rotate);
            break;
        }
        case ToolAction::Scale: {
            if (!m_selection->hasSelectedBodies()) break;
            m_gizmo->setMode(GizmoMode::Scale);
            break;
        }
        case ToolAction::Mirror: {
            const auto& sel = m_selection->getSelection();
            if (!sel.empty() && sel[0].bodyId >= 0) {
                m_mirrorBodyId = sel[0].bodyId;
                m_mirrorPickFace = false;
                m_showMirrorPopup = true;
            }
            break;
        }

        case ToolAction::Extrude: {
            const auto& sel = m_selection->getSelection();
            if (m_selection->selectedFaceCount() >= 1) {
                for (const auto& entry : sel) {
                    if (entry.type == SelectionType::Face && !entry.shape.IsNull()) {
                        beginInteractiveExtrude(entry.shape);
                        break;
                    }
                }
            }
            break;
        }

        case ToolAction::Fillet: {
            if (m_selection->selectedEdgeCount() >= 1)
                beginInteractiveEdgeOp(EdgeOpType::Fillet);
            break;
        }

        case ToolAction::Chamfer: {
            if (m_selection->selectedEdgeCount() >= 1)
                beginInteractiveEdgeOp(EdgeOpType::Chamfer);
            break;
        }

        default: break;
    }
}

void Application::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();

    // Undo/Redo — use GLFW directly so it works even when ImGui has text input focus
    bool ctrlHeld = glfwGetKey(m_window->handle(), GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                    glfwGetKey(m_window->handle(), GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    if (ctrlHeld && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (!m_edgeOpActive && !m_extruding && !m_pushPullActive) {
            if (m_history->canUndo()) {
                m_history->undo(*m_document);
                m_selection->clear();
                m_hoveredBodyId = -1;
                m_meshesDirty = true;
            }
        }
    }
    if (ctrlHeld && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        if (!m_edgeOpActive && !m_extruding && !m_pushPullActive) {
            if (m_history->canRedo()) {
                m_history->redo(*m_document);
                m_selection->clear();
                m_hoveredBodyId = -1;
                m_meshesDirty = true;
            }
        }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K)) {
        m_commandPalette->toggle();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_I)) {
        importStepFile();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_E)) {
        exportStepFile();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        saveProject();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
        loadProject();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (m_sketchRotating) {
            // Revert the sketch to its pre-rotate state.
            if (m_sketchRotateBefore && m_activeSketch) {
                for (auto& [id, op] : m_sketchRotateOriginals)
                    m_activeSketch->movePoint(id, op);
            }
            m_sketchRotating = false;
            m_sketchRotateBefore.reset();
            m_sketchRotateOriginals.clear();
        } else if (m_mirrorPickFace) {
            m_mirrorPickFace = false; // cancel "mirror across a face" mode
        } else if (m_gizmoDragging) {
            // Revert the body to where the drag started — same idea as cancelling
            // any other in-progress operation. Also cancel the gizmo's own drag
            // state so it doesn't keep dragging once the mouse moves again.
            try {
                if (m_gizmoDragBodyId >= 0 && !m_gizmoDragOriginalShape.IsNull()) {
                    m_document->updateBody(m_gizmoDragBodyId, m_gizmoDragOriginalShape);
                }
            } catch (...) {}
            m_gizmo->cancelDrag();
            m_gizmoDragging = false;
            m_gizmoDragOriginalShape.Nullify();
            m_gizmoDragBodyId = -1;
            m_gizmoTotalDelta = glm::vec3(0.0f);
            m_meshesDirty = true;
        } else if (m_pushPullActive) {
            cancelPushPull();
        } else if (m_edgeOpActive) {
            cancelInteractiveEdgeOp();
        } else if (m_extruding) {
            cancelInteractiveExtrude();
        } else if (m_inSketchMode) {
            m_sketchTool->onCancel();
            exitSketchMode();
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_edgeOpActive) {
        m_edgeOpValue = static_cast<float>(std::atof(m_edgeOpInputBuf));
        updateInteractiveEdgeOp();
        commitInteractiveEdgeOp();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_extruding) {
        m_extrudeDistance = static_cast<float>(std::atof(m_extrudeInputBuf));
        updateInteractiveExtrude();
        commitInteractiveExtrude();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_pushPullActive) {
        m_pushPullDistance = static_cast<float>(std::atof(m_pushPullInputBuf));
        updatePushPull();
        commitPushPull();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        m_viewport->getCamera().reset();
    }
    // Delete selected body (through history for undo)
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && m_selection->hasSelection()) {
        const auto& sel = m_selection->getSelection();
        std::vector<int> bodiesToDelete;
        std::vector<int> sketchesToDelete;
        for (const auto& entry : sel) {
            // A selected sketch (or sketch region) deletes the whole sketch; a
            // body/face/edge selection deletes its body.
            if (entry.type == SelectionType::Sketch || entry.type == SelectionType::SketchRegion) {
                if (entry.sketchId >= 0) {
                    bool already = false;
                    for (int s : sketchesToDelete) { if (s == entry.sketchId) { already = true; break; } }
                    if (!already) sketchesToDelete.push_back(entry.sketchId);
                }
            } else if (entry.bodyId >= 0) {
                bool already = false;
                for (int b : bodiesToDelete) { if (b == entry.bodyId) { already = true; break; } }
                if (!already) bodiesToDelete.push_back(entry.bodyId);
            }
        }
        for (int bodyId : bodiesToDelete) {
            auto op = std::make_unique<DeleteOp>();
            op->setBodyId(bodyId);
            m_history->pushOperation(std::move(op), *m_document);
        }
        for (int sketchId : sketchesToDelete) {
            m_document->removeSketch(sketchId);
            markDirty();
        }
        m_selection->clear();
        m_hoveredBodyId = -1;
        m_meshesDirty = true;
    }
    // Gizmo mode switching
    if (!m_inSketchMode && !io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) {
            m_gizmo->setMode(GizmoMode::Translate);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_E)) {
            m_gizmo->setMode(GizmoMode::Rotate);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R)) {
            m_gizmo->setMode(GizmoMode::Scale);
        }
    }
}

void Application::rebuildMeshes() {
    m_shapeRenderer->clear();
    m_edgeRenderer->clear();
    auto ids = m_document->getAllBodyIds();
    for (int id : ids) {
        if (!m_document->isBodyVisible(id)) continue;
        const TopoDS_Shape& shape = m_document->getBody(id);
        int idx = m_shapeRenderer->tessellate(shape, 0.1f);
        if (idx >= 0) {
            // Use the body's own colour (defaults to light grey) instead of an
            // index-based palette, so colours are stable and user-controllable.
            m_shapeRenderer->setColor(idx, m_document->getBodyColor(id));
        }
        m_edgeRenderer->addShape(shape, 0.1f);
    }
}

void Application::handleViewCubeAction(int action) {
    ViewCubeAction a = static_cast<ViewCubeAction>(action);
    Camera& cam = m_viewport->getCamera();

    // Incremental rotations: 90° around the camera's current axes (FreeCAD-like).
    constexpr float kRot = static_cast<float>(M_PI * 0.5);
    switch (a) {
        case ViewCubeAction::RotateLeft:  cam.rotateAroundTarget(-kRot, 0.0f); return;
        case ViewCubeAction::RotateRight: cam.rotateAroundTarget( kRot, 0.0f); return;
        case ViewCubeAction::RotateUp:    cam.rotateAroundTarget(0.0f, -kRot); return;
        case ViewCubeAction::RotateDown:  cam.rotateAroundTarget(0.0f,  kRot); return;
        default: break;
    }

    // Compute model bbox to centre and size the view.
    Bnd_Box bbox;
    for (int id : m_document->getAllBodyIds()) {
        if (!m_document->isBodyVisible(id)) continue;
        try { BRepBndLib::Add(m_document->getBody(id), bbox); } catch (...) {}
    }
    glm::vec3 cmin(-1.0f), cmax(1.0f);
    if (!bbox.IsVoid()) {
        double x0,y0,z0,x1,y1,z1; bbox.Get(x0,y0,z0,x1,y1,z1);
        cmin = glm::vec3(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(z0));
        cmax = glm::vec3(static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(z1));
    }
    glm::vec3 center = (cmin + cmax) * 0.5f;
    float radius = glm::length(cmax - cmin) * 0.5f;
    if (radius < 1.0f) radius = 1.0f;

    // Direction the camera looks FROM (model→camera) and its up vector. World is
    // Y-up, so Top/Bottom views need a non-Y up vector to be defined.
    glm::vec3 dir(0.0f), up(0.0f, 1.0f, 0.0f);
    switch (a) {
        case ViewCubeAction::Front:  dir = { 0, 0, 1}; up = {0, 1, 0}; break;
        case ViewCubeAction::Back:   dir = { 0, 0,-1}; up = {0, 1, 0}; break;
        case ViewCubeAction::Right:  dir = { 1, 0, 0}; up = {0, 1, 0}; break;
        case ViewCubeAction::Left:   dir = {-1, 0, 0}; up = {0, 1, 0}; break;
        case ViewCubeAction::Top:    dir = { 0, 1, 0}; up = {0, 0,-1}; break;
        case ViewCubeAction::Bottom: dir = { 0,-1, 0}; up = {0, 0, 1}; break;
        case ViewCubeAction::FrontTopRight:    dir = { 1, 1, 1}; break;
        case ViewCubeAction::FrontTopLeft:     dir = {-1, 1, 1}; break;
        case ViewCubeAction::BackTopRight:     dir = { 1, 1,-1}; break;
        case ViewCubeAction::BackTopLeft:      dir = {-1, 1,-1}; break;
        case ViewCubeAction::FrontBottomRight: dir = { 1,-1, 1}; break;
        case ViewCubeAction::FrontBottomLeft:  dir = {-1,-1, 1}; break;
        case ViewCubeAction::BackBottomRight:  dir = { 1,-1,-1}; break;
        case ViewCubeAction::BackBottomLeft:   dir = {-1,-1,-1}; break;
        default: return;
    }
    dir = glm::normalize(dir);

    cam.setOrthographic(false);
    cam.setTarget(center);
    cam.setPosition(center + dir * (radius * 3.0f)); // approx; zoomToFit refines
    cam.setUp(up);
    cam.zoomToFit(cmin, cmax);
}

void Application::saveProject() {
    FileDialogs::saveFile("Save Project", "project.materializr",
        {{"Materializr Project", "*.materializr"}},
        [this](const std::string& path) {
            if (path.empty()) return;
            ProjectHistory hist = captureProjectHistory();
            auto result = ProjectIO::save(path, *m_document, &hist);
            if (result.success) {
                m_currentProjectPath = path;
                markSaved();
                std::fprintf(stdout, "Project saved to %s\n", path.c_str());
                if (m_closeAfterSave) m_confirmedClose = true;
            } else {
                std::fprintf(stderr, "Save failed: %s\n", result.errorMessage.c_str());
            }
            m_closeAfterSave = false;
        });
}

void Application::saveProjectQuick() {
    if (m_currentProjectPath.empty()) {
        saveProject();
        return;
    }
    ProjectHistory hist = captureProjectHistory();
    auto result = ProjectIO::save(m_currentProjectPath, *m_document, &hist);
    if (result.success) {
        markSaved();
        std::fprintf(stdout, "Project saved to %s\n", m_currentProjectPath.c_str());
        if (m_closeAfterSave) m_confirmedClose = true;
    } else {
        std::fprintf(stderr, "Save failed: %s\n", result.errorMessage.c_str());
    }
    m_closeAfterSave = false;
}

ProjectHistory Application::captureProjectHistory() {
    ProjectHistory h;
    int n = m_history->currentStep() + 1; // number of applied steps
    if (n <= 0) return h;                  // nothing to persist

    // Current full body set (id -> shape): the state after the last applied step.
    std::map<int, TopoDS_Shape> cur;
    for (int id : m_document->getAllBodyIds()) cur[id] = m_document->getBody(id);

    // Walk the steps backward, reading each op's stored before-shapes. This is
    // non-destructive (unlike undo()) and never recomputes geometry.
    std::vector<ProjectHistoryStep> steps(n);
    for (int i = n - 1; i >= 0; --i) {
        const Operation* op = m_history->getStep(i);
        if (!op) continue;
        steps[i].typeId = op->typeId();
        steps[i].name = op->name();
        steps[i].description = op->description();
        steps[i].enabled = op->isEnabled();
        if (!op->isEnabled()) continue; // a disabled step changed nothing

        OperationDiff d = op->captureDiff();
        for (const auto& [id, before] : d.modifiedBefore) {
            auto it = cur.find(id);
            if (it != cur.end()) steps[i].changed.push_back({id, it->second}); // after
            cur[id] = before;                                                  // step back
        }
        for (int id : d.created) {
            auto it = cur.find(id);
            if (it != cur.end()) steps[i].changed.push_back({id, it->second}); // after
            cur.erase(id);                                                     // didn't exist before
        }
        for (const auto& [id, before] : d.deletedBefore) {
            steps[i].deleted.push_back(id); // gone after this step
            cur[id] = before;               // existed before
        }
    }

    // `cur` is now the initial state (before step 0).
    h.present = true;
    for (const auto& [id, shape] : cur) h.initialState.push_back({id, shape});
    h.steps = std::move(steps);
    return h;
}

void Application::rebuildHistoryFromProject(const ProjectHistory& hist) {
    m_history->clear();
    if (!hist.present) return;

    // Accumulate full states forward from the initial snapshot, giving each
    // reloaded step a ReplayOp that knows its complete before/after body set.
    std::map<int, TopoDS_Shape> running;
    for (const auto& [id, shape] : hist.initialState) running[id] = shape;

    auto toVec = [](const std::map<int, TopoDS_Shape>& m) {
        ReplayOp::BodyState v; v.reserve(m.size());
        for (const auto& [id, s] : m) v.push_back({id, s});
        return v;
    };

    for (const auto& st : hist.steps) {
        ReplayOp::BodyState before = toVec(running);
        for (const auto& [id, shape] : st.changed) running[id] = shape;
        for (int id : st.deleted) running.erase(id);
        ReplayOp::BodyState after = toVec(running);

        auto op = std::make_unique<ReplayOp>(st.typeId, st.name, st.description,
                                             std::move(before), std::move(after));
        op->setEnabled(st.enabled);
        m_history->pushExecuted(std::move(op));
    }
}

void Application::loadProject() {
    FileDialogs::openFile("Open Project",
        {{"Materializr Project", "*.materializr"}},
        [this](const std::string& path) {
            if (path.empty()) return;
            m_document->clear();
            m_history->clear();
            m_selection->clear();
            ProjectHistory hist;
            auto result = ProjectIO::load(path, *m_document, &hist);
            if (result.success) {
                rebuildHistoryFromProject(hist);
                m_currentProjectPath = path;
                markSaved();
                m_meshesDirty = true;
                std::fprintf(stdout, "Loaded %d bodies, %d history steps from %s\n",
                             result.bodiesLoaded, static_cast<int>(hist.steps.size()),
                             path.c_str());
            } else {
                std::fprintf(stderr, "Load failed: %s\n", result.errorMessage.c_str());
            }
        });
}

bool Application::isDirty() const {
    return (m_history && m_history->currentStep() != m_savedAtHistoryStep)
        || m_unsavedNonHistoryChanges;
}

void Application::markDirty() {
    m_unsavedNonHistoryChanges = true;
}

void Application::markSaved() {
    m_savedAtHistoryStep = m_history ? m_history->currentStep() : -1;
    m_unsavedNonHistoryChanges = false;
}

void Application::requestClose() {
    if (m_confirmedClose) return;
    if (!isDirty()) { m_confirmedClose = true; return; }
    m_showSavePrompt = true;
    m_closeAfterSave = false;
    glfwSetWindowShouldClose(m_window->handle(), GLFW_FALSE);
}

void Application::renderSavePrompt() {
    if (m_showSavePrompt) {
        ImGui::OpenPopup("Unsaved Changes");
        m_showSavePrompt = false; // OpenPopup latches; only call once per request
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("You have unsaved changes. Save before exiting?");
        ImGui::Separator();
        if (ImGui::Button("Save", ImVec2(100, 0))) {
            m_closeAfterSave = true;
            saveProjectQuick();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(100, 0))) {
            m_confirmedClose = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            m_closeAfterSave = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Application::importStepFile() {
    FileDialogs::openFile("Import STEP",
        {{"STEP Files", "*.step *.stp *.STEP *.STP"}},
        [this](const std::string& path) {
            if (path.empty()) return;
            auto result = StepIO::import(path, *m_document);
            if (result.success) {
                m_meshesDirty = true;
                markDirty();
                std::fprintf(stdout, "Imported %d bodies from %s\n", result.bodiesImported, path.c_str());
            } else {
                std::fprintf(stderr, "Import failed: %s\n", result.errorMessage.c_str());
            }
        });
}

void Application::exportStepFile() {
    FileDialogs::saveFile("Export STEP", "export.step",
        {{"STEP Files", "*.step *.stp"}},
        [this](const std::string& path) {
            if (path.empty()) return;
            auto result = StepIO::exportFile(path, *m_document);
            if (result.success) std::fprintf(stdout, "Exported to %s\n", path.c_str());
            else std::fprintf(stderr, "Export failed: %s\n", result.errorMessage.c_str());
        });
}

void Application::exportStlFile() {
    FileDialogs::saveFile("Export STL", "export.stl",
        {{"STL Files", "*.stl"}},
        [this](const std::string& path) {
            if (path.empty()) return;
            auto result = StlExport::exportFile(path, *m_document);
            if (result.success) std::fprintf(stdout, "Exported %d triangles to %s\n", result.triangleCount, path.c_str());
            else std::fprintf(stderr, "STL export failed: %s\n", result.errorMessage.c_str());
        });
}

void Application::enterSketchMode() {
    // If a planar face is selected, route through enterSketchOnFace for consistency
    if (m_selection && m_selection->hasSelectedFaces()) {
        const auto& sel = m_selection->getSelection();
        for (const auto& entry : sel) {
            if (entry.type == SelectionType::Face && !entry.shape.IsNull()) {
                enterSketchOnFace(TopoDS::Face(entry.shape));
                return;
            }
        }
    }

    m_activeSketch = std::make_shared<Sketch>();
    m_sketchSolver = std::make_unique<SketchSolver>();
    m_activeSketchId = -1;

    m_sketchTool->setSketch(m_activeSketch.get());
    m_sketchTool->setSolver(m_sketchSolver.get());
    m_sketchTool->setMode(SketchToolMode::Line);
    m_inSketchMode = true;
    m_toolbar->setSketchMode(true);
    alignCameraToActiveSketch();
}

void Application::enterSketchOnPlane(const gp_Pln& plane) {
    // Start a fresh, freestanding sketch on a world base plane (no source face),
    // so the user can model from scratch with no existing body. Drawing tools,
    // the adjustable grid and the ortho camera all come from the shared sketch
    // path via alignCameraToActiveSketch() / renderSketchTools().
    m_activeSketch = std::make_shared<Sketch>();
    m_sketchSolver = std::make_unique<SketchSolver>();
    m_activeSketchId = -1;

    m_activeSketch->setPlane(plane);

    m_sketchTool->setSketch(m_activeSketch.get());
    m_sketchTool->setSolver(m_sketchSolver.get());
    m_sketchTool->setMode(SketchToolMode::Line);
    m_inSketchMode = true;
    m_toolbar->setSketchMode(true);
    alignCameraToActiveSketch();
}

void Application::enterSketchOnFace(const TopoDS_Face& face) {
    m_activeSketch = std::make_shared<Sketch>();
    m_sketchSolver = std::make_unique<SketchSolver>();
    m_activeSketchId = -1;

    Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
    if (!surf.IsNull() && surf->IsKind(STANDARD_TYPE(Geom_Plane))) {
        Handle(Geom_Plane) geomPlane = Handle(Geom_Plane)::DownCast(surf);
        m_activeSketch->setPlane(geomPlane->Pln());
        m_activeSketch->setSourceFace(face);
    } else {
        // Fallback to default XY plane if face is non-planar
        m_activeSketch->setPlane(gp_Pln(gp_Pnt(0,0,0), gp_Dir(0,0,1)));
        std::fprintf(stderr, "Selected face is not planar; using XY plane instead\n");
    }

    m_sketchTool->setSketch(m_activeSketch.get());
    m_sketchTool->setSolver(m_sketchSolver.get());
    m_sketchTool->setMode(SketchToolMode::Line);
    m_inSketchMode = true;
    m_toolbar->setSketchMode(true);
    alignCameraToActiveSketch();
}

void Application::recordSketchMutation(const std::function<void()>& mutator) {
    if (!m_activeSketch) { mutator(); return; }
    // Signature includes counts AND element IDs so that swaps (trim line→line,
    // trim circle→arc) register as a mutation even though counts may be equal.
    auto signature = [](const Sketch& s) {
        size_t h = 1469598103934665603ull;
        auto mix = [&](size_t v) { h = (h ^ v) * 1099511628211ull; };
        mix(s.getLines().size());
        for (const auto& l : s.getLines()) mix(static_cast<size_t>(l.id));
        mix(s.getCircles().size());
        for (const auto& c : s.getCircles()) mix(static_cast<size_t>(c.id));
        mix(s.getArcs().size());
        for (const auto& a : s.getArcs()) mix(static_cast<size_t>(a.id));
        mix(s.getSplines().size());
        for (const auto& sp : s.getSplines()) mix(static_cast<size_t>(sp.id));
        mix(s.getPolygons().size());
        for (const auto& p : s.getPolygons()) mix(static_cast<size_t>(p.id));
        return h;
    };
    size_t beforeSig = signature(*m_activeSketch);
    auto before = std::make_shared<Sketch>(*m_activeSketch);
    mutator();
    size_t afterSig = signature(*m_activeSketch);
    if (afterSig == beforeSig) return; // nothing structural changed → no history step
    auto after = std::make_shared<Sketch>(*m_activeSketch);
    auto op = std::make_unique<SketchEditOp>(m_activeSketch, std::move(before), std::move(after));
    m_history->pushExecuted(std::move(op));
}

void Application::editSketch(int sketchId) {
    auto sketch = m_document->getSketch(sketchId);
    if (!sketch) return;

    m_activeSketch = sketch; // shared ownership - edits go straight to the stored sketch
    m_sketchSolver = std::make_unique<SketchSolver>();
    m_activeSketchId = sketchId;

    m_sketchTool->setSketch(m_activeSketch.get());
    m_sketchTool->setSolver(m_sketchSolver.get());
    // When re-entering an existing sketch the user is much more likely to want
    // to tweak it than draw fresh geometry; start in Select/Move mode so they
    // can immediately click & drag points/lines.
    m_sketchTool->setMode(SketchToolMode::Select);
    m_inSketchMode = true;
    m_toolbar->setSketchMode(true);
    m_selection->clear();
    alignCameraToActiveSketch();
}

void Application::extrudeSketchById(int sketchId) {
    auto sketch = m_document->getSketch(sketchId);
    if (!sketch) return;
    TopoDS_Face profile = buildSketchProfileFace(*sketch);
    if (profile.IsNull()) {
        std::fprintf(stderr, "Sketch has no closed profile to extrude\n");
        return;
    }
    beginInteractiveExtrude(profile);
}

void Application::alignCameraToActiveSketch() {
    if (!m_activeSketch || !m_viewport) return;

    const gp_Pln& pln = m_activeSketch->getPlane();
    const gp_Ax3& ax = pln.Position();
    gp_Pnt o = ax.Location();
    gp_Dir n = ax.Direction();
    gp_Dir y = ax.YDirection();

    glm::vec3 planeOrigin(static_cast<float>(o.X()), static_cast<float>(o.Y()), static_cast<float>(o.Z()));
    glm::vec3 normal(static_cast<float>(n.X()), static_cast<float>(n.Y()), static_cast<float>(n.Z()));
    glm::vec3 up(static_cast<float>(y.X()), static_cast<float>(y.Y()), static_cast<float>(y.Z()));

    // Pick an ortho size that frames the source face if present, otherwise default
    // to something readable. Use the current sketch grid step's scale as a hint.
    float orthoSize = std::max(20.0f, m_sketchGridStep * 40.0f);
    if (!m_activeSketch->getSourceFace().IsNull()) {
        try {
            Bnd_Box bb;
            BRepBndLib::Add(m_activeSketch->getSourceFace(), bb);
            if (!bb.IsVoid()) {
                double xmin, ymin, zmin, xmax, ymax, zmax;
                bb.Get(xmin, ymin, zmin, xmax, ymax, zmax);
                float dx = static_cast<float>(xmax - xmin);
                float dy = static_cast<float>(ymax - ymin);
                float dz = static_cast<float>(zmax - zmin);
                float diag = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
                if (diag > 1e-3f) orthoSize = diag * 1.2f;
            }
        } catch (...) {}
    }

    Camera& cam = m_viewport->getCamera();
    float standoff = std::max(orthoSize * 4.0f, 10.0f);
    cam.setTarget(planeOrigin);
    cam.setPosition(planeOrigin + normal * standoff);
    cam.setUp(up);
    cam.setOrthoSize(orthoSize);
    cam.setOrthographic(true);
}

TopoDS_Face Application::buildSketchProfileFace(const Sketch& sketch) const {
    auto wires = sketch.buildWires();
    if (wires.empty()) return TopoDS_Face();

    // Pick the wire with the largest 3D bbox diagonal as the outer; the rest are holes.
    // This produces a single face with holes so a prism over a "ring" sketch becomes a tube.
    int outerIdx = 0;
    double bestExtent = -1.0;
    std::vector<double> extents(wires.size(), 0.0);
    for (size_t i = 0; i < wires.size(); ++i) {
        Bnd_Box bb;
        BRepBndLib::Add(wires[i], bb);
        if (bb.IsVoid()) continue;
        double xmin, ymin, zmin, xmax, ymax, zmax;
        bb.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        double dx = xmax - xmin, dy = ymax - ymin, dz = zmax - zmin;
        double diag = dx*dx + dy*dy + dz*dz;
        extents[i] = diag;
        if (diag > bestExtent) {
            bestExtent = diag;
            outerIdx = static_cast<int>(i);
        }
    }

    BRepBuilderAPI_MakeFace faceMaker(sketch.getPlane(), wires[outerIdx]);
    if (!faceMaker.IsDone()) return TopoDS_Face();

    for (size_t i = 0; i < wires.size(); ++i) {
        if (static_cast<int>(i) == outerIdx) continue;
        // Reverse inner wire orientation so it acts as a hole
        TopoDS_Wire inner = TopoDS::Wire(wires[i].Reversed());
        faceMaker.Add(inner);
    }
    faceMaker.Build();
    if (!faceMaker.IsDone()) return TopoDS_Face();
    return faceMaker.Face();
}

glm::vec2 Application::screenToSketch(float sx, float sy, float vpW, float vpH) {
    Camera& cam = m_viewport->getCamera();
    glm::mat4 view = cam.getViewMatrix();
    glm::mat4 proj = cam.getProjectionMatrix();
    glm::mat4 invVP = glm::inverse(proj * view);

    // Normalize to [-1,1]
    float nx = (sx / vpW) * 2.0f - 1.0f;
    float ny = 1.0f - (sy / vpH) * 2.0f;

    // Unproject near and far points
    glm::vec4 nearPt = invVP * glm::vec4(nx, ny, -1.0f, 1.0f);
    glm::vec4 farPt = invVP * glm::vec4(nx, ny, 1.0f, 1.0f);
    nearPt /= nearPt.w;
    farPt /= farPt.w;

    glm::vec3 rayOrigin(nearPt);
    glm::vec3 rayDir = glm::normalize(glm::vec3(farPt) - glm::vec3(nearPt));

    // Intersect ray with sketch plane
    const gp_Pln& pln = m_activeSketch->getPlane();
    const gp_Ax3& ax = pln.Position();
    glm::vec3 planeOrigin(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
    glm::vec3 planeNormal(ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z());
    glm::vec3 planeX(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
    glm::vec3 planeY(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());

    float denom = glm::dot(rayDir, planeNormal);
    if (std::abs(denom) < 1e-8f) return glm::vec2(0);

    float t = glm::dot(planeOrigin - rayOrigin, planeNormal) / denom;
    glm::vec3 hitPoint = rayOrigin + rayDir * t;

    // Project hit point onto sketch plane's 2D coordinate system
    glm::vec3 local = hitPoint - planeOrigin;
    return glm::vec2(glm::dot(local, planeX), glm::dot(local, planeY));
}

void Application::beginInteractiveEdgeOp(EdgeOpType type) {
    const auto& sel = m_selection->getSelection();
    int bodyId = -1;
    std::vector<TopoDS_Shape> edges;
    for (const auto& entry : sel) {
        if (entry.type == SelectionType::Edge && !entry.shape.IsNull()) {
            if (bodyId < 0) bodyId = entry.bodyId;
            if (entry.bodyId == bodyId) edges.push_back(entry.shape);
        }
    }
    if (bodyId < 0 || edges.empty()) return;

    m_edgeOpType = type;
    m_edgeOpActive = true;
    m_edgeOpBodyId = bodyId;
    m_edgeOpEdges = edges;
    m_edgeOpValue = 0.0f; // start at no change; drag the arrow outward or type a value
    std::snprintf(m_edgeOpInputBuf, sizeof(m_edgeOpInputBuf), "%.1f", m_edgeOpValue);
    m_edgeOpInputFocus = true;

    // Save original shape for live preview
    try {
        m_edgeOpPreviousShape = m_document->getBody(bodyId);
    } catch (...) { m_edgeOpActive = false; return; }

    // First edge's midpoint + tangent, for the drag handle / measurement.
    m_edgeOpHasHandle = false;
    try {
        BRepAdaptor_Curve curve(TopoDS::Edge(edges.front()));
        double t = (curve.FirstParameter() + curve.LastParameter()) * 0.5;
        gp_Pnt p; gp_Vec tan;
        curve.D1(t, p, tan);
        m_edgeOpMid = glm::vec3(p.X(), p.Y(), p.Z());
        if (tan.Magnitude() > 1e-9) {
            m_edgeOpDir = glm::normalize(glm::vec3(tan.X(), tan.Y(), tan.Z()));
            // Outward handle direction: from the body centre to the edge, made
            // perpendicular to the edge, so the arrow faces straight out of the edge.
            Bnd_Box bb; BRepBndLib::Add(m_edgeOpPreviousShape, bb);
            if (!bb.IsVoid()) {
                double x1,y1,z1,x2,y2,z2; bb.Get(x1,y1,z1,x2,y2,z2);
                glm::vec3 c((x1+x2)*0.5f, (y1+y2)*0.5f, (z1+z2)*0.5f);
                glm::vec3 out = m_edgeOpMid - c;
                out -= glm::dot(out, m_edgeOpDir) * m_edgeOpDir; // perpendicular to edge
                if (glm::length(out) > 1e-5f) m_edgeOpOutDir = glm::normalize(out);
            }
            m_edgeOpHasHandle = true;
        }
    } catch (...) {}

    updateInteractiveEdgeOp();
}

void Application::updateInteractiveEdgeOp() {
    if (!m_edgeOpActive || m_edgeOpBodyId < 0) return;

    // Restore original first, so dragging back to ~0 shows no fillet/chamfer.
    m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
    m_meshesDirty = true;
    if (m_edgeOpValue < 0.01f) return;

    try {
        if (m_edgeOpType == EdgeOpType::Fillet) {
            auto op = std::make_unique<FilletOp>();
            op->setBody(m_edgeOpBodyId);
            std::vector<TopoDS_Edge> typedEdges;
            for (const auto& e : m_edgeOpEdges) typedEdges.push_back(TopoDS::Edge(e));
            op->setEdges(typedEdges);
            op->setRadius(static_cast<double>(m_edgeOpValue));
            if (op->execute(*m_document)) {
                m_meshesDirty = true;
            } else {
                // Failed — restore original
                m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
            }
        } else {
            auto op = std::make_unique<ChamferOp>();
            op->setBody(m_edgeOpBodyId);
            std::vector<TopoDS_Edge> typedEdges;
            for (const auto& e : m_edgeOpEdges) typedEdges.push_back(TopoDS::Edge(e));
            op->setEdges(typedEdges);
            op->setDistance(static_cast<double>(m_edgeOpValue));
            if (op->execute(*m_document)) {
                m_meshesDirty = true;
            } else {
                m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
            }
        }
    } catch (...) {
        m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
    }
}

void Application::commitInteractiveEdgeOp() {
    // Restore original, then do it properly through history
    m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);

    // Confirming with no size set is a no-op — just cancel out.
    if (m_edgeOpValue < 0.01f) {
        m_edgeOpActive = false;
        m_edgeOpEdges.clear();
        m_edgeOpPreviousShape.Nullify();
        m_edgeOpType = EdgeOpType::None;
        m_meshesDirty = true;
        return;
    }

    if (m_edgeOpType == EdgeOpType::Fillet) {
        auto op = std::make_unique<FilletOp>();
        op->setBody(m_edgeOpBodyId);
        std::vector<TopoDS_Edge> typedEdges;
        for (const auto& e : m_edgeOpEdges) typedEdges.push_back(TopoDS::Edge(e));
        op->setEdges(typedEdges);
        op->setRadius(static_cast<double>(m_edgeOpValue));
        m_history->pushOperation(std::move(op), *m_document);
    } else {
        auto op = std::make_unique<ChamferOp>();
        op->setBody(m_edgeOpBodyId);
        std::vector<TopoDS_Edge> typedEdges;
        for (const auto& e : m_edgeOpEdges) typedEdges.push_back(TopoDS::Edge(e));
        op->setEdges(typedEdges);
        op->setDistance(static_cast<double>(m_edgeOpValue));
        m_history->pushOperation(std::move(op), *m_document);
    }

    m_edgeOpActive = false;
    m_edgeOpEdges.clear();
    m_edgeOpPreviousShape.Nullify();
    m_selection->clear();
    m_meshesDirty = true;
    std::fprintf(stdout, "%s %.1f mm committed\n",
                 m_edgeOpType == EdgeOpType::Fillet ? "Fillet" : "Chamfer", m_edgeOpValue);
    m_edgeOpType = EdgeOpType::None;
}

void Application::cancelInteractiveEdgeOp() {
    if (m_edgeOpBodyId >= 0 && !m_edgeOpPreviousShape.IsNull()) {
        m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
    }
    m_edgeOpActive = false;
    m_edgeOpEdges.clear();
    m_edgeOpPreviousShape.Nullify();
    m_edgeOpType = EdgeOpType::None;
    m_meshesDirty = true;
}

void Application::beginInteractiveExtrude(const TopoDS_Shape& profile) {
    m_extrudeProfile = profile;
    m_extruding = true;
    m_extrudeDistance = 5.0f;
    std::snprintf(m_extrudeInputBuf, sizeof(m_extrudeInputBuf), "%.1f", m_extrudeDistance);
    m_extrudeInputFocus = true;

    // Compute face normal and center
    if (profile.ShapeType() == TopAbs_FACE) {
        BRepGProp_Face prop(TopoDS::Face(profile));
        gp_Pnt center;
        gp_Vec norm;
        double u1, u2, v1, v2;
        prop.Bounds(u1, u2, v1, v2);
        prop.Normal((u1 + u2) * 0.5, (v1 + v2) * 0.5, center, norm);
        if (norm.Magnitude() > 1e-10) {
            m_extrudeNormal = glm::vec3(norm.X(), norm.Y(), norm.Z());
            m_extrudeNormal = glm::normalize(m_extrudeNormal);
        }
        m_extrudeOrigin = glm::vec3(center.X(), center.Y(), center.Z());
    }

    // Create initial preview body
    auto op = std::make_unique<ExtrudeOp>();
    op->setProfile(profile);
    op->setDistance(m_extrudeDistance);
    op->setMode(ExtrudeMode::NewBody);
    if (m_history->pushOperation(std::move(op), *m_document)) {
        auto ids = m_document->getAllBodyIds();
        m_extrudePreviewBodyId = ids.back();
        m_meshesDirty = true;
    }
}

void Application::updateInteractiveExtrude() {
    if (!m_extruding || m_extrudePreviewBodyId < 0) return;
    if (!std::isfinite(m_extrudeDistance)) { m_extrudeDistance = 0.0f; return; }

    // Remove old preview and create new one at current distance
    m_document->removeBody(m_extrudePreviewBodyId);
    m_history->undo(*m_document); // undo the last extrude

    auto op = std::make_unique<ExtrudeOp>();
    op->setProfile(m_extrudeProfile);
    op->setDistance(static_cast<double>(m_extrudeDistance));
    op->setMode(ExtrudeMode::NewBody);
    if (m_history->pushOperation(std::move(op), *m_document)) {
        auto ids = m_document->getAllBodyIds();
        m_extrudePreviewBodyId = ids.back();
        m_meshesDirty = true;
    }
}

void Application::commitInteractiveExtrude() {
    // The current extrude is already in history — just finalize
    m_extruding = false;
    m_extrudeProfile.Nullify();
    m_extrudePreviewBodyId = -1;
    m_meshesDirty = true;
    std::fprintf(stdout, "Extruded %.1f mm\n", m_extrudeDistance);
}

void Application::cancelInteractiveExtrude() {
    if (m_extrudePreviewBodyId >= 0) {
        m_document->removeBody(m_extrudePreviewBodyId);
        m_history->undo(*m_document);
    }
    m_extruding = false;
    m_extrudeProfile.Nullify();
    m_extrudePreviewBodyId = -1;
    m_meshesDirty = true;
}

Application::SketchRegionHit Application::pickSketchRegion(float screenX, float screenY,
                                                           float vpW, float vpH) const {
    SketchRegionHit hit;
    if (!m_document || !m_viewport) return hit;

    const Camera& cam = m_viewport->getCamera();
    glm::mat4 view = cam.getViewMatrix();
    glm::mat4 proj = cam.getProjectionMatrix();
    glm::mat4 invVP = glm::inverse(proj * view);

    // Build a world-space ray through a given pixel.
    auto rayAt = [&](float sx, float sy, glm::vec3& origin, glm::vec3& dir) {
        float ndcx = (sx / vpW) * 2.0f - 1.0f;
        float ndcy = 1.0f - (sy / vpH) * 2.0f;
        glm::vec4 n = invVP * glm::vec4(ndcx, ndcy, -1.0f, 1.0f);
        glm::vec4 f = invVP * glm::vec4(ndcx, ndcy, 1.0f, 1.0f);
        n /= n.w; f /= f.w;
        origin = glm::vec3(n);
        dir = glm::normalize(glm::vec3(f) - glm::vec3(n));
    };

    glm::vec3 rayOrigin, rayDir;
    rayAt(screenX, screenY, rayOrigin, rayDir);

    float bestT = std::numeric_limits<float>::infinity();

    auto testSketch = [&](int sketchId, const Sketch& sketch) {
        const gp_Pln& pln = sketch.getPlane();
        const gp_Ax3& ax = pln.Position();
        glm::vec3 planeOrigin(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
        glm::vec3 planeNormal(ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z());
        glm::vec3 planeX(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
        glm::vec3 planeY(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());

        auto projectToPlane = [&](glm::vec3 o, glm::vec3 d, float& tOut, glm::vec2& p2dOut) -> bool {
            float denom = glm::dot(d, planeNormal);
            if (std::abs(denom) < 1e-8f) return false;
            float t = glm::dot(planeOrigin - o, planeNormal) / denom;
            if (t <= 0.0f) return false;
            glm::vec3 local = (o + d * t) - planeOrigin;
            tOut = t;
            p2dOut = glm::vec2(glm::dot(local, planeX), glm::dot(local, planeY));
            return true;
        };

        float t;
        glm::vec2 p2d;
        if (!projectToPlane(rayOrigin, rayDir, t, p2d)) return;
        if (t >= bestT) return;

        // Screen-space pick tolerance: how far ~6px maps to on this plane, so the
        // boundary catch area is a consistent, comfortable width at any zoom.
        float tol = 0.0f;
        glm::vec3 o2, d2; float t2; glm::vec2 p2d2;
        rayAt(screenX + 6.0f, screenY, o2, d2);
        if (projectToPlane(o2, d2, t2, p2d2)) tol = glm::length(p2d2 - p2d);

        auto regions = sketch.buildRegions();
        for (size_t i = 0; i < regions.size(); ++i) {
            if (sketch.isPointInOrNearRegion(regions[i], p2d, tol)) {
                bestT = t;
                hit.sketchId = sketchId;
                hit.regionIndex = static_cast<int>(i);
                hit.worldPoint = rayOrigin + rayDir * t;
                break; // first match per sketch is fine; nesting handled by the test
            }
        }
    };

    // Test the active sketch first (most relevant when in sketch mode)
    if (m_activeSketch) testSketch(m_activeSketchId, *m_activeSketch);
    // Then all stored sketches
    for (int sid : m_document->getAllSketchIds()) {
        if (!m_document->isSketchVisible(sid)) continue;
        if (sid == m_activeSketchId) continue;
        auto sk = m_document->getSketch(sid);
        if (sk) testSketch(sid, *sk);
    }

    return hit;
}

void Application::beginPushPull() {
    m_pushPullTargets.clear();
    m_pushPullPreviewBodyIds.clear();
    m_pushPullPreviousBodies.clear();
    m_pushPullPreviewPushed = false;

    // Gather all selected SketchRegion entries AND body face selections.
    for (const auto& e : m_selection->getSelection()) {
        if (e.type == SelectionType::SketchRegion) {
            auto sketch = m_document->getSketch(e.sketchId);
            if (!sketch) continue;
            auto regions = sketch->buildRegions();
            if (e.subShapeIndex < 0 || e.subShapeIndex >= static_cast<int>(regions.size())) continue;
            PushPullTarget t;
            t.sketchId = e.sketchId;
            t.regionIndex = e.subShapeIndex;
            t.sourceBodyId = sketch->getSourceBody();
            t.profile = regions[e.subShapeIndex].face;
            if (t.profile.IsNull()) continue;
            m_pushPullTargets.push_back(t);
        } else if (e.type == SelectionType::Face && !e.shape.IsNull()) {
            // Push/Pull on a body face: face is the profile, the owning body is the source.
            // Positive distance extrudes outward (Fuse), negative cuts inward (Cut).
            PushPullTarget t;
            t.sketchId = -1;
            t.regionIndex = -1;
            t.sourceBodyId = e.bodyId;
            t.profile = TopoDS::Face(e.shape);
            if (t.profile.IsNull()) continue;
            m_pushPullTargets.push_back(t);
        }
    }

    if (m_pushPullTargets.empty()) {
        std::fprintf(stderr, "Push/Pull: select a sketch region or a body face first\n");
        return;
    }

    // Arrow along the first target's outward normal, at its centre — the user
    // drags this to set the distance (and a measurement reads off it).
    m_pushPullHasArrow = false;
    try {
        const TopoDS_Face& f = m_pushPullTargets.front().profile;
        if (!f.IsNull()) {
            BRepGProp_Face prop(f);
            double u1, u2, v1, v2;
            prop.Bounds(u1, u2, v1, v2);
            gp_Pnt c; gp_Vec n;
            prop.Normal((u1 + u2) * 0.5, (v1 + v2) * 0.5, c, n);
            if (n.Magnitude() > 1e-10) {
                m_pushPullNormal = glm::normalize(glm::vec3(n.X(), n.Y(), n.Z()));
                m_pushPullOrigin = glm::vec3(c.X(), c.Y(), c.Z());
                m_pushPullHasArrow = true;
            }
        }
    } catch (...) {}

    m_pushPullActive = true;
    m_pushPullDistance = 0.0f; // start at no change; drag the arrow or type a value
    std::snprintf(m_pushPullInputBuf, sizeof(m_pushPullInputBuf), "%.1f", m_pushPullDistance);
    m_pushPullInputFocus = true;

    updatePushPull();
}

void Application::updatePushPull() {
    if (!m_pushPullActive) return;
    if (!std::isfinite(m_pushPullDistance)) { m_pushPullDistance = 0.0f; return; }

    // Only undo OUR previous preview — not any other pushpull that may already be
    // committed at the top of the history.
    if (m_pushPullPreviewPushed && m_history->canUndo()) {
        m_history->undo(*m_document);
        m_pushPullPreviewPushed = false;
    }

    auto op = std::make_unique<PushPullOp>();
    std::vector<PushPullOp::Target> targets;
    for (const auto& t : m_pushPullTargets) {
        PushPullOp::Target ot;
        ot.profile = t.profile;
        ot.sourceBodyId = t.sourceBodyId;
        targets.push_back(ot);
    }
    op->setTargets(std::move(targets));
    op->setDistance(static_cast<double>(m_pushPullDistance));
    if (m_history->pushOperation(std::move(op), *m_document)) {
        m_pushPullPreviewPushed = true;
    }
    m_meshesDirty = true;
}

void Application::commitPushPull() {
    // The last preview push IS the final state — just clean up
    m_pushPullActive = false;
    m_pushPullPreviewPushed = false;
    m_pushPullTargets.clear();
    m_meshesDirty = true;
    m_selection->clear();
    std::fprintf(stdout, "Push/Pull committed at %.2f mm\n", m_pushPullDistance);
}

void Application::cancelPushPull() {
    if (!m_pushPullActive) return;
    if (m_pushPullPreviewPushed && m_history->canUndo()) {
        m_history->undo(*m_document);
        m_pushPullPreviewPushed = false;
    }
    m_pushPullActive = false;
    m_pushPullTargets.clear();
    m_meshesDirty = true;
}

void Application::exitSketchMode() {
    m_inSketchMode = false;
    m_toolbar->setSketchMode(false);
    m_sketchTool->setMode(SketchToolMode::None);
    m_sketchTool->setSketch(nullptr);
    m_sketchTool->setSolver(nullptr);

    // Persist the sketch into the document if it has any geometry. New sketches get added;
    // edits to existing sketches are already reflected via the shared_ptr.
    if (m_activeSketch && m_activeSketch->elementCount() > 0) {
        if (m_activeSketchId < 0) {
            m_activeSketchId = m_document->addSketch(m_activeSketch);
            markDirty();
            std::fprintf(stdout, "Sketch saved (id %d)\n", m_activeSketchId);
        }
    } else if (m_activeSketchId < 0) {
        std::fprintf(stdout, "Sketch discarded (empty)\n");
    }

    m_activeSketch.reset();
    m_sketchSolver.reset();
    m_activeSketchId = -1;
    m_meshesDirty = true; // refresh sketch rendering set
}

void Application::run() {
    while (true) {
        m_window->pollEvents();

        // The save-prompt's Don't Save / post-save-success path sets this flag
        // directly. Check it every frame so we exit without requiring the user
        // to click the X a second time.
        if (m_confirmedClose) break;

        // Intercept window-close requests: if there are unsaved changes, show
        // the prompt and cancel the close until the user picks Save/Don't Save.
        if (m_window->shouldClose()) {
            requestClose();
            if (m_confirmedClose) break;
        }

        beginFrame();
        renderDockspace();
        renderMenuBar();

        if (m_renderersReady) {
            renderViewport();

            m_toolbar->setGridStep(m_sketchGridStep);
            m_toolbar->setSnapToGrid(m_snapToGrid);
            m_toolbar->setCameraOrtho(m_viewport->getCamera().isOrthographic());
            ToolAction action = m_toolbar->render();
            m_sketchGridStep = m_toolbar->getGridStep();
            m_snapToGrid = m_toolbar->getSnapToGrid();
            if (action != ToolAction::None) {
                handleToolAction(static_cast<int>(action));
            }

            // Active interactive tool (plugin system)
            if (auto* tool = PluginRegistry::instance().activeTool()) {
                if (!tool->update(*m_pluginContext)) {
                    // The tool finished (it ran its own commit()/cancel()); just
                    // clear it. Do NOT call deactivateTool() here — that cancels,
                    // which would undo a just-committed operation (e.g. push/pull).
                    PluginRegistry::instance().finishActiveTool();
                } else {
                    tool->renderOverlay(*m_pluginContext);
                }
            }

            renderInteractionsPanel();
            renderSettings();
            renderMirrorPopup();

            // Help system: dockable user guide, modal About, modal "Check for
            // Updates" popup that fires a one-shot HTTPS GET to GitHub on open.
            m_helpPanel->render();
            m_shortcutsPanel->render();
            m_aboutDialog->render();
            renderUpdatePopup();

            if (m_historyPanel->render()) {
                m_meshesDirty = true;
            }

            if (m_itemsPanel->render()) {
                m_hoveredBodyId = -1;
                m_meshesDirty = true;
            }
            if (m_propertiesPanel->render()) {
                m_meshesDirty = true;
            }
            m_statusBar->setSketchMode(m_inSketchMode);
            m_statusBar->render();
            m_commandPalette->render();
            FileDialogs::render();
            renderSavePrompt();

            handleShortcuts();
        }

        // Autosave: only for projects already on disk, and only when there are
        // pending changes. The timer counts from the last save, so a quiet model
        // never gets written and the interval is measured from when edits begin.
        {
            double now = ImGui::GetTime();
            if (m_autosaveEnabled && !m_currentProjectPath.empty()) {
                if (isDirty()) {
                    if (now - m_lastAutosaveTime >= m_autosaveIntervalSec) {
                        saveProjectQuick();
                        m_lastAutosaveTime = now;
                    }
                } else {
                    m_lastAutosaveTime = now;
                }
            } else {
                m_lastAutosaveTime = now;
            }
        }

        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        endFrame();

        m_window->swapBuffers();
    }

    // Persist preferences on a clean exit (in addition to saving on each change).
    saveAppSettings();
}

} // namespace materializr

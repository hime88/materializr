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

static const char* mouseButtonName(int b) {
    switch (b) {
        case 0: return "Left";
        case 1: return "Right";
        case 2: return "Middle";
        default: return "?";
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

// Map a screen drag onto a world direction: project the mouse delta onto the
// screen-space image of `normal` at `origin`. Falls back to vertical drag when
// that direction is nearly perpendicular to the screen (face head-on) — otherwise
// normalizing a near-zero vector yields NaN, which propagates into a NaN prism
// and crashes the boolean kernel.
static float projectDragOntoNormal(const glm::vec3& origin, const glm::vec3& normal,
                                   const glm::vec2& mouseDelta, const glm::mat4& vp) {
    glm::vec4 o = vp * glm::vec4(origin, 1.0f);
    glm::vec4 t = vp * glm::vec4(origin + normal, 1.0f);
    if (o.w <= 1e-5f || t.w <= 1e-5f) return -mouseDelta.y * 0.05f;
    glm::vec2 os(o.x / o.w, o.y / o.w), ts(t.x / t.w, t.y / t.w);
    glm::vec2 sd(ts.x - os.x, -(ts.y - os.y)); // screen +y is down
    float len = glm::length(sd);
    if (len < 1e-4f) return -mouseDelta.y * 0.05f; // head-on: use vertical drag
    return glm::dot(mouseDelta, sd / len) * 0.05f;
}

void Application::renderViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");

    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    int w = static_cast<int>(contentSize.x);
    int h = static_cast<int>(contentSize.y);

    if (w > 0 && h > 0) {
        m_viewport->resize(w, h);

        if (m_meshesDirty) {
            rebuildMeshes();
            m_meshesDirty = false;
        }

        m_viewport->bind();

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        m_backgroundRenderer->render();
        glEnable(GL_DEPTH_TEST);

        Camera& cam = m_viewport->getCamera();
        glm::mat4 view = cam.getViewMatrix();
        glm::mat4 proj = cam.getProjectionMatrix();

        // Grid: in any sketch mode (whether on a face or from scratch), lay the
        // infinite world grid on the sketch plane so it shows face-on and any
        // nearby face can be referenced. Outside sketch mode, use the XZ ground.
        {
            Grid::Plane gp; // defaults to the XZ ground
            bool sketching = m_inSketchMode && m_activeSketch;
            if (sketching) {
                const gp_Ax3& ax = m_activeSketch->getPlane().Position();
                auto v3 = [](const gp_Dir& d){ return glm::vec3(d.X(), d.Y(), d.Z()); };
                gp.origin = glm::vec3(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
                gp.u = v3(ax.XDirection());
                gp.v = v3(ax.YDirection());
                gp.normal = v3(ax.Direction());
            }
            // Fade radius sized to the view so the grid fills it without a hard edge.
            float fadeDist = cam.isOrthographic()
                ? cam.getOrthoSize() * 8.0f
                : glm::length(cam.getPosition() - cam.getTarget()) * 8.0f;
            m_grid->render(view, proj, cam.getTarget(), std::max(fadeDist, 10.0f),
                           gp, std::max(m_sketchGridStep, 0.01f));
        }
        m_planeRenderer->render(view, proj);
        m_shapeRenderer->render(view, proj, cam.getPosition());
        m_edgeRenderer->render(view, proj);

        // Render selection highlight (face/edge/body)
        m_selectionHighlight->render(*m_selection, *m_document, view, proj);

        // Update gizmo visibility and position based on selection
        if (m_selection->hasSelectedBodies() && !m_inSketchMode && !m_extruding && !m_edgeOpActive) {
            const auto& sel = m_selection->getSelection();
            int bodyId = sel[0].bodyId;
            try {
                const TopoDS_Shape& shape = m_document->getBody(bodyId);
                Bnd_Box bbox;
                BRepBndLib::Add(shape, bbox);
                double xmin, ymin, zmin, xmax, ymax, zmax;
                bbox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
                glm::vec3 center((xmin+xmax)*0.5f, (ymin+ymax)*0.5f, (zmin+zmax)*0.5f);
                m_gizmo->setPosition(center);
                m_gizmo->setVisible(true);
            } catch (...) {
                m_gizmo->setVisible(false);
            }
        } else {
            m_gizmo->setVisible(false);
        }

        if (m_gizmo->isVisible()) {
            m_gizmo->render(view, proj);
        }

        // Render all stored sketches (visible only) plus the active sketch
        for (int sid : m_document->getAllSketchIds()) {
            if (!m_document->isSketchVisible(sid)) continue;
            if (m_inSketchMode && sid == m_activeSketchId) continue; // drawn below with tool
            auto sk = m_document->getSketch(sid);
            if (sk) {
                m_sketchRenderer->render(sk.get(), nullptr, view, proj, nullptr);
            }
        }
        if (m_inSketchMode && m_activeSketch) {
            // Keep the tool's snap step in sync with the user-chosen grid. The
            // grid itself is the infinite world grid above (now aligned to the
            // sketch plane), so face sketches no longer need a separate per-face
            // grid — drawing across to neighbouring faces just works.
            m_sketchTool->setGridStep(m_sketchGridStep);
            m_sketchRenderer->render(m_activeSketch.get(), m_sketchTool.get(), view, proj,
                                     m_sketchSolver.get());
        }

        // Highlight hovered/selected sketch regions
        auto highlightRegion = [&](int sketchId, int regionIdx, const glm::vec3& color, float w) {
            if (sketchId < 0 || regionIdx < 0) return;
            std::shared_ptr<Sketch> sk;
            if (sketchId == m_activeSketchId && m_activeSketch) sk = m_activeSketch;
            else sk = m_document->getSketch(sketchId);
            if (!sk) return;
            m_sketchRenderer->renderRegionBoundary(sk.get(), regionIdx, color, w, view, proj);
        };
        // Selected regions in solid yellow
        for (const auto& e : m_selection->getSelection()) {
            if (e.type == SelectionType::SketchRegion) {
                highlightRegion(e.sketchId, e.subShapeIndex,
                                glm::vec3(1.0f, 0.85f, 0.1f), 4.0f);
            }
        }
        // Hovered region in cyan (drawn last so it's on top)
        highlightRegion(m_hoveredSketchId, m_hoveredRegionIndex,
                        glm::vec3(0.2f, 0.9f, 1.0f), 3.0f);

        // Box-select rectangle (screen-space, drawn last so it's on top).
        if (m_boxSelect && m_boxSelect->isActive()) {
            m_boxSelect->render(contentSize.x, contentSize.y);
        }

        m_viewport->unbind();

        ImGui::Image(
            static_cast<ImTextureID>(m_viewport->getTextureID()),
            contentSize,
            ImVec2(0, 1),
            ImVec2(1, 0)
        );

        // --- Live dimension overlay: a measuring annotation (offset line with
        // arrowheads + value) shown while drawing a sketch line/circle, extruding,
        // or moving a body. Drawn in screen space over the viewport image. ---
        {
            ImVec2 imgMin = ImGui::GetItemRectMin();
            ImVec2 imgSize = ImGui::GetItemRectSize();
            glm::mat4 vpMat = proj * view;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            auto toImg = [&](glm::vec3 w, ImVec2& out) -> bool {
                glm::vec4 c = vpMat * glm::vec4(w, 1.0f);
                if (c.w <= 1e-5f) return false; // behind camera
                out = ImVec2(imgMin.x + (c.x / c.w * 0.5f + 0.5f) * imgSize.x,
                             imgMin.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * imgSize.y);
                return true;
            };
            auto drawDim = [&](glm::vec3 aW, glm::vec3 bW, const char* label) {
                ImVec2 sa, sb;
                if (!toImg(aW, sa) || !toImg(bW, sb)) return;
                ImVec2 dir(sb.x - sa.x, sb.y - sa.y);
                float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (len < 2.0f) return;
                dir.x /= len; dir.y /= len;
                ImVec2 perp(-dir.y, dir.x);
                const float off = 26.0f, ah = 7.0f;
                ImVec2 da(sa.x + perp.x * off, sa.y + perp.y * off);
                ImVec2 db(sb.x + perp.x * off, sb.y + perp.y * off);
                ImU32 col = IM_COL32(235, 235, 240, 255);
                ImU32 ext = IM_COL32(170, 170, 180, 150);
                dl->AddLine(sa, da, ext, 1.0f);                 // extension lines
                dl->AddLine(sb, db, ext, 1.0f);
                dl->AddLine(da, db, col, 1.5f);                 // dimension line
                auto arrow = [&](ImVec2 tip, ImVec2 along) {
                    ImVec2 base(tip.x - along.x * ah, tip.y - along.y * ah);
                    dl->AddTriangleFilled(tip,
                        ImVec2(base.x + perp.x * ah * 0.5f, base.y + perp.y * ah * 0.5f),
                        ImVec2(base.x - perp.x * ah * 0.5f, base.y - perp.y * ah * 0.5f), col);
                };
                arrow(da, ImVec2(-dir.x, -dir.y));
                arrow(db, dir);
                ImVec2 ts = ImGui::CalcTextSize(label);
                ImVec2 mid((da.x + db.x) * 0.5f + perp.x * 12.0f,
                           (da.y + db.y) * 0.5f + perp.y * 12.0f);
                ImVec2 tp(mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f);
                dl->AddRectFilled(ImVec2(tp.x - 3, tp.y - 2), ImVec2(tp.x + ts.x + 3, tp.y + ts.y + 2),
                                  IM_COL32(20, 20, 28, 205), 3.0f);
                dl->AddText(tp, col, label);
            };

            char dbuf[40];
            if (m_extruding) {
                std::snprintf(dbuf, sizeof(dbuf), "%.1f mm", std::abs(m_extrudeDistance));
                drawDim(m_extrudeOrigin,
                        m_extrudeOrigin + m_extrudeNormal * m_extrudeDistance, dbuf);
            } else if (m_pushPullActive && m_pushPullHasArrow) {
                // Arrow out of the face + signed-distance measurement.
                std::snprintf(dbuf, sizeof(dbuf), "%.1f mm", m_pushPullDistance);
                drawDim(m_pushPullOrigin,
                        m_pushPullOrigin + m_pushPullNormal * m_pushPullDistance, dbuf);
            } else if (m_edgeOpActive && m_edgeOpHasHandle) {
                // Arrow straight out of the edge (outward, perpendicular) + measurement.
                std::snprintf(dbuf, sizeof(dbuf), "%.1f mm", m_edgeOpValue);
                drawDim(m_edgeOpMid, m_edgeOpMid + m_edgeOpOutDir * m_edgeOpValue, dbuf);
            } else if (m_gizmoDragging && glm::length(m_gizmoTotalDelta) > 1e-3f) {
                // Translate drag: original body centre -> current centre.
                try {
                    Bnd_Box ob, cb;
                    BRepBndLib::Add(m_gizmoDragOriginalShape, ob);
                    BRepBndLib::Add(m_document->getBody(m_gizmoDragBodyId), cb);
                    if (!ob.IsVoid() && !cb.IsVoid()) {
                        double ox1, oy1, oz1, ox2, oy2, oz2, cx1, cy1, cz1, cx2, cy2, cz2;
                        ob.Get(ox1, oy1, oz1, ox2, oy2, oz2);
                        cb.Get(cx1, cy1, cz1, cx2, cy2, cz2);
                        glm::vec3 oc((ox1 + ox2) * 0.5, (oy1 + oy2) * 0.5, (oz1 + oz2) * 0.5);
                        glm::vec3 cc((cx1 + cx2) * 0.5, (cy1 + cy2) * 0.5, (cz1 + cz2) * 0.5);
                        float dist = glm::length(cc - oc);
                        if (dist > 1e-3f) {
                            std::snprintf(dbuf, sizeof(dbuf), "%.1f mm", dist);
                            drawDim(oc, cc, dbuf);
                        }
                    }
                } catch (...) {}
            }

            // Rotate (°) / Scale (%) readout near the body during a gizmo drag —
            // the analogue of the mm readout for moves.
            if (m_gizmoDragging && (m_gizmo->getMode() == GizmoMode::Rotate ||
                                    m_gizmo->getMode() == GizmoMode::Scale)) {
                try {
                    Bnd_Box gb; BRepBndLib::Add(m_document->getBody(m_gizmoDragBodyId), gb);
                    if (!gb.IsVoid()) {
                        double bx1,by1,bz1,bx2,by2,bz2; gb.Get(bx1,by1,bz1,bx2,by2,bz2);
                        glm::vec3 bc((bx1+bx2)*0.5,(by1+by2)*0.5,(bz1+bz2)*0.5);
                        ImVec2 sp;
                        if (toImg(bc, sp)) {
                            char rb[48];
                            if (m_gizmo->getMode() == GizmoMode::Rotate) {
                                // Show the ACTUAL applied angle (after soft 45° snap),
                                // not the raw mouse angle, so the readout matches the body.
                                float n = std::round(m_gizmoTotalAngle / 45.0f) * 45.0f;
                                float shown = (std::abs(m_gizmoTotalAngle - n) < 7.0f) ? n : m_gizmoTotalAngle;
                                std::snprintf(rb, sizeof(rb), "%.0f deg", shown);
                            } else
                                std::snprintf(rb, sizeof(rb), "X %.0f%%  Y %.0f%%  Z %.0f%%",
                                              m_gizmoTotalScale.x*100, m_gizmoTotalScale.y*100,
                                              m_gizmoTotalScale.z*100);
                            ImVec2 ts = ImGui::CalcTextSize(rb);
                            ImVec2 tp(sp.x - ts.x*0.5f, sp.y - ts.y - 14.0f);
                            dl->AddRectFilled(ImVec2(tp.x-4, tp.y-2), ImVec2(tp.x+ts.x+4, tp.y+ts.y+2),
                                              IM_COL32(20,20,28,205), 3.0f);
                            dl->AddText(tp, IM_COL32(235,235,240,255), rb);
                        }
                    }
                } catch (...) {}
            }

            // Sketch preview dimensions: line length, circle diameter (across the
            // full width — makers dimension by diameter), rectangle both sides.
            if (m_inSketchMode && m_activeSketch && m_sketchTool && m_sketchTool->hasPreview()) {
                const gp_Ax3& ax = m_activeSketch->getPlane().Position();
                glm::vec3 O(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
                glm::vec3 X(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
                glm::vec3 Y(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());
                auto sketch2world = [&](glm::vec2 p) { return O + p.x * X + p.y * Y; };

                SketchToolMode pm = m_sketchTool->getPreviewType();
                glm::vec2 ps = m_sketchTool->getPreviewStart();
                glm::vec2 pe = m_sketchTool->getPreviewEnd();

                if (pm == SketchToolMode::Line) {
                    float length = glm::length(pe - ps);
                    if (length > 1e-3f) {
                        std::snprintf(dbuf, sizeof(dbuf), "%.1f mm", length);
                        drawDim(sketch2world(ps), sketch2world(pe), dbuf);
                    }
                } else if (pm == SketchToolMode::Circle) {
                    // ps = centre, pe = a point on the rim. Span the full diameter.
                    glm::vec2 rvec = pe - ps;
                    float dia = 2.0f * glm::length(rvec);
                    if (dia > 1e-3f) {
                        std::snprintf(dbuf, sizeof(dbuf), "%.1f mm dia", dia);
                        drawDim(sketch2world(ps - rvec), sketch2world(pe), dbuf);
                    }
                } else if (pm == SketchToolMode::Rectangle) {
                    // ps, pe = opposite corners. Dimension the bottom and right sides.
                    glm::vec2 bl(ps.x, ps.y), br(pe.x, ps.y), tr(pe.x, pe.y);
                    float w = std::abs(pe.x - ps.x), h = std::abs(pe.y - ps.y);
                    if (w > 1e-3f) {
                        std::snprintf(dbuf, sizeof(dbuf), "%.1f mm", w);
                        drawDim(sketch2world(bl), sketch2world(br), dbuf);
                    }
                    if (h > 1e-3f) {
                        std::snprintf(dbuf, sizeof(dbuf), "%.1f mm", h);
                        drawDim(sketch2world(br), sketch2world(tr), dbuf);
                    }
                }
            }
        }

        if (ImGui::IsItemHovered()) {
            ImGuiIO& io = ImGui::GetIO();
            if (io.MouseWheel != 0.0f) cam.zoom(io.MouseWheel);
            // Camera drag uses the configurable bindings (File > Settings). The
            // orbit button pans instead when Shift is held; a distinct pan button
            // always pans.
            if (ImGui::IsMouseDragging(m_orbitButton)) {
                ImVec2 delta = io.MouseDelta;
                if (io.KeyShift) cam.pan(delta.x, delta.y);
                else cam.orbit(delta.x, delta.y);
            }
            if (m_panButton != m_orbitButton && ImGui::IsMouseDragging(m_panButton)) {
                cam.pan(io.MouseDelta.x, io.MouseDelta.y);
            }

            // Pause interactive operations while a camera button is also being
            // dragged — otherwise the changing view matrix re-projects the same
            // mouse motion onto a moving target each frame and the value jolts.
            bool camDragging = ImGui::IsMouseDragging(m_orbitButton) ||
                (m_panButton != m_orbitButton && ImGui::IsMouseDragging(m_panButton));

            // Interactive extrude drag: left-drag moves distance along normal
            if (m_extruding && !camDragging &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                glm::vec2 md(io.MouseDelta.x, io.MouseDelta.y);
                m_extrudeDistance += projectDragOntoNormal(m_extrudeOrigin, m_extrudeNormal,
                                                           md, proj * view);
                std::snprintf(m_extrudeInputBuf, sizeof(m_extrudeInputBuf), "%.1f", m_extrudeDistance);
                updateInteractiveExtrude();
            }

            // Push/Pull face arrow: left-drag moves the distance along the face normal.
            if (m_pushPullActive && m_pushPullHasArrow && !camDragging &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                glm::vec2 md(io.MouseDelta.x, io.MouseDelta.y);
                m_pushPullDistance += projectDragOntoNormal(m_pushPullOrigin, m_pushPullNormal,
                                                            md, proj * view);
                std::snprintf(m_pushPullInputBuf, sizeof(m_pushPullInputBuf), "%.1f", m_pushPullDistance);
                updatePushPull();
            }

            // Fillet/Chamfer drag handle: left-drag sets the radius/distance to the
            // perpendicular distance from the edge to the cursor (on a plane through
            // the edge midpoint facing the camera).
            if (m_edgeOpActive && m_edgeOpHasHandle && !camDragging &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                ImVec2 mp = ImGui::GetMousePos();
                ImVec2 wp = ImGui::GetItemRectMin();
                glm::mat4 invVP = glm::inverse(proj * view);
                float nx = ((mp.x - wp.x) / contentSize.x) * 2.0f - 1.0f;
                float ny = 1.0f - ((mp.y - wp.y) / contentSize.y) * 2.0f;
                glm::vec4 np = invVP * glm::vec4(nx, ny, -1.0f, 1.0f);
                glm::vec4 fp = invVP * glm::vec4(nx, ny, 1.0f, 1.0f);
                glm::vec3 ro(np / np.w), rd = glm::normalize(glm::vec3(fp / fp.w) - ro);
                glm::vec3 camFwd = glm::normalize(cam.getTarget() - cam.getPosition());
                float denom = glm::dot(rd, camFwd);
                if (std::abs(denom) > 1e-6f) {
                    float t = glm::dot(m_edgeOpMid - ro, camFwd) / denom;
                    glm::vec3 hit = ro + rd * t;
                    // Signed distance along the outward arrow: dragging away from the
                    // edge grows the value (≥0.1 mm); dragging back toward/through the
                    // edge returns to 0 (no change).
                    float proj = glm::dot(hit - m_edgeOpMid, m_edgeOpOutDir);
                    m_edgeOpValue = (proj <= 0.0f) ? 0.0f : std::max(0.1f, proj);
                    std::snprintf(m_edgeOpInputBuf, sizeof(m_edgeOpInputBuf), "%.1f", m_edgeOpValue);
                    updateInteractiveEdgeOp();
                }
            }

            // Gizmo input + Face hover highlighting + picking (suppressed while an
            // interactive op owns the left-drag: extrude, push/pull, fillet/chamfer).
            if (!m_inSketchMode && !m_extruding && !m_pushPullActive && !m_edgeOpActive) {
                ImVec2 mousePos = ImGui::GetMousePos();
                ImVec2 winPos = ImGui::GetItemRectMin();
                float localX = mousePos.x - winPos.x;
                float localY = mousePos.y - winPos.y;

                // Gizmo interaction takes priority
                bool gizmoConsumedInput = false;
                if (m_gizmo->isVisible()) {
                    bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
                    bool mouseJustPressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

                    GizmoResult gResult = m_gizmo->handleInput(
                        localX, localY, contentSize.x, contentSize.y,
                        mouseDown, mouseJustPressed, cam);

                    // Helpers shared by drag-apply and commit.
                    auto axisDirOf = [](GizmoAxis a) -> glm::vec3 {
                        if (a == GizmoAxis::X) return glm::vec3(1, 0, 0);
                        if (a == GizmoAxis::Y) return glm::vec3(0, 1, 0);
                        return glm::vec3(0, 0, 1);
                    };
                    auto softSnap45 = [](float deg) {
                        float n = std::round(deg / 45.0f) * 45.0f;
                        return (std::abs(deg - n) < 7.0f) ? n : deg; // free, snaps near 45°
                    };

                    // Start drag: save originals for every selected body (so Move
                    // can apply to all of them) and reset accumulators.
                    if (gResult.activeAxis != GizmoAxis::None && !m_gizmoDragging) {
                        m_gizmoDragOriginals.clear();
                        for (const auto& sel : m_selection->getSelection()) {
                            if (sel.type != SelectionType::Body) continue;
                            try {
                                m_gizmoDragOriginals.push_back(
                                    {sel.bodyId, m_document->getBody(sel.bodyId)});
                            } catch (...) {}
                        }
                        if (!m_gizmoDragOriginals.empty()) {
                            m_gizmoDragBodyId = m_gizmoDragOriginals.front().first;
                            m_gizmoDragOriginalShape = m_gizmoDragOriginals.front().second;
                            m_gizmoDragging = true;
                            m_gizmoTotalDelta = glm::vec3(0.0f);
                            m_gizmoTotalAngle = 0.0f;
                            m_gizmoScaleAccum = glm::vec3(0.0f);
                            m_gizmoTotalScale = glm::vec3(1.0f);
                        }
                    }

                    // During drag: accumulate totals and (re)apply to the ORIGINAL
                    // shape each frame, so snapping and per-axis scale stay stable.
                    if (gResult.changed && m_gizmoDragging) {
                        try {
                            Bnd_Box ob; BRepBndLib::Add(m_gizmoDragOriginalShape, ob);
                            double ox1,oy1,oz1,ox2,oy2,oz2; ob.Get(ox1,oy1,oz1,ox2,oy2,oz2);
                            gp_Pnt center((ox1+ox2)/2,(oy1+oy2)/2,(oz1+oz2)/2);

                            TopoDS_Shape result;
                            bool applied = false;

                            if (gResult.mode == GizmoMode::Translate) {
                                m_gizmoTotalDelta += gResult.delta;
                                glm::vec3 d = m_gizmoTotalDelta;
                                if (m_snapToGrid && m_sketchGridStep > 0.0f) {
                                    float step = m_sketchGridStep, thr = step * 0.4f;
                                    auto s1 = [&](float v){ float n=std::round(v/step)*step; return std::abs(v-n)<thr?n:v; };
                                    d.x = s1(d.x); d.y = s1(d.y); d.z = s1(d.z);
                                }
                                gp_Trsf trsf; trsf.SetTranslation(gp_Vec(d.x, d.y, d.z));
                                // Apply the same translation to every selected body,
                                // each from its own original shape.
                                for (auto& [id, orig] : m_gizmoDragOriginals) {
                                    BRepBuilderAPI_Transform xf(orig, trsf, true);
                                    if (xf.IsDone()) m_document->updateBody(id, xf.Shape());
                                }
                                m_meshesDirty = true;
                                applied = false; // already handled per-body above
                            } else if (gResult.mode == GizmoMode::Rotate) {
                                glm::vec3 ad = axisDirOf(gResult.activeAxis);
                                m_gizmoRotAxis = ad;
                                m_gizmoTotalAngle += glm::dot(gResult.delta, ad);
                                float ang = softSnap45(m_gizmoTotalAngle);
                                gp_Trsf trsf;
                                trsf.SetRotation(gp_Ax1(center, gp_Dir(ad.x, ad.y, ad.z)),
                                                 ang * M_PI / 180.0);
                                BRepBuilderAPI_Transform xf(m_gizmoDragOriginalShape, trsf, true);
                                if (xf.IsDone()) { result = xf.Shape(); applied = true; }
                            } else { // Scale — per-axis, non-uniform about the centre
                                float os = static_cast<float>(glm::length(
                                    glm::vec3(ox2-ox1, oy2-oy1, oz2-oz1)));
                                if (os < 0.001f) os = 1.0f;
                                int ai = gResult.activeAxis == GizmoAxis::X ? 0
                                       : gResult.activeAxis == GizmoAxis::Y ? 1 : 2;
                                m_gizmoScaleAccum[ai] += (ai==0?gResult.delta.x:ai==1?gResult.delta.y:gResult.delta.z);
                                if (m_scaleUniform) {
                                    // Drive all axes from the dragged axis's factor.
                                    float f = glm::clamp(1.0f + m_gizmoScaleAccum[ai]/os, 0.05f, 20.0f);
                                    f = std::round(f * 100.0f) / 100.0f; // snap to 1%
                                    m_gizmoTotalScale = glm::vec3(f);
                                } else {
                                    for (int k = 0; k < 3; ++k) {
                                        float f = glm::clamp(1.0f + m_gizmoScaleAccum[k]/os, 0.05f, 20.0f);
                                        m_gizmoTotalScale[k] = std::round(f * 100.0f) / 100.0f; // snap to 1%
                                    }
                                }
                                gp_GTrsf gt;
                                gt.SetVectorialPart(gp_Mat(m_gizmoTotalScale.x,0,0, 0,m_gizmoTotalScale.y,0, 0,0,m_gizmoTotalScale.z));
                                double cx=center.X(), cy=center.Y(), cz=center.Z();
                                gt.SetTranslationPart(gp_XYZ(cx - m_gizmoTotalScale.x*cx,
                                                             cy - m_gizmoTotalScale.y*cy,
                                                             cz - m_gizmoTotalScale.z*cz));
                                BRepBuilderAPI_GTransform xf(m_gizmoDragOriginalShape, gt, true);
                                if (xf.IsDone()) { result = xf.Shape(); applied = true; }
                            }

                            if (applied) {
                                m_document->updateBody(m_gizmoDragBodyId, result);
                                m_meshesDirty = true;
                            }
                        } catch (...) {}
                        gizmoConsumedInput = true;
                    }

                    // End drag: commit the right TransformOp(s) for the gizmo's mode.
                    if (m_gizmoDragging && gResult.activeAxis == GizmoAxis::None && !mouseDown) {
                        try {
                            GizmoMode gm = m_gizmo->getMode();
                            if (gm == GizmoMode::Translate) {
                                // Multi-body Move: restore every body's original so
                                // each TransformOp captures the right previousShape,
                                // then push one Translate op per body.
                                for (auto& [id, orig] : m_gizmoDragOriginals) {
                                    m_document->updateBody(id, orig);
                                }
                                glm::vec3 d = m_gizmoTotalDelta;
                                if (m_snapToGrid && m_sketchGridStep > 0.0f) {
                                    float step = m_sketchGridStep, thr = step * 0.4f;
                                    auto s1 = [&](float v){ float n=std::round(v/step)*step; return std::abs(v-n)<thr?n:v; };
                                    d.x = s1(d.x); d.y = s1(d.y); d.z = s1(d.z);
                                }
                                if (glm::length(d) > 1e-4f) {
                                    for (auto& [id, orig] : m_gizmoDragOriginals) {
                                        Bnd_Box bb; BRepBndLib::Add(orig, bb);
                                        double x0,y0,z0,x1,y1,z1; bb.Get(x0,y0,z0,x1,y1,z1);
                                        auto op = std::make_unique<TransformOp>();
                                        op->setBodyId(id);
                                        op->setCenter((x0+x1)/2,(y0+y1)/2,(z0+z1)/2);
                                        op->setType(TransformType::Translate);
                                        op->setTranslation(d.x, d.y, d.z);
                                        m_history->pushOperation(std::move(op), *m_document);
                                    }
                                }
                            } else {
                                // Rotate/Scale: single-body path (primary only).
                                m_document->updateBody(m_gizmoDragBodyId, m_gizmoDragOriginalShape);
                                Bnd_Box ob; BRepBndLib::Add(m_gizmoDragOriginalShape, ob);
                                double ox1,oy1,oz1,ox2,oy2,oz2; ob.Get(ox1,oy1,oz1,ox2,oy2,oz2);
                                gp_Pnt center((ox1+ox2)/2,(oy1+oy2)/2,(oz1+oz2)/2);

                                auto op = std::make_unique<TransformOp>();
                                op->setBodyId(m_gizmoDragBodyId);
                                op->setCenter(center.X(), center.Y(), center.Z());
                                bool valid = true;
                                if (gm == GizmoMode::Rotate) {
                                    float ang = softSnap45(m_gizmoTotalAngle);
                                    op->setType(TransformType::Rotate);
                                    op->setRotation(m_gizmoRotAxis.x, m_gizmoRotAxis.y, m_gizmoRotAxis.z, ang);
                                    valid = std::abs(ang) > 1e-3f;
                                } else {
                                    op->setType(TransformType::Scale);
                                    op->setScaleXYZ(m_gizmoTotalScale.x, m_gizmoTotalScale.y, m_gizmoTotalScale.z);
                                    valid = glm::length(m_gizmoTotalScale - glm::vec3(1.0f)) > 1e-3f;
                                }
                                if (valid) m_history->pushOperation(std::move(op), *m_document);
                            }
                            m_meshesDirty = true;
                        } catch (...) {}

                        m_gizmoDragging = false;
                        m_gizmoDragOriginalShape.Nullify();
                        m_gizmoDragBodyId = -1;
                        m_gizmoDragOriginals.clear();
                    }

                    if (gResult.activeAxis != GizmoAxis::None) {
                        gizmoConsumedInput = true;
                    }
                }

                if (!gizmoConsumedInput && !m_viewCube->wasHovered()) {
                    auto result = m_picker->pick(localX, localY,
                        contentSize.x, contentSize.y, cam, *m_document);

                    m_hoveredBodyId = result.hit ? result.bodyId : -1;

                    // Sketch-region hover (takes priority over body picking when present)
                    SketchRegionHit regionHit = pickSketchRegion(localX, localY,
                        contentSize.x, contentSize.y);
                    // Reject a sketch region that sits behind a body under the cursor —
                    // only what's visible should be selectable. Compare hit distances
                    // from the camera (origin-independent) and drop the region if the
                    // body face is nearer.
                    if (regionHit.regionIndex >= 0 && result.hit) {
                        glm::vec3 camPos = cam.getPosition();
                        float bodyD = glm::length(result.hitPoint - camPos);
                        float sketchD = glm::length(regionHit.worldPoint - camPos);
                        if (bodyD < sketchD - 1e-3f) {
                            regionHit.sketchId = -1;
                            regionHit.regionIndex = -1;
                        }
                    }
                    m_hoveredSketchId = regionHit.sketchId;
                    m_hoveredRegionIndex = regionHit.regionIndex;

                    bool regionConsumedClick = false;
                    if (regionHit.regionIndex >= 0 &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        SelectionEntry entry;
                        entry.type = SelectionType::SketchRegion;
                        entry.sketchId = regionHit.sketchId;
                        entry.subShapeIndex = regionHit.regionIndex;
                        if (io.KeyCtrl) {
                            m_selection->addToSelection(entry);
                        } else {
                            m_selection->select(entry);
                        }
                        regionConsumedClick = true;
                    }

                    // Mirror "across a face" mode: the next planar face click
                    // defines the mirror plane (Esc cancels via handleShortcuts).
                    if (m_mirrorPickFace && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        if (result.hit && !result.pickedShape.IsNull() &&
                            result.pickedShape.ShapeType() == TopAbs_FACE && m_mirrorBodyId >= 0) {
                            try {
                                TopoDS_Face face = TopoDS::Face(result.pickedShape);
                                Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
                                if (!surf.IsNull() && surf->IsKind(STANDARD_TYPE(Geom_Plane))) {
                                    gp_Pln pln = Handle(Geom_Plane)::DownCast(surf)->Pln();
                                    const gp_Ax3& ax = pln.Position();
                                    auto op = std::make_unique<MirrorOp>();
                                    op->setBody(m_mirrorBodyId);
                                    op->setPlane(MirrorPlane::Custom);
                                    op->setCustomPlane(gp_Ax2(ax.Location(), ax.Direction()));
                                    op->setKeepOriginal(true);
                                    if (m_history->pushOperation(std::move(op), *m_document))
                                        m_meshesDirty = true;
                                }
                            } catch (...) {}
                        }
                        m_mirrorPickFace = false;
                        regionConsumedClick = true; // don't also change selection
                    }

                    // Double-click to select body, single-click to select face
                    if (!regionConsumedClick && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (result.hit) {
                            SelectionEntry entry;
                            entry.type = SelectionType::Body;
                            entry.bodyId = result.bodyId;
                            try { entry.shape = m_document->getBody(result.bodyId); } catch (...) {}
                            if (io.KeyCtrl) {
                                m_selection->addToSelection(entry);
                            } else {
                                m_selection->select(entry);
                            }
                        }
                    } else if (!regionConsumedClick && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        int ownerStep = -1; // fillet/chamfer step to open in the editor
                        if (result.hit) {
                            SelectionEntry entry;
                            // If click is near an edge (<8px), select edge; otherwise face
                            if (result.edgeScreenDist < 8.0f && !result.nearestEdge.IsNull()) {
                                entry.type = SelectionType::Edge;
                                entry.bodyId = result.bodyId;
                                entry.shape = result.nearestEdge;
                            } else {
                                entry.type = SelectionType::Face;
                                entry.bodyId = result.bodyId;
                                entry.subShapeIndex = result.faceIndex;
                                entry.shape = result.pickedShape;
                                // Trace a clicked face back to the fillet/chamfer that
                                // produced it, so the user can re-edit it after the fact.
                                if (!entry.shape.IsNull()) {
                                    int upTo = m_history->currentStep();
                                    for (int s = 0; s <= upTo; ++s) {
                                        const Operation* op = m_history->getStep(s);
                                        if (op && op->isEnabled() && op->ownsFace(entry.shape)) {
                                            ownerStep = s;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (io.KeyCtrl) {
                                m_selection->addToSelection(entry);
                            } else {
                                m_selection->select(entry);
                            }
                        } else {
                            // Empty-space click: begin a box-select drag instead of
                            // clearing immediately. The release handler below decides
                            // whether to multi-select bodies (drag had area) or treat
                            // it as a plain click and clear.
                            bool boxEligible = !m_inSketchMode && !m_extruding &&
                                !m_pushPullActive && !m_edgeOpActive && !m_gizmoDragging &&
                                m_orbitButton != ImGuiMouseButton_Left &&
                                m_panButton  != ImGuiMouseButton_Left;
                            if (boxEligible && m_boxSelect) {
                                ImVec2 mp = ImGui::GetMousePos();
                                ImVec2 wp = ImGui::GetItemRectMin();
                                m_boxSelect->begin(glm::vec2(mp.x - wp.x, mp.y - wp.y));
                            } else if (!io.KeyCtrl) {
                                m_selection->clear();
                            }
                        }
                        // Open the owning fillet/chamfer in the History editor, or
                        // close that editor when clicking anything else.
                        m_historyPanel->setEditingStep(ownerStep);
                    }

                    // Box-select drag + release. Update while LEFT is held; on
                    // release, intersect bodies' screen-space bboxes with the
                    // rectangle and add them to selection (Ctrl preserves the
                    // existing selection).
                    if (m_boxSelect && m_boxSelect->isActive()) {
                        ImVec2 mp = ImGui::GetMousePos();
                        ImVec2 wp = ImGui::GetItemRectMin();
                        glm::vec2 curScreen(mp.x - wp.x, mp.y - wp.y);
                        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                            m_boxSelect->update(curScreen);
                        }
                        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                            glm::vec2 mn = m_boxSelect->getMin();
                            glm::vec2 mx = m_boxSelect->getMax();
                            m_boxSelect->end();

                            // Tiny rectangle = treat as a plain click → clear.
                            if (glm::distance(mn, mx) < 4.0f) {
                                if (!io.KeyCtrl) m_selection->clear();
                            } else {
                                if (!io.KeyCtrl) m_selection->clear();
                                glm::mat4 vp = proj * view;
                                for (int id : m_document->getAllBodyIds()) {
                                    if (!m_document->isBodyVisible(id)) continue;
                                    try {
                                        const TopoDS_Shape& shape = m_document->getBody(id);
                                        Bnd_Box bb; BRepBndLib::Add(shape, bb);
                                        if (bb.IsVoid()) continue;
                                        double x0,y0,z0,x1,y1,z1;
                                        bb.Get(x0,y0,z0,x1,y1,z1);
                                        // Project the 8 bbox corners into screen pixels,
                                        // skipping any behind the camera (w<=0).
                                        glm::vec2 bMin( FLT_MAX,  FLT_MAX);
                                        glm::vec2 bMax(-FLT_MAX, -FLT_MAX);
                                        bool any = false;
                                        for (int c = 0; c < 8; ++c) {
                                            glm::vec4 p((c & 1) ? x1 : x0,
                                                        (c & 2) ? y1 : y0,
                                                        (c & 4) ? z1 : z0, 1.0f);
                                            glm::vec4 cp = vp * p;
                                            if (cp.w <= 0.0f) continue;
                                            glm::vec2 ndc(cp.x / cp.w, cp.y / cp.w);
                                            glm::vec2 sp(
                                                (ndc.x * 0.5f + 0.5f) * contentSize.x,
                                                (1.0f - (ndc.y * 0.5f + 0.5f)) * contentSize.y);
                                            bMin = glm::min(bMin, sp);
                                            bMax = glm::max(bMax, sp);
                                            any = true;
                                        }
                                        if (!any) continue;
                                        // AABB overlap test against the box rect.
                                        if (bMax.x >= mn.x && bMin.x <= mx.x &&
                                            bMax.y >= mn.y && bMin.y <= mx.y) {
                                            SelectionEntry e;
                                            e.type = SelectionType::Body;
                                            e.bodyId = id;
                                            e.shape = shape;
                                            m_selection->addToSelection(e);
                                        }
                                    } catch (...) {}
                                }
                            }
                        }
                    }

                    // Right click on a face: context menu (only if not a pan drag)
                    ImVec2 dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
                    bool wasDragging = (std::abs(dragDelta.x) > 1.0f || std::abs(dragDelta.y) > 1.0f);
                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !wasDragging) {
                        if (result.hit && !result.pickedShape.IsNull()) {
                            m_contextMenuBodyId = result.bodyId;
                            m_contextMenuFace = result.pickedShape;
                            m_contextMenuPending = true;
                        }
                    }
                }
            }

            // Sketch mode mouse input — ray-plane intersection. Skipped while
            // the camera is being dragged so the in-progress preview (e.g. the
            // line endpoint following the cursor) doesn't jolt as the view moves.
            if (m_inSketchMode && m_activeSketch && !camDragging) {
                ImVec2 mousePos = ImGui::GetMousePos();
                ImVec2 winPos = ImGui::GetItemRectMin();
                float localX = mousePos.x - winPos.x;
                float localY = mousePos.y - winPos.y;
                glm::vec2 sketchCoord = screenToSketch(localX, localY, contentSize.x, contentSize.y);

                // Interactive sketch-rotate takes over input while active: the
                // cursor angle around the pivot drives the rotation; left-click
                // commits, Esc (handled in handleShortcuts) cancels.
                if (m_sketchRotating) {
                    glm::vec2 v0 = m_sketchRotateAnchor - m_sketchRotateCenter;
                    glm::vec2 v1 = sketchCoord          - m_sketchRotateCenter;
                    if (glm::length(v0) > 1e-4f && glm::length(v1) > 1e-4f) {
                        float ang = std::atan2(v1.y, v1.x) - std::atan2(v0.y, v0.x);
                        float ca = std::cos(ang), sa = std::sin(ang);
                        for (auto& [id, op] : m_sketchRotateOriginals) {
                            glm::vec2 d = op - m_sketchRotateCenter;
                            glm::vec2 r(d.x * ca - d.y * sa, d.x * sa + d.y * ca);
                            m_activeSketch->movePoint(id, m_sketchRotateCenter + r);
                        }
                    }
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        auto after = std::make_shared<Sketch>(*m_activeSketch);
                        auto op = std::make_unique<SketchEditOp>(
                            m_activeSketch, m_sketchRotateBefore, after);
                        m_history->pushExecuted(std::move(op));
                        m_sketchRotating = false;
                        m_sketchRotateBefore.reset();
                        m_sketchRotateOriginals.clear();
                    }
                    // Skip the normal sketch-tool input while rotating.
                } else if (m_sketchTool->getMode() == SketchToolMode::Select &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    // Double-click in Select mode → select every element in the
                    // sketch (saves a Ctrl+click marathon to grab everything).
                    m_sketchTool->selectAll();
                    m_sketchDragBefore.reset(); // not a drag
                } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    // Select/drag mutates point positions only — no structural
                    // change — so recordSketchMutation's signature wouldn't see
                    // it. Snapshot manually for the drag-commit on mouse-up.
                    if (m_sketchTool->getMode() == SketchToolMode::Select) {
                        m_sketchDragBefore = std::make_shared<Sketch>(*m_activeSketch);
                    }
                    recordSketchMutation([&]{ m_sketchTool->onMouseDown(sketchCoord, io.KeyCtrl); });
                }
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    m_sketchTool->onMouseUp(sketchCoord);
                    if (m_sketchDragBefore) {
                        // Compare point positions; commit a SketchEditOp if any moved.
                        const auto& before = m_sketchDragBefore->getPoints();
                        const auto& after  = m_activeSketch->getPoints();
                        bool changed = (before.size() != after.size());
                        for (size_t i = 0; !changed && i < before.size(); ++i) {
                            if (glm::distance(before[i].pos, after[i].pos) > 1e-5f)
                                changed = true;
                        }
                        if (changed) {
                            auto after_ptr = std::make_shared<Sketch>(*m_activeSketch);
                            auto op = std::make_unique<SketchEditOp>(
                                m_activeSketch, m_sketchDragBefore, after_ptr);
                            m_history->pushExecuted(std::move(op));
                        }
                        m_sketchDragBefore.reset();
                    }
                }
                m_sketchTool->onMouseMove(sketchCoord);
            }
        }
    }

    // ViewCube overlay
    ViewCubeAction vcAction = m_viewCube->render(m_viewport->getCamera(), m_invertCubeDrag);
    if (vcAction != ViewCubeAction::None) {
        handleViewCubeAction(static_cast<int>(vcAction));
    }

    // Right-click face context menu
    if (m_contextMenuPending) {
        ImGui::OpenPopup("FaceContextMenu");
        m_contextMenuPending = false;
    }
    if (ImGui::BeginPopup("FaceContextMenu")) {
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Face Options");
        ImGui::Separator();

        if (ImGui::MenuItem("Sketch on this Face")) {
            // Select the face, then enter sketch mode (enterSketchMode reads the selection)
            SelectionEntry entry;
            entry.type = SelectionType::Face;
            entry.bodyId = m_contextMenuBodyId;
            entry.shape = m_contextMenuFace;
            m_selection->select(entry);
            enterSketchMode();
            m_contextMenuFace.Nullify();
        }
        if (ImGui::MenuItem("Extrude Face")) {
            beginInteractiveExtrude(m_contextMenuFace);
            m_contextMenuFace.Nullify();
        }
        if (ImGui::MenuItem("Select Body")) {
            SelectionEntry entry;
            entry.type = SelectionType::Body;
            entry.bodyId = m_contextMenuBodyId;
            try { entry.shape = m_document->getBody(m_contextMenuBodyId); } catch (...) {}
            m_selection->select(entry);
            m_contextMenuFace.Nullify();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Cancel")) {
            m_contextMenuFace.Nullify();
        }
        ImGui::EndPopup();
    }

    // Gizmo hint
    if (m_gizmo->isVisible()) {
        ImGui::SetCursorPos(ImVec2(10, 30));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGui::Text("Arrows: Move | Rings: Rotate | Cubes: Scale");
        ImGui::PopStyleColor();
    }

    // Interactive extrude UI
    if (m_extruding) {
        ImGui::SetCursorPos(ImVec2(10, 30));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
        ImGui::Text("EXTRUDE - Drag in viewport or type distance. Enter to confirm, Escape to cancel.");
        ImGui::PopStyleColor();

        // Floating distance input panel
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 260,
                                        ImGui::GetWindowPos().y + 50));
        ImGui::SetNextWindowSize(ImVec2(240, 0));
        ImGui::Begin("##ExtrudeInput", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("Extrude Distance (mm)");
        ImGui::Separator();

        if (m_extrudeInputFocus) {
            ImGui::SetKeyboardFocusHere();
            m_extrudeInputFocus = false;
        }

        bool valueChanged = false;
        if (ImGui::InputText("##dist", m_extrudeInputBuf, sizeof(m_extrudeInputBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            // Enter pressed — commit
            m_extrudeDistance = static_cast<float>(std::atof(m_extrudeInputBuf));
            updateInteractiveExtrude();
            commitInteractiveExtrude();
        } else {
            // Update distance from text as user types
            float parsed = static_cast<float>(std::atof(m_extrudeInputBuf));
            if (std::abs(parsed - m_extrudeDistance) > 0.01f && std::abs(parsed) > 0.01f) {
                m_extrudeDistance = parsed;
                updateInteractiveExtrude();
            }
        }

        ImGui::SameLine();
        ImGui::Text("mm");

        // Slider for quick adjustment
        if (ImGui::SliderFloat("##slider", &m_extrudeDistance, -50.0f, 50.0f, "%.1f mm")) {
            std::snprintf(m_extrudeInputBuf, sizeof(m_extrudeInputBuf), "%.1f", m_extrudeDistance);
            updateInteractiveExtrude();
        }

        ImGui::Spacing();
        if (ImGui::Button("Confirm (Enter)", ImVec2(110, 0))) {
            commitInteractiveExtrude();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel (Esc)", ImVec2(110, 0))) {
            cancelInteractiveExtrude();
        }

        ImGui::End();
    }

    // Interactive Push/Pull UI
    if (m_pushPullActive) {
        ImGui::SetCursorPos(ImVec2(10, 30));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.85f, 1.0f, 1.0f));
        ImGui::Text("PUSH/PULL - Positive = extrude, Negative = cut. Enter to confirm, Escape to cancel.");
        ImGui::PopStyleColor();

        ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 260,
                                        ImGui::GetWindowPos().y + 50));
        ImGui::SetNextWindowSize(ImVec2(240, 0));
        ImGui::Begin("##PushPullInput", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("Distance (mm) - signed");
        ImGui::Separator();

        if (m_pushPullInputFocus) {
            ImGui::SetKeyboardFocusHere();
            m_pushPullInputFocus = false;
        }

        if (ImGui::InputText("##ppdist", m_pushPullInputBuf, sizeof(m_pushPullInputBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            m_pushPullDistance = static_cast<float>(std::atof(m_pushPullInputBuf));
            updatePushPull();
            commitPushPull();
        } else {
            float parsed = static_cast<float>(std::atof(m_pushPullInputBuf));
            if (std::abs(parsed - m_pushPullDistance) > 0.01f) {
                m_pushPullDistance = parsed;
                updatePushPull();
            }
        }

        ImGui::SameLine();
        ImGui::Text("mm");

        if (ImGui::SliderFloat("##ppslider", &m_pushPullDistance, -50.0f, 50.0f, "%.1f mm")) {
            std::snprintf(m_pushPullInputBuf, sizeof(m_pushPullInputBuf), "%.1f", m_pushPullDistance);
            updatePushPull();
        }

        ImGui::Spacing();
        if (ImGui::Button("Confirm (Enter)", ImVec2(110, 0))) {
            commitPushPull();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel (Esc)", ImVec2(110, 0))) {
            cancelPushPull();
        }

        ImGui::End();
    }

    // Interactive fillet/chamfer UI
    if (m_edgeOpActive) {
        const char* opName = m_edgeOpType == EdgeOpType::Fillet ? "FILLET" : "CHAMFER";
        const char* label = m_edgeOpType == EdgeOpType::Fillet ? "Radius (mm)" : "Distance (mm)";

        ImGui::SetCursorPos(ImVec2(10, 30));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.5f, 1.0f));
        ImGui::Text("%s - Type value or use slider. Enter to confirm, Escape to cancel.", opName);
        ImGui::PopStyleColor();

        ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 260,
                                        ImGui::GetWindowPos().y + 50));
        ImGui::SetNextWindowSize(ImVec2(240, 0));
        ImGui::Begin("##EdgeOpInput", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("%s", label);
        ImGui::Separator();

        if (m_edgeOpInputFocus) {
            ImGui::SetKeyboardFocusHere();
            m_edgeOpInputFocus = false;
        }

        if (ImGui::InputText("##val", m_edgeOpInputBuf, sizeof(m_edgeOpInputBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            m_edgeOpValue = static_cast<float>(std::atof(m_edgeOpInputBuf));
            updateInteractiveEdgeOp();
            commitInteractiveEdgeOp();
        } else {
            float parsed = static_cast<float>(std::atof(m_edgeOpInputBuf));
            if (std::abs(parsed - m_edgeOpValue) > 0.01f && parsed > 0.01f) {
                m_edgeOpValue = parsed;
                updateInteractiveEdgeOp();
            }
        }

        ImGui::SameLine();
        ImGui::Text("mm");

        if (ImGui::SliderFloat("##eslider", &m_edgeOpValue, 0.1f, 20.0f, "%.1f mm")) {
            std::snprintf(m_edgeOpInputBuf, sizeof(m_edgeOpInputBuf), "%.1f", m_edgeOpValue);
            updateInteractiveEdgeOp();
        }

        ImGui::Spacing();
        if (ImGui::Button("Confirm (Enter)", ImVec2(110, 0))) {
            commitInteractiveEdgeOp();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel (Esc)", ImVec2(110, 0))) {
            cancelInteractiveEdgeOp();
        }

        ImGui::End();
    }

    // Scale gizmo side panel (X/Y/Z % + uniform + Apply), shown in Scale mode.
    renderScalePanel();

    // Sketch mode indicator
    if (m_inSketchMode) {
        ImGui::SetCursorPos(ImVec2(10, 30));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.4f, 1.0f));
        ImGui::Text("SKETCH MODE - Press Escape to finish");
        ImGui::PopStyleColor();
    }

    // Inline dimension input while placing a sketch shape
    if (m_inSketchMode && m_sketchTool && m_sketchTool->hasPreview()) {
        SketchToolMode mode = m_sketchTool->getPreviewType();
        const char* dimLabel = nullptr;
        switch (mode) {
            case SketchToolMode::Line:      dimLabel = "Length (mm)"; break;
            case SketchToolMode::Circle:    dimLabel = "Radius (mm)"; break;
            case SketchToolMode::Polygon:   dimLabel = "Radius (mm)"; break;
            case SketchToolMode::Rectangle: dimLabel = "Side (mm)";   break;
            default: dimLabel = nullptr;
        }
        if (dimLabel) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 230,
                                            ImGui::GetWindowPos().y + 50));
            ImGui::SetNextWindowSize(ImVec2(220, 0));
            ImGui::Begin("##SketchDimInput", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_AlwaysAutoResize);

            ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", dimLabel);
            ImGui::Separator();
            ImGui::TextWrapped("Type a value and press Enter. The shape extends from your first click toward the cursor.");

            // Grab keyboard focus the first frame placement begins
            if (!m_sketchDimWasShown) {
                ImGui::SetKeyboardFocusHere();
                m_sketchDimWasShown = true;
            }

            if (ImGui::InputText("##sketchDim", m_sketchDimBuf, sizeof(m_sketchDimBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_CharsDecimal |
                                 ImGuiInputTextFlags_AutoSelectAll)) {
                float v = static_cast<float>(std::atof(m_sketchDimBuf));
                if (v > 0.0f) {
                    recordSketchMutation([&]{ m_sketchTool->applyDimension(v); });
                }
                m_sketchDimBuf[0] = '\0';
                m_sketchDimWasShown = false; // re-focus on the next placement
            }

            ImGui::End();
        }
    } else {
        // Reset when not placing
        m_sketchDimBuf[0] = '\0';
        m_sketchDimWasShown = false;
    }

    ImGui::End();
    ImGui::PopStyleVar();
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

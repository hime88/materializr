#include "ui/UiTheme.h"
#include "gl_common.h"
#include <SDL.h>

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#include <pmmintrin.h>
#define MZR_HAS_SSE 1
#endif

namespace {
// OCCT's boolean/intersection math assumes the default FPU mode (round-to-
// nearest, denormals kept). GL drivers flip the SSE flush-to-zero / denormals-
// are-zero flags during rendering, after which an OCCT boolean that ran clean
// can silently degenerate. Call this after any GL work that precedes OCCT (the
// deferred-op progress frames) to put the FPU back the way OCCT expects.
inline void resetFpuForOcct() {
#ifdef MZR_HAS_SSE
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_OFF);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_OFF);
#endif
}
} // namespace

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "app/Application.h"
#include "app/Window.h"
#include "ui_scale.h"
#include "touch_mode.h"
#include "viewport/Viewport.h"
#include "viewport/Grid.h"
#include "viewport/ShapeRenderer.h"
#include "viewport/SketchRenderer.h"
#include "viewport/ViewCube.h"
#include "viewport/Picker.h"
#include "viewport/Gizmo.h"
#include "viewport/SelectionHighlight.h"
#include "viewport/BoxSelect.h"
#include "viewport/SectionView.h"
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
#include "ui/MeasureTool.h"
#include "ui/UpdateChecker.h"
#include "modeling/Sketch.h"
#include "modeling/CopyOp.h"
#include "modeling/SketchSolver.h"
#include "modeling/SketchTool.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/ReplayOp.h"
#include "modeling/OperationFactory.h"
#include "modeling/PushPullOp.h"
#include "modeling/CombineSketchesOp.h"
#include "modeling/DuplicateSketchOp.h"
#include "modeling/TransformOp.h"
#include "modeling/MirrorOp.h"
#include "modeling/FilletOp.h"
#include "modeling/ChamferOp.h"
#include "modeling/ShellOp.h"
#include "modeling/DeleteOp.h"
#include "modeling/SketchEditOp.h"
#include "modeling/SketchTransformOp.h"
#include "modeling/ResizeCylindricalOp.h"
#include "io/StepIO.h"
#include "io/StlExport.h"
#include "io/SvgExport.h"
#include "io/FileDialogs.h"
#include "modeling/SvgImport.h"
#include "io/ProjectIO.h"
#include "io/SketchRecovery.h"
#include "io/ProjectRecovery.h"
#include "io/Settings.h"
#include "android_files.h" // androidLastDocUri/Name + androidOpenUri (Open Recent on SAF)
#include "core/EventBus.h"
#include "core/Events.h"
#include "plugin/PluginContext.h"
#include "plugin/PluginRegistry.h"

namespace materializr { namespace force_link { void linkAll(); } }

#include <imgui.h>
#include <imgui_internal.h> // dock-node tab-bar policy (per-node LocalFlags)
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

#include "app/Window.h"
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <gp_Ax3.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <Geom_Plane.hxx>
#include <GeomLib_IsPlanarSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_ToroidalSurface.hxx>
#include <GeomAbs_CurveType.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <cstring>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <TopExp_Explorer.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Mat.hxx>
#include <gp_XYZ.hxx>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <BRep_Builder.hxx>
#include <stdexcept>
#include <cstdio>
#ifdef _WIN32
#include <windows.h> // GetModuleFileNameA for exe dir (font path lookup)
#elif defined(__APPLE__)
#include <mach-o/dyld.h> // _NSGetExecutablePath for exe dir (no /proc on macOS)
#else
#include <unistd.h>  // readlink for resolving /proc/self/exe → exe dir (font path lookup)
#endif

namespace materializr {

Application::Application(bool safeMode) : m_safeMode(safeMode) {
    m_window = std::make_unique<Window>(1600, 900, "Materializr");
    m_viewport = std::make_unique<Viewport>();
    m_grid = std::make_unique<Grid>();
    m_shapeRenderer = std::make_unique<ShapeRenderer>();
    m_sketchRenderer = std::make_unique<SketchRenderer>();
    m_edgeRenderer = std::make_unique<EdgeRenderer>();
    // PlaneRenderer ownership moved to ConstructionPlanePlugin (registered
    // as a plugin render pass). Application no longer touches it directly.
    m_backgroundRenderer = std::make_unique<BackgroundRenderer>();
    m_document = std::make_unique<Document>();
    m_history = std::make_unique<History>();
    m_selection = std::make_unique<SelectionManager>();
    m_eventBus = std::make_unique<EventBus>();

    // Cascade: when a SketchEditOp commits via a user-driven path that
    // identifies the sketch (Properties → Constraints panel today), re-run
    // every enabled ExtrudeOp downstream of that sketch so its body follows.
    m_eventBus->subscribe<SketchEditedEvent>(
        [this](const SketchEditedEvent& e) { cascadeFromSketchEdit(e.sketchId); });

    // Transient status/error messages from non-UI code (plugins, ops).
    m_eventBus->subscribe<materializr::ToastEvent>(
        [this](const materializr::ToastEvent& e) { showToast(e.text, e.seconds); });

    // Document body lifecycle → renderer slot lifecycle. Without this, a
    // PushPullOp::undo (firing on every preview frame during a drag) deletes
    // the body from Document but the renderer keeps drawing its stale mesh
    // — the "banding" effect of N overlapping preview prisms accumulating
    // during a drag. We drop the slot immediately rather than waiting for
    // someone to put the id in m_dirtyBodyIds (nothing does, today).
    m_eventBus->subscribe<materializr::BodyRemovedEvent>(
        [this](const materializr::BodyRemovedEvent& e) {
            if (e.bodyId < 0) return;
            if (m_shapeRenderer) m_shapeRenderer->removeBody(e.bodyId);
            if (m_edgeRenderer)  m_edgeRenderer->removeBody(e.bodyId);
            // Also clear any pending dirty entry — the body is gone, no
            // point asking the partial rebuild to revisit it.
            m_dirtyBodyIds.erase(e.bodyId);
        });

    // Construction-plane lifecycle is handled by ConstructionPlanePlugin —
    // it owns the PlaneRenderer, subscribes to PlaneAdded/Removed/Changed
    // events, and registers a render pass. Application is hands-off.

    // NOTE: we explicitly do NOT cascade off generic HistoryStepEvents. That
    // event also fires for in-flight push/pull preview undos (every drag
    // frame), and those undo-landings on a SketchEditOp would otherwise
    // re-cascade every frame — piling up duplicate bodies. The cascade is
    // driven solely by the explicit SketchEditedEvent above, published by
    // PropertiesPanel (live constraint editor) and HistoryPanel's Apply
    // Changes button. Other history mutators stay out of it.
    m_pluginContext = std::make_unique<PluginContext>();

    m_toolbar = std::make_unique<Toolbar>();
    m_historyPanel = std::make_unique<HistoryPanel>();
    m_itemsPanel = std::make_unique<ItemsPanel>();

    m_sketchTool = std::make_unique<SketchTool>();
    m_viewCube = std::make_unique<ViewCube>();
    m_picker = std::make_unique<Picker>();
    m_gizmo = std::make_unique<Gizmo>();
    m_selectionHighlight = std::make_unique<SelectionHighlight>();
    m_boxSelect = std::make_unique<BoxSelect>();
    m_sectionView = std::make_unique<SectionView>();
    m_sectionView->setDocument(m_document.get());
    m_statusBar = std::make_unique<StatusBar>();
    m_themeManager = std::make_unique<ThemeManager>();
    m_propertiesPanel = std::make_unique<PropertiesPanel>();
    m_aboutDialog = std::make_unique<AboutDialog>();
    m_shortcutsPanel = std::make_unique<ShortcutsPanel>();
    m_helpPanel = std::make_unique<HelpPanel>();
    m_measureTool = std::make_unique<MeasureTool>();
    m_measureTool->setDocument(m_document.get());
    m_measureTool->setSelectionManager(m_selection.get());

    // Wire up references
    m_toolbar->setSelectionManager(m_selection.get());
    m_toolbar->setHistory(m_history.get());
    m_toolbar->setPluginContext(m_pluginContext.get());
    // Touch mode gates the UI scale and the whole input/UX model, and it must be
    // known before fonts and widget sizes are baked below. Resolve it from the
    // saved settings up front (loadAppSettings() re-reads everything once the
    // ImGui context exists); falls back to the platform default if there's no
    // settings file yet.
    {
        AppSettings early = SettingsIO::load(SettingsIO::defaultPath());
        materializr::setTouchMode(early.touchMode);
    }
    // Scale the Tools-panel button heights to match the HiDPI font (touch mode),
    // otherwise 30px buttons under a 2x font overlap. 1.0 in desktop mode.
    if (m_window) {
        m_toolbar->setUiScale(m_window->uiScale());
        // Global scale for fixed-size dialogs/popups (uiSz/uiW), so their
        // hard-coded pixel widths grow with the font instead of clipping.
        materializr::setUiScale(m_window->uiScale());
    }
    m_history->setThreadsLastDeclineCallback([this]{ showThreadsLastToast(); });
    m_historyPanel->setHistory(m_history.get());
    m_historyPanel->setDocument(m_document.get());
    m_historyPanel->setEventBus(m_eventBus.get());
    m_itemsPanel->setDocument(m_document.get());
    m_itemsPanel->setSelectionManager(m_selection.get());
    m_itemsPanel->setHistory(m_history.get());
    m_itemsPanel->setDirtyCallback([this]() { markDirty(); });
    m_itemsPanel->setExportStlCallback([this](int bodyId) { exportBodyAsStl(bodyId); });
    m_itemsPanel->setEditSketchCallback([this](int sketchId) { editSketch(sketchId); });
    m_itemsPanel->setExportSketchSvgCallback([this](int sketchId) { exportSketchAsSvg(sketchId); });
    m_itemsPanel->setDuplicateSketchCallback([this](int sketchId) { duplicateSketch(sketchId); });
    m_itemsPanel->setCombineSketchesCallback(
        [this](const std::vector<int>& ids) { combineSketches(ids); });
    m_itemsPanel->setRotatePlaneCallback([this](int planeId) { beginRotatePlaneAboutAxis(planeId); });
    m_statusBar->setDocument(m_document.get());
    m_statusBar->setSelectionManager(m_selection.get());
    m_propertiesPanel->setHistory(m_history.get());
    m_propertiesPanel->setDocument(m_document.get());
    m_propertiesPanel->setSelectionManager(m_selection.get());
    m_propertiesPanel->setEventBus(m_eventBus.get());
    m_propertiesPanel->setRotatePlaneCallback([this](int planeId) { beginRotatePlaneAboutAxis(planeId); });
    m_propertiesPanel->setDirtyCallback([this]() { markDirty(); });
    m_propertiesPanel->setLinkInfoCallback(
        [this](bool isBody, int id) { return linkHintFor(isBody, id); });
    m_propertiesPanel->setRelinkCallback(
        [this](bool isBody, int id) { relinkSketch(isBody, id); });
    // Element-size edits from the Properties panel while sketching: snapshot +
    // re-solve inside recordSketchMutation (so it's one undoable SketchEditOp),
    // then cascade to any body built from the sketch.
    m_propertiesPanel->setSketchMutateCallback(
        [this](const std::function<void()>& mut) {
            recordSketchMutation([&]() {
                mut();
                if (m_activeSketch) {
                    SketchSolver solver;
                    solver.solve(*m_activeSketch);
                }
            });
            if (m_eventBus && m_activeSketchId >= 0)
                m_eventBus->publish(SketchEditedEvent{m_activeSketchId});
            m_meshesDirty = true;
            markDirty();
        });
    // If no system file-dialog helper exists, Open/Save/Export would otherwise
    // do nothing at all — surface that instead of failing silently.
    FileDialogs::setUnavailableNotifier([this]() {
        showToast("No file-dialog program found \xE2\x80\x94 install 'zenity' "
                  "(GNOME) or 'kdialog' (KDE) to Open / Save / Export.", 8.0);
    });

    initImGui();
    renderSplashFrame("Loading settings & last project");
    loadAppSettings(); // restore persisted preferences before the theme is applied
    renderSplashFrame("Preparing renderers");
    m_themeManager->apply();
    initRenderers();
    renderSplashFrame("Almost there");
    setupCommands();

    // Wire EventBus into core services
    m_document->setEventBus(m_eventBus.get());
    m_history->setEventBus(m_eventBus.get());
    m_selection->setEventBus(m_eventBus.get());

    // Plugin system
    m_pluginContext->_bind(m_document.get(), m_history.get(), m_selection.get(),
                          m_eventBus.get(), &m_viewport->getCamera(), &m_meshesDirty,
                          &m_inSketchMode);
    materializr::force_link::linkAll();
    PluginRegistry::instance().initAll(*m_pluginContext);
}

Application::~Application() {
    PluginRegistry::instance().shutdownAll();
    m_backgroundRenderer.reset();
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

// Resolve a TTF shipped in assets/fonts against the layouts we run from:
//   1. <exe>/../share/materializr/fonts/  (AppImage)
//   2. <exe>/../Resources/assets/fonts/   (macOS .app bundle)
//   3. <exe>/../assets/fonts/             (dev: binary in build/)
//   4. <exe>/assets/fonts/                (Windows portable zip: assets next to exe)
//   5. <cwd>/assets/fonts/                (dev: launched from repo root)
// Returns "" when the font isn't found anywhere — callers degrade gracefully.
std::string Application::resolveBundledFont(const std::string& fname) const {
    char exePath[4096];
    std::string exeDir;
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(nullptr, exePath, sizeof(exePath) - 1);
    if (n > 0) {
        exePath[n] = '\0';
        std::string p(exePath);
        auto slash = p.find_last_of("\\/");
        if (slash != std::string::npos) exeDir = p.substr(0, slash);
    }
#elif defined(__APPLE__)
    // No /proc on macOS — ask dyld for the executable path. The buffer is sized
    // generously; _NSGetExecutablePath fills it and NUL-terminates on success.
    uint32_t n = sizeof(exePath);
    if (_NSGetExecutablePath(exePath, &n) == 0) {
        std::string p(exePath);
        auto slash = p.find_last_of('/');
        if (slash != std::string::npos) exeDir = p.substr(0, slash);
    }
#else
    ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (n > 0) {
        exePath[n] = '\0';
        std::string p(exePath);
        auto slash = p.find_last_of('/');
        if (slash != std::string::npos) exeDir = p.substr(0, slash);
    }
#endif
    const std::string candidates[] = {
        exeDir + "/../share/materializr/fonts/" + fname,
        exeDir + "/../Resources/assets/fonts/" + fname,
        exeDir + "/../assets/fonts/" + fname,
        exeDir + "/assets/fonts/" + fname,
        "assets/fonts/" + fname,
    };
    for (const auto& path : candidates) {
        if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
            std::fclose(f);
            return path;
        }
    }
    return std::string();
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
    // loadAppSettings() ran before the ImGui context existed, so its
    // applyAppSettings couldn't reach io yet — push the loaded double-click
    // window now that there's a context.
    io.MouseDoubleClickTime = m_doubleClickTime;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.08f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.14f, 0.18f, 1.0f);

    // HiDPI / touch scaling. 1.0 on desktop (no change); on a tablet this scales
    // all padding/spacing/widget sizes so buttons are finger-sized. The font is
    // loaded at the matching size below so text stays crisp (not just upscaled).
    const float uiScale = m_window ? m_window->uiScale() : 1.0f;
    if (uiScale != 1.0f) style.ScaleAllSizes(uiScale);  // scales padding/spacing/scrollbar/grab
    if (materializr::touchMode()) {
        // Touch has no hover; a tooltip can only appear while a finger is held on
        // a widget. Drop the stationary gate (a fingertip is already stationary)
        // and shorten the delays so a brief press-and-hold reveals it.
        style.HoverStationaryDelay = 0.0f;
        style.HoverDelayShort = 0.15f;
        style.HoverDelayNormal = 0.30f;
        // Fatten resize hit targets for fingers. The dock splitter the panels
        // resize against is only a couple px wide (even ×uiScale), so grabbing
        // it on a touchscreen is a near-miss; widen it and add a general touch
        // hit-padding so window borders / grips are reachable too. (Kept modest
        // — TouchExtraPadding grows ALL reactive boxes, so overgrowing it makes
        // overlapping widgets fight for the touch.)
        style.DockingSeparatorSize = 12.0f;
        style.TouchExtraPadding = ImVec2(8.0f, 8.0f);
    }

    // Swap ImGui's default ProggyClean for JetBrains Mono — slashed zero,
    // distinct 0/8/B/6, designed for engineering UIs. resolveBundledFont() tries
    // the AppImage, macOS .app Resources/, dev-build, and cwd layouts (see its
    // candidate list). Falls through to the bundled default if the TTF isn't
    // present, so a font miss never bricks the UI.
    {
        std::string path = resolveBundledFont("JetBrainsMono-Regular.ttf");
        if (!path.empty()) {
            ImFont* fnt = io.Fonts->AddFontFromFileTTF(path.c_str(), 15.0f * uiScale);
            if (fnt) std::fprintf(stderr, "Loaded font: %s\n", path.c_str());
        }
        // If nothing loaded, ImGui will lazily fall back to its baked-in default.
    }

    ImGui_ImplSDL2_InitForOpenGL(m_window->handle(), m_window->glContext());
#if defined(__ANDROID__)
    ImGui_ImplOpenGL3_Init("#version 300 es");
#else
    ImGui_ImplOpenGL3_Init("#version 330");
#endif
}

void Application::shutdownImGui() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
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
    if (!m_sectionView->initialize()) {
        std::fprintf(stderr, "Failed to initialize section view\n");
    }
    // Plugin-provided render passes (e.g. ConstructionPlanePlugin's plane
    // renderer). Each pass declares its own initialize() callback — run them
    // on the GL thread now, before the first frame, so the plugin can compile
    // shaders / allocate GL resources.
    for (auto& pass : materializr::PluginRegistry::instance().renderPasses()) {
        if (pass.initialize && !pass.initialize()) {
            std::fprintf(stderr, "Failed to initialize render pass: %s\n",
                         pass.name.c_str());
        }
    }

    // Create a demo box so there's something to see (a 20 mm cube) — but only
    // on a truly empty launch. If loadAppSettings already auto-opened the
    // user's last project, the document is populated and dropping the demo
    // box in on top would surprise them.
    if (m_document->getAllBodyIds().empty()) {
        TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
        m_document->addBody(box, "Demo Box");
        m_meshesDirty = true;
    }

    // Frame whatever the document holds — the demo box on a fresh launch, or
    // the auto-opened project's bodies — so nothing is clipped by the default
    // camera distance.
    try {
        Bnd_Box bbox;
        for (int id : m_document->getAllBodyIds()) {
            try { BRepBndLib::Add(m_document->getBody(id), bbox); } catch (...) {}
        }
        if (!bbox.IsVoid()) {
            double x0, y0, z0, x1, y1, z1;
            bbox.Get(x0, y0, z0, x1, y1, z1);
            m_viewport->getCamera().zoomToFit(
                glm::vec3(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(z0)),
                glm::vec3(static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(z1)));
        }
    } catch (...) {}

    m_renderersReady = true;
}

void Application::setupCommands() {
    // Commands are now registered by plugins via PluginRegistry.
}

void Application::showThreadsLastToast() {
    m_toastText = "Threads are applied LAST. Delete the Thread step, make "
                  "this change, then re-thread.";
    m_toastExpiry = ImGui::GetTime() + 5.0;
}

void Application::showToast(const std::string& text, double seconds) {
    m_toastText = text;
    m_toastExpiry = ImGui::GetTime() + seconds;
}

void Application::renderTransientToast() {
    if (m_toastText.empty()) return;
    if (ImGui::GetTime() > m_toastExpiry) { m_toastText.clear(); return; }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
               vp->WorkPos.y + 80.0f),
        ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.35f, 0.18f, 0.10f, 1.0f));
    if (ImGui::Begin("##toast", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoInputs)) {
        ImGui::PushTextWrapPos(420.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.6f, 1.0f), "%s",
                           m_toastText.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

materializr::IopContext Application::iopContext() {
    return materializr::IopContext{
        *m_document, *m_history, *m_selection,
        [this] { m_meshesDirty = true; },
        [this](float f, const char* l) { return renderProgressFrame(f, l); },
        [this](std::function<void()> t) { m_deferredHeavyTask = std::move(t); }};
}

// Seed the placement rotation (shared by the Text and SVG tools) so the
// artwork reads upright in the CURRENT view — some sketch planes have
// their 2D axes pointing away from the camera's right/up, and unrotated
// placements came out sideways or upside-down. Projects the camera's
// right vector into sketch space and snaps to the nearest 90°.
void Application::seedUprightPlacementAngle() {
    if (!m_activeSketch || !m_viewport || !m_sketchTool) return;
    const gp_Ax3& ax = m_activeSketch->getPlane().Position();
    glm::vec3 xd(ax.XDirection().X(), ax.XDirection().Y(),
                 ax.XDirection().Z());
    glm::vec3 yd(ax.YDirection().X(), ax.YDirection().Y(),
                 ax.YDirection().Z());
    const Camera& cam = m_viewport->getCamera();
    glm::vec3 fwd = glm::normalize(cam.getTarget() - cam.getPosition());
    glm::vec3 right = glm::normalize(glm::cross(fwd, cam.getUp()));
    glm::vec2 d(glm::dot(right, xd), glm::dot(right, yd));
    if (glm::length(d) > 1e-4f) {
        float aDeg = glm::degrees(std::atan2(d.y, d.x));
        m_sketchTool->setTextAngle(
            static_cast<int>(std::round(aDeg / 90.0f)) * 90);
    }
}

void Application::cancelActiveIops() {
    auto ctx = iopContext();
    for (auto* c : m_iops)
        if (c->active()) c->cancel(ctx);
}

bool Application::anyInteractivePreviewActive() const {
    return anyIopActive() || m_extruding || m_pushPullActive ||
           m_patternActive || m_resizeCylActive || m_threadActive;
}

void Application::cancelAllInteractivePreviews() {
    cancelActiveIops();
    // Legacy history-replay previews write the document every frame; a
    // controller preview running beside one corrupts both restore paths.
    if (m_extruding) cancelInteractiveExtrude();
    if (m_pushPullActive) cancelPushPull();
    if (m_patternActive) cancelPattern();
    if (m_resizeCylActive) cancelResizeCylindrical();
    if (m_threadActive) cancelThread();
    // Fillet / chamfer preview — was missing from this list, so switching
    // tools mid-fillet left the previewed body stuck (the new op then
    // snapshotted it as its "pre-state" and Cancel restored the preview,
    // not the original). (Steve: "switching tools, the action that was
    // never committed gets a weird half-cancel I can't undo".)
    if (m_edgeOpActive) cancelInteractiveEdgeOp();
    if (m_moveFaceActive) cancelMoveFace();
}

void Application::beginIop(materializr::InteractiveOpController& ctl) {
    cancelAllInteractivePreviews();
    ctl.begin(iopContext());
}

void Application::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    // Touch tooltip timeout. A finger lift leaves io.MousePos parked on the last
    // tapped widget, so its tooltip hangs forever (no hover-out on touch). If the
    // pointer hasn't moved for 15 s with no button down, blank MousePos for this
    // frame so nothing is hovered and the tip clears; the next touch restores it.
    if (materializr::touchMode()) {
        ImGuiIO& io = ImGui::GetIO();
        bool buttonDown = io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2];
        bool moved = std::abs(io.MousePos.x - m_tipLastMouseX) > 0.5f ||
                     std::abs(io.MousePos.y - m_tipLastMouseY) > 0.5f;
        if (moved || buttonDown) {
            m_tipLastMouseX = io.MousePos.x;
            m_tipLastMouseY = io.MousePos.y;
            m_tipStationarySince = ImGui::GetTime();
        } else if (ImGui::GetTime() - m_tipStationarySince > 15.0) {
            io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX); // ImGui "no mouse" sentinel
        }
    }
    ImGui::NewFrame();
}

void Application::endFrame() {
    // Long-press feedback ring: a circle that fills as a stationary one-finger
    // press approaches the context-menu threshold, so the gesture is discoverable
    // and the user knows when to lift. Drawn over everything via the foreground
    // list; vanishes the moment the press moves (it became a drag) or lifts.
    if (materializr::touchMode() && m_window) {
        float hx = 0.0f, hy = 0.0f;
        float hp = m_window->holdProgress(hx, hy);
        if (hp > 0.0f) {
            float s = m_window->uiScale();
            float r = 16.0f * s;
            auto* dl = ImGui::GetForegroundDrawList();
            dl->AddCircle(ImVec2(hx, hy), r, IM_COL32(255, 255, 255, 60), 0, 2.0f * s);
            const float a0 = -1.5707963f;                 // start at 12 o'clock
            dl->PathArcTo(ImVec2(hx, hy), r, a0, a0 + hp * 6.2831853f, 48);
            dl->PathStroke(IM_COL32(120, 180, 255, 235), 0, 3.0f * s);
            if (hp >= 1.0f)
                dl->AddCircleFilled(ImVec2(hx, hy), 4.0f * s, IM_COL32(120, 180, 255, 235));
        }
    }
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    // Raise/dismiss the Android soft keyboard to match the focused text field.
    if (m_window) m_window->updateTextInput(ImGui::GetIO().WantTextInput || m_softKeyboardForced);
}

void Application::renderSplashFrame(const char* status) {
    // One self-contained frame shown while startup blocks (auto-opening a
    // big project takes ~10 s on slower machines — this used to be a blank
    // window). Polls events so the WM doesn't flag us unresponsive.
    if (!m_window) return;

    auto drawOnce = [&]() {
        m_window->pollEvents();
        int fbw = 0, fbh = 0;
        m_window->framebufferSize(fbw, fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.075f, 0.082f, 0.11f, 1.0f); // matches the app background
        glClear(GL_COLOR_BUFFER_BIT);

        beginFrame();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::Begin("##splash", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings);

        const char* title = "M A T E R I A L I Z R";
        char ver[48];
        std::snprintf(ver, sizeof(ver), "version %s", MATERIALIZR_VERSION);

        ImGui::SetWindowFontScale(2.2f);
        ImVec2 ts = ImGui::CalcTextSize(title);
        ImGui::SetCursorPos(ImVec2((vp->WorkSize.x - ts.x) * 0.5f,
                                   vp->WorkSize.y * 0.40f));
        ImGui::TextColored(materializr::accentText(), "%s", title);
        ImGui::SetWindowFontScale(1.0f);

        ImVec2 vs = ImGui::CalcTextSize(ver);
        ImGui::SetCursorPos(ImVec2((vp->WorkSize.x - vs.x) * 0.5f,
                                   vp->WorkSize.y * 0.40f + ts.y * 2.2f + 8.0f));
        ImGui::TextDisabled("%s", ver);

        // Status line with a marching-dots heartbeat.
        int dots = static_cast<int>(ImGui::GetTime() * 3.0) % 4;
        char line[128];
        std::snprintf(line, sizeof(line), "%s%.*s", status, dots, "...");
        ImVec2 ls = ImGui::CalcTextSize(line);
        ImGui::SetCursorPos(ImVec2((vp->WorkSize.x - ls.x) * 0.5f,
                                   vp->WorkSize.y * 0.40f + ts.y * 2.2f + 40.0f));
        ImGui::Text("%s", line);

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        endFrame();
        m_window->swapBuffers();
    };

    // Render the FIRST splash frame to both buffers in the double-buffered swap
    // chain. The back buffer is undefined until the first swap; if the WM
    // presents the window in that gap you get an intermittent black flash before
    // the text. Priming both removes it. Later calls already have defined
    // content behind them, so a single pass is enough.
    drawOnce();
    if (!m_splashPrimed) {
        drawOnce();
        m_splashPrimed = true;
    }
}

void Application::drawIndeterminateBar() {
    // A segment sweeping left → right (marquee), not a bouncing fill. Used for
    // work with no readable progress (the projection boolean, a thread sweep).
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float fullW = std::max(ImGui::GetContentRegionAvail().x, 260.0f);
    float barH = ImGui::GetFrameHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + fullW, p0.y + barH),
                      ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
    float t = static_cast<float>(ImGui::GetTime());
    float segW = fullW * 0.28f;
    float x = std::fmod(t * fullW * 0.7f, fullW + segW) - segW; // wraps L→R
    float xa = std::max(0.0f, x);
    float xb = std::min(fullW, x + segW);
    if (xb > xa)
        dl->AddRectFilled(ImVec2(p0.x + xa, p0.y), ImVec2(p0.x + xb, p0.y + barH),
                          ImGui::GetColorU32(ImGuiCol_PlotHistogram), 3.0f);
    ImGui::Dummy(ImVec2(fullW, barH));
}

bool Application::renderProgressFrame(float fraction, const char* label) {
    // Called from inside a long op's execute() via the progress reporter. Must
    // run BETWEEN main frames (the op is deferred to m_deferredHeavyTask), so a
    // fresh ImGui frame here is safe. fraction==0 marks a new op → reset the
    // cancel latch so a prior cancel doesn't carry over. (fraction<0 is the
    // indeterminate spinner and must NOT reset it.)
    if (fraction == 0.0f) m_progressCancelled = false;
    if (m_progressCancelled || !m_window) return m_progressCancelled;

    m_window->pollEvents();
    int fbw = 0, fbh = 0;
    m_window->framebufferSize(fbw, fbh);   // SDL backend (upstream used glfw here)
    glViewport(0, 0, fbw, fbh);
    glClearColor(0.075f, 0.082f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    beginFrame();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float boxW = 440.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - boxW) * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.42f));
    ImGui::SetNextWindowSize(ImVec2(boxW, 0));
    ImGui::Begin("##progress", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextColored(materializr::accentText(), "Working\xE2\x80\xA6");
    ImGui::Spacing();
    if (label && label[0]) ImGui::TextWrapped("%s", label);
    ImGui::Spacing();
    if (fraction < 0.0f) {
        drawIndeterminateBar();
    } else {
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(fraction * 100.0f + 0.5f));
        ImGui::ProgressBar(fraction, ImVec2(-1, 0), pct);
    }
    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(110, 0)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_progressCancelled = true;
    }
    ImGui::End();
    endFrame();
    m_window->swapBuffers();
    // GL just ran — restore the FPU mode before control returns to the OCCT op.
    resetFpuForOcct();
    return m_progressCancelled;
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

void Application::renderMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
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
            if (ImGui::MenuItem("Exit", "Alt+F4")) m_window->requestClose(true);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            // Disabled while a legacy preview is live: those previews
            // undo/re-push their op per frame, and an outside undo pops the
            // preview op so the preview's NEXT cycle pops the user's last
            // COMMITTED op instead — which then gets erased for good when
            // the preview pushes over the redo tail. (How "pull, confirm,
            // pull the other way" ate the first body.)
            const bool histLocked = anyInteractivePreviewActive();
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false,
                                !histLocked && m_history->canUndo())) {
                const Operation* undone =
                    m_history->getStep(m_history->currentStep());
                m_history->undo(*m_document);
                // Keep a sketch-driven body in sync after undoing a sketch edit
                // (the SketchEditOp undo only reverts geometry; the cascade did
                // the body). Mirrors the keyboard Ctrl+Z path.
                if (m_inSketchMode && m_activeSketch && m_activeSketchId >= 0)
                    cascadeFromSketchEdit(m_activeSketchId);
                if (auto* st = dynamic_cast<const materializr::SketchTransformOp*>(undone))
                    cascadeFromSketchEdit(st->getSketchId());
                m_meshesDirty = true;
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false,
                                !histLocked && m_history->canRedo())) {
                m_history->redo(*m_document);
                const Operation* redone =
                    m_history->getStep(m_history->currentStep());
                if (m_inSketchMode && m_activeSketch && m_activeSketchId >= 0)
                    cascadeFromSketchEdit(m_activeSketchId);
                if (auto* st = dynamic_cast<const materializr::SketchTransformOp*>(redone))
                    cascadeFromSketchEdit(st->getSketchId());
                m_meshesDirty = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Reset Camera", "Home")) m_viewport->getCamera().reset();
            // The F shortcut's menu twin — and the only way to frame on touch.
            if (ImGui::MenuItem("Frame Selection", "F")) frameSelection();
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
            // Plugin-contributed Help items (e.g. the Tutorial's "Getting
            // Started"). Lets a plugin add a launcher without Application
            // knowing about it. See renderPluginMenuItems.
            renderPluginMenuItems("Help");
            ImGui::Separator();
            if (ImGui::MenuItem("About Materializr...")) m_aboutDialog->setVisible(true);
            ImGui::EndMenu();
        }
        // Touch: soft-keyboard toggle, right-aligned. Forces the system keyboard
        // up so you can type into the focused field (rename, save, dimensions);
        // tap again to dismiss. Check mark shows when it's forced on. (Window mode
        // — immersive vs. windowed in a desktop dock — is automatic; see
        // MaterializrActivity, so there's no toggle here.)
        if (materializr::touchMode()) {
            const char* kb = "Keyboard";
            float btnW = ImGui::CalcTextSize(kb).x + ImGui::GetFrameHeight() +
                         ImGui::GetStyle().ItemSpacing.x * 2.0f;
            float x = ImGui::GetWindowWidth() - btnW;
            if (x > ImGui::GetCursorPosX()) ImGui::SameLine(x);
            if (ImGui::MenuItem(kb, nullptr, m_softKeyboardForced))
                m_softKeyboardForced = !m_softKeyboardForced;
        }
        ImGui::EndMainMenuBar();
    }
}

void Application::renderSmallScreenWarning() {
    if (m_smallScreenWarned || m_smallScreenAck) return;
    ImGuiIO& io = ImGui::GetIO();
    // Effective UI canvas in logical points (HiDPI / touch scale is already baked
    // into DisplaySize). The reference tablet sits around 893x558 and is roomy;
    // phones land well under, especially in height. Tunable constants.
    const bool small = io.DisplaySize.x < 640.0f || io.DisplaySize.y < 470.0f;
    if (!small) return;

    if (!ImGui::IsPopupOpen("Small screen")) ImGui::OpenPopup("Small screen");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Small screen", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(uiW(440));
        ImGui::TextWrapped(
            "Materializr is designed for tablets and larger displays. On a small "
            "screen the panels and toolbars are cramped and some controls may be "
            "hard to reach — a tablet or larger is strongly recommended.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        static bool dontShow = false;
        ImGui::Checkbox("Don't show this again", &dontShow);
        ImGui::Spacing();
        if (ImGui::Button("OK", uiSz(140, 0))) {
            m_smallScreenAck = true;                 // gone for this run
            if (dontShow) { m_smallScreenWarned = true; saveAppSettings(); }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
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

void Application::renderPanelCollapseHandles() {
    // Touch-only edge tabs that collapse/restore each docked side column. They
    // anchor to the panel/viewport boundary, which slides to the screen edge
    // once a side is collapsed — so a hidden panel still has a visible pull-tab.
    // Desktop uses View > Hide Panels / F9 instead, so this is touch-gated.
    if (!materializr::touchMode()) return;
    if (m_viewportWinW <= 0.0f || m_viewportWinH <= 0.0f) return;

    const float hw = uiW(22.0f);                          // tab width
    const float hh = uiW(80.0f);                          // tab height (touch target)
    const float cy = m_viewportWinY + m_viewportWinH * 0.5f - hh * 0.5f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    // Left tab — flush against the viewport's left edge, sitting just INSIDE the
    // viewport (not over the Tools panel, whose space is tight). When the panel
    // is collapsed the viewport edge is the screen edge, so the tab hugs it.
    // The arrow points the way the tap moves the panel: '<' collapses it toward
    // the edge, '>' pulls it back out.
    {
        float x = m_viewportWinX;
        if (x < vp->WorkPos.x) x = vp->WorkPos.x;
        ImGui::SetNextWindowPos(ImVec2(x, cy));
        ImGui::SetNextWindowSize(ImVec2(hw, hh));
        ImGui::Begin("##collapseLeft", nullptr, flags);
        if (ImGui::Button(m_leftPanelHidden ? ">" : "<", ImVec2(hw, hh))) {
            m_leftPanelHidden = !m_leftPanelHidden;
            saveAppSettings();
        }
        ImGui::End();
    }
    // Right tab — flush against the viewport's right edge, sitting just INSIDE
    // the viewport (not over the Items/Properties column).
    {
        float x = m_viewportWinX + m_viewportWinW - hw;
        const float maxX = vp->WorkPos.x + vp->WorkSize.x - hw;
        if (x > maxX) x = maxX;
        ImGui::SetNextWindowPos(ImVec2(x, cy));
        ImGui::SetNextWindowSize(ImVec2(hw, hh));
        ImGui::Begin("##collapseRight", nullptr, flags);
        if (ImGui::Button(m_rightPanelHidden ? "<" : ">", ImVec2(hw, hh))) {
            m_rightPanelHidden = !m_rightPanelHidden;
            saveAppSettings();
        }
        ImGui::End();
    }

    ImGui::PopStyleVar();
}

void Application::loadAppSettings() {
    AppSettings s = SettingsIO::load(SettingsIO::defaultPath());
    applyAppSettings(s);

    // CLI --safe-mode: stomp anything that could a) crash a driver on a hot
    // restart, b) hang the launch by reopening a large project before the
    // user can intervene, or c) reach for the network during recovery.
    // Persist the safe values so the next normal launch is also recovered
    // without further action.
    if (m_safeMode) {
        m_lightAmbient            = 0.40f;
        m_lightHeadlight          = false;
        m_lightFill               = true;
        m_msaaSamples             = 0;   // disable multisample buffers entirely
        m_meshQuality             = 0;   // Low — coarsest tessellation
        m_autosaveEnabled         = false;
        m_autoOpenLastProject     = false;
        m_checkForUpdatesOnLaunch = false;
        std::fprintf(stdout,
                     "[safe-mode] Rendering reset to safe defaults "
                     "(MSAA off, mesh quality Low); autosave, "
                     "auto-open-last-project, and update-check disabled.\n");
        saveAppSettings();
    }

    applyRenderingSettings();
    m_meshesDirty = true; // re-tessellate at the loaded quality

    // Auto-open the previously-open project. Suppressed by --safe-mode (the
    // toggle was forced off above), and only reached otherwise if the project
    // wasn't closed via File → Close Project before quit (closeProject clears
    // the path in settings).
    if (m_autoOpenLastProject && !s.lastProjectPath.empty()) {
        // Defer to the first main-loop iteration so the load runs in the
        // between-frames slot where a loading bar can pump (and the window is
        // already up) — otherwise the synchronous load froze startup with the
        // OS flagging "not responding".
        std::string p = s.lastProjectPath;
        m_deferredHeavyTask = [this, p]() { loadProjectWithProgress(p); };
    }

    // Auto check for updates: hit the GitHub releases API and, if a newer
    // tag is available, pre-populate the update popup so it pops on the
    // first frame. UpdateChecker has a 5-second connect / 10-second total
    // timeout, so the worst case here is a few seconds of startup delay on
    // a broken network. Suppressed by --safe-mode.
    if (m_checkForUpdatesOnLaunch && !m_safeMode) {
        // Run on a worker thread — the synchronous version blocked startup for
        // up to its 10 s network timeout ("not responding"). The main loop
        // polls m_updateCheckFuture each frame and pops the popup when it's in.
        m_updateCheckFuture = std::async(std::launch::async, []() {
            auto t0 = std::chrono::steady_clock::now();
            auto r = UpdateChecker::check("materializr-cad", "materializr");
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr, "[update-check] %lld ms (ok=%d)\n",
                         static_cast<long long>(dt), r.ok ? 1 : 0);
            return r;
        });
    }
}

void Application::applyRenderingSettings() {
    LightingParams lp;
    lp.ambient = m_lightAmbient;
    lp.headlight = m_lightHeadlight;
    lp.fill = m_lightFill;
    m_shapeRenderer->setLighting(lp);
    m_viewport->setSamples(m_msaaSamples);
    if (m_selectionHighlight) m_selectionHighlight->setLineWidth(m_selectionLineWidth);
    if (m_sketchRenderer) m_sketchRenderer->setLineWidth(m_sketchLineWidth);
}

void Application::meshQualityParams(float& deflection, float& angularDeflection) const {
    // Absolute linear deflection (mm) and angular deflection (radians). Lower
    // values produce denser, smoother meshes.
    switch (m_meshQuality) {
        case 0: deflection = 0.50f; angularDeflection = 0.50f; break; // Low
        case 2: deflection = 0.03f; angularDeflection = 0.15f; break; // High
        case 3: deflection = 0.01f; angularDeflection = 0.10f; break; // Ultra
        case 1:
        default: deflection = 0.10f; angularDeflection = 0.30f; break; // Medium
    }
}

AppSettings Application::currentSettings() const {
    AppSettings s;
    s.theme = (m_themeManager->getTheme() == Theme::Light) ? 1 : 0;
    s.touchMode = m_touchMode;
    s.orbitButton = m_orbitButton;
    s.panButton = m_panButton;
    s.levelOrbit = m_viewport->getCamera().isLevelOrbit();
    s.mouseSensitivity = m_viewport->getCamera().getMouseSensitivity();
    s.autosaveEnabled = m_autosaveEnabled;
    s.autosaveIntervalSec = static_cast<int>(m_autosaveIntervalSec);
    s.invertCubeDrag = m_invertCubeDrag;
    s.doubleClickTimeSec = m_doubleClickTime;
    s.lightAmbient = m_lightAmbient;
    s.lightHeadlight = m_lightHeadlight;
    s.lightFill = m_lightFill;
    s.msaaSamples = m_msaaSamples;
    s.meshQuality = m_meshQuality;
    s.selectionLineWidth = m_selectionLineWidth;
    s.sketchLineWidth = m_sketchLineWidth;
    s.sketchGridOpacity = m_sketchGridOpacity;
    s.sketchGridThickness = m_sketchGridThickness;
    s.smallScreenWarned = m_smallScreenWarned;
    s.leftPanelHidden = m_leftPanelHidden;
    s.rightPanelHidden = m_rightPanelHidden;
    s.showTools = m_showTools;
    s.showInteractions = m_showInteractions;
    s.showHistory = m_showHistory;
    s.showItems = m_showItems;
    s.showProperties = m_showProperties;
    s.touchOrbitSens = m_touchOrbitSens;
    s.touchPanSens = m_touchPanSens;
    s.touchZoomSens = m_touchZoomSens;
    s.showToolbarTooltips = m_showToolbarTooltips;
    s.autoOpenLastProject = m_autoOpenLastProject;
    s.recentProjects = m_recentProjects;
    s.lastProjectPath = m_currentProjectPath; // empty after closeProject()
    s.lastFileDir = materializr::FileDialogs::getLastDir();
    s.checkForUpdatesOnLaunch = m_checkForUpdatesOnLaunch;
    s.snapToGrid = m_snapToGrid;
    s.sketchGridStep = m_sketchGridStep;
    // Mirror the live sketch-tool inference level back into the saved settings
    // so cycling the toolbar Full→Reduced→Off button persists across launches.
    s.inferenceLevel = m_sketchTool
        ? static_cast<int>(m_sketchTool->getInferenceLevel()) : 0;
    s.showInferenceToolbarToggle = m_showInferenceToolbarToggle;
    s.angleSnapDeg = m_sketchTool ? m_sketchTool->getAngleSnapDeg() : 15;
    return s;
}

// Push a settings struct onto the live members. Preferences only — session
// state (lastProjectPath) and one-shot startup actions (auto-open, update
// check) are deliberately not handled here; loadAppSettings/importSettings
// layer those on top as appropriate. Camera buttons land on both the active
// and the Settings-dialog "staged" copies so an import takes effect at once.
void Application::applyAppSettings(const AppSettings& s) {
    m_themeManager->setTheme(s.theme == 1 ? Theme::Light : Theme::Dark);
    // Touch mode drives the UI scale and input/UX model. The scale + fonts are
    // baked at startup (resolved early in the ctor), so a change here fully
    // applies on the next launch; keeping the global in sync means everything
    // reads a consistent value within the run.
    materializr::setTouchMode(s.touchMode);
    m_touchMode = s.touchMode;   // staged value for the Settings dialog
    // Camera button bindings are honoured on every platform. Android defaults to
    // trackpad mode (AppSettings sets orbit/pan = Left there) so one-finger touch
    // orbits out of the box, but an attached mouse/trackpad can be rebound via the
    // Settings dialog and the choice persists — touch pan/zoom stays on two-finger
    // gestures regardless, and sketch-mode drawing still overrides orbit.
    m_orbitButton = s.orbitButton;
    m_panButton = s.panButton;
    m_settingsOrbitButton = s.orbitButton;
    m_settingsPanButton = s.panButton;
    m_viewport->getCamera().setLevelOrbit(s.levelOrbit);
    m_viewport->getCamera().setMouseSensitivity(s.mouseSensitivity);
    m_autosaveEnabled = s.autosaveEnabled;
    m_autosaveIntervalSec = static_cast<float>(s.autosaveIntervalSec);
    m_invertCubeDrag = s.invertCubeDrag;
    m_doubleClickTime = s.doubleClickTimeSec;
    if (ImGui::GetCurrentContext())
        ImGui::GetIO().MouseDoubleClickTime = m_doubleClickTime;
    m_lightAmbient = s.lightAmbient;
    m_lightHeadlight = s.lightHeadlight;
    m_lightFill = s.lightFill;
    m_msaaSamples = s.msaaSamples;
    m_meshQuality = s.meshQuality;
    m_selectionLineWidth = s.selectionLineWidth;
    m_sketchLineWidth = s.sketchLineWidth;
    m_sketchGridOpacity = s.sketchGridOpacity;
    m_sketchGridThickness = s.sketchGridThickness;
    m_smallScreenWarned = s.smallScreenWarned;
    m_leftPanelHidden = s.leftPanelHidden;
    m_rightPanelHidden = s.rightPanelHidden;
    m_showTools = s.showTools;
    m_showInteractions = s.showInteractions;
    m_showHistory = s.showHistory;
    m_showItems = s.showItems;
    m_showProperties = s.showProperties;
    m_touchOrbitSens = s.touchOrbitSens;
    m_touchPanSens = s.touchPanSens;
    m_touchZoomSens = s.touchZoomSens;
    m_showToolbarTooltips = s.showToolbarTooltips;
    m_autoOpenLastProject = s.autoOpenLastProject;
    m_recentProjects = s.recentProjects;
    m_checkForUpdatesOnLaunch = s.checkForUpdatesOnLaunch;
    m_snapToGrid = s.snapToGrid;
    m_sketchGridStep = s.sketchGridStep;
    m_showInferenceToolbarToggle = s.showInferenceToolbarToggle;
    materializr::FileDialogs::setLastDir(s.lastFileDir);
    if (m_sketchTool) {
        using IL = SketchTool::InferenceLevel;
        IL lvl = (s.inferenceLevel == 1) ? IL::Reduced
               : (s.inferenceLevel == 2) ? IL::Off
               : (s.inferenceLevel == 3) ? IL::Max
                                         : IL::Full;
        m_sketchTool->setInferenceLevel(lvl);
        m_sketchTool->setAngleSnapDeg(s.angleSnapDeg);
    }
    // Mirror onto the toolbar so the in-sketch grid controls show the loaded
    // values right away rather than waiting for the first frame's sync.
    if (m_toolbar) {
        m_toolbar->setSnapToGrid(s.snapToGrid);
        m_toolbar->setGridStep(s.sketchGridStep);
        m_toolbar->setShowInferenceToggle(s.showInferenceToolbarToggle);
    }
}

void Application::saveAppSettings() {
    SettingsIO::save(SettingsIO::defaultPath(), currentSettings());
}

void Application::exportSettings() {
    FileDialogs::saveFile("Export Settings", "materializr-settings.json",
        {{"JSON Files", "*.json"}},
        [this](const std::string& path) {
            if (path.empty()) return;
            if (SettingsIO::exportJson(path, currentSettings()))
                std::fprintf(stdout, "Exported settings to %s\n", path.c_str());
            else
                std::fprintf(stderr, "Failed to export settings to %s\n", path.c_str());
        });
}

void Application::importSettings() {
    FileDialogs::openFile("Import Settings",
        {{"JSON Files", "*.json"}},
        [this](const std::string& path) {
            if (path.empty()) return;
            bool ok = false;
            AppSettings s = SettingsIO::importJson(path, &ok);
            if (!ok) {
                std::fprintf(stderr, "Failed to import settings from %s\n", path.c_str());
                return;
            }
            // Apply the imported preferences live, then persist them to the
            // regular settings file so they survive the next launch. Theme is
            // applied explicitly since applyAppSettings only stages it.
            applyAppSettings(s);
            m_themeManager->apply();
            m_orbitButton = m_settingsOrbitButton; // commit staged camera buttons
            m_panButton = m_settingsPanButton;
            applyRenderingSettings();
            m_meshesDirty = true; // re-tessellate at the imported quality
            saveAppSettings();
            std::fprintf(stdout, "Imported settings from %s\n", path.c_str());
        });
}



void Application::handleToolAction(int action) {
    ToolAction a = static_cast<ToolAction>(action);
    switch (a) {
        case ToolAction::StartSketch: enterSketchMode(); break;
        // Each plane is built so that alignCameraToActiveSketch (camera at +normal,
        // up = plane YDirection) reproduces the matching ViewCube view exactly, in
        // this Y-up world: XY = Top, XZ = Front, YZ = Right. gp_Ax3(origin, normal,
        // xDir); YDirection (the camera up) = normal × xDir.
        // For the explicit base-plane buttons we prime the camera with the
        // canonical Top / Front / Right "up" first — without it, alignCamera
        // ToActiveSketch's continuity-preservation logic snaps the up vector
        // to whichever in-plane axis happened to project from the previous
        // view (e.g. world +X), making XY / XZ visually indistinguishable
        // with an empty viewport. Setting the canonical up here makes each
        // plane land on its CAD-traditional orientation.
        case ToolAction::StartSketchXY: // Top: camera +Y, screen-up = world +Z (user +Y)
            if (m_viewport) m_viewport->getCamera().setUp({0.0f, 0.0f, 1.0f});
            enterSketchOnPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0), gp_Dir(1, 0, 0)))); break;
        case ToolAction::StartSketchXZ: // Front: camera +Z, screen-up = world +Y (user +Z)
            if (m_viewport) m_viewport->getCamera().setUp({0.0f, 1.0f, 0.0f});
            enterSketchOnPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)))); break;
        case ToolAction::StartSketchYZ: // Right: camera +X, screen-up = world +Y (user +Z)
            if (m_viewport) m_viewport->getCamera().setUp({0.0f, 1.0f, 0.0f});
            enterSketchOnPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0), gp_Dir(0, 0, -1)))); break;
        case ToolAction::SketchOnFace: {
            const auto& sel = m_selection->getSelection();
            for (const auto& entry : sel) {
                if (entry.type == SelectionType::Face && !entry.shape.IsNull()) {
                    enterSketchOnFace(TopoDS::Face(entry.shape), entry.bodyId);
                    break;
                }
                // Sketch on a construction plane: same enter-sketch path the
                // XY/XZ/YZ start-sketch toolbar actions use, just with the
                // plane's stored gp_Pln rather than a canonical one.
                if (entry.type == SelectionType::Plane && entry.planeId >= 0) {
                    const auto* p = m_document->getPlane(entry.planeId);
                    if (p) enterSketchOnPlane(p->plane);
                    break;
                }
            }
            break;
        }
        case ToolAction::ExitSketchDiscard: {
            // Rewind history to where the user entered this sketch, then
            // leave sketch mode. Drops every line / circle / arc / etc. the
            // user drew, plus any in-progress placement state.
            if (m_inSketchMode && m_history && m_sketchTool) {
                m_sketchTool->onCancel(); // clear m_isPlacing etc.
                while (m_history->currentStep() > m_sketchEntryHistoryStep &&
                       m_history->canUndo()) {
                    m_history->undo(*m_document);
                }
                m_sketchEntryHistoryStep = -1;
                m_meshesDirty = true;
                // After undo'ing everything we did since entry, also remove
                // the sketch from the document if it ended up empty.
                if (m_activeSketch && m_activeSketch->elementCount() == 0 &&
                    m_activeSketchId >= 0) {
                    m_document->removeSketch(m_activeSketchId);
                }
            }
            // Use the same exit path the close-without-saving flow uses —
            // leaves the camera where it is and clears sketch state.
            if (m_inSketchMode) exitSketchMode();
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
            // "Extrude From" — always creates a new body (Push/Pull is the
            // modify-in-place tool). Priority: selected region(s) > whole
            // sketch > face silhouette. The region branch is what makes a
            // single letter of a text sketch (or the circle inside a
            // rectangle) extrudable on its own — clicking a region selects
            // it, so the explicit pick must win over the whole profile.
            const auto& sel = m_selection->getSelection();
            bool started = false;
            {
                // Collect every selected region of ONE sketch (Ctrl+click
                // several letters → one combined extrude).
                int regionSketch = -1;
                std::vector<int> regionIdxs;
                for (const auto& entry : sel) {
                    if (entry.type != SelectionType::SketchRegion ||
                        entry.sketchId < 0)
                        continue;
                    if (regionSketch < 0) regionSketch = entry.sketchId;
                    if (entry.sketchId == regionSketch)
                        regionIdxs.push_back(entry.subShapeIndex);
                }
                if (regionSketch >= 0 && !regionIdxs.empty()) {
                    auto sketch = m_document->getSketch(regionSketch);
                    if (sketch) {
                        auto regions = sketch->buildRegions();
                        std::vector<TopoDS_Face> profileFaces;
                        for (int idx : regionIdxs) {
                            if (idx < 0 ||
                                idx >= static_cast<int>(regions.size()))
                                continue;
                            if (!regions[idx].face.IsNull())
                                profileFaces.push_back(regions[idx].face);
                        }
                        if (profileFaces.size() == 1) {
                            beginInteractiveExtrude(profileFaces.front(),
                                                    ExtrudeMode::NewBody,
                                                    /*targetBody=*/-1,
                                                    regionSketch);
                            started = true;
                        } else if (profileFaces.size() > 1) {
                            TopoDS_Compound comp;
                            BRep_Builder bb;
                            bb.MakeCompound(comp);
                            for (const auto& f : profileFaces)
                                bb.Add(comp, f);
                            beginInteractiveExtrude(comp,
                                                    ExtrudeMode::NewBody,
                                                    /*targetBody=*/-1,
                                                    regionSketch);
                            started = true;
                        }
                    }
                }
            }
            if (!started) {
                for (const auto& entry : sel) {
                    if (entry.type == SelectionType::Sketch &&
                        entry.sketchId >= 0) {
                        extrudeSketchById(entry.sketchId, ExtrudeMode::NewBody);
                        started = true;
                        break;
                    }
                }
            }
            if (!started) {
                for (const auto& entry : sel) {
                    if (entry.type == SelectionType::Face && !entry.shape.IsNull()) {
                        beginInteractiveExtrude(entry.shape,
                                                ExtrudeMode::NewBody,
                                                /*targetBody=*/-1);
                        break;
                    }
                }
            }
            break;
        }

        case ToolAction::SubtractSketch: {
            // Clicking a sketch in the viewport selects a region, so handle that
            // first; fall back to a whole-sketch selection (from the Items panel).
            const auto& sel = m_selection->getSelection();
            bool started = false;
            for (const auto& entry : sel) {
                if (entry.type == SelectionType::SketchRegion && entry.sketchId >= 0) {
                    subtractSketchRegion(entry.sketchId, entry.subShapeIndex);
                    started = true;
                    break;
                }
            }
            if (!started) {
                for (const auto& entry : sel) {
                    if (entry.type == SelectionType::Sketch && entry.sketchId >= 0) {
                        extrudeSketchById(entry.sketchId, ExtrudeMode::Subtract);
                        break;
                    }
                }
            }
            break;
        }
        case ToolAction::PushPull: {
            beginPushPull();
            break;
        }
        case ToolAction::MoveFace: {
            beginMoveFace();
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
            if (m_inSketchMode) {
                // Side count comes from the toolbar's Polygon popout.
                m_sketchTool->setPolygonSides(m_toolbar->getRequestedPolygonSides());
                m_sketchTool->setMode(SketchToolMode::Polygon);
            }
            break;
        case ToolAction::Trim:
            if (m_inSketchMode) m_sketchTool->setMode(SketchToolMode::Trim);
            break;
        case ToolAction::SketchText:
            if (m_inSketchMode) {
                // First activation: default to the UI font (always bundled).
                if (m_sketchTool->getTextFontPath().empty()) {
                    m_sketchTool->setTextFontPath(
                        resolveBundledFont("JetBrainsMono-Regular.ttf"));
                }
                seedUprightPlacementAngle();
                m_sketchTool->setMode(SketchToolMode::Text);
            }
            break;

        case ToolAction::SketchSvg:
            if (m_inSketchMode) {
                materializr::FileDialogs::openFile(
                    "Import SVG", {{"SVG Files", "*.svg *.SVG"}},
                    [this](const std::string& path) {
                        if (path.empty() || !m_sketchTool) return;
                        materializr::SvgPaths svg;
                        if (!materializr::SvgImport::load(path, svg)) return;
                        m_sketchTool->setSvgPaths(std::move(svg));
                        seedUprightPlacementAngle();
                        m_sketchTool->setMode(SketchToolMode::Svg);
                    });
            }
            break;

        // --- Sketch constraints (opt-in only; nothing autoConstrains) ---
        case ToolAction::SketchConstrainCoincident:
            applySketchConstraint(ConstraintType::Coincident); break;
        case ToolAction::SketchConstrainHorizontal:
            applySketchConstraint(ConstraintType::Horizontal); break;
        case ToolAction::SketchConstrainVertical:
            applySketchConstraint(ConstraintType::Vertical); break;
        case ToolAction::SketchConstrainParallel:
            applySketchConstraint(ConstraintType::Parallel); break;
        case ToolAction::SketchConstrainPerpendicular:
            applySketchConstraint(ConstraintType::Perpendicular); break;
        case ToolAction::SketchConstrainEqual:
            applySketchConstraint(ConstraintType::Equal); break;
        case ToolAction::SketchConstrainFixed:
            applySketchConstraint(ConstraintType::Fixed); break;
        case ToolAction::SketchDimDistance:
            applySketchConstraint(ConstraintType::Distance); break;
        case ToolAction::SketchDimAngle:
            applySketchConstraint(ConstraintType::Angle); break;
        case ToolAction::SketchDimRadius:
            applySketchConstraint(ConstraintType::Radius); break;
        case ToolAction::SketchConstrainTangent:
            applySketchConstraint(ConstraintType::Tangent); break;
        case ToolAction::SketchConstrainConcentric:
            applySketchConstraint(ConstraintType::Concentric); break;

        // --- Sketch element transforms (operate on the Select-mode selection) ---
        // Rotate is handled by the sketch gizmo's ring handle (see Application_
        // Viewport.cpp), not as a toolbar action.
        case ToolAction::SketchCopy:
        case ToolAction::SketchMirror: {
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

        case ToolAction::SketchLinearPattern:
            beginSketchPattern(PatternKind::Linear);
            break;
        case ToolAction::SketchRadialPattern:
            beginSketchPattern(PatternKind::Radial);
            break;
        case ToolAction::SketchCycleInference:
            if (m_sketchTool) {
                using IL = SketchTool::InferenceLevel;
                IL cur = m_sketchTool->getInferenceLevel();
                // Cycle strongest -> weakest, wrapping: Max -> Full -> Reduced -> Off -> Max.
                IL next = cur == IL::Max     ? IL::Full
                        : cur == IL::Full    ? IL::Reduced
                        : cur == IL::Reduced ? IL::Off
                                             : IL::Max;
                m_sketchTool->setInferenceLevel(next);
                // Persist immediately. The settings combo saves on change, but
                // this toolbar button didn't — so a level picked here was lost on
                // restart (Android kills the process on swipe-away, so there's no
                // exit-save to fall back on). inferenceLevel is a saved setting.
                saveAppSettings();
            }
            break;
        case ToolAction::SketchToggleDrawOrigin:
            if (m_sketchTool) {
                using RM = SketchTool::RectMode;
                using CM = SketchTool::CircleMode;
                if (m_sketchTool->getMode() == SketchToolMode::Rectangle)
                    m_sketchTool->setRectMode(
                        m_sketchTool->getRectMode() == RM::Corner ? RM::Center : RM::Corner);
                else if (m_sketchTool->getMode() == SketchToolMode::Circle)
                    m_sketchTool->setCircleMode(
                        m_sketchTool->getCircleMode() == CM::Center ? CM::TwoPoint : CM::Center);
            }
            break;

        case ToolAction::ResetCamera: m_viewport->getCamera().reset(); break;
        case ToolAction::Measure:
            if (m_measureTool) {
                // Activating the tool drops the user at the mode picker. The
                // panel renders three mode buttons (Object / Edge / Point-to-
                // Point) and waits for them to pick one.
                m_measureTool->setMode(MeasureMode::PickMode);
            }
            break;

        case ToolAction::Move: {
            // A selected face turns Move into Move Face — same verb to the user,
            // the selection picks body-vs-face. Trigger whenever a face is in the
            // selection (even if a hole edge / wall is the *primary* pick — those
            // refine hole behavior), so the edge doesn't route us to body-move.
            bool moveFaceSel = false;
            for (const auto& e : m_selection->getSelection())
                if (e.type == SelectionType::Face && !e.shape.IsNull()) { moveFaceSel = true; break; }
            if (moveFaceSel) {
                beginMoveFace();
                break;
            }
            // Bodies / standalone sketches / construction planes all get the
            // Move gizmo — the viewport gizmo-visibility block handles whichever
            // selection type is active. SketchRegion picks count as the parent
            // sketch. For sketches the click "arms" the gizmo for the current
            // sketch id; selection-change clears the arm so the next sketch
            // selection again shows just the toolbar options.
            const bool isPlane =
                m_selection->primaryType() == SelectionType::Plane;
            const bool isAxis  =
                m_selection->primaryType() == SelectionType::Axis;
            if (!m_selection->hasSelectedBodies() &&
                !m_selection->hasSelectedSketches() &&
                !m_selection->hasSelectedSketchRegions() &&
                !isPlane && !isAxis) break;
            m_gizmo->setMode(GizmoMode::Translate);
            m_selection->setNavigationOnly(false);
            for (const auto& e : m_selection->getSelection()) {
                if ((e.type == SelectionType::Sketch ||
                     e.type == SelectionType::SketchRegion) && e.sketchId >= 0) {
                    m_sketchGizmoArmed = true;
                    m_sketchGizmoArmedFor = e.sketchId;
                    break;
                }
                if (e.type == SelectionType::Plane && e.planeId >= 0) {
                    m_planeGizmoArmed = true;
                    m_planeGizmoArmedFor = e.planeId;
                    break;
                }
                if (e.type == SelectionType::Axis && e.axisId >= 0) {
                    m_axisGizmoArmed = true;
                    m_axisGizmoArmedFor = e.axisId;
                    break;
                }
            }
            break;
        }
        case ToolAction::Rotate: {
            // A selected face turns Rotate into a face TILT (the loft engine
            // with a rotation about the face centre — same mechanic as Move).
            {
                bool faceSel = false;
                for (const auto& e : m_selection->getSelection())
                    if (e.type == SelectionType::Face && !e.shape.IsNull()) { faceSel = true; break; }
                if (faceSel) { beginMoveFace(FaceXform::Rotate); break; }
            }
            // Axis doesn't get Rotate — an infinite line has no meaningful
            // rotation handle. Rotate is body / sketch / plane only.
            const bool isPlane =
                m_selection->primaryType() == SelectionType::Plane;
            if (!m_selection->hasSelectedBodies() &&
                !m_selection->hasSelectedSketches() &&
                !m_selection->hasSelectedSketchRegions() &&
                !isPlane) break;
            m_gizmo->setMode(GizmoMode::Rotate);
            m_selection->setNavigationOnly(false);
            for (const auto& e : m_selection->getSelection()) {
                if ((e.type == SelectionType::Sketch ||
                     e.type == SelectionType::SketchRegion) && e.sketchId >= 0) {
                    m_sketchGizmoArmed = true;
                    m_sketchGizmoArmedFor = e.sketchId;
                    break;
                }
                if (e.type == SelectionType::Plane && e.planeId >= 0) {
                    m_planeGizmoArmed = true;
                    m_planeGizmoArmedFor = e.planeId;
                    break;
                }
            }
            break;
        }
        case ToolAction::Scale: {
            // A selected face turns Scale into a face scale (the loft engine,
            // scaling the top about the face centre).
            {
                bool faceSel = false;
                for (const auto& e : m_selection->getSelection())
                    if (e.type == SelectionType::Face && !e.shape.IsNull()) { faceSel = true; break; }
                if (faceSel) { beginMoveFace(FaceXform::Scale); break; }
            }
            // Scale-on-sketch is a no-op (the plane is 2D-infinite), so we
            // keep this body-only.
            if (!m_selection->hasSelectedBodies()) break;
            m_gizmo->setMode(GizmoMode::Scale);
            m_selection->setNavigationOnly(false);
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

        case ToolAction::Revolve:
            // (kept in the enum for stability with older bindings; the
            //  Toolbar entry was removed in the RevolvePlugin refactor.
            //  Dispatch is handled by the requestInteractiveOp path now.)
            beginRevolve();
            break;

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

        case ToolAction::EditDiameter: {
            // Detection populates the resize-* fields when it returns true,
            // so begin() can use them straight away.
            if (detectCylindricalResizeCandidate()) beginResizeCylindrical();
            break;
        }
        case ToolAction::Thread: {
            // Same detector as Edit Diameter — it fills the m_resizeCyl*
            // fields (axis, radius, extent, hole-vs-boss) that the thread
            // popup copies from.
            if (detectCylindricalResizeCandidate()) beginThread();
            break;
        }

        case ToolAction::Shell: {
            beginIop(m_shellCtl);
            break;
        }

        case ToolAction::Taper: {
            beginIop(m_taperCtl);
            break;
        }

        case ToolAction::ScaleFace: {
            beginIop(m_scaleFaceCtl);
            break;
        }

        case ToolAction::ProjectSketch: {
            beginIop(m_projectSketchCtl);
            break;
        }

        case ToolAction::RemoveFace: {
            beginIop(m_defeatureCtl);
            break;
        }

        case ToolAction::EditFilletChamfer: {
            // Find the FilletOp / ChamferOp in history that owns the picked face,
            // then re-open it for editing with the existing radius / distance.
            TopoDS_Shape pickedFace;
            int pickedBodyId = -1;
            for (const auto& e : m_selection->getSelection()) {
                if (e.type == SelectionType::Face && !e.shape.IsNull()) {
                    pickedFace = e.shape; pickedBodyId = e.bodyId; break;
                }
            }
            if (pickedFace.IsNull()) break;
            // Remember which body's face was clicked — the edit path uses it to
            // detect a baked feature (clicked body doesn't change after edit).
            m_edgeOpPickedBodyId = pickedBodyId;
            const auto& ops = m_history->operations();
            for (int i = 0; i < static_cast<int>(ops.size()); ++i) {
                const auto& op = ops[i];
                if (op && op->isEnabled() && op->ownsFace(pickedFace) &&
                    (op->typeId() == "fillet" || op->typeId() == "chamfer")) {
                    beginInteractiveEdgeOpEdit(i);
                    break;
                }
            }
            break;
        }

        default: break;
    }
}

void Application::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();

    // Undo/Redo — poll the hardware Ctrl state directly so it works even when
    // ImGui has text input focus. Always false on Android (no modifier keys).
    bool ctrlHeld = Window::isCtrlDown();
    if (ctrlHeld && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (!m_edgeOpActive && !m_extruding && !m_pushPullActive) {
            // Mid-placement Ctrl+Z cancels the IN-PROGRESS shape first (the
            // editor convention — and Steve's muscle memory); the next
            // Ctrl+Z then undoes committed elements as usual.
            if (m_inSketchMode && m_sketchTool && m_sketchTool->isPlacing()) {
                m_sketchTool->onCancel();
            } else if (m_history->canUndo() &&
                       // In sketch mode, NEVER undo past the sketch's own edits
                       // into the host body — rolling the body back while the
                       // sketch is live (and rendering against it) crashed.
                       (!m_inSketchMode ||
                        m_history->currentStep() > m_sketchEntryHistoryStep)) {
                const Operation* undone =
                    m_history->getStep(m_history->currentStep());
                m_history->undo(*m_document);
                // A linked 3D sketch move (SketchTransformOp) updated its body via
                // the cascade; re-cascade so the body follows the reverted plane.
                // (No-op for detached sketches — the guard in cascade returns early.)
                if (auto* st = dynamic_cast<const materializr::SketchTransformOp*>(undone))
                    cascadeFromSketchEdit(st->getSketchId());
                // In sketch mode, the host face is the anchor for the whole
                // sketch session — clearing the selection would drop its blue
                // highlight even though the sketch is still active. Skip the
                // body-selection reset (sketch-element selection inside the
                // SketchTool is unaffected by m_selection).
                if (!m_inSketchMode) {
                    m_selection->clear();
                    m_hoveredBodyId = -1;
                } else if (m_activeSketch) {
                    // Undoing a line restores the state to just its first-click
                    // anchor (added before the line's history step) — a stray
                    // point. Sweep any such orphan so undo leaves no dangling
                    // vertex.
                    m_activeSketch->pruneOrphanPoints();
                    // A sketch edit's body update was applied through the cascade
                    // (editStep) — the SketchEditOp's own undo only reverts the
                    // sketch geometry, not the body. Re-cascade so the body follows
                    // the now-reverted sketch instead of staying at its last shape.
                    if (m_activeSketchId >= 0) cascadeFromSketchEdit(m_activeSketchId);
                }
                m_meshesDirty = true;
            }
        }
    }
    if (ctrlHeld && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        if (!m_edgeOpActive && !m_extruding && !m_pushPullActive) {
            if (m_history->canRedo()) {
                m_history->redo(*m_document);
                const Operation* redone =
                    m_history->getStep(m_history->currentStep());
                if (!m_inSketchMode) {
                    m_selection->clear();
                    m_hoveredBodyId = -1;
                } else if (m_activeSketch && m_activeSketchId >= 0) {
                    // Mirror of the undo path: re-sync the body to the
                    // re-applied sketch edit.
                    cascadeFromSketchEdit(m_activeSketchId);
                }
                if (auto* st = dynamic_cast<const materializr::SketchTransformOp*>(redone))
                    cascadeFromSketchEdit(st->getSketchId());
                m_meshesDirty = true;
            }
        }
    }
    // Ctrl+A: context-aware select-all. Skipped when ImGui has text-input focus
    // so the standard "select all text" behaviour in input fields still works.
    if (ctrlHeld && ImGui::IsKeyPressed(ImGuiKey_A, false) && !io.WantTextInput) {
        if (m_inSketchMode && m_activeSketch && m_sketchTool) {
            // In a sketch: hand off to the sketch tool's selectAll (also wired
            // to the double-click shortcut).
            m_sketchTool->setMode(SketchToolMode::Select);
            m_sketchTool->selectAll();
        } else if (m_selection && m_selection->hasSelection() &&
                   (m_selection->primaryType() == SelectionType::Edge ||
                    m_selection->primaryType() == SelectionType::Face)) {
            // Extend an edge/face selection to every edge/face on the same
            // body (or bodies) that already contributes to the selection.
            SelectionType targetType = m_selection->primaryType();
            std::set<int> bodyIds;
            for (const auto& entry : m_selection->getSelection()) {
                if (entry.type == targetType && entry.bodyId >= 0) {
                    bodyIds.insert(entry.bodyId);
                }
            }
            for (int bodyId : bodyIds) {
                try {
                    const TopoDS_Shape& shape = m_document->getBody(bodyId);
                    TopAbs_ShapeEnum kind = (targetType == SelectionType::Edge)
                                                ? TopAbs_EDGE : TopAbs_FACE;
                    int idx = 0;
                    for (TopExp_Explorer it(shape, kind); it.More(); it.Next(), ++idx) {
                        SelectionEntry e;
                        e.type = targetType;
                        e.bodyId = bodyId;
                        // subShapeIndex isn't strictly needed for findEntry —
                        // it falls back to IsSame(shape) — but populating it
                        // for faces matches what the picker does.
                        if (targetType == SelectionType::Face) e.subShapeIndex = idx;
                        e.shape = it.Current();
                        m_selection->addToSelection(e);
                    }
                } catch (...) {}
            }
        } else if (m_document) {
            // No useful sub-shape context: select every visible body.
            m_selection->clear();
            for (int bodyId : m_document->getAllBodyIds()) {
                if (!m_document->isBodyVisible(bodyId)) continue;
                SelectionEntry e;
                e.type = SelectionType::Body;
                e.bodyId = bodyId;
                try { e.shape = m_document->getBody(bodyId); } catch (...) {}
                m_selection->addToSelection(e);
            }
        }
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
    // Ctrl+D — Duplicate in place. Branches on selection type:
    //   Body   → CopyOp (full history support, undoable via Ctrl+Z)
    //   Axis   → Document::addAxis with the source's origin/direction
    //   Plane  → Document::addPlane with the source's gp_Pln
    //   Sketch → deep-clone (points + lines + circles + arcs); constraints
    //            skipped for now (id remapping is non-trivial)
    //   Face   → no-op; duplicating a face has no clean semantic.
    // After the duplicate lands the SELECTION is replaced with the new
    // entity so the user immediately operates on the fresh copy. Skipped
    // when a text field has focus so it doesn't fire while typing.
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) &&
        !ImGui::IsAnyItemActive() && m_selection) {
        const auto& sel = m_selection->getSelection();
        if (!sel.empty()) {
            const auto& first = sel[0];
            if (first.type == SelectionType::Body && first.bodyId >= 0) {
                int srcFolder = m_document->getBodyFolder(first.bodyId);
                auto op = std::make_unique<CopyOp>();
                op->setSourceBodyId(first.bodyId);
                op->setOffset(0.0, 0.0, 0.0);
                CopyOp* opPtr = op.get();
                if (m_history->pushOperation(std::move(op), *m_document)) {
                    int newId = opPtr->getCreatedBodyId();
                    if (newId >= 0) {
                        if (srcFolder >= 0)
                            m_document->setBodyFolder(newId, srcFolder);
                        SelectionEntry e;
                        e.type = SelectionType::Body;
                        e.bodyId = newId;
                        try { e.shape = m_document->getBody(newId); } catch (...) {}
                        m_selection->select(e);
                    }
                    m_meshesDirty = true;
                }
            } else if (first.type == SelectionType::Axis && first.axisId >= 0) {
                if (const auto* a = m_document->getAxis(first.axisId)) {
                    int newId = m_document->addAxis(a->origin, a->direction,
                                                     a->name + " copy");
                    if (newId >= 0) {
                        SelectionEntry e;
                        e.type = SelectionType::Axis;
                        e.axisId = newId;
                        m_selection->select(e);
                    }
                    markDirty();
                }
            } else if (first.type == SelectionType::Plane && first.planeId >= 0) {
                if (const auto* p = m_document->getPlane(first.planeId)) {
                    int newId = m_document->addPlane(p->plane,
                                                      p->name + " copy");
                    if (newId >= 0) {
                        SelectionEntry e;
                        e.type = SelectionType::Plane;
                        e.planeId = newId;
                        m_selection->select(e);
                    }
                    markDirty();
                }
            } else if ((first.type == SelectionType::Sketch ||
                        first.type == SelectionType::SketchRegion) &&
                       first.sketchId >= 0) {
                auto src = m_document->getSketch(first.sketchId);
                if (src) {
                    auto dst = std::make_shared<materializr::Sketch>();
                    dst->setPlane(src->getPlane());
                    dst->setSourceBody(src->getSourceBody());
                    if (!src->getSourceFace().IsNull())
                        dst->setSourceFace(src->getSourceFace());
                    // Re-add points first, build an id remap so derived
                    // elements (lines / circles / arcs) can reference the
                    // new point ids. Constraints carry point/line ids
                    // too — skipped for now; deferred until id remapping
                    // for constraints lands.
                    std::map<int, int> pmap;
                    for (const auto& p : src->getPoints())
                        pmap[p.id] = dst->addPoint(p.pos);
                    for (const auto& l : src->getLines()) {
                        auto a = pmap.find(l.startPointId);
                        auto b = pmap.find(l.endPointId);
                        if (a != pmap.end() && b != pmap.end())
                            dst->addLine(a->second, b->second);
                    }
                    for (const auto& c : src->getCircles()) {
                        auto cp = pmap.find(c.centerPointId);
                        if (cp != pmap.end())
                            dst->addCircle(cp->second, c.radius);
                    }
                    for (const auto& arc : src->getArcs()) {
                        auto cp = pmap.find(arc.centerPointId);
                        auto sp = pmap.find(arc.startPointId);
                        auto ep = pmap.find(arc.endPointId);
                        if (cp != pmap.end() && sp != pmap.end() && ep != pmap.end())
                            dst->addArc(cp->second, sp->second, ep->second, arc.radius);
                    }
                    int newId = m_document->addSketch(dst,
                                  m_document->getSketchName(first.sketchId) + " copy");
                    if (newId >= 0) {
                        SelectionEntry e;
                        e.type = SelectionType::Sketch;
                        e.sketchId = newId;
                        m_selection->select(e);
                    }
                    markDirty();
                    m_meshesDirty = true;
                }
            }
            // Face / Edge / etc.: no-op intentionally — duplicating a
            // face / edge has no clean standalone interpretation.
        }
    }
    // Backspace during spline placement removes the last control point —
    // the natural "oops, one back" while clicking out a curve.
    if (m_inSketchMode && m_sketchTool &&
        m_sketchTool->getMode() == SketchToolMode::Spline &&
        !m_sketchTool->splinePointsInProgress().empty() &&
        !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        recordSketchMutation([&] { m_sketchTool->removeLastSplinePoint(); });
    }
    // Backspace while the Text / SVG tool is active removes the WHOLE last
    // stamp — re-place a misjudged logo without leaving the tool.
    if (m_inSketchMode && m_sketchTool &&
        (m_sketchTool->getMode() == SketchToolMode::Text ||
         m_sketchTool->getMode() == SketchToolMode::Svg) &&
        m_sketchTool->hasLastStamp() &&
        !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        recordSketchMutation([&] { m_sketchTool->undoLastStamp(); });
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (m_sketchGizmoHandle != SketchGizmoHandle::None) {
            // Revert each involved point to its drag-start position and exit
            // the gizmo drag (or popup adjust) without pushing a history op.
            if (m_activeSketch) {
                for (auto& [id, orig] : m_sketchGizmoOriginals)
                    m_activeSketch->movePoint(id, orig);
            }
            m_sketchGizmoHandle = SketchGizmoHandle::None;
            m_sketchGizmoBefore.reset();
            m_sketchGizmoOriginals.clear();
            m_sketchGizmoRotateAdjusting = false;
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
        } else if (anyIopActive()) {
            for (auto* c : m_iops)
                if (c->active()) { c->cancel(iopContext()); break; }
        } else if (m_resizeCylActive) {
            cancelResizeCylindrical();
        } else if (m_edgeOpActive) {
            cancelInteractiveEdgeOp();
        } else if (m_moveFaceActive) {
            cancelMoveFace();
        } else if (m_extruding) {
            cancelInteractiveExtrude();
        } else if (m_inSketchMode) {
            // Two-step Escape inside sketch mode:
            //   1st press while a shape placement is in progress → cancel
            //      just that placement (Line mid-stroke, Circle awaiting
            //      its radius click, Polygon awaiting its second click,
            //      Spline mid-stream, etc.). The sketch stays active so
            //      the user can resume drawing.
            //   2nd press (or 1st press when nothing is in progress) →
            //      exit sketch mode entirely (same as Finish Sketch but
            //      without an explicit click).
            if (m_sketchTool && m_sketchTool->isPlacing()) {
                m_sketchTool->onCancel();
            } else {
                exitSketchMode();
            }
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
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_moveFaceActive) {
        commitMoveFace();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        m_viewport->getCamera().reset();
    }
    // F9 = collapse/restore BOTH docked side columns (max-viewport toggle).
    // Guarded against text fields so it doesn't fire mid-typing.
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F9)) {
        bool hide = !(m_leftPanelHidden && m_rightPanelHidden);
        m_leftPanelHidden = m_rightPanelHidden = hide;
        saveAppSettings();
    }
    // F = Frame: zoom-fit to the current selection, or to visible bodies if
    // nothing's selected. The whole point is the user can hide everything
    // they don't care about, hit F, and have the camera snap onto the
    // remaining part — no more pan-zoom-tilt dance to reach a small off-
    // origin object. Suppressed in sketch mode + while a text field has
    // focus so it doesn't fire while typing constraint values.
    if (!m_inSketchMode && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_F)) {
        frameSelection();
    }
    // Delete: while in sketch mode, restrict to sketch-element deletion so the
    // host body (which stays selected to keep its face highlighted, per the
    // Ctrl+Z fix) doesn't get nuked under the user's nose. Outside sketch mode
    // Delete still removes selected bodies / sketches through history.
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (m_inSketchMode && m_activeSketch && m_sketchTool) {
            // Delete the sketch-element selection only — the host body (which
            // stays selected to keep its face highlighted) must not get nuked.
            deleteSelectedSketchElements();
        } else if (m_selection->hasSelection()) {
            const auto& sel = m_selection->getSelection();
            std::vector<int> bodiesToDelete;
            std::vector<int> sketchesToDelete;
            std::vector<int> planesToDelete;
            std::vector<int> axesToDelete;
            for (const auto& entry : sel) {
                // A selected sketch (or sketch region) deletes the whole
                // sketch; a body/face/edge selection deletes its body;
                // planes and axes delete directly (same as the Items
                // panel's right-click — the Delete key used to silently
                // ignore them).
                if (entry.type == SelectionType::Sketch || entry.type == SelectionType::SketchRegion) {
                    if (entry.sketchId >= 0) {
                        bool already = false;
                        for (int s : sketchesToDelete) { if (s == entry.sketchId) { already = true; break; } }
                        if (!already) sketchesToDelete.push_back(entry.sketchId);
                    }
                } else if (entry.type == SelectionType::Plane) {
                    if (entry.planeId >= 0) planesToDelete.push_back(entry.planeId);
                } else if (entry.type == SelectionType::Axis) {
                    if (entry.axisId >= 0) axesToDelete.push_back(entry.axisId);
                } else if (entry.bodyId >= 0) {
                    bool already = false;
                    for (int b : bodiesToDelete) { if (b == entry.bodyId) { already = true; break; } }
                    if (!already) bodiesToDelete.push_back(entry.bodyId);
                }
            }
            for (int pid : planesToDelete) { m_document->removePlane(pid); markDirty(); }
            for (int aid : axesToDelete)   { m_document->removeAxis(aid);  markDirty(); }
            if (!planesToDelete.empty() || !axesToDelete.empty())
                m_selection->clear();
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
    }
    // Gizmo mode switching. WantTextInput is true while an InputText (rename
    // field, dimension input, etc.) has focus — letting W/E/R fire there both
    // switches gizmo mode AND inserts the character, which is rude.
    if (!m_inSketchMode && !io.KeyCtrl && !io.WantTextInput) {
        bool changed = false;
        if (ImGui::IsKeyPressed(ImGuiKey_W)) {
            m_gizmo->setMode(GizmoMode::Translate); changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_E)) {
            m_gizmo->setMode(GizmoMode::Rotate); changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R)) {
            m_gizmo->setMode(GizmoMode::Scale); changed = true;
        }
        // Explicit ask for a gizmo: drop any navigation-only suppression.
        if (changed) m_selection->setNavigationOnly(false);
    }
}

void Application::rebuildMeshes() {
    float deflection, angularDeflection;
    meshQualityParams(deflection, angularDeflection);

    if (m_meshesDirty) {
        // Full rebuild — clear everything and re-tessellate every visible
        // body. Used on project load, mesh-quality change, theme switch.
        m_shapeRenderer->clear();
        m_edgeRenderer->clear();
        auto ids = m_document->getAllBodyIds();
        int meshN = static_cast<int>(ids.size()), meshI = 0;
        for (int id : ids) {
            // During a load (deferred slot), pump a per-body progress frame so
            // tessellating a heavy model keeps the window responsive.
            if (m_pumpMeshProgress) {
                renderProgressFrame(meshN > 0 ? float(meshI) / float(meshN) : -1.0f,
                                    "Preparing view\xE2\x80\xA6");
            }
            ++meshI;
            if (id < 0) continue;        // defensive: skip bad ids
            if (!m_document->isBodyVisible(id)) continue;
            TopoDS_Shape shape;
            try { shape = m_document->getBody(id); } catch (...) { continue; }
            int idx = m_shapeRenderer->setBodyMesh(id, shape, deflection,
                                                   angularDeflection);
            if (idx >= 0) {
                m_shapeRenderer->setColor(idx, m_document->getBodyColor(id));
                if (m_extruding && m_extrudeMode == ExtrudeMode::Subtract &&
                    id == m_extrudePreviewBodyId) {
                    m_shapeRenderer->setSubtractPreview(idx, true);
                }
            }
            m_edgeRenderer->setBodyEdges(id, shape, deflection);
        }
        m_dirtyBodyIds.clear();
        return;
    }

    // Partial rebuild — only the bodies in m_dirtyBodyIds need new meshes.
    // The other (potentially 100+) bodies are left untouched, which is the
    // whole point of this path: interactive ops like push/pull stay smooth
    // on a complex project.
    if (m_dirtyBodyIds.empty()) return;

    // Copy out: setBodyMesh may mutate the renderer's internal slots; safer
    // to iterate a snapshot.
    std::vector<int> ids(m_dirtyBodyIds.begin(), m_dirtyBodyIds.end());
    m_dirtyBodyIds.clear();
    for (int id : ids) {
        bool exists = false;
        try { (void)m_document->getBody(id); exists = true; } catch (...) {}
        if (!exists || !m_document->isBodyVisible(id)) {
            m_shapeRenderer->removeBody(id);
            m_edgeRenderer->removeBody(id);
            continue;
        }
        const TopoDS_Shape& shape = m_document->getBody(id);
        int idx = m_shapeRenderer->setBodyMesh(id, shape, deflection,
                                               angularDeflection);
        if (idx >= 0) {
            m_shapeRenderer->setColor(idx, m_document->getBodyColor(id));
            if (m_extruding && m_extrudeMode == ExtrudeMode::Subtract &&
                id == m_extrudePreviewBodyId) {
                m_shapeRenderer->setSubtractPreview(idx, true);
            }
        }
        m_edgeRenderer->setBodyEdges(id, shape, deflection);
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

        // Roll: rotate the camera's "up" vector around the view direction by
        // 90°. Doesn't change camera position / target, so a snapped ortho
        // view stays snapped — just spins in place.
        case ViewCubeAction::Home:
            // Default 3/4 isometric view (FrontTopRight). Camera offset along
            // (+1,+1,+1) so all three labelled faces (Front, Top, Right) are
            // visible, with world +Y up.
            handleViewCubeAction(static_cast<int>(ViewCubeAction::FrontTopRight));
            return;

        case ViewCubeAction::RollLeft:
        case ViewCubeAction::RollRight: {
            glm::vec3 viewDir = glm::normalize(cam.getTarget() - cam.getPosition());
            float ang = (a == ViewCubeAction::RollLeft) ? +kRot : -kRot;
            float ca = std::cos(ang), sa = std::sin(ang);
            // Rodrigues' rotation of m_up around viewDir.
            glm::vec3 u = cam.getUp();
            glm::vec3 rotated = u * ca + glm::cross(viewDir, u) * sa
                              + viewDir * glm::dot(viewDir, u) * (1.0f - ca);
            cam.setUp(glm::normalize(rotated));
            return;
        }
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
        // Top + Bottom: the "up" direction is computed from the CURRENT
        // camera's horizontal forward so the snap respects turntable
        // orientation — i.e. whatever was "ahead of you" in the orbiting
        // view ends up at the top of the screen when you look straight
        // down. Without this the view always snaps to the same up
        // direction regardless of where you'd yawed to, which feels
        // jarring after a turntable spin.
        case ViewCubeAction::Top:
        case ViewCubeAction::Bottom: {
            dir = (a == ViewCubeAction::Top) ? glm::vec3(0, 1, 0)
                                             : glm::vec3(0,-1, 0);
            glm::vec3 fwd = cam.getTarget() - cam.getPosition();
            glm::vec3 horiz(fwd.x, 0.0f, fwd.z);
            if (glm::length(horiz) < 1e-3f) horiz = glm::vec3(0, 0, -1);
            up = glm::normalize(horiz);
            break;
        }
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
        {{"Materializr Project", "*.materializr"}, {"All Files", "*"}},
        [this](const std::string& chosenPath) {
            if (chosenPath.empty()) return;
            std::string path = chosenPath;
#if !defined(__ANDROID__)
            // Keep the .materializr extension. The project file is gzip-
            // compressed, so without the extension the OS shows it as a generic
            // "compressed archive" and the open filter can't find it. (On
            // Android the SAF picker, not this path, names the file.)
            if (std::filesystem::path(path).extension() != ".materializr")
                path += ".materializr";
#endif
            ProjectHistory hist = captureProjectHistory();
            auto result = ProjectIO::save(path, *m_document, &hist);
            if (result.success) {
                m_currentProjectPath = path;
                markSaved();
                saveAppSettings(); // persist lastProjectPath for auto-open
                // Save As also lands in Open Recent (persistable ref on Android).
                {
                    std::string ref, name;
#if defined(__ANDROID__)
                    ref  = materializr::androidLastDocUri();
                    name = materializr::androidLastDocName();
                    if (ref.empty()) ref = path;
#else
                    ref = path;
#endif
                    if (name.empty()) name = std::filesystem::path(path).filename().string();
                    addRecentProject(ref, name);
                }
                std::fprintf(stdout, "Project saved to %s\n", path.c_str());
                if (m_closeAfterSave) {
                    if (m_postSaveAction == PostSaveAction::CloseProject) {
                        doCloseProject();
                        m_postSaveAction = PostSaveAction::None;
                    } else if (m_postSaveAction == PostSaveAction::OpenProject) {
                        auto act = std::move(m_pendingOpenAction);
                        m_pendingOpenAction = nullptr;
                        m_postSaveAction = PostSaveAction::None;
                        if (act) act();
                    } else {
                        m_confirmedClose = true;
                    }
                }
            } else {
                std::fprintf(stderr, "Save failed: %s\n", result.errorMessage.c_str());
            }
            m_closeAfterSave = false;
        });
}

void Application::saveProjectQuick() {
    // Saving mid-preview would persist the preview body and its phantom
    // history step into the file (and the half-applied state crashed at
    // least once). An explicit save expresses "keep what's committed" —
    // cancel live previews first.
    cancelAllInteractivePreviews();
    if (m_currentProjectPath.empty()) {
        saveProject();
        return;
    }
    ProjectHistory hist = captureProjectHistory();
    auto result = ProjectIO::save(m_currentProjectPath, *m_document, &hist);
    if (result.success) {
        markSaved();
        saveAppSettings(); // persist lastProjectPath for auto-open
        std::fprintf(stdout, "Project saved to %s\n", m_currentProjectPath.c_str());
        if (m_closeAfterSave) {
            if (m_postSaveAction == PostSaveAction::CloseProject) {
                doCloseProject();
                m_postSaveAction = PostSaveAction::None;
            } else if (m_postSaveAction == PostSaveAction::OpenProject) {
                auto act = std::move(m_pendingOpenAction);
                m_pendingOpenAction = nullptr;
                m_postSaveAction = PostSaveAction::None;
                if (act) act();
            } else {
                m_confirmedClose = true;
            }
        }
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
        // SketchEditOp's params blob needs the live document to look up the
        // sketch id its m_target belongs to. Other ops use the parameterless
        // serializeParams() — base Operation returns "" so they're a no-op.
        if (auto* sk = dynamic_cast<const materializr::SketchEditOp*>(op)) {
            steps[i].params = sk->serializeWithDocument(*m_document);
        } else {
            steps[i].params = op->serializeParams();
        }
        steps[i].timestampUnix = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::seconds>(
                op->timestamp().time_since_epoch()).count());
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

void Application::rebuildHistoryFromProject(const ProjectHistory& hist,
                                            const std::string& savedByVersion) {
    m_history->clear();
    if (!hist.present) return;

    // Health report: count steps that reload as baked (non-editable) ReplayOps.
    // A body-affecting baked step means geometry the user can see but can't edit
    // (e.g. frozen by an older save) — surfaced after the loop so the parametric
    // state of a project is visible up front instead of discovered mid-edit.
    int bakedBodySteps = 0;     // baked steps that change/delete a body
    int bakedSketchSteps = 0;   // baked sketch-only steps (benign)

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

        // While `running` still holds the pre-step state, derive what this
        // step did to the body set so a rehydrated real op can restore its
        // post-execution bookkeeping (Operation::rehydrateFromReload).
        Operation::ReloadState reload;
        for (const auto& [id, shape] : st.changed) {
            if (running.find(id) == running.end()) {
                reload.created.push_back(id);
            } else {
                reload.modifiedBefore.push_back({id, running[id]});
                reload.modifiedAfter.push_back({id, shape});
            }
        }
        for (int id : st.deleted) {
            auto it = running.find(id);
            if (it != running.end()) reload.deletedBefore.push_back({id, it->second});
        }

        for (const auto& [id, shape] : st.changed) running[id] = shape;
        for (int id : st.deleted) running.erase(id);
        ReplayOp::BodyState after = toVec(running);

        // First, try to reconstruct a real op type from typeId + params blob
        // — this is how reloaded steps stay editable (live-Properties panel
        // works on the sketch, but the History → click-step → Properties
        // path needs an actual SketchEditOp, not a ReplayOp). Falls through
        // to the generic ReplayOp path on any parse failure.
        std::unique_ptr<Operation> op;
        if (st.typeId == "sketchedit" && !st.params.empty()) {
            op = ProjectIO::rehydrateSketchEditOp(st.params, *m_document);
        }

        // Backward-compat: boolean/delete steps written before they serialised
        // params have an empty blob, so they'd reload as baked ReplayOps and
        // silently overwrite any edit made to an UPSTREAM step (e.g. a fillet
        // feeding a union). Synthesise a params blob from the step's body diff
        // — target = the modified body, tool/victim = the deleted body — plus
        // the boolean mode parsed from the saved description. New projects carry
        // real params and skip this path.
        std::string params = st.params;
        if (params.empty()) {
            char buf[320];
            if (st.typeId == "boolean" && reload.modifiedBefore.size() == 1 &&
                reload.deletedBefore.size() == 1) {
                int mode = st.description.find("Subtract")  != std::string::npos ? 1
                         : st.description.find("Intersect") != std::string::npos ? 2 : 0;
                std::snprintf(buf, sizeof(buf), "target=%d;tool=%d;mode=%d",
                              reload.modifiedBefore[0].first,
                              reload.deletedBefore[0].first, mode);
                params = buf;
            } else if (st.typeId == "delete" && reload.deletedBefore.size() == 1) {
                std::snprintf(buf, sizeof(buf), "body=%d",
                              reload.deletedBefore[0].first);
                params = buf;
            } else if (st.typeId == "transform" &&
                       reload.modifiedBefore.size() == 1 &&
                       reload.modifiedAfter.size() == 1) {
                // Recover the rigid transform from the before/after snapshots so
                // the step reloads as a real op that re-applies to the LIVE body
                // (instead of a baked ReplayOp that overwrites upstream edits).
                gp_Trsf t;
                if (TransformOp::rigidTrsfBetween(reload.modifiedBefore[0].second,
                                                  reload.modifiedAfter[0].second, t)) {
                    std::snprintf(buf, sizeof(buf),
                        "body=%d;raw=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
                        reload.modifiedBefore[0].first,
                        t.Value(1,1), t.Value(1,2), t.Value(1,3), t.Value(1,4),
                        t.Value(2,1), t.Value(2,2), t.Value(2,3), t.Value(2,4),
                        t.Value(3,1), t.Value(3,2), t.Value(3,3), t.Value(3,4));
                    params = buf;
                }
            }
        }

        // Generic factory path: build the real op from typeId, restore its
        // parameters, then its post-execution state. Only ops that opt into
        // rehydrateFromReload() (returning true) come back editable; everyone
        // else falls through to the baked ReplayOp below, unchanged.
        if (!op && !params.empty()) {
            auto candidate = OperationFactory::create(st.typeId);
            if (candidate && candidate->deserializeParams(params) &&
                candidate->rehydrateFromReload(reload, *m_document)) {
                op = std::move(candidate);
                std::fprintf(stderr, "[Reload] step '%s' (%s): rehydrated as "
                                     "real op (created=%zu modified=%zu)\n",
                             st.name.c_str(), st.typeId.c_str(),
                             reload.created.size(), reload.modifiedBefore.size());
            }
        }
        if (!op) {
            const bool affectsBody = !st.changed.empty() || !st.deleted.empty();
            if (affectsBody) ++bakedBodySteps; else ++bakedSketchSteps;
            std::fprintf(stderr, "[Reload] step '%s' (%s): baked ReplayOp "
                                 "(params=%s, affectsBody=%d)\n",
                         st.name.c_str(), st.typeId.c_str(),
                         st.params.empty() ? "none" : "present", (int)affectsBody);
            op = std::make_unique<ReplayOp>(
                st.typeId, st.name, st.description,
                std::move(before), std::move(after));
            // Carry the saved blob into the ReplayOp so future loaders /
            // editors can still see it.
            if (!st.params.empty())
                static_cast<ReplayOp*>(op.get())->setStoredParams(st.params);
        }
        op->setEnabled(st.enabled);
        // Restore the original timestamp so the HistoryPanel's date grouping
        // is preserved across reload. Legacy projects (timestamp == 0) get
        // bumped to "yesterday" so they group under that header instead of
        // landing on today and mixing with new work.
        if (st.timestampUnix > 0) {
            op->setTimestamp(std::chrono::system_clock::time_point{
                std::chrono::seconds{st.timestampUnix}});
        } else {
            op->setTimestamp(std::chrono::system_clock::now() -
                             std::chrono::hours{24});
        }
        m_history->pushExecuted(std::move(op));
    }

    // After all ops are rehydrated, the document bodies reflect their final state
    // (potentially after downstream Transforms). Re-resolve each fillet/chamfer's
    // generated-face indices against the final body so ownsFace() matches the
    // face positions the user actually sees and can click.
    refreshAllEdgeOpFaces();

    // Health report. Two sources of non-editable geometry:
    //  • baked body-affecting STEPS (an op that didn't round-trip), and
    //  • base bodies in the INITIAL STATE that are not touched by any history
    //    step — geometry with no construction history (truly imported or frozen).
    //
    // A body in initialState that IS later modified by a step (common when the
    // project was saved mid-undo, causing a push/pull to lose its created-body
    // tracking) is NOT truly frozen: the ops still drive it. Count only bodies
    // that no step's diff references at all.
    std::set<int> touchedByOps;
    for (const auto& st : hist.steps) {
        for (const auto& [id, shp] : st.changed) touchedByOps.insert(id);
    }
    int frozenBodies = 0;
    for (const auto& [id, shp] : hist.initialState) {
        if (!touchedByOps.count(id)) ++frozenBodies;
    }
    std::fprintf(stderr,
        "[Reload] health: %d steps, %d baked body-features, %d baked sketch-edits, "
        "%zu base bodies (%d truly frozen, %zu op-driven phantoms)\n",
        static_cast<int>(hist.steps.size()), bakedBodySteps, bakedSketchSteps,
        hist.initialState.size(), frozenBodies,
        hist.initialState.size() - static_cast<std::size_t>(frozenBodies));
    const int nonEditable = bakedBodySteps + frozenBodies;
    // Only warn about frozen geometry if the file predates version tagging
    // (no SAVED_BY line). A file that WAS saved by a versioned build is current
    // — phantom initialState bodies in it are save-tracking artifacts, not a
    // true format downgrade, so we must not call it "older format".
    if (nonEditable > 0 && savedByVersion.empty()) {
        const int n        = frozenBodies > 0 ? frozenBodies : bakedBodySteps;
        const char* what   = frozenBodies > 0 ? "body(ies)" : "feature(s)";
        std::string msg =
            "This project was saved in an older format: " + std::to_string(n) + " " +
            what + " are frozen and can't be edited by value. The shapes are intact "
            "\xE2\x80\x94 to change a baked round/chamfer, select its face and use "
            "Repair Geometry to restore the sharp edge, then redo it. New saves "
            "won't have this.";
        showToast(msg, 9.0);
    }
}

void Application::ensureSketchSourceFace(int sketchId) {
    auto sk = m_document->getSketch(sketchId);
    if (!sk) return;
    if (!sk->getSourceFace().IsNull()) return; // already set; nothing to do
    int bid = sk->getSourceBody();
    if (bid < 0) return;
    TopoDS_Shape body;
    try { body = m_document->getBody(bid); } catch (...) { return; }
    if (body.IsNull()) return;

    const gp_Pln& sketchPln = sk->getPlane();
    gp_Pnt sO = sketchPln.Location();
    gp_Dir sN = sketchPln.Axis().Direction();

    // Two passes — first prefer faces that have inner wires (i.e., faces with
    // holes) since those are usually what the user sketched on; second pass
    // accepts the first geometric match. Tolerances loose enough to survive
    // a save/load + history-replay round trip without being so loose that
    // unrelated parallel faces match.
    auto matchPass = [&](bool requireHoles) -> TopoDS_Face {
        TopoDS_Face hit;
        for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
            TopoDS_Face f = TopoDS::Face(ex.Current());
            Handle(Geom_Surface) surf = BRep_Tool::Surface(f);
            if (surf.IsNull()) continue;
            Handle(Geom_Plane) gpln = Handle(Geom_Plane)::DownCast(surf);
            if (gpln.IsNull()) continue;
            gp_Pln fPln = gpln->Pln();
            gp_Dir fN = fPln.Axis().Direction();
            // Normals parallel (either direction). Tolerance ~0.6° of slack.
            if (std::abs(sN.Dot(fN)) < 0.9999) continue;
            // Sketch origin should lie on the face's plane within 0.05 mm.
            gp_Pnt fO = fPln.Location();
            gp_Vec d(fO, sO);
            double dist = std::abs(d.Dot(gp_Vec(fN)));
            if (dist > 0.05) continue;
            if (requireHoles) {
                // Walk wires — need at least one beyond the outer wire to
                // qualify as a face-with-hole.
                TopoDS_Wire outer = BRepTools::OuterWire(f);
                int wireCount = 0;
                for (TopExp_Explorer we(f, TopAbs_WIRE); we.More(); we.Next()) {
                    ++wireCount;
                }
                if (wireCount < 2 || outer.IsNull()) continue;
            }
            hit = f;
            break;
        }
        return hit;
    };
    TopoDS_Face f = matchPass(/*requireHoles=*/true);
    if (f.IsNull()) f = matchPass(/*requireHoles=*/false);
    if (!f.IsNull()) sk->setSourceFace(f);
}

bool Application::loadProjectAt(const std::string& path) {
    if (path.empty()) return false;
    m_document->clear();
    m_history->clear();
    m_selection->clear();
    ProjectHistory hist;
    auto result = ProjectIO::load(path, *m_document, &hist);
    if (!result.success) {
        std::fprintf(stderr, "Load failed: %s\n", result.errorMessage.c_str());
        return false;
    }
    rebuildHistoryFromProject(hist, result.savedByVersion);
    // A reopened project should sit at the history tip with no redo stack — a
    // phantom redo tail would, e.g., block autosave (which won't save below-tip).
    m_history->dropRedoTail();
    m_currentProjectPath = path;
    markSaved();
    m_meshesDirty = true;
    // Home view = the ViewCube Home button: default isometric orientation AND
    // zoom-to-fit the loaded geometry (computed from the OCCT body bounds, which
    // are ready now). Plain Camera::reset() only sets a fixed (5,5,5) eye that
    // ignores the model's size/position, leaving it zoomed-in or off-screen.
    handleViewCubeAction(static_cast<int>(ViewCubeAction::FrontTopRight));

    // m_sourceFace (the TopoDS_Face the sketch was drawn on) isn't part of
    // the project file — only the plane and sourceBodyId are. Re-derive it
    // for every loaded sketch so Sketch::buildRegions can union the host
    // face's wires (holes, fillets) into the sketch profile.
    for (int sid : m_document->getAllSketchIds()) {
        ensureSketchSourceFace(sid);
    }

    std::fprintf(stdout, "Loaded %d bodies, %d history steps from %s\n",
                 result.bodiesLoaded, static_cast<int>(hist.steps.size()),
                 path.c_str());
    // (Plane re-sync happens automatically — ConstructionPlanePlugin's
    // PlaneAddedEvent subscriber flips its own dirty flag during the
    // history replay above.)
    // Persist as the last-open project so the next launch can auto-reopen it.
    saveAppSettings();
    return true;
}

void Application::loadProjectWithProgress(const std::string& path) {
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::duration d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    };
    // Show something immediately so the window isn't a frozen blank.
    renderProgressFrame(-1.0f, "Loading project\xE2\x80\xA6");

    auto t0 = clock::now();
    bool ok = loadProjectAt(path);   // ProjectIO::load (BREP read) + history rebuild
    auto t1 = clock::now();
    if (ok) {
        // Tessellate up front HERE (between frames) so the per-body progress
        // frames are safe, instead of letting the first render frame block.
        m_pumpMeshProgress = true;
        rebuildMeshes();
        m_pumpMeshProgress = false;
        m_meshesDirty = false;
    }
    auto t2 = clock::now();
    std::fprintf(stderr, "[load-timing] parse+history=%lld ms  tessellate=%lld ms  total=%lld ms\n",
                 static_cast<long long>(ms(t1 - t0)),
                 static_cast<long long>(ms(t2 - t1)),
                 static_cast<long long>(ms(t2 - t0)));
}

void Application::addRecentProject(const std::string& ref, const std::string& name) {
    if (ref.empty()) return;
    constexpr size_t kMaxRecents = 10;
    // Drop any existing entry with the same ref, then push this to the front.
    for (auto it = m_recentProjects.begin(); it != m_recentProjects.end(); ) {
        if (it->ref == ref) it = m_recentProjects.erase(it);
        else                ++it;
    }
    AppSettings::RecentProject rp;
    rp.ref  = ref;
    rp.name = name.empty() ? ref : name;
    m_recentProjects.insert(m_recentProjects.begin(), rp);
    if (m_recentProjects.size() > kMaxRecents)
        m_recentProjects.resize(kMaxRecents);
    saveAppSettings();
}

void Application::removeRecentProject(const std::string& ref) {
    bool changed = false;
    for (auto it = m_recentProjects.begin(); it != m_recentProjects.end(); ) {
        if (it->ref == ref) { it = m_recentProjects.erase(it); changed = true; }
        else                ++it;
    }
    if (changed) saveAppSettings();
}

void Application::guardedOpen(std::function<void()> doOpen) {
    if (!isDirty()) { doOpen(); return; }
    // Unsaved changes: defer the open until the save prompt resolves so we never
    // silently discard work (this also closes the same gap on the Open dialog).
    m_pendingOpenAction = std::move(doOpen);
    m_postSaveAction = PostSaveAction::OpenProject;
    m_showSavePrompt = true;
}

void Application::openRecentProject(const AppSettings::RecentProject& r) {
    // Copy first: addRecentProject/removeRecentProject mutate m_recentProjects,
    // which may be the vector backing the reference `r`.
    const std::string ref  = r.ref;
    const std::string name = r.name;
    guardedOpen([this, ref, name]() {
#if defined(__ANDROID__)
        // ref is a persisted SAF content:// URI — resolve to a temp file, no picker.
        std::string tmp = materializr::androidOpenUri(ref);
        if (tmp.empty()) {
            showToast("Couldn't open \"" + name + "\" - access may have been revoked.");
            removeRecentProject(ref);
            return;
        }
        if (loadProjectAt(tmp)) addRecentProject(ref, name);  // bump to front
        else { showToast("Failed to open \"" + name + "\"."); removeRecentProject(ref); }
#else
        if (loadProjectAt(ref)) addRecentProject(ref, name);  // bump to front
        else {
            showToast("Couldn't open \"" + name + "\" - the file may have moved or been deleted.");
            removeRecentProject(ref);
        }
#endif
    });
}

void Application::loadProject() {
    FileDialogs::openFile("Open Project",
        {{"Materializr Project", "*.materializr"}, {"All Files", "*"}},
        [this](const std::string& path) {
            if (path.empty()) return;
            // Guard unsaved changes (the picked path is captured for after the
            // save prompt resolves), then load + record in Open Recent.
            guardedOpen([this, path]() {
                if (!loadProjectAt(path)) return;
                // Record with a *persistable* ref: the SAF content:// URI on
                // Android (the `path` is a throwaway temp there), the real path
                // on desktop.
                std::string ref, name;
#if defined(__ANDROID__)
                ref  = materializr::androidLastDocUri();
                name = materializr::androidLastDocName();
                if (ref.empty()) ref = path; // fallback (non-persistable provider)
#else
                ref = path;
#endif
                if (name.empty()) name = std::filesystem::path(path).filename().string();
                addRecentProject(ref, name);
            });
        });
}

void Application::closeProject() {
    // If nothing to lose, close immediately. If autosave is on and the project
    // already has a path, autosave quietly before closing. Otherwise (dirty +
    // no autosave) route through the save-prompt with CloseProject intent.
    if (!isDirty()) { doCloseProject(); return; }
    // The quiet-autosave shortcut only applies at the history tip — saving in
    // an undone state would silently drop the redo tail from the file (only
    // applied steps persist). Below the tip, fall through to the explicit
    // prompt so losing those steps is the user's call, not autosave's.
    if (m_autosaveEnabled && !m_currentProjectPath.empty() &&
        !(m_history && m_history->canRedo())) {
        saveProjectQuick();
        doCloseProject();
        return;
    }
    m_postSaveAction = PostSaveAction::CloseProject;
    m_showSavePrompt = true;
}

void Application::doCloseProject() {
    m_document->clear();
    m_history->clear();
    m_selection->clear();
    m_currentProjectPath.clear();
    m_savedAtHistoryStep = -1;
    m_unsavedNonHistoryChanges = false;
    m_meshesDirty = true;
    // Home view (empty scene → sensible default at origin), same as ViewCube Home.
    handleViewCubeAction(static_cast<int>(ViewCubeAction::FrontTopRight));
    // Persist: lastProjectPath now empty → no auto-open on next launch.
    saveAppSettings();
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
    m_window->requestClose(false);
}

void Application::renderSavePrompt() {
    if (m_showSavePrompt) {
        ImGui::OpenPopup("Unsaved Changes");
        m_showSavePrompt = false; // OpenPopup latches; only call once per request
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* prompt;
        switch (m_postSaveAction) {
            case PostSaveAction::CloseProject:
                prompt = "You have unsaved changes. Save before closing the project?"; break;
            case PostSaveAction::OpenProject:
                prompt = "You have unsaved changes. Save before opening another project?"; break;
            default:
                prompt = "You have unsaved changes. Save before exiting?"; break;
        }
        ImGui::Text("%s", prompt);
        ImGui::Separator();
        if (ImGui::Button("Save", ImVec2(100, 0))) {
            m_closeAfterSave = true;
            saveProjectQuick();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(100, 0))) {
            if (m_postSaveAction == PostSaveAction::CloseProject) {
                doCloseProject();
                m_postSaveAction = PostSaveAction::None;
            } else if (m_postSaveAction == PostSaveAction::OpenProject) {
                auto act = std::move(m_pendingOpenAction);
                m_pendingOpenAction = nullptr;
                m_postSaveAction = PostSaveAction::None;
                if (act) act();
            } else {
                m_confirmedClose = true;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            m_closeAfterSave = false;
            m_pendingOpenAction = nullptr;
            m_postSaveAction = PostSaveAction::None;
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
        [this](std::string path) {
            if (path.empty()) return;
            // Keep a STEP extension — accept either .step or .stp; append
            // .step only when the typed name has neither.
            std::string ext = std::filesystem::path(path).extension().string();
            if (ext != ".step" && ext != ".stp") path += ".step";
            auto result = StepIO::exportFile(path, *m_document);
            if (result.success) std::fprintf(stdout, "Exported to %s\n", path.c_str());
            else std::fprintf(stderr, "Export failed: %s\n", result.errorMessage.c_str());
        });
}

void Application::exportBodyAsStl(int bodyId) {
    if (!m_document || bodyId < 0) return;
    // Build a safe default filename from the body's name. Strip / replace
    // characters that the OS would reject in a filename so the dialog
    // doesn't open with an invalid suggestion the user has to fix.
    std::string name = m_document->getBodyName(bodyId);
    if (name.empty()) name = "body-" + std::to_string(bodyId);
    for (char& ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|') ch = '_';
    }
    std::string defaultFile = name + ".stl";

    TopoDS_Shape shape;
    try { shape = m_document->getBody(bodyId); } catch (...) {}
    if (shape.IsNull()) {
        std::fprintf(stderr, "Export STL: body %d has no geometry\n", bodyId);
        return;
    }

#if defined(__ANDROID__)
    // Touch: offer Share (to Drive/email/3D apps) or Save-to-device. Both context
    // menus (viewport long-press + Items panel) route here.
    FileDialogs::androidExportShareOrSave(defaultFile, "application/octet-stream",
        [shape](const std::string& path) {
            auto result = StlExport::exportShape(path, shape);
            if (result.success)
                std::fprintf(stdout, "Exported %d triangles to %s\n",
                             result.triangleCount, path.c_str());
            else
                std::fprintf(stderr, "STL export failed: %s\n", result.errorMessage.c_str());
            return result.success;
        });
#else
    FileDialogs::saveFile("Export Body to STL", defaultFile,
        {{"STL Files", "*.stl"}},
        [shape](std::string path) {
            if (path.empty()) return;
            // Keep the .stl extension — pfd/zenity don't force it, so a typed
            // name with no extension saved a valid but extensionless file
            // (mirrors the .materializr project-save enforcement).
            if (std::filesystem::path(path).extension() != ".stl")
                path += ".stl";
            auto result = StlExport::exportShape(path, shape);
            if (result.success) {
                std::fprintf(stdout, "Exported %d triangles to %s\n",
                             result.triangleCount, path.c_str());
            } else {
                std::fprintf(stderr, "STL export failed: %s\n",
                             result.errorMessage.c_str());
            }
        });
#endif
}

void Application::exportSketchAsSvg(int sketchId) {
    if (!m_document || sketchId < 0) return;
    auto sketch = m_document->getSketch(sketchId);
    if (!sketch) return;

    // Safe default filename from the sketch name (strip characters the OS rejects).
    std::string name = m_document->getSketchName(sketchId);
    if (name.empty()) name = "sketch-" + std::to_string(sketchId);
    for (char& ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|') ch = '_';
    }
    std::string defaultFile = name + ".svg";

    // Capture the shared_ptr so the (async) dialog callback can't dangle.
    auto sk = sketch;

#if defined(__ANDROID__)
    FileDialogs::androidExportShareOrSave(defaultFile, "image/svg+xml",
        [sk](const std::string& path) {
            auto result = materializr::SvgExport::exportSketch(path, *sk);
            if (result.success)
                std::fprintf(stdout, "Exported %d curve(s) to %s\n",
                             result.curveCount, path.c_str());
            else
                std::fprintf(stderr, "SVG export failed: %s\n", result.errorMessage.c_str());
            return result.success;
        });
#else
    FileDialogs::saveFile("Export Sketch to SVG", defaultFile,
        {{"SVG Files", "*.svg"}},
        [sk](std::string path) {
            if (path.empty()) return;
            // Keep the .svg extension — the picker doesn't force it.
            if (std::filesystem::path(path).extension() != ".svg") path += ".svg";
            auto result = materializr::SvgExport::exportSketch(path, *sk);
            if (result.success) {
                std::fprintf(stdout, "Exported %d curve(s) to %s\n",
                             result.curveCount, path.c_str());
            } else {
                std::fprintf(stderr, "SVG export failed: %s\n",
                             result.errorMessage.c_str());
            }
        });
#endif
}

void Application::combineSketches(const std::vector<int>& ids) {
    if (ids.size() < 2 || !m_document) return;
    auto target = m_document->getSketch(ids.front());
    if (!target) return;

    // Keep only the others that are COPLANAR with the target (parallel plane +
    // lying on it). Combining non-coplanar sketches has no meaning.
    const gp_Pln& tp = target->getPlane();
    gp_Vec tN(tp.Axis().Direction());
    gp_Pnt tO = tp.Location();
    std::vector<int> coplanar;
    for (size_t i = 1; i < ids.size(); ++i) {
        auto sk = m_document->getSketch(ids[i]);
        if (!sk) continue;
        const gp_Pln& sp = sk->getPlane();
        gp_Vec sN(sp.Axis().Direction());
        if (std::abs(sN.Dot(tN)) < 0.999) continue;
        gp_Vec d(sp.Location().X() - tO.X(), sp.Location().Y() - tO.Y(),
                 sp.Location().Z() - tO.Z());
        if (std::abs(d.Dot(tN)) > 0.05) continue;
        coplanar.push_back(ids[i]);
    }
    if (coplanar.empty()) {
        showToast("Combine needs sketches that share a plane.");
        return;
    }

    auto op = std::make_unique<CombineSketchesOp>();
    op->setTarget(ids.front(), *target);
    for (int oid : coplanar) {
        auto sk = m_document->getSketch(oid);
        if (sk) op->addOther(oid, *sk, m_document->getSketchName(oid),
                             m_document->isSketchVisible(oid));
    }
    if (m_history->pushOperation(std::move(op), *m_document)) {
        m_selection->clear();
        markDirty();
        m_meshesDirty = true;
        std::fprintf(stdout, "Combined %d sketch(es) into %d\n",
                     static_cast<int>(coplanar.size()), ids.front());
    }
}

void Application::duplicateSketch(int sketchId) {
    if (!m_document || !m_history) return;
    auto src = m_document->getSketch(sketchId);
    if (!src) return;

    // Independent deep copy: geometry, constraints, plane and source-body link
    // all come along, but it's a separate Sketch object with its own id, so
    // editing it never touches the original or any body built from it.
    auto copy = std::make_shared<Sketch>(*src);

    std::string base = m_document->getSketchName(sketchId);
    if (base.empty()) base = "Sketch";
    const std::string name = base + " copy";

    auto op = std::make_unique<DuplicateSketchOp>();
    op->setCopy(copy, sketchId, name);
    DuplicateSketchOp* raw = op.get();  // valid while History owns the op
    if (m_history->pushOperation(std::move(op), *m_document)) {
        markDirty();
        m_meshesDirty = true;
        std::fprintf(stdout, "Duplicated sketch %d -> %d\n",
                     sketchId, raw->newSketchId());
        showToast("Duplicated \"" + base + "\" \xE2\x80\x94 edit the copy freely "
                  "(e.g. resize holes); the original is untouched.");
    }
}

void Application::enterSketchMode() {
    // If a planar face is selected, route through enterSketchOnFace for consistency
    if (m_selection && m_selection->hasSelectedFaces()) {
        const auto& sel = m_selection->getSelection();
        for (const auto& entry : sel) {
            if (entry.type == SelectionType::Face && !entry.shape.IsNull()) {
                enterSketchOnFace(TopoDS::Face(entry.shape), entry.bodyId);
                return;
            }
        }
    }

    m_activeSketch = std::make_shared<Sketch>();
    m_sketchSolver = std::make_unique<SketchSolver>();
    m_activeSketchId = -1;

    m_sketchTool->setSketch(m_activeSketch.get());
    m_sketchTool->setSolver(m_sketchSolver.get());
    m_sketchSolver->setSketch(m_activeSketch.get());
    m_sketchTool->setMode(SketchToolMode::Line);
    m_inSketchMode = true;
    m_sketchEntryHistoryStep = m_history ? m_history->currentStep() : -1;
    if (m_history) m_history->setUndoFloor(m_sketchEntryHistoryStep);  // no undo past sketch entry
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
    m_sketchSolver->setSketch(m_activeSketch.get());
    m_sketchTool->setMode(SketchToolMode::Line);
    m_inSketchMode = true;
    m_sketchEntryHistoryStep = m_history ? m_history->currentStep() : -1;
    if (m_history) m_history->setUndoFloor(m_sketchEntryHistoryStep);  // no undo past sketch entry
    m_toolbar->setSketchMode(true);
    alignCameraToActiveSketch();
}

void Application::enterSketchOnFace(const TopoDS_Face& face, int sourceBodyId) {
    // Sketching needs a FLAT face. A curved face (cylinder / sphere / fillet)
    // has no single plane — we'd otherwise drop the sketch onto a tangent plane
    // at an arbitrary point on the curve, which isn't useful and a construction
    // plane (Add Plane…) covers properly. Refuse with guidance.
    //
    // Detect planarity GEOMETRICALLY, not by surface type: a face can be flat
    // while backed by a non-Geom_Plane surface — e.g. a slanted side face
    // produced by scaling a box's top into a frustum is a planar trapezoid on a
    // ruled/BSpline surface. A literal Geom_Plane type-check called those
    // "curved" by mistake. GeomLib_IsPlanarSurface accepts the flat ones (and
    // recovers their plane) while still rejecting genuinely warped faces.
    gp_Pln pln;
    {
        Handle(Geom_Surface) s = BRep_Tool::Surface(face);
        bool planar = false;
        if (!s.IsNull()) {
            if (s->IsKind(STANDARD_TYPE(Geom_Plane))) {
                pln = Handle(Geom_Plane)::DownCast(s)->Pln();
                planar = true;
            } else {
                GeomLib_IsPlanarSurface tester(s, 1.0e-7);
                if (tester.IsPlanar()) { pln = tester.Plan(); planar = true; }
            }
        }
        if (!planar) {
            showToast("Can't sketch on a curved face \xE2\x80\x94 use Add "
                      "Plane\xE2\x80\xA6 to place a construction plane.");
            return;
        }
    }

    // Align the sketch plane's X axis to the face's LONGEST straight edge so the
    // grid runs parallel to the face. The plane recovered from the surface uses
    // the surface's intrinsic parametric X, which for a lofted face (e.g. a
    // scaled-down box top) can sit ~45° off the visible edges. For an ordinary
    // box face this is a no-op or a 90° turn that looks identical on a square
    // grid; a face with no straight edge (a circular cap) keeps the surface X.
    {
        const gp_Dir n = pln.Position().Direction();
        gp_Dir bestX;
        double bestLen = -1.0;
        bool found = false;
        for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
            BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
            if (c.GetType() != GeomAbs_Line) continue;
            gp_Pnt p0, p1;
            c.D0(c.FirstParameter(), p0);
            c.D0(c.LastParameter(), p1);
            gp_Vec ev(p0, p1);
            // Project the edge into the plane; skip edges ~parallel to the normal.
            gp_Vec proj = ev - gp_Vec(n) * (ev * gp_Vec(n));
            double L = proj.Magnitude();
            if (L > bestLen + 1e-9) { bestLen = L; bestX = gp_Dir(proj); found = (L > 1e-6); }
        }
        if (found) {
            gp_Ax3 ax(pln.Position().Location(), n, bestX);
            pln = gp_Pln(ax);
        }
    }

    m_activeSketch = std::make_shared<Sketch>();
    m_sketchSolver = std::make_unique<SketchSolver>();
    m_activeSketchId = -1;

    // Remember which body this face belongs to so a later Subtract (and other
    // body-relative ops) know what to cut from. Every face-sketch entry point
    // routes through here, so setting it here keeps the source body consistent.
    m_activeSketch->setSourceBody(sourceBodyId);

    {
        // `pln` was computed above and already handles planar faces whose surface
        // isn't a literal Geom_Plane (scaled-frustum side faces, etc.).
        // Honour the face's topological orientation: a REVERSED face's outward
        // normal is opposite to its surface normal, so flip the sketch plane so
        // the camera lands on the visible side of the face.
        if (face.Orientation() == TopAbs_REVERSED) {
            gp_Ax3 ax = pln.Position();
            ax.ZReverse();
            pln = gp_Pln(ax);
        }
        // STEP-imported faces sometimes carry an orientation flag that doesn't
        // match the geometric outward direction, so the orientation check
        // alone isn't enough — we end up with a sketch plane pointing INTO
        // the body and push/pull goes the wrong way. Verify by probing the
        // body's solid classifier on BOTH sides of the face: one direction
        // should be OUT and the other IN. If the directions are reversed
        // (forward is IN, opposite is OUT), flip the plane. We probe both
        // sides at a generous offset (1 mm) so tessellation slack near the
        // surface doesn't confuse the classifier.
        if (sourceBodyId >= 0) {
            try {
                const TopoDS_Shape& body = m_document->getBody(sourceBodyId);
                if (!body.IsNull()) {
                    Bnd_Box bb;
                    BRepBndLib::Add(face, bb);
                    if (!bb.IsVoid()) {
                        double xmin, ymin, zmin, xmax, ymax, zmax;
                        bb.Get(xmin, ymin, zmin, xmax, ymax, zmax);
                        gp_Pnt c((xmin + xmax) * 0.5,
                                 (ymin + ymax) * 0.5,
                                 (zmin + zmax) * 0.5);
                        gp_Dir nd = pln.Position().Direction();
                        const double eps = 1.0; // mm
                        gp_Pnt fwd(c.X() + nd.X() * eps,
                                   c.Y() + nd.Y() * eps,
                                   c.Z() + nd.Z() * eps);
                        gp_Pnt back(c.X() - nd.X() * eps,
                                    c.Y() - nd.Y() * eps,
                                    c.Z() - nd.Z() * eps);
                        BRepClass3d_SolidClassifier fwdCls(body, fwd,  1e-6);
                        BRepClass3d_SolidClassifier backCls(body, back, 1e-6);
                        bool fwdIsIn  = (fwdCls.State()  == TopAbs_IN);
                        bool backIsIn = (backCls.State() == TopAbs_IN);
                        // Only flip if we have a clear "forward is inside,
                        // opposite is outside" disagreement. Mixed / ambiguous
                        // states (ON / UNKNOWN) leave the existing direction
                        // alone so we don't double-flip a face that was
                        // already correctly oriented by the topology check.
                        if (fwdIsIn && !backIsIn) {
                            gp_Ax3 ax = pln.Position();
                            ax.ZReverse();
                            pln = gp_Pln(ax);
                        }
                    }
                }
            } catch (...) {}
        }
        m_activeSketch->setPlane(pln);
        m_activeSketch->setSourceFace(face);

        // Walk the face's vertices and edges, project them onto the sketch
        // plane in 2D, and stash them on the Sketch as reference geometry.
        // The inference snap reads these so the cursor can land on the host
        // face's existing corners / edge midpoints / straight edges even
        // before any sketch elements are drawn.
        {
            Sketch::FaceReference refs;
            const gp_Ax3& ax3 = pln.Position();
            gp_Pnt O = ax3.Location();
            gp_Dir Xd = ax3.XDirection();
            gp_Dir Yd = ax3.YDirection();
            auto project = [&](const gp_Pnt& p) -> glm::vec2 {
                double dx = p.X() - O.X();
                double dy = p.Y() - O.Y();
                double dz = p.Z() - O.Z();
                double u = dx * Xd.X() + dy * Xd.Y() + dz * Xd.Z();
                double v = dx * Yd.X() + dy * Yd.Y() + dz * Yd.Z();
                return glm::vec2(static_cast<float>(u), static_cast<float>(v));
            };
            auto dedup = [](std::vector<glm::vec2>& v, glm::vec2 p) {
                for (const auto& q : v) {
                    if (glm::length(q - p) < 1e-4f) return;
                }
                v.push_back(p);
            };

            // Vertices — the face's corner points.
            for (TopExp_Explorer ex(face, TopAbs_VERTEX); ex.More(); ex.Next()) {
                gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(ex.Current()));
                dedup(refs.points, project(p));
            }
            // Edges — for straight edges, also stash the endpoint pair as a
            // reference line (so on-line / midpoint inferences can fire) and
            // the midpoint as a reference point. Curved edges get their
            // endpoints (already covered by vertex iteration above).
            for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
                TopoDS_Edge edge = TopoDS::Edge(ex.Current());
                BRepAdaptor_Curve curve(edge);
                double f = curve.FirstParameter();
                double l = curve.LastParameter();
                gp_Pnt pStart, pEnd;
                curve.D0(f, pStart);
                curve.D0(l, pEnd);
                glm::vec2 a = project(pStart);
                glm::vec2 b = project(pEnd);
                if (curve.GetType() == GeomAbs_Line) {
                    refs.lines.emplace_back(a, b);
                    dedup(refs.points, 0.5f * (a + b)); // midpoint
                } else if (curve.GetType() == GeomAbs_Circle) {
                    // Circle / arc edge — add the centre as a snap point
                    // (very common target: hole centres, fillet centres).
                    // Also sample the perimeter so the cursor can catch the
                    // curve itself along a few spots until proper curve-
                    // perimeter snapping ships.
                    gp_Circ circ = curve.Circle();
                    dedup(refs.points, project(circ.Location()));
                    const int samples = 8;
                    for (int i = 1; i < samples; ++i) {
                        double t = f + (l - f) * (double(i) / samples);
                        gp_Pnt p;
                        curve.D0(t, p);
                        dedup(refs.points, project(p));
                    }
                } else if (curve.GetType() == GeomAbs_Ellipse) {
                    // Ellipse also has a centre; treat it as a snap target.
                    gp_Elips el = curve.Ellipse();
                    dedup(refs.points, project(el.Location()));
                    const int samples = 8;
                    for (int i = 1; i < samples; ++i) {
                        double t = f + (l - f) * (double(i) / samples);
                        gp_Pnt p;
                        curve.D0(t, p);
                        dedup(refs.points, project(p));
                    }
                } else {
                    // Splines / hyperbolas / etc. — just sample perimeter
                    // points so something snappable exists along the curve.
                    const int samples = 8;
                    for (int i = 1; i < samples; ++i) {
                        double t = f + (l - f) * (double(i) / samples);
                        gp_Pnt p;
                        curve.D0(t, p);
                        dedup(refs.points, project(p));
                    }
                }
            }
            m_activeSketch->setFaceReferences(std::move(refs));
        }
    }

    m_sketchTool->setSketch(m_activeSketch.get());
    m_sketchTool->setSolver(m_sketchSolver.get());
    m_sketchSolver->setSketch(m_activeSketch.get());
    m_sketchTool->setMode(SketchToolMode::Line);
    m_inSketchMode = true;
    m_sketchEntryHistoryStep = m_history ? m_history->currentStep() : -1;
    if (m_history) m_history->setUndoFloor(m_sketchEntryHistoryStep);  // no undo past sketch entry
    m_toolbar->setSketchMode(true);
    alignCameraToActiveSketch();
}

void Application::applySketchConstraint(ConstraintType type) {
    if (!m_inSketchMode || !m_activeSketch || !m_sketchTool || !m_sketchSolver) return;

    const auto& selPts = m_sketchTool->getSelectedPoints();
    const auto& selLns = m_sketchTool->getSelectedLines();

    auto pushConstraint = [&](ConstraintType t, int a, int b = -1,
                              double v = 0.0, double vy = 0.0) {
        Constraint c;
        c.id = 0; // sketch assigns
        c.type = t;
        c.entityA = a;
        c.entityB = b;
        c.value = v;
        c.valueY = vy;
        c.isSatisfied = false;
        m_activeSketch->addConstraint(c);
    };

    int added = 0;
    // Wrap the whole mutation in recordSketchMutation so the constraint add
    // (+ subsequent solver pass) becomes a single SketchEditOp on the history
    // stack. Ctrl+Z removes the constraint(s) just added. Description is
    // specialised by SketchEditOp::description() reading the constraint diff.
    recordSketchMutation([&]{
    switch (type) {
        case ConstraintType::Horizontal:
        case ConstraintType::Vertical: {
            // Apply to every selected line independently.
            for (int lid : selLns) {
                pushConstraint(type, lid);
                ++added;
            }
            break;
        }
        case ConstraintType::Coincident: {
            // Chain pairs: (p0,p1), (p0,p2), ... so any number of points fuse
            // to the same spot. (Pairwise from the first is cheaper than full
            // mesh and the solver converges to the same result.)
            std::vector<int> v(selPts.begin(), selPts.end());
            for (size_t i = 1; i < v.size(); ++i) {
                pushConstraint(ConstraintType::Coincident, v[0], v[i]);
                ++added;
            }
            break;
        }
        case ConstraintType::Parallel:
        case ConstraintType::Perpendicular:
        case ConstraintType::Equal: {
            // Each subsequent line gets bound to the first.
            std::vector<int> v(selLns.begin(), selLns.end());
            for (size_t i = 1; i < v.size(); ++i) {
                pushConstraint(type, v[0], v[i]);
                ++added;
            }
            break;
        }
        case ConstraintType::Fixed: {
            // Pin each selected point at its CURRENT position.
            for (int pid : selPts) {
                const SketchPoint* pp = m_activeSketch->getPoint(pid);
                if (!pp) continue;
                pushConstraint(ConstraintType::Fixed, pid, -1,
                               static_cast<double>(pp->pos.x),
                               static_cast<double>(pp->pos.y));
                ++added;
            }
            break;
        }
        case ConstraintType::Distance: {
            // Pairwise from the first selected point — initial value is the
            // geometry's current distance, so the constraint isn't immediately
            // destructive (it just locks the present distance in place).
            std::vector<int> v(selPts.begin(), selPts.end());
            for (size_t i = 1; i < v.size(); ++i) {
                const SketchPoint* p0 = m_activeSketch->getPoint(v[0]);
                const SketchPoint* pi = m_activeSketch->getPoint(v[i]);
                if (!p0 || !pi) continue;
                double dist = static_cast<double>(glm::length(p0->pos - pi->pos));
                pushConstraint(ConstraintType::Distance, v[0], v[i], dist);
                ++added;
            }
            break;
        }
        case ConstraintType::Angle: {
            // Each subsequent line bound to the first; initial value is the
            // signed angle the geometry currently makes.
            std::vector<int> v(selLns.begin(), selLns.end());
            if (v.size() < 2) break;
            const auto& lines = m_activeSketch->getLines();
            auto lineDir = [&](int id) {
                for (const auto& l : lines) {
                    if (l.id != id) continue;
                    const SketchPoint* s = m_activeSketch->getPoint(l.startPointId);
                    const SketchPoint* e = m_activeSketch->getPoint(l.endPointId);
                    if (s && e) return e->pos - s->pos;
                    break;
                }
                return glm::vec2(0.0f);
            };
            glm::vec2 dirA = lineDir(v[0]);
            if (glm::length(dirA) < 1e-6f) break;
            float angA = std::atan2(dirA.y, dirA.x);
            for (size_t i = 1; i < v.size(); ++i) {
                glm::vec2 dirB = lineDir(v[i]);
                if (glm::length(dirB) < 1e-6f) continue;
                float angB = std::atan2(dirB.y, dirB.x);
                double signedAngle = static_cast<double>(angB - angA);
                const double TWO_PI = 2.0 * M_PI;
                while (signedAngle >  M_PI) signedAngle -= TWO_PI;
                while (signedAngle < -M_PI) signedAngle += TWO_PI;
                pushConstraint(ConstraintType::Angle, v[0], v[i], signedAngle);
                ++added;
            }
            break;
        }
        case ConstraintType::Radius: {
            // Lock each selected circle / arc at its current radius. Adding
            // the constraint is non-destructive; user edits the value later.
            const auto& selC = m_sketchTool->getSelectedCircles();
            const auto& selA = m_sketchTool->getSelectedArcs();
            for (int cid : selC) {
                for (const auto& circ : m_activeSketch->getCircles()) {
                    if (circ.id == cid) {
                        pushConstraint(ConstraintType::Radius, cid, -1, circ.radius);
                        ++added;
                        break;
                    }
                }
            }
            for (int aid : selA) {
                for (const auto& arc : m_activeSketch->getArcs()) {
                    if (arc.id == aid) {
                        pushConstraint(ConstraintType::Radius, aid, -1, arc.radius);
                        ++added;
                        break;
                    }
                }
            }
            break;
        }
        case ConstraintType::Tangent: {
            // Tangent constraint takes one arc/circle (entityA) and one line
            // (entityB). Pair every selected curve with every selected line.
            const auto& selC = m_sketchTool->getSelectedCircles();
            const auto& selA = m_sketchTool->getSelectedArcs();
            for (int lid : selLns) {
                for (int cid : selC) {
                    pushConstraint(ConstraintType::Tangent, cid, lid);
                    ++added;
                }
                for (int aid : selA) {
                    pushConstraint(ConstraintType::Tangent, aid, lid);
                    ++added;
                }
            }
            break;
        }
        case ConstraintType::Concentric: {
            // Two circles / arcs share a centre. Build a flat list of selected
            // curves and pair the first with each subsequent.
            std::vector<int> curves;
            for (int cid : m_sketchTool->getSelectedCircles()) curves.push_back(cid);
            for (int aid : m_sketchTool->getSelectedArcs())    curves.push_back(aid);
            for (size_t i = 1; i < curves.size(); ++i) {
                pushConstraint(ConstraintType::Concentric, curves[0], curves[i]);
                ++added;
            }
            break;
        }
        default:
            break;
    }

    if (added > 0) {
        m_sketchSolver->setSketch(m_activeSketch.get());
        m_sketchSolver->solve(*m_activeSketch);
    }
    }); // end recordSketchMutation
    if (added > 0) markDirty();
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
        // Constraints too — including their values so an edit (not just an
        // add / remove) registers as a mutation and pushes a history step.
        mix(s.getConstraints().size());
        for (const auto& c : s.getConstraints()) {
            mix(static_cast<size_t>(c.id));
            mix(static_cast<size_t>(c.type));
            mix(static_cast<size_t>(c.entityA));
            mix(static_cast<size_t>(c.entityB));
            size_t vb; std::memcpy(&vb, &c.value, sizeof(vb));
            mix(vb);
            std::memcpy(&vb, &c.valueY, sizeof(vb));
            mix(vb);
        }
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

void Application::sketchChainBack() {
    if (!m_inSketchMode || !m_sketchTool) return;
    SketchToolMode m = m_sketchTool->getMode();
    if (m == SketchToolMode::Line) {
        if (m_sketchTool->lineSegmentCount() < 1) return;
        // dropLineChainTail removes EXACTLY the tracked last segment; wrapping
        // it makes the removal one undoable step. (Don't prune here — backing
        // to the lone start vertex should KEEP it as the chain's live anchor.)
        recordSketchMutation([&]{ m_sketchTool->dropLineChainTail(); });
    } else if (m == SketchToolMode::Spline) {
        // Spline control points live in the tool until Confirm (no per-point
        // history step), so just pop the last one.
        m_sketchTool->removeLastSplinePoint();
    } else {
        return;
    }
    m_meshesDirty = true;
}

void Application::sketchChainCancel() {
    if (!m_inSketchMode || !m_sketchTool) return;
    if (m_sketchTool->getMode() == SketchToolMode::Line) {
        // Peel every segment of THIS chain in one undo step (each dropLineChainTail
        // removes the current tail; it returns false once only the start is left).
        recordSketchMutation([&]{ while (m_sketchTool->dropLineChainTail()) {} });
    }
    // Reset placement (drops the spline points / the lone start vertex). Any
    // now-disconnected start vertex is swept below.
    m_sketchTool->onCancel();
    if (m_activeSketch) m_activeSketch->pruneOrphanPoints();
    m_meshesDirty = true;
}

void Application::deleteSelectedSketchElements() {
    if (!m_inSketchMode || !m_activeSketch || !m_sketchTool) return;
    // Delete the SketchTool's element selection (points + lines) if any,
    // wrapped in recordSketchMutation so Ctrl+Z / Undo brings them back.
    const auto pts = m_sketchTool->getSelectedPoints();
    const auto lns = m_sketchTool->getSelectedLines();
    if (pts.empty() && lns.empty()) return;
    recordSketchMutation([&]{
        for (int lid : lns) m_activeSketch->removeElement(lid);
        for (int pid : pts) m_activeSketch->removeElement(pid);
        // Deleting a line leaves its two endpoints behind (they weren't in
        // the selection) — sweep up the now-unreferenced points so no orphan
        // vertices linger.
        m_activeSketch->pruneOrphanPoints();
    });
    m_sketchTool->clearElementSelection();
    markDirty();
}

void Application::frameSelection() {
    Bnd_Box bb;
    // Selection first.
    if (m_selection) {
        for (const auto& e : m_selection->getSelection()) {
            if (e.bodyId < 0) continue;
            try {
                const TopoDS_Shape& s = m_document->getBody(e.bodyId);
                if (!s.IsNull()) BRepBndLib::AddOptimal(s, bb,
                                                        Standard_False, Standard_False);
            } catch (...) {}
        }
    }
    // Fall back to all visible bodies.
    if (bb.IsVoid()) {
        for (int id : m_document->getAllBodyIds()) {
            if (!m_document->isBodyVisible(id)) continue;
            try {
                const TopoDS_Shape& s = m_document->getBody(id);
                if (!s.IsNull()) BRepBndLib::AddOptimal(s, bb,
                                                        Standard_False, Standard_False);
            } catch (...) {}
        }
    }
    if (!bb.IsVoid()) {
        Standard_Real x0,y0,z0,x1,y1,z1;
        bb.Get(x0,y0,z0,x1,y1,z1);
        m_viewport->getCamera().zoomToFit(
            glm::vec3((float)x0,(float)y0,(float)z0),
            glm::vec3((float)x1,(float)y1,(float)z1));
    }
}

void Application::editSketch(int sketchId) {
    auto sketch = m_document->getSketch(sketchId);
    if (!sketch) return;

    // For sketches loaded from a previous session, sourceFace isn't part of
    // the project file — re-bind it from the host body before the user
    // starts editing / using sketch regions.
    ensureSketchSourceFace(sketchId);

    m_activeSketch = sketch; // shared ownership - edits go straight to the stored sketch
    m_sketchSolver = std::make_unique<SketchSolver>();
    m_activeSketchId = sketchId;

    m_sketchTool->setSketch(m_activeSketch.get());
    m_sketchTool->setSolver(m_sketchSolver.get());
    m_sketchSolver->setSketch(m_activeSketch.get());
    // When re-entering an existing sketch the user is much more likely to want
    // to tweak it than draw fresh geometry; start in Select/Move mode so they
    // can immediately click & drag points/lines.
    m_sketchTool->setMode(SketchToolMode::Select);
    m_inSketchMode = true;
    m_sketchEntryHistoryStep = m_history ? m_history->currentStep() : -1;
    if (m_history) m_history->setUndoFloor(m_sketchEntryHistoryStep);  // no undo past sketch entry
    m_toolbar->setSketchMode(true);
    m_selection->clear();
    alignCameraToActiveSketch();
}

void Application::extrudeSketchById(int sketchId, ExtrudeMode mode) {
    auto sketch = m_document->getSketch(sketchId);
    if (!sketch) return;
    // Even-odd island compound — multi-shape sketches (SVG, text) extrude
    // every island with its proper holes instead of feeding OCCT one face
    // with disjoint "holes" (which came out non-manifold).
    TopoDS_Shape profile = sketch->buildProfileShape();
    if (profile.IsNull()) {
        std::fprintf(stderr, "Sketch has no closed profile to extrude\n");
        return;
    }

    int targetBody = -1;
    if (mode == ExtrudeMode::Subtract) {
        targetBody = sketch->getSourceBody();
        if (targetBody < 0) {
            std::fprintf(stderr, "Subtract needs a sketch drawn on a body face; "
                                 "this sketch has no source body\n");
            return;
        }
    }
    beginInteractiveExtrude(profile, mode, targetBody, sketchId);
}

void Application::subtractSketchRegion(int sketchId, int regionIndex) {
    auto sketch = m_document->getSketch(sketchId);
    if (!sketch) return;

    int targetBody = sketch->getSourceBody();
    if (targetBody < 0) {
        std::fprintf(stderr, "Subtract needs a sketch drawn on a body face; "
                             "this sketch has no source body\n");
        return;
    }

    auto regions = sketch->buildRegions();
    if (regionIndex < 0 || regionIndex >= static_cast<int>(regions.size())) return;
    const TopoDS_Face& profile = regions[regionIndex].face;
    if (profile.IsNull()) {
        std::fprintf(stderr, "Sketch region has no profile face to subtract\n");
        return;
    }
    beginInteractiveExtrude(profile, ExtrudeMode::Subtract, targetBody, sketchId);
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

    // Frame the host face when one is present: target the face's 3D centre
    // (the plane origin may not coincide with it for off-centre faces), and
    // size the ortho box to its bbox diagonal.
    float orthoSize = std::max(20.0f, m_sketchGridStep * 40.0f);
    glm::vec3 lookAt = planeOrigin;
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
                lookAt = glm::vec3(static_cast<float>((xmin + xmax) * 0.5),
                                   static_cast<float>((ymin + ymax) * 0.5),
                                   static_cast<float>((zmin + zmax) * 0.5));
            }
        } catch (...) {}
    }

    // Snap the look-at point to the nearest world-grid intersection PROJECTED
    // onto the sketch plane. The grid in the viewport then draws lines that
    // pass through actual world-grid positions on this plane (so a "1 mm"
    // grid step lands on whole-mm boundaries even when the face's centre is
    // at fractional world coords). The same snapped point doubles as the
    // camera target — when the user later orbits out of ortho, the orbit
    // pivots around this stable, world-aligned anchor close to the face.
    {
        float step = std::max(m_sketchGridStep, 0.01f);
        glm::vec3 rounded(std::round(lookAt.x / step) * step,
                          std::round(lookAt.y / step) * step,
                          std::round(lookAt.z / step) * step);
        lookAt = rounded - normal * glm::dot(rounded - planeOrigin, normal);
    }
    m_sketchSnappedAnchor = lookAt;

    Camera& cam = m_viewport->getCamera();
    float standoff = std::max(orthoSize * 4.0f, 10.0f);

    // Pick an "up" direction that keeps the apparent orientation as close to
    // the user's previous view as possible.
    //
    // First try projecting the camera's current up onto the sketch plane;
    // works for vertical / tilted faces where the previous up has a useful
    // component in-plane. For HORIZONTAL faces (top / bottom), the camera's
    // up axis is parallel to the plane normal so the projection is zero —
    // fall back to projecting the camera's horizontal FORWARD direction
    // instead. That preserves turntable continuity: whichever way the user
    // was facing before clicking the face ends up at the top of the new
    // sketch view, instead of jumping to the face's arbitrary internal Y
    // direction (which causes the random 90° rotations on top / bottom).
    glm::vec3 chosenUp = up; // ultimate fallback: face's own Y
    glm::vec3 prevUp = cam.getUp();
    glm::vec3 projUp = prevUp - normal * glm::dot(prevUp, normal);
    if (glm::length(projUp) > 0.1f) {
        chosenUp = glm::normalize(projUp);
    } else {
        glm::vec3 fwd = cam.getTarget() - cam.getPosition();
        glm::vec3 projFwd = fwd - normal * glm::dot(fwd, normal);
        if (glm::length(projFwd) > 0.1f) {
            chosenUp = glm::normalize(projFwd);
        }
    }
    // Snap the chosen up to the nearest 90° of the face's natural axes so the
    // view always lands axis-aligned. Without this the up vector inherits any
    // arbitrary yaw the user had before clicking the face — the sketch comes
    // out "cocked" at whatever orbit angle they happened to be in.
    glm::vec3 faceY = up;
    glm::vec3 faceX = glm::cross(faceY, normal); // in-plane, perpendicular to faceY
    if (glm::length(faceX) > 1e-4f) faceX = glm::normalize(faceX);
    float dY = glm::dot(chosenUp, faceY);
    float dX = glm::dot(chosenUp, faceX);
    if (std::abs(dY) >= std::abs(dX)) {
        chosenUp = (dY >= 0.0f) ? faceY : -faceY;
    } else {
        chosenUp = (dX >= 0.0f) ? faceX : -faceX;
    }

    // Stand off on the face's OUTWARD side. The sketch plane's normal is
    // the underlying surface's — a REVERSED face stores it pointing INTO
    // the body, which used to fling the camera to the far side of the part
    // ("still flat with it, but opposite side" — Steve, on a narrow side
    // face). BRepGProp_Face::Normal applies the orientation flag.
    glm::vec3 standDir = normal;
    if (!m_activeSketch->getSourceFace().IsNull()) {
        try {
            BRepGProp_Face gpFace(TopoDS::Face(m_activeSketch->getSourceFace()));
            double u1, u2, v1, v2;
            gpFace.Bounds(u1, u2, v1, v2);
            gp_Pnt p;
            gp_Vec nv;
            gpFace.Normal(0.5 * (u1 + u2), 0.5 * (v1 + v2), p, nv);
            if (nv.Magnitude() > 1e-9) {
                glm::vec3 outward(static_cast<float>(nv.X()),
                                  static_cast<float>(nv.Y()),
                                  static_cast<float>(nv.Z()));
                if (glm::dot(outward, normal) < 0.0f) standDir = -normal;
            }
        } catch (...) {}
    }

    cam.setTarget(lookAt);
    cam.setPosition(lookAt + standDir * standoff);
    cam.setUp(chosenUp);
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


void Application::exitSketchMode() {
    m_inSketchMode = false;
    if (m_history) m_history->clearUndoFloor();  // undo is unrestricted again
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
    } else if (m_activeSketch) {
        // An EXISTING sketch the user emptied during this edit: drop it from
        // the document rather than leaving a blank entry in the Items panel.
        m_document->removeSketch(m_activeSketchId);
        markDirty();
        std::fprintf(stdout, "Sketch %d removed (emptied)\n", m_activeSketchId);
    }

    m_activeSketch.reset();
    m_sketchSolver.reset();
    m_activeSketchId = -1;
    m_meshesDirty = true; // refresh sketch rendering set

    // The sketch is resolved (committed to the document or discarded), so the
    // crash-recovery draft is no longer "unfinished" — drop it. A draft only
    // survives to the next launch when the app exits WITHOUT reaching here.
    materializr::clearSketchDraft();
    m_lastDraftElemCount = -1;

    // Stay where the user is — don't yank them back to the pre-sketch camera.
    // Exiting sketch should feel like leaving ortho-snap mode: the area being
    // looked at remains framed, only the sketch grid disappears. Any orbit
    // they do drops ortho mode and returns to perspective with a level
    // horizon (handled in Camera::orbitLevel).
}

void Application::writeSketchDraftIfDue() {
    // Periodically persist the in-progress sketch so a crash / kill doesn't lose
    // it. Cheap (sketches are tiny); throttled to ~2 s and skipped when nothing
    // changed since the last write.
    if (!m_inSketchMode || !m_activeSketch) return;
    int elems = m_activeSketch->elementCount();
    if (elems == 0) return; // nothing worth recovering yet
    double now = ImGui::GetTime();
    if (elems == m_lastDraftElemCount && now - m_lastDraftWrite < 2.0) return;
    if (now - m_lastDraftWrite < 0.75) return; // hard floor against thrashing
    materializr::writeSketchDraft(*m_activeSketch,
                                  m_activeSketch->getSourceBody(),
                                  m_currentProjectPath);
    m_lastDraftWrite = now;
    m_lastDraftElemCount = elems;
}

void Application::renderSketchRecoveryPrompt() {
    if (!m_pendingSketchRecovery) return;
    ImGui::OpenPopup("Recover Sketch?");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Recover Sketch?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            "An unfinished sketch from your last session was found.");
        ImGui::TextDisabled(
            "It wasn't committed before the app closed (a crash, or a restart).");
        ImGui::Spacing();
        if (ImGui::Button("Restore it", ImVec2(140, 0))) {
            restoreSketchDraftNow();
            m_pendingSketchRecovery = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(140, 0))) {
            materializr::clearSketchDraft();
            m_pendingSketchRecovery = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Application::restoreSketchDraftNow() {
    Sketch draft;
    materializr::SketchDraftMeta meta;
    if (!materializr::readSketchDraft(draft, meta) || !meta.valid) {
        materializr::clearSketchDraft();
        return;
    }
    // Re-enter sketch mode on the draft's plane (empty sketch; sets the undo
    // boundary here), then graft the geometry in AS A RECORDED MUTATION so the
    // restore is one undoable history step. Without this the restored geometry
    // had no history behind it, so Ctrl+Z couldn't touch it (the per-stroke
    // history from before the crash isn't in the draft — only the final shape).
    // (Face sketches re-bind their host face at Finish via ensureSketchSourceFace;
    // here we just restore the drawing on its plane so no work is lost.)
    enterSketchOnPlane(draft.getPlane());
    recordSketchMutation([&]{
        *m_activeSketch = draft;             // copy geometry + ids + constraints
        m_activeSketch->setSourceBody(meta.sourceBodyId);
    });
    m_sketchSolver->setSketch(m_activeSketch.get());
    m_sketchTool->setSketch(m_activeSketch.get());
    alignCameraToActiveSketch();
    m_meshesDirty = true;
    m_lastDraftElemCount = -1; // force a fresh draft write going forward
    std::fprintf(stdout, "[Recovery] restored in-progress sketch (%d elements)\n",
                 m_activeSketch->elementCount());
}

void Application::writeProjectRecoveryIfDue() {
    // Snapshot the whole project to the crash-recovery sidecar — including an
    // UNSAVED one, which the user-facing autosave can't touch (it needs a path).
    // Snapshots immediately when a new step commits (so a hang in the NEXT op's
    // preview loses nothing), else throttled for non-structural dirtiness.
    if (m_pendingProjectRecovery) return;      // don't clobber a snapshot we may restore
    if (!isDirty()) return;                    // nothing unsaved to protect
    // Same guards as autosave: never snapshot a half-baked live preview / sketch,
    // and never below the history tip (the file only persists applied steps, so a
    // below-tip save would silently drop the redo tail).
    if (m_history && m_history->canRedo()) return;
    if (anyInteractivePreviewActive() || m_inSketchMode || m_edgeOpActive ||
        m_moveFaceActive) return;
    const int bodies = m_document ? m_document->bodyCount() : 0;
    const int curStep = m_history ? m_history->currentStep() : -1;
    if (bodies == 0 && curStep < 0) return;    // empty new document: nothing to lose

    const double now = SDL_GetTicks() / 1000.0;
    const bool newStep = (curStep != m_lastRecoveryStep);
    const double kThrottleSec = 5.0;
    if (!newStep && now - m_lastRecoveryWrite < kThrottleSec) return;

    ProjectHistory hist = captureProjectHistory();
    if (materializr::writeProjectRecovery(*m_document, &hist, m_currentProjectPath,
                                          bodies, curStep + 1)) {
        m_lastRecoveryWrite = now;
        m_lastRecoveryStep = curStep;
    }
}

void Application::renderProjectRecoveryPrompt() {
    if (!m_pendingProjectRecovery) return;
    ImGui::OpenPopup("Recover Project?");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Recover Project?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        materializr::ProjectRecoveryMeta meta;
        materializr::readProjectRecoveryMeta(meta);
        ImGui::TextUnformatted(
            "Unsaved work from your last session was recovered.");
        if (!meta.projectPath.empty())
            ImGui::TextDisabled("Project: %s", meta.projectPath.c_str());
        else
            ImGui::TextDisabled("An unsaved project (never written to a file).");
        ImGui::TextDisabled("%d bodies, %d history steps.",
                            meta.bodyCount, meta.stepCount);
        ImGui::TextDisabled(
            "Materializr didn't close cleanly (a crash, hang, or restart).");
        ImGui::Spacing();
        if (ImGui::Button("Restore it", ImVec2(140, 0))) {
            restoreProjectRecoveryNow();
            m_pendingProjectRecovery = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(140, 0))) {
            materializr::clearProjectRecovery();
            m_pendingProjectRecovery = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Application::restoreProjectRecoveryNow() {
    materializr::ProjectRecoveryMeta meta;
    materializr::readProjectRecoveryMeta(meta);
    const std::string recPath = materializr::projectRecoveryPath();
    // Load the snapshot through the normal project loader (rebuilds bodies +
    // editable history). loadProjectAt sets m_currentProjectPath to the sidecar
    // and marks it saved — override both with the project's ORIGINAL identity so
    // the user can't overwrite the sidecar and unsaved work stays unsaved/dirty.
    if (!loadProjectAt(recPath)) {
        std::fprintf(stderr, "[Recovery] failed to load project snapshot\n");
        materializr::clearProjectRecovery();
        return;
    }
    m_currentProjectPath = meta.projectPath; // "" if it was never saved
    markDirty();                             // unsaved since the snapshot
    saveAppSettings();                       // fix lastProjectPath off the sidecar
    m_lastRecoveryStep = -2;                 // force a fresh snapshot going forward
    std::fprintf(stdout, "[Recovery] restored project (%d bodies, %d steps)\n",
                 meta.bodyCount, meta.stepCount);
}

void Application::run() {
    // A draft surviving from a previous session means it ended mid-sketch
    // (crash / kill / quit while drawing) — offer to restore on first frame.
    m_pendingSketchRecovery = materializr::hasSketchDraft();
    // A whole-project recovery snapshot surviving means the last session ended
    // unexpectedly with unsaved work — offer to restore that too.
    m_pendingProjectRecovery = materializr::hasProjectRecovery();

    // Opt-in perf instrumentation (MZR_PERF=1): once a second, report how many
    // frames we actually RENDERED vs how many loop iterations ran, plus which
    // "active work" state forced rendering. Lets us see e.g. "in-sketch idle =
    // 60 rendered/s" (a wasteful continuous-render state) vs "true idle = ~0".
    const bool kPerf = std::getenv("MZR_PERF") != nullptr;
    uint32_t perfLastMs = SDL_GetTicks();
    int perfRendered = 0, perfIters = 0;
    // Startup render-grace: keep drawing for the first few seconds regardless of
    // reported window focus, so the UI always appears after the loading screen
    // even if the WM is slow to hand the new window focus. See foreground below.
    const uint32_t runStartMs = SDL_GetTicks();

    while (true) {
        // True while any interactive tool or animation is in flight and needs
        // continuous rendering even with no user input.
        auto hasActiveWork = [&]() -> bool {
            // Always-on: self-completing work that needs frames to FINISH —
            // a pending heavy task to run, a toast that must tick down and clear
            // (regressed once as "toast never clears"), a modal popup, or an
            // extension tool that may animate on its own.
            if (m_deferredHeavyTask || m_showUpdatePopup || !m_toastText.empty())
                return true;
            if (PluginRegistry::instance().activeTool()) return true;
            // Interactive manipulation states (sketch + every live preview/op)
            // are INPUT-driven: they only need continuous frames while the user
            // is acting on them. Render for a short grace window after the last
            // input (m_interactiveGraceUntil, refreshed on any event below), then
            // idle — the preview stays on screen, frozen, and wakes instantly on
            // the next drag/keypress. The grace also covers the ~0.3s sketch
            // hover-dwell charge. Previously each of these pinned a flat 60fps
            // the whole time it was open (e.g. a push/pull left mid-edit) —
            // wasteful on the iGPU, a battery/thermal sink on mobile.
            bool interactive =
                m_inSketchMode || m_pushPullActive || m_gizmoDragging ||
                m_edgeOpActive || m_resizeCylActive || m_moveFaceActive ||
                m_revolveActive;
            if (!interactive)
                for (auto* c : m_iops) if (c && c->active()) { interactive = true; break; }
            if (interactive && SDL_GetTicks() / 1000.0 < m_interactiveGraceUntil)
                return true;
            return false;
        };

        ++perfIters;
        if (kPerf) {
            uint32_t nowMs = SDL_GetTicks();
            if (nowMs - perfLastMs >= 1000) {
                std::string st;
                if (m_inSketchMode)            st += "sketch ";
                if (m_pushPullActive)          st += "pushpull ";
                if (m_gizmoDragging)           st += "gizmo ";
                if (m_edgeOpActive)            st += "edgeop ";
                if (m_moveFaceActive)          st += "moveface ";
                if (m_resizeCylActive)         st += "resizecyl ";
                if (m_revolveActive)           st += "revolve ";
                if (m_deferredHeavyTask)       st += "heavy ";
                if (!m_toastText.empty())      st += "toast ";
                if (m_showUpdatePopup)         st += "update ";
                bool iop = false;
                for (auto* c : m_iops) if (c && c->active()) iop = true;
                if (iop)                       st += "iop ";
                if (PluginRegistry::instance().activeTool()) st += "tool ";
                if (st.empty())                st = "(idle)";
                std::fprintf(stderr,
                    "[perf] rendered=%d/s iters=%d/s wake=%d state=%s\n",
                    perfRendered, perfIters, m_wakeFrames, st.c_str());
                perfRendered = 0; perfIters = 0; perfLastMs = nowMs;
            }
        }

        // Suspend rendering entirely while backgrounded (not the focused window,
        // or minimized): a backgrounded GL app still composited at 60fps is what
        // makes the whole desktop's cursor lag on a shared GPU, and it's pure
        // waste on mobile. Autosave + deferred tasks below still run; FOCUS_GAINED
        // is a significant event so we repaint instantly on return.
#if defined(__ANDROID__)
        // Android exemption: the OS already pauses the activity (and SDL the GL
        // surface) when backgrounded, so the gate buys nothing here — and
        // SDL_WINDOW_INPUT_FOCUS isn't set until the first touch, so gating on it
        // froze the startup splash→UI handoff until the user tapped the screen.
        const bool foreground = true;
#else
        // Desktop: respect window focus to suspend when backgrounded — BUT force
        // rendering for a short grace after launch. The WM may not report
        // INPUT_FOCUS for a beat (especially when launched from a terminal that
        // keeps focus), and the idle-skip below would otherwise leave the first
        // real UI frame undrawn behind the loading screen until the user clicks.
        const bool foreground = m_window->isForeground() ||
                                (SDL_GetTicks() - runStartMs < 3000u);
#endif

        // When idle (or backgrounded), block up to 500 ms for the next event.
        // 500 ms is enough for autosave / update-check polling; anything
        // interactive wakes us immediately via SDL events. Force the wait when
        // backgrounded so an active preview can't busy-spin pollEvents(0).
        int waitMs = (!foreground || (m_wakeFrames == 0 && !hasActiveWork())) ? 500 : 0;
        int eventLevel = m_window->pollEvents(waitMs);
        // Significant events (click, key, scroll, resize, focus): 5 frames.
        // Trivial events (mouse motion, expose): 25 frames — at 60 fps that is
        // ~416 ms, enough for ImGui's default 300 ms hover-tooltip delay to fire
        // AFTER the cursor stops moving. Without this extra tail, the tooltip
        // timer freezes the moment we stop rendering (ImGui time only advances
        // inside NewFrame). The idle timeout (eventLevel == 0) is not a trigger:
        // we skip rendering until a real event or active work wakes us.
        if (eventLevel >= 2)
            m_wakeFrames = 5;
        else if (eventLevel == 1)
            m_wakeFrames = std::max(m_wakeFrames, 25);

        // Any input refreshes the interactive-state render grace (see
        // hasActiveWork): keep rendering for kGraceSec after the last event,
        // then idle. 1s comfortably covers the ~0.3s sketch hover-dwell charge;
        // rendering resumes instantly on the next event, so it feels snappy while
        // keeping idle previews (sketch, push/pull, …) at ~0fps.
        if (eventLevel > 0) {
            constexpr double kGraceSec = 1.0;
            m_interactiveGraceUntil = SDL_GetTicks() / 1000.0 + kGraceSec;
        }

        // Last frame's GL (driver/ImGui render) can leave the SSE FPU in
        // flush-to-zero / denormals-are-zero mode, which makes OCCT geometry —
        // including SVG import tessellation and wire-building done mid-frame —
        // come out subtly different run to run (a different region degenerates
        // into an uncuttable sliver each re-import). Put the FPU back to OCCT's
        // expected mode at the top of EVERY frame so all geometry is stable.
        resetFpuForOcct();

        // Run a heavy op deferred from last frame's commit HERE, between frames,
        // so its progress reporter (renderProgressFrame) can pump its own frames
        // without nesting ImGui frames.
        if (m_deferredHeavyTask) {
            auto task = std::move(m_deferredHeavyTask);
            m_deferredHeavyTask = nullptr;
            task();
            m_wakeFrames = 5; // task finished — repaint the result
        }

        // Apply the launch-time update check once its worker finishes — never
        // block waiting for it.
        if (m_updateCheckFuture.valid() &&
            m_updateCheckFuture.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            auto r = m_updateCheckFuture.get();
            if (r.ok && r.updateAvailable) {
                m_updateCurrent    = r.current;
                m_updateLatest     = r.latest;
                m_updateAvailable  = true;
                m_updateReleaseUrl = r.releasePageUrl;
                m_updateMessage    = "";
                m_updateChecked    = true;
                m_showUpdatePopup  = true;
                m_wakeFrames = 5;
            }
        }

        // Keep the in-progress sketch crash-recoverable.
        writeSketchDraftIfDue();
        // Keep the whole committed project crash/hang-recoverable (incl. unsaved).
        writeProjectRecoveryIfDue();

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

        // Autosave — MUST run before the idle short-circuit below. A change
        // wakes only a brief render burst, then the loop idles and `continue`s
        // past everything down-stream; if autosave lived after the skip it
        // would essentially never fire for a model you edit and then leave
        // alone. The timer uses SDL_GetTicks (wall clock) rather than
        // ImGui::GetTime(), which is frozen while we're not rendering and so
        // would never let the interval elapse during idle.
        // Only for projects already on disk, only when there are pending
        // changes; the interval is measured from the last save.
        if (m_autosaveEnabled && !m_currentProjectPath.empty()) {
            double now = SDL_GetTicks() / 1000.0;
            if (isDirty()) {
                // Never autosave while the user is below the history tip
                // (mid undo-exploration): the file only persists APPLIED
                // steps, so saving now would silently truncate the redo
                // tail from the project. Resume once they redo back to the
                // tip or push a new op (which discards the tail anyway).
                if (m_history && m_history->canRedo()) {
                    // hold off — keep checking each interval
                } else if (anyInteractivePreviewActive() || m_inSketchMode ||
                           m_edgeOpActive || m_moveFaceActive) {
                    // hold off — an autosave must never cancel (or serialize) a
                    // live tool preview / an in-progress sketch out from under
                    // the user (a half-baked uncommitted-sketch state has
                    // crashed before). Resume once the tool / sketch closes.
                } else if (now - m_lastAutosaveTime >= m_autosaveIntervalSec) {
                    // Defensive: a serialization failure (OCCT throw, bad
                    // state) must never take the whole app down on a background
                    // autosave — log and skip, try again next interval.
                    try { saveProjectQuick(); }
                    catch (...) {
                        std::fprintf(stderr, "[Autosave] failed - skipped\n");
                    }
                    m_lastAutosaveTime = now;
                }
            } else {
                m_lastAutosaveTime = now;
            }
        } else {
            m_lastAutosaveTime = SDL_GetTicks() / 1000.0;
        }

        // Skip rendering entirely when nothing has changed — saves ~30 % idle
        // GPU on a static viewport. hasActiveWork() is re-evaluated after the
        // deferred task and close checks above may have changed state.
        if (!foreground || (m_wakeFrames == 0 && !hasActiveWork())) continue;
        if (m_wakeFrames > 0) m_wakeFrames--;
        ++perfRendered;   // passed the idle skip → this iteration renders a frame

        beginFrame();
        renderDockspace();
        renderMenuBar();
        renderSmallScreenWarning();

        if (m_renderersReady) {
            renderViewport();

            m_toolbar->setGridStep(m_sketchGridStep);
            m_toolbar->setSnapToGrid(m_snapToGrid);
            m_toolbar->setCameraOrtho(m_viewport->getCamera().isOrthographic());
            // "Edit Diameter" button only appears when the picked face is a
            // cylinder on a solid-cylinder or tube body. Detection populates
            // m_resizeCyl* fields as a side effect — we throw the result away
            // here, those are only used by the actual begin path.
            m_toolbar->setCanEditDiameter(!m_resizeCylActive &&
                                          detectCylindricalResizeCandidate());
            // "Frozen round" hint: a selected fillet-shaped face (cylinder /
            // torus) that NO enabled op owns reloaded as baked geometry — there's
            // no editable FilletOp behind it. The toolbar surfaces a one-liner
            // pointing at Repair Geometry. A FULL 2π cylinder is a hole / pin
            // (Edit Diameter handles it), not a round, so it's excluded.
            {
                bool frozenRound = false;
                TopoDS_Shape pf;
                for (const auto& e : m_selection->getSelection())
                    if (e.type == SelectionType::Face && !e.shape.IsNull()) {
                        pf = e.shape; break;
                    }
                if (!pf.IsNull() && pf.ShapeType() == TopAbs_FACE) {
                    try {
                        TopoDS_Face f = TopoDS::Face(pf);
                        Handle(Geom_Surface) s = BRep_Tool::Surface(f);
                        bool round = false;
                        if (!s.IsNull()) {
                            if (s->IsKind(STANDARD_TYPE(Geom_ToroidalSurface))) {
                                round = true; // curved-edge fillet
                            } else if (s->IsKind(STANDARD_TYPE(Geom_CylindricalSurface))) {
                                double u1, u2, v1, v2;
                                BRepTools::UVBounds(f, u1, u2, v1, v2);
                                round = (u2 - u1) < 2.0 * M_PI - 0.05; // partial = fillet
                            }
                        }
                        if (round) {
                            frozenRound = true; // assume frozen until an op claims it
                            for (const auto& op : m_history->operations())
                                if (op && op->isEnabled() && op->ownsFace(pf) &&
                                    (op->typeId() == "fillet" ||
                                     op->typeId() == "chamfer")) {
                                    frozenRound = false;
                                    break;
                                }
                        }
                    } catch (...) {}
                }
                m_toolbar->setSelectedFaceFrozenRound(frozenRound);
            }
            m_toolbar->setShowTooltips(m_showToolbarTooltips);
            // Mirror the live inference level (Full/Reduced/Off) so the sketch
            // toolbar button shows the current state. Int to keep Toolbar free
            // of a SketchTool.h dependency (matches setActiveSketchMode).
            m_toolbar->setInferenceLevel(m_inSketchMode && m_sketchTool
                ? static_cast<int>(m_sketchTool->getInferenceLevel()) : 0);
            // Mirror the live rect/circle draw-origin so the per-tool toggle
            // button shows the current mode.
            if (m_inSketchMode && m_sketchTool) {
                m_toolbar->setRectMode(static_cast<int>(m_sketchTool->getRectMode()));
                m_toolbar->setCircleMode(static_cast<int>(m_sketchTool->getCircleMode()));
            }
            // Per-frame hide/show of the toolbar's inference cycle button.
            // Users who set the level once in Settings can declutter the
            // sketch toolbar; default is on (the live toggle is visible).
            m_toolbar->setShowInferenceToggle(m_showInferenceToolbarToggle);
            // Pass the active sketch tool mode so the matching button gets
            // a highlight border — disambiguates which tool is currently in
            // use (Line vs Circle vs etc.) when in sketch mode.
            m_toolbar->setActiveSketchMode(m_inSketchMode && m_sketchTool
                ? static_cast<int>(m_sketchTool->getMode()) : 0);
            // Drive the Constraints section: it only appears when sketch
            // elements are actually selected, and only shows buttons that
            // match the selection arity.
            if (m_inSketchMode && m_sketchTool) {
                m_toolbar->setSketchSelectionCounts(
                    static_cast<int>(m_sketchTool->getSelectedPoints().size()),
                    static_cast<int>(m_sketchTool->getSelectedLines().size()),
                    static_cast<int>(m_sketchTool->getSelectedCircles().size()),
                    static_cast<int>(m_sketchTool->getSelectedArcs().size()));
            } else {
                m_toolbar->setSketchSelectionCounts(0, 0, 0, 0);
            }
            // Solver-state badge. Only meaningful when in sketch mode AND
            // there are constraints to evaluate; otherwise hide the badge.
            if (m_inSketchMode && m_activeSketch && m_sketchSolver &&
                !m_activeSketch->getConstraints().empty()) {
                m_toolbar->setSketchSolverState(static_cast<int>(m_sketchSolver->getState()));
                m_toolbar->setSketchSolverDof(m_sketchSolver->degreesOfFreedom());
            } else {
                m_toolbar->setSketchSolverState(-1);
                m_toolbar->setSketchSolverDof(0);
            }
            // The Tools palette is the LEFT docked column; it collapses with the
            // left edge handle (or Hide Panels). All the setters above are
            // harmless no-ops on an unsubmitted window.
            ToolAction action = ToolAction::None;
            if (!m_leftPanelHidden && m_showTools) {
                action = m_toolbar->render();
                m_sketchGridStep = m_toolbar->getGridStep();
                m_snapToGrid = m_toolbar->getSnapToGrid();
                if (m_sketchTool) {
                    m_sketchTool->setGridStep(m_sketchGridStep);
                    m_sketchTool->setSnapToGridEnabled(m_snapToGrid);
                }
            }
            if (action != ToolAction::None) {
                handleToolAction(static_cast<int>(action));
            }

            // A plugin toolbar action may have requested an interactive op that
            // needs Application's popup machinery (e.g. PatternPlugin asking for
            // the Linear / Radial pattern popup). Dispatch any pending request.
            if (m_pluginContext) {
                std::string pending = m_pluginContext->takeRequestedInteractiveOp();
                if (!pending.empty()) {
                    if      (pending == "LinearPattern") beginPattern(PatternKind::Linear);
                    else if (pending == "RadialPattern") beginPattern(PatternKind::Radial);
                    else if (pending == "Loft")          beginLoft();
                    else if (pending == "LoftPickSecond") m_loftPickHintPending = true;
                    else if (pending == "ConstructionPlane") beginConstructionPlane();
                    else if (pending == "ConstructionAxis")  beginConstructionAxis();
                    else if (pending == "Revolve")           beginRevolve();
                    else if (pending == "Midplane")          beginConstructionPlaneMode(4);
                    else if (pending == "PlaneNormalToAxis") beginConstructionPlaneMode(5);
                    else if (pending == "TangentPlane")      beginConstructionPlaneMode(6);
                    else if (pending == "PlaneThroughAxis")  beginConstructionPlaneMode(7);
                    else if (pending == "AxisFromCylinder")  beginConstructionAxisMode(3);
                    else if (pending == "AxisAlongEdge")     beginConstructionAxisMode(4);
                    else if (pending == "AxisTwoPoints")     beginConstructionAxisMode(5);
                    else if (pending == "AxisNormalToFace")  beginConstructionAxisMode(6);
                    else if (pending == "AxisTwoPlanes")     beginConstructionAxisMode(7);
                    else if (pending == "PrimitiveBox")      beginPrimitivePopup(0);
                    else if (pending == "PrimitiveCylinder") beginPrimitivePopup(1);
                    else if (pending == "PrimitiveSphere")   beginPrimitivePopup(2);
                    else if (pending == "PrimitiveCone")     beginPrimitivePopup(3);
                    else if (pending == "PrimitiveTorus")    beginPrimitivePopup(4);
                    // Unknown ids are silently ignored — future plugins can
                    // ship their own without modifying Application by routing
                    // through whatever new dispatcher is added here.
                }
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

            // The Interactions reference is docked in the RIGHT column (above
            // Items), so it collapses with the right edge handle too.
            if (!m_rightPanelHidden && m_showInteractions) renderInteractionsPanel();
            renderSettings();
            renderMirrorPopup();

            // Loft (plugin) "pick a second sketch" hint banner. LoftPlugin
            // triggers this when the user clicks Loft with one sketch in the
            // selection. A modal would grey out the viewport and prevent
            // picking the second sketch, so we render it as a non-blocking
            // floating window pinned near the top of the viewport instead.
            // Auto-dismisses once the selection covers two sketches (or
            // two sketch regions from distinct sketches) — at which point a
            // second click on Loft will commit. Manual dismiss via the X.
            if (m_loftPickHintPending) {
                m_loftPickHintVisible = true;
                m_loftPickHintPending = false;
            }
            if (m_loftPickHintVisible) {
                // Count distinct parent sketches in the current selection.
                int distinct = 0;
                std::vector<int> seen;
                if (m_selection) {
                    for (const auto& e : m_selection->getSelection()) {
                        if ((e.type == SelectionType::Sketch ||
                             e.type == SelectionType::SketchRegion) &&
                            e.sketchId >= 0) {
                            bool dup = false;
                            for (int x : seen) if (x == e.sketchId) { dup = true; break; }
                            if (!dup) { seen.push_back(e.sketchId); ++distinct; }
                        }
                    }
                }
                if (distinct >= 2) {
                    m_loftPickHintVisible = false;
                } else {
                    ImGuiViewport* vp = ImGui::GetMainViewport();
                    ImGui::SetNextWindowPos(
                        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                               vp->WorkPos.y + 60.0f),
                        ImGuiCond_Always, ImVec2(0.5f, 0.0f));
                    ImGui::SetNextWindowBgAlpha(0.92f);
                    ImGuiWindowFlags flags =
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_NoFocusOnAppearing |
                        ImGuiWindowFlags_NoNav;
                    bool open = true;
                    if (ImGui::Begin("Pick a second sketch", &open, flags)) {
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f),
                                           "Loft needs a second profile.");
                        ImGui::TextWrapped("Ctrl-click another sketch (or one "
                                           "of its regions), then click Loft "
                                           "again to commit.");
                    }
                    ImGui::End();
                    if (!open) m_loftPickHintVisible = false;
                }
            }

            // Help system: dockable user guide, modal About, modal "Check for
            // Updates" popup that fires a one-shot HTTPS GET to GitHub on open.
            m_helpPanel->render();
            m_shortcutsPanel->render();
            m_aboutDialog->render();
            renderUpdatePopup();
            renderMultiTransformPanel();
            renderResizeCylindricalPanel();
            {
                auto ctx = iopContext();
                for (auto* c : m_iops) c->renderPanel(ctx);
            }
            renderPatternPanel();
            renderThreadPanel();
            renderSectionPanel();
            renderTextToolPanel();
            renderSvgToolPanel();
            renderLoftPanel();
            renderConstructionPlanePanel();
            renderConstructionAxisPanel();
            renderPrimitivePopup();
            renderRevolvePopup();
            renderRotatePlaneAboutAxisPopup();
            renderSketchMovePanel();
            renderSketchPatternPopup();

            // Keep measurement results in sync with the current selection,
            // then draw the panel. Cheap when inactive.
            if (m_measureTool) {
                m_measureTool->update();
                m_measureTool->renderPanel();
            }

            // History / Items / Properties are the RIGHT docked column; they
            // collapse with the right edge handle (or Hide Panels).
            if (!m_rightPanelHidden) {
                m_historyPanel->setHistoryLocked(anyInteractivePreviewActive());
                if (m_showHistory && m_historyPanel->render()) {
                    m_meshesDirty = true;
                }

                if (m_showItems && m_itemsPanel->render()) {
                    m_hoveredBodyId = -1;
                    m_meshesDirty = true;
                }
                m_propertiesPanel->setSketchContext(
                    m_inSketchMode, m_activeSketch.get(), m_activeSketchId,
                    m_sketchTool.get());
                if (m_showProperties && m_propertiesPanel->render()) {
                    m_meshesDirty = true;
                }
            }
            // Touch edge tabs to collapse/restore each side column (drawn on top
            // of the panels, and still visible when a side is collapsed).
            renderPanelCollapseHandles();

            // Plugin overlays — free-floating per-frame ImGui windows (e.g. the
            // Tutorial). Drawn after the panels so they float on top; non-modal,
            // so they never block the panels or the viewport.
            for (auto& ov : PluginRegistry::instance().overlayContributions()) {
                if (ov.render) ov.render(*m_pluginContext);
            }
            m_statusBar->setSketchMode(m_inSketchMode);
            // Project name = the save file's basename (no extension), or
            // "New project" when unsaved.
            {
                std::string pn;
                if (!m_currentProjectPath.empty()) {
                    pn = m_currentProjectPath;
                    auto slash = pn.find_last_of("/\\");
                    if (slash != std::string::npos) pn = pn.substr(slash + 1);
                    auto dot = pn.rfind(".materializr");
                    if (dot != std::string::npos) pn = pn.substr(0, dot);
                }
                m_statusBar->setProjectName(pn);
            }
            // Spline placement is the one tool that needs a keyboard step
            // to finish — without this hint it reads as "adds dots and
            // then nothing" (Steve, verbatim).
            if (m_inSketchMode && m_sketchTool &&
                m_sketchTool->getMode() == SketchToolMode::Spline) {
                size_t nPts = m_sketchTool->splinePointsInProgress().size();
                m_statusBar->setMessage(
                    nPts == 0
                        ? "Spline: click to place control points"
                        : "Spline: click FIRST point to close the loop, "
                          "LAST point (or ENTER) to finish open");
            } else {
                m_statusBar->setMessage("");
            }
            m_statusBar->render();
            renderTransientToast();
            FileDialogs::render();
            renderSavePrompt();
            // Project recovery takes precedence — one modal at a time, and a
            // restored project supersedes any leftover sketch draft anyway.
            renderProjectRecoveryPrompt();
            if (!m_pendingProjectRecovery) renderSketchRecoveryPrompt();

            handleShortcuts();
        }

        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        endFrame();

        m_window->swapBuffers();
    }

    // Persist preferences on a clean exit (in addition to saving on each change).
    saveAppSettings();
    // Clean exit → the crash-recovery snapshot is no longer "unfinished work".
    // A snapshot surviving to the next launch therefore means a crash/hang/kill.
    materializr::clearProjectRecovery();
}

} // namespace materializr

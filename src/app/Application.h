#pragma once

#include <memory>
#include <vector>
#include <functional>
#include <string>
#include <glm/glm.hpp>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

#include "modeling/ExtrudeOp.h" // for ExtrudeMode

namespace materializr {

class Window;
class Viewport;
class Grid;
class ShapeRenderer;
class SketchRenderer;
class EdgeRenderer;
class PlaneRenderer;
class BackgroundRenderer;
class ViewCube;
class Picker;
class Gizmo;
class SelectionHighlight;
class BoxSelect;
class Toolbar;
class HistoryPanel;
class AboutDialog;
class ShortcutsPanel;
class HelpPanel;
class MeasureTool;
class ItemsPanel;
class CommandPalette;
class StatusBar;
class ThemeManager;
class PropertiesPanel;
class Sketch;
class SketchSolver;
class SketchTool;
class EventBus;
class PluginContext;

} // namespace materializr

class Document;
class History;
class SelectionManager;

namespace materializr {

struct ProjectHistory; // io/ProjectIO.h

class Application {
public:
    Application();
    ~Application();

    void run();

private:
    void initImGui();
    void shutdownImGui();
    void initRenderers();
    void setupCommands();
    void beginFrame();
    void endFrame();
    void renderDockspace();
    void renderViewport();
    void renderMenuBar();
    void renderInteractionsPanel();
    void renderSettings();
    void loadAppSettings();   // restore persisted preferences at startup
    void saveAppSettings();   // write persisted preferences
    void renderMirrorPopup();
    void renderUpdatePopup();
    void renderMultiTransformPanel();
    void applyMultiBodyRotation();
    void renderScalePanel();
    void handleToolAction(int action);
    void handleShortcuts();
    void handleViewCubeAction(int action);
    void rebuildMeshes();
    glm::vec2 screenToSketch(float sx, float sy, float vpW, float vpH);

    void importStepFile();
    void exportStepFile();
    void saveProject();         // Save dialog (Save As behavior)
    void saveProjectQuick();    // Save to current path if known, else falls through to saveProject
    void loadProject();
    // Snapshot the operation history (parameters + per-step body diffs) for the
    // project file, and rebuild a replayable history from a loaded project.
    ProjectHistory captureProjectHistory();
    void rebuildHistoryFromProject(const ProjectHistory& hist);

    // Dirty tracking + unsaved-changes prompt
    bool isDirty() const;
    void markDirty();           // for changes that don't go through History
    void markSaved();
    void renderSavePrompt();
    void requestClose();        // called when the user clicks the window X

    void enterSketchMode();
    void enterSketchOnPlane(const gp_Pln& plane);
    void enterSketchOnFace(const TopoDS_Face& face, int sourceBodyId = -1);
    void editSketch(int sketchId);
    void extrudeSketchById(int sketchId, ExtrudeMode mode = ExtrudeMode::NewBody);
    // Interactive subtract of a single sketch region from the body the sketch
    // was drawn on (red preview). Used by the region toolbar where viewport
    // clicks land, since clicking a sketch selects a region, not the whole sketch.
    void subtractSketchRegion(int sketchId, int regionIndex);
    TopoDS_Face buildSketchProfileFace(const Sketch& sketch) const;
    void exitSketchMode();

    // Snapshot the active sketch, run `mutator`, and if the element count
    // changed, push a SketchEditOp so the user can Ctrl+Z drawing actions.
    void recordSketchMutation(const std::function<void()>& mutator);

    // Align the orbit camera to look straight at the active sketch's plane in ortho.
    // Called when entering sketch mode / editing an existing sketch.
    void alignCameraToActiveSketch();

    // Sketch region hover/pick + Push/Pull
    struct SketchRegionHit { int sketchId = -1; int regionIndex = -1; glm::vec3 worldPoint{0.0f}; };
    SketchRegionHit pickSketchRegion(float screenX, float screenY,
                                     float vpW, float vpH) const;
    void beginPushPull();
    void updatePushPull();
    void commitPushPull();
    void cancelPushPull();
    void beginInteractiveExtrude(const TopoDS_Shape& profile,
                                 ExtrudeMode mode = ExtrudeMode::NewBody,
                                 int targetBody = -1);
    void updateInteractiveExtrude();
    // Signed distance to pass to ExtrudeOp: Subtract cuts into the body (the
    // profile normal points outward), so it uses the negated distance.
    double extrudeOpDistance() const;
    void commitInteractiveExtrude();
    void cancelInteractiveExtrude();

    std::unique_ptr<Window> m_window;
    std::unique_ptr<Viewport> m_viewport;
    std::unique_ptr<Grid> m_grid;
    std::unique_ptr<ShapeRenderer> m_shapeRenderer;
    std::unique_ptr<SketchRenderer> m_sketchRenderer;
    std::unique_ptr<EdgeRenderer> m_edgeRenderer;
    std::unique_ptr<PlaneRenderer> m_planeRenderer;
    std::unique_ptr<BackgroundRenderer> m_backgroundRenderer;
    std::unique_ptr<ViewCube> m_viewCube;
    std::unique_ptr<Picker> m_picker;
    std::unique_ptr<Gizmo> m_gizmo;
    std::unique_ptr<SelectionHighlight> m_selectionHighlight;
    std::unique_ptr<BoxSelect> m_boxSelect;
    std::unique_ptr<Document> m_document;
    std::unique_ptr<History> m_history;
    std::unique_ptr<SelectionManager> m_selection;
    std::unique_ptr<EventBus> m_eventBus;
    std::unique_ptr<PluginContext> m_pluginContext;

    // UI panels
    std::unique_ptr<Toolbar> m_toolbar;
    std::unique_ptr<HistoryPanel> m_historyPanel;
    std::unique_ptr<ItemsPanel> m_itemsPanel;
    std::unique_ptr<CommandPalette> m_commandPalette;
    std::unique_ptr<StatusBar> m_statusBar;
    std::unique_ptr<ThemeManager> m_themeManager;
    std::unique_ptr<PropertiesPanel> m_propertiesPanel;
    std::unique_ptr<AboutDialog> m_aboutDialog;
    std::unique_ptr<ShortcutsPanel> m_shortcutsPanel;
    std::unique_ptr<HelpPanel> m_helpPanel;
    std::unique_ptr<MeasureTool> m_measureTool;

    // Update-check popup state (Help → Check for Updates).
    bool m_showUpdatePopup = false;
    bool m_updateChecked = false;
    std::string m_updateCurrent;
    std::string m_updateLatest;
    std::string m_updateMessage;
    std::string m_updateReleaseUrl;
    bool m_updateAvailable = false;

public:
    // Camera state captured at sketch entry so exitSketchMode can put the user
    // back where they were instead of leaving them inside/behind the face.
    // Public so a file-scope helper in Application.cpp can populate it.
    struct SavedCamera {
        bool valid = false;
        glm::vec3 position{0.0f};
        glm::vec3 target{0.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
        bool ortho = false;
        float orthoSize = 10.0f;
    };

private:
    SavedCamera m_savedCameraForSketch;

    // Sketch
    std::shared_ptr<Sketch> m_activeSketch;
    // Snapshot taken at left-mouse-down in Select mode so a point/line drag
    // (which only moves positions, no structural change) can be committed to
    // history on mouse-up.
    std::shared_ptr<Sketch> m_sketchDragBefore;

    // Sketch Move/Rotate gizmo (drawn on the selection centroid in Select mode):
    // axis arrows + free-move dot + rotate ring. Held-drag — clicking a handle
    // arms the corresponding op, releasing commits. Rotate also pops a small
    // type-in panel on release so the angle can be set exactly.
    enum class SketchGizmoHandle { None, MoveX, MoveY, MoveFree, Rotate };
    SketchGizmoHandle m_sketchGizmoHandle = SketchGizmoHandle::None;
    glm::vec2 m_sketchGizmoCenter{0.0f};   // centroid at drag start (rotate pivot)
    glm::vec2 m_sketchGizmoAnchor{0.0f};   // cursor sketch pos at drag start
    std::shared_ptr<Sketch> m_sketchGizmoBefore;
    std::vector<std::pair<int, glm::vec2>> m_sketchGizmoOriginals;
    // Rotate handle: snap drag to 15°; on release enter "adjusting" mode where
    // the popup is shown with the current angle pre-filled. Apply (or Enter)
    // commits the typed angle; Cancel / Esc reverts.
    bool m_sketchGizmoRotateAdjusting = false;
    float m_sketchGizmoRotateDegrees = 0.0f;
    char m_sketchGizmoRotateBuf[32] = "0.0";
    // Screen-space anchor for the rotate adjust popup, updated each frame the
    // gizmo is on screen so the popup tracks the centroid if the camera moves.
    glm::vec2 m_sketchGizmoAdjustAnchor{0.0f};

    // Sketch box-select: when in Select mode and the user clicks on empty space,
    // begin a rectangle drag; on release, sketch elements whose screen-space
    // projection lies inside the rectangle are added to the selection. Reuses
    // the shared m_boxSelect overlay so the rectangle visuals are identical to
    // the 3D mode's.
    bool m_sketchBoxSelectActive = false;

    std::unique_ptr<SketchSolver> m_sketchSolver;
    std::unique_ptr<SketchTool> m_sketchTool;
    bool m_inSketchMode = false;
    int m_activeSketchId = -1; // document id of the sketch being edited, or -1 if new

    // Hovered sketch region (for highlight in viewport)
    int m_hoveredSketchId = -1;
    int m_hoveredRegionIndex = -1;

    // Numeric dimension input shown while placing a sketch shape
    char m_sketchDimBuf[32] = "";
    bool m_sketchDimWasShown = false; // tracks placing transitions to grab keyboard focus

    // Sketch grid step in mm (drives both the visual face grid and snap-to-line)
    float m_sketchGridStep = 1.0f;

    // Push/Pull interactive operation state
    bool m_pushPullActive = false;
    bool m_pushPullPreviewPushed = false; // true while a preview PushPullOp is on the history stack
    float m_pushPullDistance = 5.0f;
    char m_pushPullInputBuf[32] = "5.0";
    bool m_pushPullInputFocus = true;
    // Face arrow: drag along this normal to drive the distance (set from the first
    // face target). m_pushPullHasArrow is false for sketch-region-only push/pull.
    glm::vec3 m_pushPullOrigin{0.0f};
    glm::vec3 m_pushPullNormal{0.0f, 0.0f, 1.0f};
    bool m_pushPullHasArrow = false;

    // Snap-to-grid for gizmo translate (shares the grid step with the sketch grid).
    bool m_snapToGrid = true;

    // Configurable camera mouse bindings (ImGuiMouseButton values: 0=Left,1=Right,
    // 2=Middle). Zoom is always the scroll wheel. Edited in File > Settings.
    int m_orbitButton = 2; // Middle
    int m_panButton = 1;   // Right
    bool m_showSettings = false;
    int m_settingsOrbitButton = 2; // staged value in the Settings dialog
    int m_settingsPanButton = 1;

    // Autosave: once the project has been saved at least once (has a path on
    // disk), periodically re-save dirty changes. Toggled in File > Settings.
    bool m_autosaveEnabled = false;
    float m_autosaveIntervalSec = 120.0f;
    double m_lastAutosaveTime = 0.0;

    // Invert the cube-drag → orbit direction (Settings).
    bool m_invertCubeDrag = false;

    // Rendering preferences (File > Settings → Rendering). Persisted.
    float m_lightAmbient = 0.40f;   // base illumination; higher = softer shadows
    bool  m_lightHeadlight = false; // key light tracks the camera
    bool  m_lightFill = true;       // soft opposing fill light
    int   m_msaaSamples = 4;        // viewport anti-aliasing: 0=off, 2, 4, 8
    int   m_meshQuality = 1;        // tessellation density: 0=Low..3=Ultra
    // Apply m_light*/m_msaaSamples to the renderer + viewport.
    void applyRenderingSettings();
    // Map m_meshQuality to OCCT tessellation parameters.
    void meshQualityParams(float& deflection, float& angularDeflection) const;
    // Each entry: a separate region operation to perform on commit
    struct PushPullTarget {
        int sketchId;
        int regionIndex;
        int sourceBodyId; // -1 for floating (NewBody)
        TopoDS_Face profile;
    };
    std::vector<PushPullTarget> m_pushPullTargets;
    std::vector<int> m_pushPullPreviewBodyIds;
    // For undoing previews
    std::vector<std::pair<int, TopoDS_Shape>> m_pushPullPreviousBodies;

    bool m_renderersReady = false;
    bool m_meshesDirty = true;
    int m_hoveredBodyId = -1;

    // Gizmo drag state for history commit
    bool m_gizmoDragging = false;
    int m_gizmoDragBodyId = -1;            // primary (for Rotate/Scale + readouts)
    TopoDS_Shape m_gizmoDragOriginalShape; // primary's original
    // For multi-body Move/Rotate/Scale: all selected bodies' originals captured
    // at drag start. Cached pivot avoids recomputing 65 bboxes every frame for
    // a single rotation around the selection centroid.
    std::vector<std::pair<int, TopoDS_Shape>> m_gizmoDragOriginals;
    glm::vec3 m_gizmoSharedPivot{0.0f};

    // Multi-body Rotate type-in panel. When the Rotate gizmo is active and 2+
    // bodies are selected, this panel offers per-axis sliders + numeric input
    // so the user can apply an exact rotation in a single commit, bypassing the
    // per-frame lag of the live gizmo path on large selections.
    float m_multiRotate[3] = {0.0f, 0.0f, 0.0f};
    // The Close button hides the panel; it auto-reopens the next time the
    // conditions (Rotate gizmo + multi-body) are freshly satisfied, so the
    // user can dismiss it without losing access to it.
    bool m_multiTransformPanelOpen = true;
    bool m_multiTransformConditionsMet = false;
    // Accumulated delta from drag start (translate only). Used so snap-to-grid
    // can snap the absolute position rather than each per-frame increment.
    glm::vec3 m_gizmoTotalDelta{0.0f};
    // Accumulated rotation (deg, about m_gizmoRotAxis) from drag start, for soft
    // 45° snapping; and accumulated per-axis scale (raw drag deltas → factors).
    float m_gizmoTotalAngle = 0.0f;
    glm::vec3 m_gizmoRotAxis{0.0f, 1.0f, 0.0f};
    glm::vec3 m_gizmoScaleAccum{0.0f}; // accumulated per-axis drag distance
    glm::vec3 m_gizmoTotalScale{1.0f, 1.0f, 1.0f}; // derived per-axis factors

    // Mirror: single button opens a popup; "across a face" arms face-pick mode.
    bool m_showMirrorPopup = false;
    bool m_mirrorPickFace = false;
    int m_mirrorBodyId = -1;

    // Scale side panel (shown in Scale gizmo mode): X/Y/Z percentages + uniform.
    float m_scalePct[3] = {100.0f, 100.0f, 100.0f};
    bool m_scaleUniform = true;

    // Interactive fillet/chamfer state
    enum class EdgeOpType { None, Fillet, Chamfer };
    EdgeOpType m_edgeOpType = EdgeOpType::None;
    bool m_edgeOpActive = false;
    int m_edgeOpBodyId = -1;
    std::vector<TopoDS_Shape> m_edgeOpEdges;
    float m_edgeOpValue = 1.0f;
    char m_edgeOpInputBuf[32] = "1.0";
    bool m_edgeOpInputFocus = true;
    TopoDS_Shape m_edgeOpPreviousShape;
    // First selected edge's midpoint + direction, for the drag handle and the
    // radius/distance measurement readout.
    glm::vec3 m_edgeOpMid{0.0f};
    glm::vec3 m_edgeOpDir{1.0f, 0.0f, 0.0f};   // along the edge
    glm::vec3 m_edgeOpOutDir{0.0f, 0.0f, 1.0f}; // perpendicular, pointing out of the body
    bool m_edgeOpHasHandle = false;
    // History index of the op being re-edited. -1 means "creating new" — the
    // commit path then pushes a fresh FilletOp / ChamferOp. >=0 means "editing
    // existing" — commit updates the op's parameter and calls editStep().
    int m_edgeOpEditingIndex = -1;

    void beginInteractiveEdgeOp(EdgeOpType type);
    // Re-edit the FilletOp or ChamferOp at the given history index. Pulls the
    // existing radius/distance + edges + body id from the op, snapshots its
    // pre-state for live preview, and reuses the same drag handle + popup UI
    // as creation. Triggered by clicking a face the op produced.
    void beginInteractiveEdgeOpEdit(int historyIndex);
    void updateInteractiveEdgeOp();
    void commitInteractiveEdgeOp();
    void cancelInteractiveEdgeOp();

    // Resize-cylindrical (edit a closed cylindrical/conical face's diameter,
    // or a single circular edge of one) ====================================
    // Triggered by picking a closed cylindrical face (edits BOTH end edges
    // together → stays a cylinder) or a single circular edge (edits ONE end
    // → turns cylinder into a cone, makes funnels). Internally the commit
    // path always builds a CONE primitive at the two end radii — for the
    // face-edit case they're equal.
    bool m_resizeCylActive = false;
    int  m_resizeCylBodyId = -1;
    bool m_resizeCylIsHole = true; // true: hole (normal toward axis), false: solid boundary
    // Axis anchored at the V_min end of the affected cylindrical region.
    // R1 (bottom) = radius at axis location, R2 (top) = radius at +H end.
    double m_resizeCylAxisOX = 0.0, m_resizeCylAxisOY = 0.0, m_resizeCylAxisOZ = 0.0;
    double m_resizeCylAxisDX = 0.0, m_resizeCylAxisDY = 0.0, m_resizeCylAxisDZ = 1.0;
    double m_resizeCylAxisXX = 1.0, m_resizeCylAxisXY = 0.0, m_resizeCylAxisXZ = 0.0;
    double m_resizeCylHeight       = 0.0;
    // Original (before this edit) radii at each end of the affected feature.
    // For an already-cylindrical face these are equal; for an existing cone
    // they differ.
    double m_resizeCylOriginalBottomR = 0.0;
    double m_resizeCylOriginalTopR    = 0.0;
    // Which ends the user is editing. Face select → both true (and the
    // single popup field drives both). Edge select on the V_min end →
    // editBottom true; V_max end → editTop true.
    bool   m_resizeCylEditBottom = true;
    bool   m_resizeCylEditTop    = true;
    double m_resizeCylNewBottomDiameter = 0.0;
    double m_resizeCylNewTopDiameter    = 0.0;
    char   m_resizeCylBotBuf[32]  = "0.0";
    char   m_resizeCylTopBuf[32]  = "0.0";
    bool   m_resizeCylInputFocus  = true;
    TopoDS_Shape m_resizeCylPreviousShape;

    // Detect whether the currently-picked face is on a recognised resizable
    // body (solid cylinder / tube). Populates the relevant m_resizeCyl* fields
    // and returns true if so. Called per frame to drive the toolbar button.
    bool detectCylindricalResizeCandidate();
    void beginResizeCylindrical();
    void updateResizeCylindrical();
    void commitResizeCylindrical();
    void cancelResizeCylindrical();
    void renderResizeCylindricalPanel();

    // Interactive extrude state
    bool m_extruding = false;
    TopoDS_Shape m_extrudeProfile;
    glm::vec3 m_extrudeNormal{0, 0, 1};
    glm::vec3 m_extrudeOrigin{0};
    float m_extrudeDistance = 5.0f;
    int m_extrudePreviewBodyId = -1;
    char m_extrudeInputBuf[32] = "5.0";
    bool m_extrudeInputFocus = true;
    // NewBody (default) or Subtract: Subtract cuts the extruded profile out of
    // m_extrudeTargetBody (the body the sketch was drawn on) on commit, and the
    // live preview is shown in red.
    ExtrudeMode m_extrudeMode = ExtrudeMode::NewBody;
    int m_extrudeTargetBody = -1;

    // Right-click face context menu state
    int m_contextMenuBodyId = -1;
    TopoDS_Shape m_contextMenuFace;
    bool m_contextMenuPending = false;

    // Project file + dirty tracking
    std::string m_currentProjectPath;          // empty until first save/load
    int m_savedAtHistoryStep = -1;             // history index when last saved/loaded
    bool m_unsavedNonHistoryChanges = false;   // for mutations outside History (imports, etc.)

    // Close-with-unsaved-changes prompt
    bool m_showSavePrompt = false;
    bool m_confirmedClose = false;
    bool m_closeAfterSave = false;             // set when user picked Save in the prompt
};

} // namespace materializr

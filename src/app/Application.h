#pragma once

#include <memory>
#include <vector>
#include <functional>
#include <string>
#include <set>
#include <glm/glm.hpp>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

#include "modeling/ExtrudeOp.h" // for ExtrudeMode

namespace materializr {

struct AppSettings;
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
    // `safeMode` is set by `--safe-mode` on the CLI; loadAppSettings honours
    // it by stomping the persisted "potentially-expensive" fields back to
    // known-safe defaults (MSAA off, mesh quality Low, default lights,
    // autosave off, auto-open-last-project off) and writing them out, so the
    // *next* normal launch is recovered without further action.
    explicit Application(bool safeMode = false);
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
    void exportSettings();    // Settings dialog → Export…  (write current prefs as JSON)
    void importSettings();    // Settings dialog → Import…  (load prefs from JSON, apply live)
    AppSettings currentSettings() const;     // gather current prefs into a struct
    void        applyAppSettings(const AppSettings& s); // push prefs onto the live members
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
    void loadProject();         // File dialog → loadProjectAt
    // Load a project file directly by path. Used by loadProject() and by the
    // "auto-open last project on launch" path.
    bool loadProjectAt(const std::string& path);
    // File → Close Project. Prompts to save if dirty (unless autosave is on),
    // then clears the document/history/selection and resets the project path.
    void closeProject();
    void doCloseProject();      // the actual clear; called from closeProject + save prompt
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
    // History step index immediately before the current sketch was entered.
    // The "Exit Sketch (discard)" button rewinds history back to this step
    // so the user can bail out of a half-built sketch without keeping any
    // partial content. -1 = no sketch in progress (or pre-sketch state
    // wasn't capturable).
    int m_sketchEntryHistoryStep = -1;
    int m_activeSketchId = -1; // document id of the sketch being edited, or -1 if new

    // Hovered sketch region (for highlight in viewport)
    int m_hoveredSketchId = -1;
    int m_hoveredRegionIndex = -1;

    // Numeric dimension input shown while placing a sketch shape
    char m_sketchDimBuf[32] = "";
    bool m_sketchDimWasShown = false; // tracks placing transitions to grab keyboard focus

    // Sketch grid step in mm (drives both the visual face grid and snap-to-line)
    float m_sketchGridStep = 1.0f;
    // World-aligned anchor used as the sketch grid origin and the camera
    // target. Computed at sketch entry from the face centre snapped to the
    // nearest grid intersection projected onto the sketch plane. Preserved
    // through orbit-exit so the new perspective view pivots around the same
    // point the user was sketching on.
    glm::vec3 m_sketchSnappedAnchor{0.0f};

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
    float m_selectionLineWidth = 3.0f; // px width of highlighted edges/body outlines
    bool  m_showToolbarTooltips = true; // hover description on each toolbar button
    // Apply m_light*/m_msaaSamples/m_selectionLineWidth to the renderer + viewport.
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

    // Interactive Shell (hollow a body by removing a picked face and offsetting
    // the remaining shell inward). Same popup-with-live-preview pattern as
    // push/pull and fillet/chamfer.
    bool m_shellActive = false;
    int  m_shellBodyId = -1;
    TopoDS_Face m_shellFace;
    float m_shellThickness = 1.0f;
    char m_shellInputBuf[32] = "1.0";
    bool m_shellInputFocus = true;
    TopoDS_Shape m_shellPreviousShape;

    void beginInteractiveShell();
    void updateInteractiveShell();
    void commitInteractiveShell();
    void cancelInteractiveShell();
    void renderShellPanel();

    // Interactive Pattern (Linear / Radial). Same live-preview-via-history idiom
    // as push/pull and resize: each parameter change replays an updated PatternOp,
    // commit leaves the op in history at the user's values, cancel undoes it.
    enum class PatternKind { Linear, Radial };
    bool m_patternActive = false;
    PatternKind m_patternKind = PatternKind::Linear;
    int m_patternBodyId = -1;
    int m_patternAxisIdx = 0; // 0=X, 1=Y, 2=Z
    int m_patternCount = 3;
    float m_patternDistance = 5.0f; // linear: spacing in mm along chosen axis
    float m_patternAngle = 360.0f;   // radial: total sweep in degrees
    float m_patternOriginX = 0.0f, m_patternOriginY = 0.0f, m_patternOriginZ = 0.0f;
    bool m_patternPickingOrigin = false; // viewport is in axis-origin-pick mode
    bool m_patternPreviewPushed = false; // true while a preview PatternOp is on history
    bool m_patternInputFocus = true;
    char m_patternCountBuf[16] = "3";
    char m_patternDistanceBuf[32] = "5.0";
    char m_patternAngleBuf[32] = "360.0";

    void beginPattern(PatternKind kind);
    void updatePattern();      // (re-)push a preview PatternOp from current state
    void commitPattern();      // leave preview as the final op + clean up state
    void cancelPattern();      // undo preview if any + clean up state
    void renderPatternPanel(); // ImGui popup contents

    // Sketch-mode Linear / Radial patterns. Simpler than body patterns: the
    // sketch is on a fixed 2D plane so there's no axis radio. Linear copies
    // along the sketch's +X axis by `m_sketchPatternDistance` per step;
    // radial rotates around the user-supplied (x, y) origin in sketch coords
    // for a total sweep of `m_sketchPatternAngle` degrees. The popup is a
    // small modal — no live preview. On apply we run an inline geometry copy
    // similar to SketchCopy / Mirror and push a single SketchEditOp.
    bool m_sketchPatternActive = false;
    PatternKind m_sketchPatternKind = PatternKind::Linear;
    int  m_sketchPatternCount = 3;
    float m_sketchPatternDistance = 5.0f;
    float m_sketchPatternAngle = 360.0f;
    float m_sketchPatternOriginX = 0.0f;
    float m_sketchPatternOriginY = 0.0f;
    bool m_sketchPatternFocusInput = false;
    char m_sketchPatternCountBuf[16]    = "3";
    char m_sketchPatternDistanceBuf[32] = "5.0";
    char m_sketchPatternAngleBuf[32]    = "360.0";
    char m_sketchPatternOXBuf[32] = "0.0";
    char m_sketchPatternOYBuf[32] = "0.0";
    // True while the user is clicking in the sketch viewport to set the
    // radial origin. The next sketch-mode click is captured by the pattern
    // popup instead of going through SketchTool.
    bool m_sketchPatternPickingOrigin = false;

    // Snapshot of the sketch at popup-open. Every parameter change replays
    // from this snapshot so the preview reflects only the current values,
    // not an accumulation of previous previews. On Apply we diff against
    // this snapshot to push the SketchEditOp; on Cancel we restore it.
    std::shared_ptr<materializr::Sketch> m_sketchPatternBefore;
    // Involved geometry captured at popup-open. We re-use the same IDs each
    // preview frame since the snapshot is restored each time (new IDs from
    // earlier preview copies wouldn't survive the restore).
    std::set<int> m_sketchPatternPts;
    std::set<int> m_sketchPatternLines;
    bool          m_sketchPatternSelectAll = false; // include all circles + arcs

    void beginSketchPattern(PatternKind kind);
    void updateSketchPattern();   // re-apply preview from m_sketchPatternBefore
    void commitSketchPattern();   // leave preview in place + push SketchEditOp
    void cancelSketchPattern();   // restore m_sketchPatternBefore + clear state
    void renderSketchPatternPopup();

    // User-facing axis convention follows 3D-printer / Z-up: X = side-to-side,
    // Y = forward-back, Z = up. Materializr's world stays Y-up internally, so
    // the user's axis index translates to a world direction via this helper
    // (user X → world X, user Y → world Z, user Z → world Y). Also returns
    // which world-axis index (0/1/2 = world X/Y/Z) the user index resolves to,
    // useful for coordinate-component access like `pos[worldIdx] = ...`.
    static glm::vec3 userAxisToWorldVec(int userIdx);
    static int       userAxisToWorldIdx(int userIdx);

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

    // Close-with-unsaved-changes prompt. The prompt is shared between two
    // close intents: exiting the app, or just closing the current project
    // (File → Close Project). The post-save action picks which happens after
    // a successful Save.
    bool m_showSavePrompt = false;
    bool m_confirmedClose = false;
    bool m_closeAfterSave = false;             // set when user picked Save in the prompt
    enum class PostSaveAction { None, CloseProject };
    PostSaveAction m_postSaveAction = PostSaveAction::None;

    // Settings option: re-open the most recent project on launch (only if it
    // wasn't explicitly closed before quit). The "last open project path" lives
    // in AppSettings::lastProjectPath and is mirrored from m_currentProjectPath
    // on save/load and cleared on closeProject().
    bool m_autoOpenLastProject = false;
    bool m_checkForUpdatesOnLaunch = true;

    // Set by the --safe-mode CLI flag. When true, loadAppSettings stomps
    // rendering, autosave, and auto-open-last-project back to safe defaults
    // and persists them.
    bool m_safeMode = false;
};

} // namespace materializr

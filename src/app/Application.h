#pragma once
#include "../platform_defs.h"

#include <memory>
#include <future>
#include <vector>
#include <functional>
#include <string>
#include <set>
#include <map>
#include <glm/glm.hpp>
#include "ui/UpdateChecker.h"
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>

#include "app/InteractiveOpController.h"
#include "app/FaceOpControllers.h"
#include <array>
#include "modeling/ExtrudeOp.h" // for ExtrudeMode
#include "modeling/SketchConstraints.h" // for ConstraintType (applySketchConstraint)
#include "modeling/Unfold.h" // for FlatPattern (m_unfoldPattern)
#include "core/SheetSpec.h" // for SheetMaterial (m_unfoldMaterial)
#include "io/Settings.h" // for AppSettings::RecentProject (m_recentProjects)

// Global (non-namespaced) op, forward-declared for configureFaceOp's signature.
class MoveFaceOp;

namespace materializr {

struct AppSettings;
class Window;
class Viewport;
class Grid;
class ShapeRenderer;
class SketchRenderer;
class EdgeRenderer;
class BackgroundRenderer;
class ViewCube;
class Picker;
class Gizmo;
class SelectionHighlight;
class BoxSelect;
class SectionView;
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
class ThreadOp;
class PushPullOp;

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
    // Locate a TTF from assets/fonts across AppImage / dev / Windows-zip
    // layouts; "" when missing. Used by the UI font load + the Text tool.
    std::string resolveBundledFont(const std::string& fname) const;
    void renderTextToolPanel(); // sketch Text tool settings (floating)
    // Transient centered toast (threads-last guidance etc.) — shown for a
    // few seconds, doesn't fight the per-frame status-bar message.
    void showThreadsLastToast();
    void showToast(const std::string& text, double seconds = 4.0);
    void renderTransientToast();
    std::string m_toastText;
    double m_toastExpiry = 0.0;
    void renderSvgToolPanel();  // SVG placement settings (floating)
    void renderMirrorToolPanel(); // interactive mirror line controls (floating)
    // Camera-upright default rotation for Text/SVG placement.
    void seedUprightPlacementAngle();
    void initRenderers();
    void setupCommands();
    void beginFrame();
    void endFrame();
    void renderSplashFrame(const char* status);
    // Self-contained progress frame for long operations, rendered between main
    // frames (via m_deferredHeavyTask). Returns true if the user hit Cancel.
    // fraction<0 = indeterminate; fraction<=0 also resets the cancel latch.
    bool renderProgressFrame(float fraction, const char* label);
    // A left→right sweeping marquee bar at the current ImGui cursor (shared by
    // the projection progress overlay and the thread-cutting modal).
    void drawIndeterminateBar();
    bool m_progressCancelled = false;
    // Set once the first splash frame has been rendered to BOTH swap-chain
    // buffers (see renderSplashFrame) — kills the intermittent black flash from
    // the undefined back buffer being presented before the first swap.
    bool m_splashPrimed = false;
    // A heavy op deferred from a controller commit to run between frames, where
    // renderProgressFrame can pump its own frames without nesting ImGui frames.
    std::function<void()> m_deferredHeavyTask;
    // Idle-render throttle: counts down frames to render after the last event
    // or active-work wakeup. Zero = skip the frame and sleep for the next event.
    int m_wakeFrames = 0;
    // True only while a load is tessellating in the deferred slot: tells
    // rebuildMeshes to pump a per-body progress frame (safe between frames).
    bool m_pumpMeshProgress = false;
    // Launch-time update check, run on a worker so a slow/unreachable network
    // can't freeze startup (it was a synchronous call with a 10 s timeout —
    // the real cause of the "not responding" on launch). Polled each frame.
    std::future<materializr::UpdateChecker::Result> m_updateCheckFuture;
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
    // Per-body STL export: opens a save dialog with the body's current name
    // (from the Items panel) as the default filename and writes JUST that
    // body's mesh. Triggered from the viewport right-click menu and the
    // Items-panel context menu so users can dump individual parts of a
    // multi-body project without juggling visibility for the file-menu
    // "Export STL" (which writes every visible body to one file).
    void exportBodyAsStl(int bodyId);
    void exportSketchAsSvg(int sketchId);
    // Zoom-fit the camera onto the selection (or all visible bodies when
    // nothing is selected). Bound to F and View > Frame Selection — the menu
    // item is the touch path.
    void frameSelection();
    // Delete the sketch tool's selected elements (points + lines), history-
    // wrapped, and sweep orphan points. Bound to Delete and the sketch context
    // bar's Delete button — the latter is the touch path.
    void deleteSelectedSketchElements();
    void saveProject();         // Save dialog (Save As behavior)
    void saveProjectQuick();    // Save to current path if known, else falls through to saveProject
    void loadProject();         // File dialog → loadProjectAt
    // Load a project file directly by path. Used by loadProject() and by the
    // "auto-open last project on launch" path.
    bool loadProjectAt(const std::string& path);
    // Like loadProjectAt but shows a loading bar and tessellates up front,
    // pumping frames so the window stays responsive (no OS "not responding").
    // Must run from the deferred-heavy-task slot (between frames), never inside
    // a live ImGui frame. Used for the auto-open-on-launch path.
    void loadProjectWithProgress(const std::string& path);

    // Open Recent: a persisted, most-recent-first list of projects. `ref` is a
    // filesystem path (desktop) or a SAF content:// URI (Android); `name` is the
    // display label. addRecentProject records a successful open/save;
    // openRecentProject re-opens one (resolving the URI on Android).
    void addRecentProject(const std::string& ref, const std::string& name);
    void openRecentProject(const AppSettings::RecentProject& r);
    void removeRecentProject(const std::string& ref);
    // Run `doOpen` now if the document is clean; otherwise route through the
    // unsaved-changes save prompt and run it once that resolves. All project
    // opens (dialog + Open Recent) go through here so none silently discard work.
    void guardedOpen(std::function<void()> doOpen);
    // File → Close Project. Prompts to save if dirty (unless autosave is on),
    // then clears the document/history/selection and resets the project path.
    void closeProject();
    void doCloseProject();      // the actual clear; called from closeProject + save prompt
    // Snapshot the operation history (parameters + per-step body diffs) for the
    // project file, and rebuild a replayable history from a loaded project.
    // Cancels live interactive previews first by default: the snapshot seeds
    // from the CURRENT doc body, so an uncommitted preview (e.g. a shell being
    // dragged) would otherwise bake into the previous step's snapshot with no
    // op behind it — a hollow body that reloads un-editable and can't be
    // re-shelled. The background recovery autosave passes false so it doesn't
    // yank an active preview out from under the user mid-drag.
    ProjectHistory captureProjectHistory(bool cancelPreviews = true);
    void rebuildHistoryFromProject(const ProjectHistory& hist,
                                   const std::string& savedByVersion = "");

    // Dirty tracking + unsaved-changes prompt
    bool isDirty() const;
    void markDirty();           // for changes that don't go through History
    void markSaved();
    void renderSavePrompt();
    void requestClose();        // called when the user clicks the window X

    // Merge coplanar sketches into the first (Items panel). Non-coplanar ones
    // are skipped; refuses (toast) if fewer than two end up coplanar.
    void combineSketches(const std::vector<int>& ids);
    // Make an independent copy of a sketch (Items panel → Duplicate Sketch).
    void duplicateSketch(int sketchId);
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

    // Touch sketch context-bar actions for chain tools (line/spline):
    //   Back   — drop the most recently placed segment / control point, keep
    //            drawing the chain.
    //   Cancel — discard the whole chain being drawn (every segment placed
    //            since the chain started), then end placement.
    void sketchChainBack();
    void sketchChainCancel();

    // Flag a single body as needing a mesh refresh. Call sites that already
    // know which body changed should prefer this over `m_meshesDirty = true`
    // — the next rebuildMeshes pass updates just this body via setBodyMesh,
    // leaving the rest of the (potentially 100+) bodies untouched. Critical
    // for push/pull preview smoothness on complex projects.
    void markBodyDirty(int bodyId) { if (bodyId >= 0) m_dirtyBodyIds.insert(bodyId); }

    // If `sketchId`'s Sketch has a sourceBodyId but no sourceFace (typical
    // for a sketch reloaded from a project file), walk the source body's
    // faces and bind the planar face whose plane coincides with the sketch's
    // plane. Without a sourceFace, Sketch::buildRegions doesn't union the
    // host face's wires (holes, fillets) into the sketch — and a push/pull
    // of a "circle around an existing hole" wrongly produces a solid bar.
    void ensureSketchSourceFace(int sketchId);

    // Apply a sketch constraint of the given type to the current
    // SketchTool element selection. Inspects the selection counts to decide
    // which arity to use (e.g. Coincident chains pairs of selected points;
    // Parallel pairs each line with the first one). No-op if the selection
    // doesn't match the constraint's requirements. Routed from the toolbar
    // Constraints section; constraints are always opt-in.
    void applySketchConstraint(ConstraintType type);

    // Align the orbit camera to look straight at the active sketch's plane in ortho.
    // Called when entering sketch mode / editing an existing sketch.
    void alignCameraToActiveSketch();

    // Sketch region hover/pick + Push/Pull. buildIfCold=false makes the pick
    // SKIP sketches whose region cache would need the heavy OCCT fuse —
    // required on the per-frame hover path (a cold complex sketch would
    // freeze the app on the first mouse move after being unhidden); click
    // frames pass true and build as before.
    struct SketchRegionHit { int sketchId = -1; int regionIndex = -1; glm::vec3 worldPoint{0.0f}; };
    SketchRegionHit pickSketchRegion(float screenX, float screenY,
                                     float vpW, float vpH,
                                     bool buildIfCold = true) const;
    void beginPushPull();
    void updatePushPull();
    void commitPushPull();
    void cancelPushPull();
    // ── Move Face (face transform → body follows via loft; see MoveFaceOp) ──
    // The face transform this gesture applies (Move / Rotate / Scale share the
    // same loft engine + deferred silhouette; only the gizmo + drag math differ).
    enum class FaceXform { Translate, Rotate, Scale };
    void beginMoveFace(FaceXform kind = FaceXform::Translate);
    // Configure a MoveFaceOp with the current gesture's kind + params, and test
    // whether the gesture has anything to apply (defined in the .cpp where the
    // op type is complete).
    void configureFaceOp(MoveFaceOp& op) const;
    bool faceXformNontrivial() const;
    // Total tilt = the live ring drag composed onto the accumulated tilts.
    glm::mat3 faceRotTotal() const;
    void bakeFaceRotationDrag(); // fold a released ring drag into the accumulator
    void updateMoveFace();   // live shear preview against the snapshot
    void commitMoveFace();
    void cancelMoveFace();
    void moveFaceSlideSketches(const glm::vec3& v); // restore + slide on-face sketches
    bool m_moveFaceActive = false;
    // Hole-move sub-mode of the Move tool: the same translate gizmo drives a
    // MoveHoleOp (slide a through-hole across its face) instead of a face shear.
    // Set when the Move selection is a recognizable hole wall (see beginMoveFace).
    bool m_moveHoleMode = false;
    TopoDS_Face m_moveHoleWall;              // the clicked hole-wall seed face
    int  m_moveFaceBodyId = -1;
    TopoDS_Face  m_moveFaceFace;
    TopoDS_Shape m_moveFacePreviousShape;    // snapshot for preview / restore
    glm::vec3 m_moveFaceP0{0.0f};            // a point on the face plane
    glm::vec3 m_moveFaceN{0.0f, 0.0f, 1.0f}; // face plane normal (outward)
    glm::vec3 m_moveFaceVec{0.0f};           // accumulated in-plane slide
    glm::vec3 m_moveFaceBase{0.0f};          // slide banked before the current drag
    glm::vec3 m_moveFaceDragStart{0.0f};     // plane hit-point at drag start
    bool m_moveFaceDragging = false;
    // Two in-plane arrow axes + which one a drag latched (0=A, 1=B, -1=none).
    glm::vec3 m_moveFaceAxisA{1.0f, 0.0f, 0.0f};
    glm::vec3 m_moveFaceAxisB{0.0f, 1.0f, 0.0f};
    int  m_moveFaceGrab = -1;
    FaceXform m_faceXformKind = FaceXform::Translate;
    glm::vec3 m_moveFacePivot{0.0f};  // face centroid (rotate/scale pivot)
    float m_moveFaceAngle = 0.0f;     // accumulated tilt (radians, Rotate)
    float m_moveFaceAngleBase = 0.0f; // tilt banked before the current drag
    float m_moveFaceScale = 1.0f;     // accumulated uniform factor (Scale)
    float m_moveFaceScaleBase = 1.0f;
    // Non-uniform scale: separate factors along the two in-plane axes. When
    // uniform (default), both track m_moveFaceScale.
    bool  m_moveFaceScaleUniform = true;
    float m_moveFaceScaleA = 1.0f, m_moveFaceScaleB = 1.0f;
    float m_moveFaceScaleABase = 1.0f, m_moveFaceScaleBBase = 1.0f;
    glm::vec3 m_moveFaceRotAxis{1.0f, 0.0f, 0.0f}; // tilt axis latched this drag
    float m_moveFaceRotStartAngle = 0.0f; // cursor angle in the ring plane at drag start
    // Composed tilt from prior ring drags this session (about the fixed axes),
    // so you can stack 5° about one then 10° about the other. The live tilt is
    // rodrigues(rotAxis, angle) * accum; on each ring release the drag is baked
    // into accum and the angle resets.
    glm::mat3 m_moveFaceRotAccum{1.0f};
    bool m_moveFaceRotHasAccum = false;
    float m_moveFaceHalfExtent = 1.0f; // face size, maps drag distance → angle/scale
    bool  m_moveFaceRotSnap = true;    // snap tilt to whole degrees (default on)
    // TWIST = the THIRD rotation ring, about the face NORMAL (lies in the face
    // plane). Lives under FaceXform::Rotate: grabbing this ring (grab 2) spins
    // the face relative to its base and commits a MoveFaceOp::Kind::Twist —
    // distinct from the two tilt rings. Mutually exclusive with a tilt within a
    // session (m_moveFaceIsTwist picks which op the gesture builds).
    float m_moveFaceTwist = 0.0f;      // accumulated twist (radians) about the normal
    float m_moveFaceTwistBase = 0.0f;  // twist banked before the current drag
    float m_moveFaceTwistStart = 0.0f; // cursor angle in the face plane at drag start
    bool  m_moveFaceIsTwist = false;   // this Rotate gesture is a twist, not a tilt
    // DEFERRED REBUILD: the body rebuild is deferred to mouse-release, so the
    // drag only moves ghost SILHOUETTES of the face's loops. Loop 0 = outer
    // outline, 1..N = hole loops (same order as the op enumerates them). Each is
    // drawn translated by m_moveFaceVec only if that loop is flagged to move.
    std::vector<std::vector<glm::vec3>> m_moveFaceSilhouetteLoops;
    bool m_moveFacePendingRebuild = false;
    // Per-loop motion, derived from the SELECTION. moveOuter = a planar face is
    // selected (outline slides, holes slant). holeVertical[i] = that hole's
    // cylindrical face was Ctrl-selected → it moves as a straight tube. One per
    // hole, in loop order (matches m_moveFaceSilhouetteLoops[1..]).
    bool m_moveFaceMoveOuter = true;
    std::vector<bool> m_moveFaceHoleSlant;     // top edge picked → top ring follows
    std::vector<bool> m_moveFaceHoleVertical;  // cylinder wall picked → tube follows
    // Sketches sitting ON the moved face — they slide with it. Original planes
    // snapshotted so the live preview / cancel can restore them.
    std::vector<int>    m_moveFaceSketchIds;
    std::vector<gp_Pln> m_moveFaceSketchPlanes0;
    void beginInteractiveExtrude(const TopoDS_Shape& profile,
                                 ExtrudeMode mode = ExtrudeMode::NewBody,
                                 int targetBody = -1,
                                 int sourceSketchId = -1);

    // Re-execute every enabled ExtrudeOp in history that was originally built
    // from this sketch — called when the user edits a constraint value via
    // the Properties → Constraints panel or the History → Apply Changes
    // path. Downstream ops (Fillet, Pattern, Push/Pull face-references) are
    // intentionally NOT re-run; that's a separate toponaming-heavy future
    // release. Result: simple "sketch -> extrude -> done" chains follow the
    // sketch immediately; chained workflows leave downstream ops on their
    // old body shape (user re-does manually).
    void cascadeFromSketchEdit(int sketchId);
    // Map each sketch id to the body ids it drives (created/modified through a
    // sketch-sourced extrude / push-pull). Used by the gizmo commit to tell a
    // unison move (body + its driving sketch) from a lone move that de-links.
    // MEMOIZED on History::revision(): the Properties panel asks for the link
    // hint every frame while a body/sketch is selected (the normal working
    // state), and rebuilding this walks the whole history + captureDiff per
    // op — hundreds of map/set node allocations per frame on a long history.
    const std::map<int, std::set<int>>& sketchBodyLinks() const;
    // Human-readable parametric-link summary for the Properties panel: for a body
    // (isBody=true) which sketch drives it, for a sketch which body it drives, plus
    // whether the link is live or was broken by an independent 3D move. "" = none.
    std::string linkHintFor(bool isBody, int id) const;
    // True if `bodyId` can be safely rebuilt by re-running history from its
    // sketch (`viaSketchId`) — i.e. nothing but that sketch's own extrude/
    // push-pull touches it. A fillet/chamfer/boolean/other feature downstream
    // can't re-bind after the geometry moves, so those bodies must move rigidly
    // (and de-link) instead of re-deriving.
    bool bodySafelyRederivable(int bodyId, int viaSketchId) const;
    // sketchBodyLinks() memo — see its declaration. ~0u forces the first build.
    mutable std::map<int, std::set<int>> m_linkMapCache;
    mutable unsigned m_linkMapRevision = ~0u;
    // Re-establish the parametric link of a detached sketch (Properties-panel
    // "Re-link"): clears the detached flag so editing the sketch drives its body
    // again. isBody=true re-links every detached sketch driving that body.
    // Geometry is left as-is — re-link resumes parametric control, it doesn't move.
    void relinkSketch(bool isBody, int id);
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
    std::unique_ptr<BackgroundRenderer> m_backgroundRenderer;
    std::unique_ptr<ViewCube> m_viewCube;
    std::unique_ptr<Picker> m_picker;
    std::unique_ptr<Gizmo> m_gizmo;
    std::unique_ptr<SelectionHighlight> m_selectionHighlight;
    std::unique_ptr<BoxSelect> m_boxSelect;
    std::unique_ptr<SectionView> m_sectionView;
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

private:
    // Sketch
    std::shared_ptr<Sketch> m_activeSketch;
    // Deferred "before" snapshot from a line-chain anchor click (first click,
    // only the start point placed). Held so the first segment's undo step
    // absorbs the anchor into ONE step; see recordSketchMutation.
    std::shared_ptr<Sketch> m_deferredSketchBefore;
    size_t m_deferredSketchBeforeSig = 0;
    Sketch* m_deferredSketchOwner = nullptr;
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

    // Right-click in the sketch viewport with at least one element selected
    // opens a context menu (currently: Add Constraint ▸ submenu). Set by the
    // input handler; consumed by the popup-render block on the next frame so
    // ImGui's OpenPopup happens inside the same window stack as the popup.
    bool m_sketchCtxMenuPending = false;

    std::unique_ptr<SketchSolver> m_sketchSolver;
    std::unique_ptr<SketchTool> m_sketchTool;
    bool m_inSketchMode = false;
    // Wall-clock (SDL_GetTicks seconds) until which interactive states (sketch,
    // push/pull, gizmo, edge/face ops, …) keep rendering continuously after the
    // last input; past it they idle like the main viewport (the live preview
    // stays on screen, frozen, and wakes instantly on the next event). Refreshed
    // on every event in the main loop. See hasActiveWork().
    double m_interactiveGraceUntil = 0.0;
    // History step index immediately before the current sketch was entered.
    // The "Exit Sketch (discard)" button rewinds history back to this step
    // so the user can bail out of a half-built sketch without keeping any
    // partial content. -1 = no sketch in progress (or pre-sketch state
    // wasn't capturable).
    int m_sketchEntryHistoryStep = -1;
    int m_activeSketchId = -1; // document id of the sketch being edited, or -1 if new

    // In-progress-sketch crash/kill recovery (see io/SketchRecovery). While in
    // sketch mode the active sketch is periodically written to a sidecar draft
    // (it isn't in the saved project until Finish Sketch). On launch a surviving
    // draft means last session ended mid-sketch; m_pendingSketchRecovery drives
    // the restore prompt.
    double m_lastDraftWrite = 0.0;       // ImGui time of last draft write
    int    m_lastDraftElemCount = -1;    // element count at last write (skip no-ops)
    bool   m_pendingSketchRecovery = false;
    void writeSketchDraftIfDue();        // throttled per-frame draft write
    void renderSketchRecoveryPrompt();   // startup "restore unfinished sketch?" modal
    void restoreSketchDraftNow();        // re-enter sketch mode with the saved draft

    // Whole-project crash/hang recovery (see io/ProjectRecovery). Independent of
    // the user-facing autosave (which only writes a SAVED file): the committed
    // model — bodies + full history — is snapshotted to a sidecar even for an
    // UNSAVED project, so a crash or a hang never loses more than the last
    // committed step. Cleared on a clean exit; a survivor drives the restore
    // prompt. Snapshots immediately on each new committed step, else throttled.
    double m_lastRecoveryWrite = 0.0;    // wall-clock secs of last recovery write
    int    m_lastRecoveryStep = -2;      // history currentStep at last write
    bool   m_pendingProjectRecovery = false;
    void writeProjectRecoveryIfDue();    // per-frame crash-recovery snapshot
    void renderProjectRecoveryPrompt();  // startup "restore unsaved project?" modal
    void restoreProjectRecoveryNow();    // load the recovery snapshot

    // Hovered sketch region (for highlight in viewport)
    int m_hoveredSketchId = -1;
    int m_hoveredRegionIndex = -1;

    // Numeric dimension input shown while placing a sketch shape
    char m_sketchDimBuf[32] = "";
    bool m_sketchDimWasShown = false; // tracks placing transitions to grab keyboard focus

    // Dimension-label click-to-edit. m_dimEditingId is the constraint being
    // edited (-1 = none); the popup near the label shows the current value
    // and Enter commits / re-solves. m_dimEditingClickedThisFrame tells the
    // sketch click handler to swallow the click that opened the popup so a
    // bare point isn't also placed underneath.
    int  m_dimEditingId = -1;
    char m_dimEditingBuf[32] = "";
    bool m_dimEditingFocus = false;
    bool m_dimEditingClickedThisFrame = false;

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
    // Live preview op (snapshot/restore engine): undone + re-executed
    // directly against the document each preview frame, appended to history
    // exactly once via pushExecuted() at commit. History is untouched
    // during the preview — see updatePushPull.
    std::unique_ptr<PushPullOp> m_pushPullLiveOp;
    bool m_pushPullPreviewApplied = false;
    bool m_pushPullSymmetric = false; // panel checkbox (plane-sketch targets)
    float m_pushPullDistance = 5.0f;
    // Unsnapped drag accumulator. The grid snap in updatePushPull mutates
    // m_pushPullDistance itself (so the readouts show the snapped value),
    // which would erase sub-step drag motion every frame — a slow drag
    // accumulated nothing, then a fast flick jumped a whole step. The drag
    // adds into THIS instead, and m_pushPullDistance is derived + snapped
    // from it. Typing/sliding a value re-bases the accumulator.
    float m_pushPullDistanceRaw = 0.0f;
    char m_pushPullInputBuf[32] = "5.0";
    bool m_pushPullInputFocus = true;
    // Face arrow: drag along this normal to drive the distance (set from the first
    // face target). m_pushPullHasArrow is false for sketch-region-only push/pull.
    glm::vec3 m_pushPullOrigin{0.0f};
    glm::vec3 m_pushPullNormal{0.0f, 0.0f, 1.0f};
    bool m_pushPullHasArrow = false;
    // Trackpad-mode sticky drag (orbitButton == panButton == LMB): a single
    // click in the viewport while the arrow is up enters this state, mouse
    // moves then drive the distance frame-by-frame without a button held,
    // and a second click exits. Same shape as the Sketch Circle tool's
    // click-move-click pattern — gives users a way to "drag" the arrow
    // when their primary click is already bound to orbit. While true,
    // gizmoOwnsDrag suppresses orbit so the cursor isn't fighting the
    // camera. (Steve: "let click then click act like click and hold".)
    bool m_pushPullSticky = false;
    // Dense-body drag protection: when any target body has >250 faces (a
    // threaded rod), the per-frame preview shows a tinted GHOST of the tool
    // volume instead of running the real boolean (which would also trigger
    // the thread reflow) every frame. The real op runs once, on commit.
    bool m_pushPullHeavyPreview = false;
    std::unique_ptr<PushPullOp> makePushPullOpFromState() const;

    // Snap-to-grid for gizmo translate (shares the grid step with the sketch grid).
    bool m_snapToGrid = true;
    // Previous-frame hover state for the corner snap widget. The widget is
    // hit-tested with raw IsMouseHoveringRect (no ImGui item) so it can't
    // claim input via the usual IsItemActive path; instead the viewport
    // input handlers check this flag to skip picker/sketch-tool clicks when
    // the mouse is over the widget. Same pattern m_viewCube uses with
    // wasHovered().
    bool m_snapWidgetHovered = false;

    // Configurable camera mouse bindings (ImGuiMouseButton values: 0=Left,1=Right,
    // 2=Middle). Zoom is always the scroll wheel. Edited in File > Settings.
#if defined(MZ_MOBILE)
    int m_orbitButton = 0; // Left (trackpad default; rebindable in Settings)
    int m_panButton = 0;   // Left
#else
    int m_orbitButton = 2; // Middle
    int m_panButton = 1;   // Right
#endif
    // Touch multi-select toggle: the finger stand-in for holding Ctrl. While on,
    // the viewport selection code runs as if Ctrl were held (taps add/toggle
    // instead of replacing). Driven by the on-screen button in the viewport.
    bool m_multiSelectToggle = false;
    // Touch "Move" navigation lock: one-finger drag orbits, taps don't draw or
    // select — so panning/zooming (esp. in a sketch) can't start a drawing.
    bool m_moveModeToggle = false;
    // Touch press-drag-release: a drawing-tool press is pending; its point is
    // placed on release (the drag previews the radius/bulge/segment first).
    bool m_sketchPressActive = false;
    // For the drag tools (circle/rectangle) the centre/first corner is dropped on
    // press so the whole shape is one press-drag-release gesture; this flags that
    // the release must complete the shape (and only if the finger actually moved).
    bool m_sketchDragCenterPlaced = false;
    // History step count captured at the start of a drawing-tool press, so that
    // if a two-finger pan/zoom takes the press over we can tell whether the
    // press already pushed a step (e.g. Line's start vertex) and roll it back.
    int m_sketchPressStepBefore = 0;
    // Interactive mirror line manipulated by a sketch-style move/rotate gizmo.
    // Reuses the SketchGizmoHandle vocabulary (MoveX/MoveY/MoveFree/Rotate).
    SketchGizmoHandle m_mirrorGizmoHandle = SketchGizmoHandle::None;
    glm::vec2 m_mirrorGizmoStartAnchor{0.0f}; // mirror anchor at drag start
    glm::vec2 m_mirrorGizmoGrab{0.0f};        // sketch-space cursor at drag start
    float m_mirrorGizmoStartAngle = 0.0f;     // mirror angle at drag start
    float m_sketchDownX = 0.0f, m_sketchDownY = 0.0f; // press pos (px) for drag slop
    // ImGui drops IsItemHovered() mid-drag once the window-move grab takes the
    // ActiveId, which freezes the live sketch preview. Latch the viewport input
    // block alive while a left press-drag that began over the viewport is in
    // flight so onMouseMove keeps following the finger. Cleared on button-up.
    bool m_viewportInputLatch = false;
    // Touch: a menu-bar toggle that force-raises the system soft keyboard (some
    // Android builds don't reliably auto-raise it on field focus). OR'd with
    // io.WantTextInput; typed text still flows into whatever field is focused.
    bool m_softKeyboardForced = false;
    bool m_showSettings = false;
    int m_settingsOrbitButton = 2; // staged value in the Settings dialog
    int m_settingsPanButton = 1;

    // Touch mode (large UI + touch gestures) staged value for the Settings
    // dialog. The live state lives in the materializr::touchMode() global; this
    // mirrors the saved setting and is written back on save. Default tracks the
    // platform (see AppSettings::touchMode); applyAppSettings keeps it in sync.
#if defined(MZ_MOBILE)
    bool m_touchMode = true;
#else
    bool m_touchMode = false;
#endif

    // Autosave: once the project has been saved at least once (has a path on
    // disk), periodically re-save dirty changes. Toggled in File > Settings.
    bool m_autosaveEnabled = false;
    float m_autosaveIntervalSec = 120.0f;
    double m_lastAutosaveTime = 0.0;

    // Invert the cube-drag → orbit direction (Settings).
    bool m_invertCubeDrag = false;

    // Double-click window (s), applied to ImGuiIO::MouseDoubleClickTime. Higher
    // suits trackpads (slower double-taps). Persisted; default = ImGui's 0.30.
    float m_doubleClickTime = 0.30f;

    // Rendering preferences (File > Settings → Rendering). Persisted.
    float m_lightAmbient = 0.40f;   // base illumination; higher = softer shadows
    bool  m_lightHeadlight = false; // key light tracks the camera
    bool  m_lightFill = true;       // soft opposing fill light
    int   m_msaaSamples = 4;        // viewport anti-aliasing: 0=off, 2, 4, 8
    int   m_meshQuality = 1;        // tessellation density: 0=Low..3=Ultra
    float m_selectionLineWidth = 3.0f; // px width of highlighted edges/body outlines
    float m_sketchLineWidth = 2.5f;    // px width of sketch geometry over the grid
    float m_sketchGridOpacity = 0.55f; // opacity of the sketch-plane grid (0..1)
    float m_sketchGridThickness = 1.0f; // grid line-width multiplier (0.1..2)
    bool  m_smallScreenWarned = false; // persisted: user ticked "don't show again"
    bool  m_smallScreenAck = false;    // dismissed for this run only
    bool  m_leftPanelHidden = false;   // persisted: Tools column collapsed
    bool  m_rightPanelHidden = false;  // persisted: Items/History/Properties column collapsed
    // "Viewport" window screen rect, captured each frame in renderViewport, used
    // to anchor the touch collapse handles at the panel/viewport boundaries.
    float m_viewportWinX = 0, m_viewportWinY = 0, m_viewportWinW = 0, m_viewportWinH = 0;
    // One-time notice on phone-sized screens (the UI is built for tablets+).
    void renderSmallScreenWarning();
    void renderPanelCollapseHandles();   // touch-mode edge tabs to hide/show each side
    void renderPluginMenuItems(const char* menuName);  // plugin MenuContributions for a menu

    // Touch tooltip-timeout state (see beginFrame): blank a parked pointer after
    // 15 s so a stuck tooltip clears.
    float  m_tipLastMouseX = -1e9f;
    float  m_tipLastMouseY = -1e9f;
    double m_tipStationarySince = 0.0;
    bool  m_showToolbarTooltips = true; // hover description on each toolbar button
    // Per-panel visibility (Settings > Panels), persisted. Default all on. These
    // gate each docked panel's render so it can be hidden to free screen space
    // and brought back from Settings — independent of the left/right column
    // collapse. (The viewport is never toggled — no multi-viewport yet.)
    bool  m_showTools        = true;
    bool  m_showInteractions = true;
    bool  m_showHistory      = true;
    bool  m_showItems        = true;
    bool  m_showProperties   = true;
    // Touch-mode camera sensitivity (Settings > Touch; persisted; 1.0 = default).
    float m_touchOrbitSens = 1.0f;
    float m_touchPanSens   = 1.0f;
    float m_touchZoomSens  = 1.0f;
    // Toggle for the sketch toolbar's live Full/Reduced/Off inference cycle
    // button. Off hides the button so users who set the level once in
    // Settings can declutter the sketch toolbar.
    bool  m_showInferenceToolbarToggle = true;
    // STL import (persisted). m_stlImportAccuracy pre-fills the import dialog's
    // fidelity slider; m_meshShowWireframe gates the facet wireframe of imported
    // mesh bodies (live — toggling it re-runs the mesh-body edge rebuild).
    float m_stlImportAccuracy = 0.5f;
    bool  m_meshShowWireframe = true;
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
    // Full-rebuild signal: clear all meshes and re-tessellate every visible
    // body. Necessary on theme/mesh-quality changes, project load, and the
    // first frame. Most edits should prefer markBodyDirty() so a 145-body
    // project doesn't pay full re-tessellation on every push/pull frame.
    bool m_meshesDirty = true;
    // Per-body partial rebuild signal. rebuildMeshes() walks this set and
    // updates only those bodies' meshes via setBodyMesh / removeBody. Cleared
    // after each rebuild pass.
    std::set<int> m_dirtyBodyIds;
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
    // World-Y of the lowest face of the drag selection at drag start. The
    // translate readout reports the body's BOTTOM (= what the user sees
    // sitting on the grid) on the user-Z axis rather than the bbox centre,
    // so a cylinder resting on Z=0 reads `Z 0.00` instead of `Z 10.00`.
    // Sketch-only drags get the pivot's Y here (no bbox → no offset).
    float m_gizmoSharedBottomY{0.0f};
    // Primary body's bbox captured ONCE at drag start (the originals never
    // change during a drag). The Scale branch needs its diagonal every
    // frame; recomputing BRepBndLib::Add per drag frame was 50-150 ms on a
    // complex body — a large slice of the "moving one part lags" report.
    glm::vec3 m_gizmoDragBBoxMin{0.0f};
    glm::vec3 m_gizmoDragBBoxMax{0.0f};

    // GPU-only gizmo drag preview (same pattern as the Revolve live preview):
    // during the drag the document is NOT touched — the accumulated transform
    // is pushed as a model matrix onto the dragged bodies' shape+edge mesh
    // slots, so a drag frame costs two uniform updates instead of a BRep
    // transform + updateBody + re-tessellation + edge re-discretization of
    // the dragged body. The real (parametric, undoable) transform is applied
    // exactly once on release; Esc just resets the matrices.
    void gizmoPreviewApply(const glm::mat4& m);
    void gizmoPreviewReset() { gizmoPreviewApply(glm::mat4(1.0f)); }

    // Standalone-sketch gizmo drag — set when the gizmo is shown on a Sketch
    // selection (no body in the selection, not in sketch-edit, perspective
    // view). m_sketchGizmoDragSketches holds {sketchId, planeBefore} for
    // every dragged sketch; on release a SketchTransformOp per sketch is
    // pushed to history. Distinct from the body-attached sketch path: those
    // ride along through TransformOp's m_previousSketchPlanes machinery.
    std::vector<std::pair<int, gp_Pln>> m_sketchGizmoDragSketches;

    // Construction-plane gizmo drag — same shape as the sketch list above,
    // but writes back via Document::setPlane instead of Sketch::setPlane.
    // Used by both the in-popup placement gizmo and (after the popup
    // commits) any post-selection drag on a Plane in the document.
    std::vector<std::pair<int, gp_Pln>> m_planeGizmoDrag;

    // Construction-plane gizmo arming — mirrors m_sketchGizmoArmed. Selection
    // alone (clicking a plane in the viewport or items panel) gets you a
    // highlight only; pressing W/E or clicking Move/Rotate in the Plane
    // tools panel arms the gizmo for the currently selected plane. Cleared
    // when the active plane in the selection changes. During the original
    // Construction Plane popup placement (m_planeOpActive) we treat the
    // gizmo as implicitly armed so the user can manipulate the preview
    // straight away.
    bool m_planeGizmoArmed = false;
    int  m_planeGizmoArmedFor = -1;

    // Construction-axis gizmo drag + arming — same shape as the plane
    // version. Axes have no Rotate semantics (an infinite line has no
    // meaningful "rotate the line around itself") so only Translate
    // writes through. The drag list stores {axisId, origin-before-drag,
    // direction-before-drag} so the live drag can rebase off a stable
    // pose each frame.
    struct AxisDragEntry { int id; gp_Pnt origin; gp_Dir direction; };
    std::vector<AxisDragEntry> m_axisGizmoDrag;
    bool m_axisGizmoArmed = false;
    int  m_axisGizmoArmedFor = -1;

    // Per-body gizmo-center cache. Without this, the body-selected branch in
    // renderViewport calls BRepBndLib::Add(shape, bbox) every frame to place
    // the Move/Rotate gizmo on the body centroid — and on a complex part
    // (1.5m airplane skeleton: many trimmed B-spline surfaces per body) that
    // bbox walk is 50–150 ms each, dropping the idle frame rate to 6 FPS the
    // moment one or two bodies are selected. Key: the body's TShape pointer
    // PLUS the shape's location — a location-only transform (multi-body move
    // commit) keeps the TShape while moving the body, and a TShape-only key
    // left the gizmo sitting at the pre-move centroid. Topology rebuilds
    // (push/pull, fillet, single-body transform via copy=true) still miss on
    // the pointer — exactly when we'd want a fresh centroid anyway.
    struct GizmoCenterCacheEntry {
        const void* tsh = nullptr;
        TopLoc_Location loc;
        glm::vec3 center{0.0f};
    };
    std::map<int, GizmoCenterCacheEntry> m_gizmoCenterCache;

    // Sketches do NOT show the gizmo automatically on selection — that lets
    // the Tools toolbar surface its Move / Rotate / Loft / Edit options
    // cleanly without a gizmo dropped on top. The user clicks Move or Rotate
    // to "arm" the gizmo for the current sketch; selection-change clears it.
    // (Bodies still get the gizmo on selection as before.)
    bool m_sketchGizmoArmed = false;
    int  m_sketchGizmoArmedFor = -1; // sketch id armed for; cleared when sel changes

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
    // Scale popup unit mode. Percent is the multi-body-safe default; mm only
    // makes sense when exactly one body is selected (we can show its
    // bbox-derived target dims). renderScalePanel forces Percent whenever
    // the selection isn't a single body.
    enum class ScaleUnitMode { Percent, Millimeter };
    ScaleUnitMode m_scaleUnitMode = ScaleUnitMode::Percent;
    // mm-mode text buffer + focus state per user axis (X, Y, Z in Z-up
    // convention). Re-seeded from the body's current bbox each frame the
    // field isn't focused so the displayed value tracks external edits
    // (undo/redo, other panels) without overwriting the user's in-flight
    // typing.
    struct ScaleMmEdit {
        char buf[24] = "0";
        bool focused = false;
        int bodyId = -1;
        double initialExtent = 0;
    };
    ScaleMmEdit m_scaleMmEdit[3];

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
    // Two-distance (asymmetric) chamfer. When on, the op takes a second setback
    // along the OTHER adjacent face, dragged via a second arrow. Chamfer-only,
    // single-edge for now. m_edgeOpFaceDirA/B are the two in-face drag
    // directions (A = ChamferOp's reference face = faces.First()). m_edgeOpGrab
    // latches which arrow a drag owns: 0 = A (distance 1), 1 = B (distance 2).
    bool  m_edgeOpTwoDist = false;
    float m_edgeOpValue2 = 0.0f;
    char  m_edgeOpInputBuf2[32] = "1.0";
    glm::vec3 m_edgeOpFaceDirA{0.0f, 0.0f, 1.0f};
    glm::vec3 m_edgeOpFaceDirB{0.0f, 1.0f, 0.0f};
    bool  m_edgeOpHasFaceDirs = false;
    bool  m_edgeOpCanTwoDist = false; // selection supports a consistent A/B chamfer
    int   m_edgeOpGrab = -1; // -1 none, 0 = arrow A, 1 = arrow B (during a drag)
    // Set on the left-click frame iff the cursor was near the edge-op arrow
    // line; cleared on release. Joins gizmoOwnsDrag so trackpad-mode left-
    // orbit can't steal the drag — and conversely, dragging from empty
    // space orbits the camera instead of yanking the value, matching the
    // Scale Face pattern. (Steve: chamfer / fillet arrows didn't grab the
    // cursor in trackpad mode.)
    bool  m_edgeOpDragging = false;
    // History index of the op being re-edited. -1 means "creating new" — the
    // commit path then pushes a fresh FilletOp / ChamferOp. >=0 means "editing
    // existing" — commit updates the op's parameter and calls editStep().
    int m_edgeOpEditingIndex = -1;
    // The body whose fillet/chamfer FACE was clicked to start an edit. Used to
    // detect a baked feature: if that body's geometry doesn't change after the
    // edit, the operation drives a different/deleted body and the clicked
    // geometry has no editable op behind it — we tell the user instead of
    // silently doing nothing.
    int m_edgeOpPickedBodyId = -1;
    // Pre-edit geometry of the picked body, captured BEFORE the first editStep
    // preview runs. Commit compares against these values to detect a frozen op
    // (one that never actually changes the body). Capturing here rather than in
    // commitInteractiveEdgeOp avoids a false-positive where the preview already
    // brought the body to the new radius and the commit's editStep then looks
    // "unchanged" because both snapshots are at the same new value.
    double m_edgeOpPrePickedVol  = 0.0;
    double m_edgeOpPrePickedArea = 0.0;
    // The radius/distance the op had when the edit began. Cancel (and the
    // confirm-at-zero "treat as cancel" path) restores it before replaying,
    // since the edit-mode live preview mutates the real op's parameter.
    float m_edgeOpOrigValue = 0.0f;
    float m_edgeOpOrigValue2 = 0.0f; // second distance at edit-begin (cancel restore)

    // Compute m_edgeOpFaceDirA/B — the two in-face drag directions for the
    // first selected edge (A = the face ChamferOp uses for distance 1). Sets
    // m_edgeOpHasFaceDirs. Requires m_edgeOpEdges + m_edgeOpPreviousShape +
    // m_edgeOpMid/m_edgeOpDir to already be set.
    void computeEdgeOpFaceDirs();
    void beginInteractiveEdgeOp(EdgeOpType type);
    // Re-edit the FilletOp or ChamferOp at the given history index. Pulls the
    // existing radius/distance + edges + body id from the op, snapshots its
    // pre-state for live preview, and reuses the same drag handle + popup UI
    // as creation. Triggered by clicking a face the op produced.
    void beginInteractiveEdgeOpEdit(int historyIndex);
    // Re-runs the live preview at m_edgeOpValue / m_edgeOpValue2. Returns
    // true iff a non-zero preview was successfully applied; false on a
    // zero/edit-mode-skip or a kernel failure (the snapshot is restored in
    // that case). Begin uses the return value to probe a starting radius
    // for new fillets so a fresh op shows a visible preview right away.
    bool updateInteractiveEdgeOp();
    void commitInteractiveEdgeOp();
    void cancelInteractiveEdgeOp();
    // Re-resolve every fillet/chamfer op's generated-face mapping against the
    // current bodies. Must run after ANY editStep replay (commit, cancel, or
    // zero-value bail) because the replay re-runs each op's execute(), leaving
    // its faces at their pre-downstream-Transform positions until rebound.
    void refreshAllEdgeOpFaces();

    // Resize-cylindrical (edit a closed cylindrical/conical face's diameter,
    // or a single circular edge of one) ====================================
    // Triggered by picking a closed cylindrical face (edits BOTH end edges
    // together → stays a cylinder) or a single circular edge (edits ONE end
    // → turns cylinder into a cone, makes funnels). Internally the commit
    // path always builds a CONE primitive at the two end radii — for the
    // face-edit case they're equal.
    bool m_resizeCylActive = false;
    bool m_resizeCylPreviewFailed = false; // last preview produced no valid body
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

    // ─── Thread (helical screw thread on a cylindrical face) ───────────────
    // beginThread copies the geometry the cylindrical-face detector left in
    // the m_resizeCyl* fields; the popup collects pitch/depth/handedness and
    // Apply pushes a ThreadOp (no live preview — the helical sweep + boolean
    // is too heavy to run per-frame).
    bool   m_threadActive = false;
    int    m_threadBodyId = -1;
    bool   m_threadIsHole = false;
    double m_threadAxis[9] = {0, 0, 0, 0, 0, 1, 1, 0, 0}; // loc, dir, xdir
    double m_threadRadius = 5.0;
    double m_threadLength = 10.0;
    float  m_threadPitch  = 1.0f;
    float  m_threadDepth  = 0.6f;
    bool   m_threadRightHanded = true;
    char   m_threadPitchBuf[32] = "1.0";
    char   m_threadDepthBuf[32] = "0.6";
    // Apply runs the helical sweep + boolean on a worker thread (it takes
    // seconds) behind a modal, so the window keeps pumping events instead of
    // going "not responding". The future carries the cut result; the main
    // thread polls it each frame and pushes the op when ready.
    std::future<TopoDS_Shape> m_threadFuture;
    bool   m_threadComputing = false;

    // Section View — render-only clipping of the scene by a plane so the
    // user can inspect interiors (thread profiles, wall thickness) without
    // destructive booleans. Plane source is a construction plane or a world
    // plane; offset slides it along its normal; flip swaps which half is
    // hidden. ShapeRenderer discards clipped fragments; SectionView overlays
    // the true B-rep intersection curves on the cut.
    bool   m_sectionEnabled    = false;
    int    m_sectionPlaneId    = -1;  // construction plane id; -1 = world
    int    m_sectionWorldPlane = 0;   // 0=XY 1=XZ 2=YZ (when planeId < 0).
                                      // XY (vertical, normal Z) — the old
                                      // XZ/ground default clipped everything
                                      // above the floor.
    float  m_sectionOffset     = 0.0f;
    bool   m_sectionFlip       = false;
    bool   m_sectionDirty      = true; // recompute overlay curves next frame
    gp_Pln sectionBasePlane() const;  // flip applied, offset NOT applied
    void   renderSectionPanel();      // floating controls while enabled

    void beginThread();        // copies detector output, opens the popup
    std::unique_ptr<ThreadOp> makeThreadOpFromState() const;
    void commitThread();       // kicks the compute onto a worker thread
    void cancelThread();
    void renderThreadPanel();  // ImGui popup contents

    // Detect whether the currently-picked face is on a recognised resizable
    // body (solid cylinder / tube). Populates the relevant m_resizeCyl* fields
    // and returns true if so. Called per frame to drive the toolbar button.
    bool detectCylindricalResizeCandidate();
    void beginResizeCylindrical();
    void updateResizeCylindrical();
    void commitResizeCylindrical();
    void cancelResizeCylindrical();
    void renderResizeCylindricalPanel();

    // Interactive face ops (Shell / Taper / Scale Face) live in
    // controllers now — see InteractiveOpController.h. Each owns its own
    // state, lifecycle, and panel; the registry below drives suppression,
    // the Esc chain, and panel rendering generically.
    ShellController m_shellCtl;
    TaperController m_taperCtl;
    ScaleFaceController m_scaleFaceCtl;
    ProjectSketchController m_projectSketchCtl;
    DefeatureController m_defeatureCtl;
    std::array<InteractiveOpController*, 5> m_iops{
        &m_shellCtl, &m_taperCtl, &m_scaleFaceCtl, &m_projectSketchCtl,
        &m_defeatureCtl};
    IopContext iopContext();
    bool anyIopActive() const {
        for (auto* c : m_iops) if (c->active()) return true;
        return false;
    }
    // Single-flight: starting one interactive op cancels any other live
    // preview — controller or legacy (push/pull, extrude, pattern, resize,
    // thread). Two concurrent previews on the same body snapshot each
    // other's PREVIEW state — cancelling the first then restores the
    // second's contaminated snapshot, leaving phantom geometry with no
    // history step (Steve's "cancelled the projection and it still stuck").
    void beginIop(InteractiveOpController& ctl);
    void cancelActiveIops(); // controller half, callable from legacy begins
    bool anyInteractivePreviewActive() const; // controllers + legacy previews
    void cancelAllInteractivePreviews();      // both halves; saves call this

    // Last hover pick, reused by cursor-zoom so wheel ticks never ray-cast
    // the document themselves (see Application_Viewport zoom handler).
    bool m_zoomFocusHit = false;
    glm::vec3 m_zoomFocusPoint{0.0f};
    int m_zoomFocusFrame = -1;

    // Pan depth-anchor gesture tracking (see the anchoredPan lambda in the
    // camera-drag handler): the anchor is captured once per pan gesture —
    // desktop gestures live for as long as a camera button stays held,
    // touch gestures for as long as two-finger pan events keep arriving.
    bool m_panAnchorHeld = false;   // desktop: a camera button hold owns the anchor
    int m_lastTouchPanFrame = -1000; // touch: last frame a two-finger pan applied
    // Orbit re-anchors its pivot onto the geometry at the VIEW CENTRE at the
    // start of each orbit gesture, so it spins around the object instead of a
    // point that drifted behind it (cursor-zoom leaves the target off the
    // surface → orbit swings the model sideways, reading as pan+rotate). Held
    // for the gesture's lifetime; the centre pick is on the view axis, so
    // moving the target along it doesn't shift the image — only the pivot.
    bool m_orbitAnchorHeld = false;

    // Click-cycling state: first click at a spot picks the visible FACE,
    // a second click at the same spot cycles to the sketch region covered
    // by / behind that face — resolves the face-vs-region ambiguity when
    // bodies sit on both sides of their source sketch plane.
    glm::vec2 m_pickCyclePos{-1000.0f, -1000.0f};
    double m_pickCycleTick = 0.0; // ImGui time of the last pick at m_pickCyclePos
    // Touch: after a double-tap escalates to the body, ImGui's queued 2nd-tap
    // click can land a few frames LATER and revert to the face. Ignore face-select
    // clicks until this time (set ~0.5s out on escalation; bounded so a genuine
    // later tap isn't swallowed).
    double m_suppressFaceClickUntil = 0.0;
    int m_pickCycleLast = -1; // -1 none, 0 face, 1 region

    // Resolve the pull direction + neutral-plane point from the current
    // axis choice, the picked faces, and the body's bounds.
    bool resolveTaperFrame(glm::vec3& dirOut, glm::vec3& neutralOut) const;

    // Interactive Pattern (Linear / Radial). Same live-preview-via-history idiom
    // as push/pull and resize: each parameter change replays an updated PatternOp,
    // commit leaves the op in history at the user's values, cancel undoes it.
    enum class PatternKind { Linear, Radial };
    bool m_patternActive = false;
    PatternKind m_patternKind = PatternKind::Linear;
    int m_patternBodyId = -1;
    int m_patternAxisIdx = 0; // 0=X, 1=Y, 2=Z (used when m_patternAxisId < 0)
    int m_patternAxisId = -1; // selected construction axis id; -1 = world axis
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

    // Interactive Loft popup — two profile wires snapshotted from the selected
    // sketches at begin time, plus Solid/Shell + Smooth/Ruled + Reverse-B
    // toggles, all driving a live preview pushed onto history (same pattern
    // as Linear/Radial Pattern). Reverse-B flips profile B's wire direction
    // before passing it to LoftOp, which is the usual fix for the "apex
    // pinch / pyramid" output when the two wires' start vertices don't
    // line up.
    bool m_loftActive = false;
    TopoDS_Wire m_loftWireA;
    TopoDS_Wire m_loftWireB;
    // Inner (hole) wires of each profile, so a ring section lofts into a tube.
    std::vector<TopoDS_Wire> m_loftHolesA;
    std::vector<TopoDS_Wire> m_loftHolesB;
    bool m_loftSolid = true;
    bool m_loftRuled = false;
    bool m_loftReverseB = false;
    bool m_loftPreviewPushed = false;

    void beginLoft();          // reads selection, snapshots wires, pushes initial preview
    void updateLoft();         // re-push preview with current params
    void commitLoft();
    void cancelLoft();
    void renderLoftPanel();

    // Interactive Construction Plane popup. ConstructionPlanePlugin fires
    // requestInteractiveOp("ConstructionPlane"); Application reads the
    // current selection (a planar face enables Parallel-to-Face mode) and
    // opens a small popup with XY / XZ / YZ / Parallel-to-Face + an offset
    // slider, live-previewed via ConstructionPlaneOp on history. Same Apply
    // / Cancel idiom as Loft and Pattern.
    bool m_planeOpActive = false;
    int  m_planeOpKindIdx = 0;   // 0=XY, 1=XZ, 2=YZ, 3=ParallelToFace
    double m_planeOpOffset = 0.0;
    gp_Pln m_planeOpBaseFace;     // host face's plane when Parallel-to-Face is available
    bool m_planeOpHaveFace = false;
    bool m_planeOpPreviewPushed = false;
    char m_planeOpOffsetBuf[32] = "0.0";
    // Typeable "rotate by N° around X/Y/Z" applied to the current preview
    // plane via Document::setPlane. Keeps the offset slider + base
    // orientation untouched (we transform the live plane on top), and the
    // user can stack multiple rotations by re-clicking Apply.
    float m_planeOpRotDeg = 0.0f;
    char  m_planeOpRotBuf[32] = "0.0";
    int   m_planeOpRotAxisIdx = 0; // 0=X, 1=Z (user up), 2=Y

    // Selection-derived inputs for the kind-index 4/5/6 creation modes,
    // captured once at beginConstructionPlane. Each reduces to a plane with
    // normal N through point P (computed by computeDerivedPlaneNP), fed to
    // the op as ParallelToFace-style basePlane + point.
    //   4 = Midplane           — needs two planar planes/faces
    //   5 = Normal to axis/edge — needs an axis or straight edge (+ point)
    //   6 = Tangent to cylinder — needs a cylindrical face (+ a side ref)
    bool   m_planeOpHaveTwoPlanes = false;
    gp_Pln m_planeOpPlaneA, m_planeOpPlaneB;
    bool   m_planeOpHaveAxis = false;
    gp_Ax1 m_planeOpAxis;
    gp_Pnt m_planeOpAxisPoint;
    bool   m_planeOpHaveCylinder = false;
    gp_Ax1 m_planeOpCylAxis;
    double m_planeOpCylRadius = 0.0;
    gp_Dir m_planeOpCylRefDir{1.0, 0.0, 0.0};
    // Resolve the captured inputs for a derived kind index into a base
    // (normal, through-point) pair, pre-offset. Returns false if the needed
    // selection isn't present.
    bool computeDerivedPlaneNP(int kindIdx, gp_Dir& outNormal, gp_Pnt& outPoint) const;

    void beginConstructionPlane();
    // Open the plane popup forced to a specific kind index (4=Midplane,
    // 5=Normal-to-Axis, 6=Tangent), used by the Properties-panel contextual
    // "Create …" buttons.
    void beginConstructionPlaneMode(int kindIdx);
    void updateConstructionPlane();
    void commitConstructionPlane();
    void cancelConstructionPlane();
    void renderConstructionPlanePanel();

    // Interactive Construction Axis popup — direct parallel to the plane
    // popup above. Plugin fires requestInteractiveOp("ConstructionAxis");
    // we open a small popup with World-X / Y / Z radios + an origin point
    // input. (Two-point and face-normal modes are listed but their
    // viewport-pick UX is deferred — typing the origin coords is enough
    // for the v0.6.x line.) Live preview via ConstructionAxisOp on
    // history, same Apply / Cancel idiom as the plane popup.
    bool m_axisOpActive = false;
    int  m_axisOpKindIdx = 0;     // 0=WorldX,1=userY,2=userZ,3=Cyl,4=Edge,5=2Pts,6=FaceNormal,7=2Planes
    double m_axisOpOrigin[3] = {0.0, 0.0, 0.0}; // world coords
    char m_axisOpOriginBuf[3][24] = {"0.0", "0.0", "0.0"};
    bool m_axisOpPreviewPushed = false;

    // Selection-derived inputs for kind indices 3–7, captured at
    // beginConstructionAxis. Each reduces to an (origin, direction) the host
    // computes (computeDerivedAxisOD) and feeds via setOrigin/setDirection.
    bool   m_axisOpHaveCylinder = false;  gp_Ax1 m_axisOpCylAxis;
    bool   m_axisOpHaveEdge = false;      gp_Ax1 m_axisOpEdgeAxis;
    bool   m_axisOpHaveTwoVerts = false;  gp_Pnt m_axisOpV1, m_axisOpV2;
    bool   m_axisOpHaveFaceNormal = false; gp_Pnt m_axisOpFacePt; gp_Dir m_axisOpFaceNormal;
    bool   m_axisOpHaveTwoPlanes = false; gp_Pln m_axisOpPlaneA, m_axisOpPlaneB;
    bool computeDerivedAxisOD(int kindIdx, gp_Pnt& outOrigin, gp_Dir& outDir) const;

    void beginConstructionAxis();
    // Open the axis popup forced to a kind index (used by the Tools-panel
    // "Add Axis…" buttons): 3=Cylinder, 4=Edge, 5=Two points, 6=Face normal,
    // 7=Two-plane intersection.
    void beginConstructionAxisMode(int kindIdx);
    void updateConstructionAxis();
    void commitConstructionAxis();
    void cancelConstructionAxis();
    void renderConstructionAxisPanel();

    // Primitive creation popup. Plugin fires requestInteractiveOp("Primitive*")
    // and Application opens a small panel with the parameters appropriate for
    // the chosen kind (extents for Box, radius/height for Cylinder/Cone/etc.
    // /Origin for all of them). Confirm pushes a PrimitiveOp onto history;
    // Cancel just closes the popup. No live preview yet — the geometry's
    // cheap to recompute on commit and a stale preview body would have to be
    // undone on every keystroke. (Steve: "primitives popup parameters; live-
    // preview / fancier UI after 1.0".)
    bool   m_primitivePopupActive = false;
    int    m_primitivePopupKind   = 0; // 0=Box,1=Cyl,2=Sphere,3=Cone,4=Torus
    double m_primitivePopupExtents[3]  = {10.0, 10.0, 10.0}; // box X/Y/Z
    double m_primitivePopupRadius      = 5.0;                 // cyl/sphere/cone bottom/torus major
    double m_primitivePopupHeight      = 10.0;                // cyl/cone
    double m_primitivePopupTopRadius   = 0.0;                 // cone tip
    double m_primitivePopupMinorRadius = 2.0;                 // torus tube
    double m_primitivePopupOrigin[3]   = {0.0, 0.0, 0.0};     // world origin
    void beginPrimitivePopup(int kindIdx);
    void commitPrimitivePopup();
    void cancelPrimitivePopup();
    void renderPrimitivePopup();

    // STL import options dialog. Opened from the Import > STL menu (the plugin
    // routes through requestInteractiveOp("StlImport")). Collects a file path
    // (via Browse), a fidelity/accuracy slider, and a wireframe toggle, then
    // runs StlIO::import on commit. Mirrors the primitive-popup begin/render/
    // commit/cancel pattern.
    bool   m_stlDialogActive = false;
    std::string m_stlDialogPath;
    float  m_stlDialogAccuracy = 0.5f;
    bool   m_stlDialogWireframe = true;
    void beginStlImportDialog();
    void commitStlImport();
    void cancelStlImport();
    void renderStlImportDialog();

    // Unfold / Flatten — "lay it flat" for laser/CNC/templates. beginUnfoldDialog
    // runs the planar-net unfold on the selected body and opens a 2D Flat-Pattern
    // dialog (cut + fold lines), with a material dropdown driving fold handling
    // and SVG export. See modeling/Unfold.h.
    bool m_unfoldDialogActive = false;
    int  m_unfoldBodyId = -1;
    std::unique_ptr<materializr::FlatPattern> m_unfoldPattern;
    materializr::Rigidity m_unfoldRigidity = materializr::Rigidity::SemiRigid;
    float m_unfoldThicknessMm = 5.0f;
    std::vector<TopoDS_Face> m_unfoldSourceFaces; // kept so the bevel slider can re-run
    float m_unfoldMaxBevelDeg = 10.0f;            // angular tolerance: max bevel per score line
    bool  m_unfoldConformal = false;              // LSCM unwrap (one stretchy piece) vs developable pieces
    bool  m_unfoldPageA4 = false;                 // PDF export page size: A4 vs US Letter
    float m_unfoldRotationDeg = 0.0f;             // viewer/export rotation of the whole flat pattern
    int   m_unfoldExportFmt = 0;                  // export format: 0 = SVG (no page grid), 1 = PDF (tiled)
    int   m_unfoldRegDensity = 2;                 // PDF alignment-mark density: 0 None,1 Sparse,2 Normal,3 Dense
    void beginUnfoldDialog();
    void recomputeUnfold();
    void renderUnfoldDialog();

    // Revolve popup. Opens when the user clicks Revolve in the body Tools
    // panel; takes a sketch profile + an axis (canonical world axis or a
    // Construction Axis from the document) + angle + mode. Apply pushes a
    // RevolveOp. Pre-fills sketch / axis / target body from the current
    // selection so the common "select profile, select axis, click Revolve"
    // flow lands ready-to-Apply.
    bool m_revolveActive = false;
    // What the revolve does. 0 = "Rotate Body": apply a TransformOp::Rotate
    // around the picked axis to every selected body (multi-body supported,
    // no sketch needed, no new geometry created). 1 = "Sweep Sketch": full
    // RevolveOp that sweeps a sketch profile around the axis into a new /
    // boolean'd body (single-body target).
    int  m_revolveWhatIdx  = 0;
    int  m_revolveSketchId = -1;
    int  m_revolveAxisId   = -1;          // -1 = use canonical world axis below
    int  m_revolveWorldAxisIdx = 2;       // 0=X, 1=Y(user)=worldZ, 2=Z(user)=worldY
    int  m_revolveBodyId   = -1;          // primary body — first one in the selection
    std::vector<int> m_revolveBodyIds;    // all selected bodies (>=1); Rotate Body iterates this
    int  m_revolveModeIdx  = 0;           // 0=NewBody 1=Union 2=Cut 3=Intersect (Sweep mode)
    float m_revolveAngle   = 360.0f;
    char  m_revolveAngleBuf[24] = "360.0";
    // Live-preview state for Rotate Body mode. Snapshot of the body's
    // original shape taken at popup open; each angle change applies a fresh
    // transform from the snapshot (not from the current state, which would
    // accumulate). Apply restores the snapshot before pushing the real
    // TransformOp so the history step computes its own previousShape
    // correctly. Cancel restores and aborts.
    bool         m_revolveLiveActive = false;
    int          m_revolveOrigBodyId = -1;
    TopoDS_Shape m_revolveOrigShape;
    float        m_revolveLastAppliedAngle = 0.0f;

    // Arc-drag interaction state. The yellow arced arrow in the viewport
    // becomes clickable when the popup is open in Rotate Body mode:
    // press over the arc to grab it, drag tangentially to spin the body
    // around the axis. We track per-frame cursor-angle deltas (rather than
    // a single offset) so the drag is robust to the atan2 wrap-around when
    // the cursor crosses the ±π boundary. m_revolveArcWasHovered persists
    // for one frame so the picker / selection code can skip clicks that
    // landed on the arc.
    bool  m_revolveArcDragging = false;
    bool  m_revolveArcWasHovered = false;
    float m_revolveArcDragAngleAccum = 0.0f;   // accumulated cursor delta (rad)
    float m_revolveArcDragLastCursorAng = 0.0f;
    float m_revolveArcDragStartBodyAng = 0.0f; // m_revolveAngle when drag began

    // Captures the current selection (sketch + axis + bodies) and opens
    // the Revolve popup. Called from both the Revolve plugin's toolbar
    // button (via requestInteractiveOp dispatch) and any future Command
    // Palette entry. Keeping it as a single helper means the "what to do
    // when revolve is requested" logic stays in one place.
    void beginRevolve();
    void renderRevolvePopup();
    void applyRevolve();
    // Cancel / commit helpers — share the restore-original logic.
    void revolveLiveBegin();
    void revolveLiveApply(float angle);
    void revolveLiveRestore();

    // Interactive "Rotate Plane About Axis" popup. Triggered from the Items
    // panel plane context menu (m_itemsPanel->setRotatePlaneCallback). Tilts /
    // hinges an existing construction plane about a chosen line by a typed
    // angle. The line can be the plane's own U / V axis (tilt in place), a
    // construction axis, a selected straight edge, or a selected cylindrical
    // face's centreline — each resolved to a gp_Ax1 at open time (transient,
    // nothing persisted). Matches the plane gizmo's model: writes straight
    // through Document::setPlane with no history op (plane transforms are
    // intentionally outside undo — see Application_Viewport.cpp's planeOnly
    // branch). Live preview re-bases from m_rotPlaneOriginal each change;
    // Apply leaves the current pose, Cancel restores the snapshot.
    bool   m_rotPlaneActive = false;
    int    m_rotPlaneId = -1;             // target plane id
    gp_Pln m_rotPlaneOriginal;            // snapshot for preview re-base + cancel
    float  m_rotPlaneAngle = 0.0f;        // degrees
    char   m_rotPlaneAngleBuf[24] = "0.0";
    int    m_rotPlaneHingeIdx = 0;        // index into the parallel vectors below
    std::vector<gp_Ax1>      m_rotPlaneHinges;       // resolved hinge per combo entry
    std::vector<std::string> m_rotPlaneHingeLabels;  // combo display labels

    void beginRotatePlaneAboutAxis(int planeId);
    void renderRotatePlaneAboutAxisPopup();
    void applyRotatePlanePreview();       // setPlane(original rotated about hinge by angle)
    void cancelRotatePlaneAboutAxis();    // restore snapshot + close

    // Sketch Move type-in panel: when the Move gizmo is active on a single
    // selected standalone sketch (the sketch-as-construction-plane workflow),
    // this small popup offers X/Y/Z inputs for an exact translation. Apply
    // pushes a single SketchTransformOp with the typed offset; same auto-
    // reopen-when-conditions-met pattern as the multi-transform rotate panel.
    float m_sketchMove[3] = {0.0f, 0.0f, 0.0f};
    // Text-buffer mirrors of m_sketchMove for the three input fields. We keep
    // a buffer + atof flow (rather than ImGui::InputFloat) because InputFloat
    // doesn't commit until Enter / focus-out, which the user routinely missed
    // before clicking Apply.
    char m_sketchMoveBuf[3][32] = { "0", "0", "0" };
    bool m_sketchMovePanelOpen = true;
    bool m_sketchMoveConditionsMet = false;

    void renderSketchMovePanel();
    void applySketchMove();

    // Snap-grid corner widget (next to the ViewCube). Shows the current step
    // (0.1 / 1 / 10 mm) and gets a solid-blue border when snap is on. Click
    // opens a small popup with the snap toggle + step radios. Changes save
    // immediately so the choice persists across launches.
    void renderSnapWidget();

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
    // Source sketch for the in-flight extrude. Stamped onto every ExtrudeOp
    // we push (preview + final) so the cascade-on-sketch-edit walker can find
    // them later. -1 means face-driven extrude (no source sketch; never
    // cascades).
    int m_extrudeSketchId = -1;
    glm::vec3 m_extrudeNormal{0, 0, 1};
    glm::vec3 m_extrudeOrigin{0};
    float m_extrudeDistance = 5.0f;
    int m_extrudePreviewBodyId = -1;
    // The exact preview op we pushed — undo is VERIFIED against this so an
    // outside history touch can never make the preview pop a committed step.
    const Operation* m_extrudePreviewOp = nullptr;
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
    int m_contextMenuPlaneId = -1; // >=0 → the pending context menu is for a plane
    int m_contextMenuSketchId = -1; // >=0 → the pending context menu is for a sketch
    bool m_contextMenuPending = false;

    // Project file + dirty tracking
    std::string m_currentProjectPath;          // empty until first save/load
    std::vector<AppSettings::RecentProject> m_recentProjects; // Open Recent (persisted)
    int m_savedAtHistoryStep = -1;             // history index when last saved/loaded
    bool m_unsavedNonHistoryChanges = false;   // for mutations outside History (imports, etc.)

    // Close-with-unsaved-changes prompt. The prompt is shared between two
    // close intents: exiting the app, or just closing the current project
    // (File → Close Project). The post-save action picks which happens after
    // a successful Save.
    bool m_showSavePrompt = false;
    bool m_confirmedClose = false;
    bool m_closeAfterSave = false;             // set when user picked Save in the prompt

    // Loft (plugin) prompt: LoftPlugin sets this via PluginContext::request
    // InteractiveOp("LoftPickSecond") when the user clicks Loft with only one
    // sketch selected. We render a modal popup nudging them to Ctrl-click a
    // second sketch. Latched + cleared by the popup.
    bool m_loftPickHintPending = false;
    bool m_loftPickHintVisible = false;  // non-modal banner, dismissed on 2-sketch select or X
    enum class PostSaveAction { None, CloseProject, OpenProject };
    PostSaveAction m_postSaveAction = PostSaveAction::None;
    // When opening a project (dialog or Open Recent) with unsaved changes, the
    // actual open is deferred here and run after the save prompt resolves.
    std::function<void()> m_pendingOpenAction;

    // Settings option: re-open the most recent project on launch (only if it
    // wasn't explicitly closed before quit). The "last open project path" lives
    // in AppSettings::lastProjectPath and is mirrored from m_currentProjectPath
    // on save/load and cleared on closeProject().
    bool m_autoOpenLastProject = false;
    bool m_checkForUpdatesOnLaunch = true;
    // Beta channel opt-in: update checks also consider GitHub pre-releases.
    bool m_includePrereleases = false;

    // Set by the --safe-mode CLI flag. When true, loadAppSettings stomps
    // rendering, autosave, and auto-open-last-project back to safe defaults
    // and persists them.
    bool m_safeMode = false;
};

} // namespace materializr

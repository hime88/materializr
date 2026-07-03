#pragma once
#include <functional>
#include <string>

class SelectionManager;
class History;

namespace materializr {

class PluginContext;

enum class ToolAction {
    None,
    // Sketch tools (still dispatched via ToolAction — tightly coupled to viewport)
    StartSketch, StartSketchXY, StartSketchXZ, StartSketchYZ,
    SketchOnFace, SelectSketch, Line, Circle, Rectangle, Arc, Spline, Polygon, Trim, SketchText,
    SketchSvg,
    FinishSketch, ExitSketchDiscard, EditSketch, ExtrudeSketch, SubtractSketch, PushPull, MoveFace, LookAtSketch,
    SketchCopy, SketchMirror, SketchLinearPattern, SketchRadialPattern,
    // Cycle the drawing-inference level Full → Reduced → Off.
    SketchCycleInference,
    // Toggle the active rect/circle draw origin (Corner↔Center / Center↔2-Point).
    SketchToggleDrawOrigin,
    // 3D tools that still need the old dispatch path. (Face extrude is owned by
    // ExtrudePlugin's toolbar button; the inline interactive extrude is reached
    // from sketch-extrude and the viewport context menu, not via a ToolAction.)
    Fillet, Chamfer, EditFilletChamfer, EditDiameter, Shell, Thread, Taper, ScaleFace,
    ProjectSketch, RemoveFace,
    // Gizmo modes + Mirror
    Move, Rotate, Scale, Mirror, Revolve,
    // Sketch constraints (operate on the current SketchTool element selection).
    // All opt-in — none of them runs unless the user clicks the button.
    SketchConstrainCoincident, SketchConstrainHorizontal, SketchConstrainVertical,
    SketchConstrainParallel, SketchConstrainPerpendicular, SketchConstrainEqual,
    SketchConstrainFixed,
    // Dimension constraints — captured at current geometry value, so adding
    // one is non-destructive. User edits the displayed value later.
    SketchDimDistance, SketchDimAngle, SketchDimRadius,
    // Geometric constraints that need circle/arc selection (Session 4 catalogue).
    SketchConstrainTangent, SketchConstrainConcentric,
    // General
    Measure, ResetCamera,
    // Fabrication: unfold a body into a flat pattern for laser/CNC/cut templates.
    Unfold
};

class Toolbar {
public:
    Toolbar();

    void setSelectionManager(const SelectionManager* sel);
    void setHistory(const ::History* h) { m_history = h; }
    void setPluginContext(PluginContext* ctx) { m_pluginCtx = ctx; }

    ToolAction render();

    void setSketchMode(bool active);
    bool isSketchMode() const;

    void setGridStep(float step) { m_gridStep = step; }
    float getGridStep() const { return m_gridStep; }

    void setCameraOrtho(bool ortho) { m_cameraOrtho = ortho; }

    void setSnapToGrid(bool snap) { m_snapToGrid = snap; }
    bool getSnapToGrid() const { return m_snapToGrid; }

    // HiDPI/touch scale for button heights (1.0 on desktop). Without this the
    // fixed-height buttons stay 30px tall while the font scales up on a tablet,
    // so the labels overlap. bh() scales a base height by it.
    void setUiScale(float s) { m_uiScale = s; }

    // Set each frame by Application from a cheap face/body inspection: shows
    // the "Edit Diameter" button in Face Operations when the picked face is a
    // cylinder on a recognized cylinder-or-tube body.
    void setCanEditDiameter(bool b) { m_canEditDiameter = b; }
    void setSelectedFaceFrozenRound(bool b) { m_selFrozenRound = b; }

    // Active SketchToolMode (int — Toolbar avoids depending on SketchTool.h).
    // Matches SketchToolMode enum: 0=None, 1=Select, 2=Line, 3=Circle,
    // 4=Rectangle, 5=Arc, 6=Spline, 7=Polygon, 8=Trim. Used to draw a
    // highlight border around the matching button so the active tool is
    // unambiguous at a glance.
    void setActiveSketchMode(int mode) { m_activeSketchMode = mode; }

    // Current sketch-element selection counts. Application updates these each
    // frame from SketchTool so the Constraints section of the sketch toolbar
    // can show only the buttons that match the selection arity, and stay
    // hidden entirely when nothing is selected.
    void setSketchSelectionCounts(int points, int lines,
                                  int circles = 0, int arcs = 0) {
        m_selPoints  = points;
        m_selLines   = lines;
        m_selCircles = circles;
        m_selArcs    = arcs;
    }
    // Live inference level shown on the sketch toolbar toggle. Int mirrors
    // SketchTool::InferenceLevel (0=Full, 1=Reduced, 2=Off, 3=Max) to keep Toolbar
    // free of a SketchTool.h dependency.
    void setInferenceLevel(int lvl) { m_inferenceLevel = lvl; }
    // Side count chosen from the Polygon popout; read by the app when it
    // handles the resulting ToolAction::Polygon.
    int  getRequestedPolygonSides() const { return m_requestedPolygonSides; }
    // Live rect/circle draw origin shown on the per-tool toggle. Ints mirror
    // SketchTool::RectMode (0=Corner,1=Center) and CircleMode (0=Center,1=2-Point).
    void setRectMode(int m) { m_rectMode = m; }
    void setCircleMode(int m) { m_circleMode = m; }
    // Off hides the inference cycle button from the sketch toolbar so a user
    // who set the level once in Settings can declutter. Default on.
    void setShowInferenceToggle(bool b) { m_showInferenceToggle = b; }
    // Most recent SketchSolver state: 0 = Fully, 1 = Under, 2 = Over (matches
    // the SketchState enum). Drives the small status badge at the top of the
    // sketch toolbar so the user knows whether the current constraint set is
    // satisfiable / has slack / is impossible. -1 = no status (no constraints).
    void setSketchSolverState(int state) { m_sketchSolverState = state; }
    void setSketchSolverDof(int dof) { m_sketchSolverDof = dof; }

    // When true (the default) every toolbar button shows a hover tooltip
    // describing what it does. Off via Settings → Interface for users who
    // don't want them. Settable any frame; takes effect on the next frame.
    void setShowTooltips(bool b) { m_showTooltips = b; }

private:
    // Scale a base button height by the UI scale (1.0 on desktop).
    float bh(float h) const { return h * m_uiScale; }
    float m_uiScale = 1.0f;

    const SelectionManager* m_selection = nullptr;
    const ::History* m_history = nullptr;
    PluginContext* m_pluginCtx = nullptr;
    bool m_sketchMode = false;
    float m_gridStep = 1.0f;
    bool m_cameraOrtho = true;
    bool m_snapToGrid = true;
    bool m_canEditDiameter = false;
    bool m_selFrozenRound  = false;
    bool m_showTooltips = true;
    int  m_activeSketchMode = 0; // SketchToolMode (see setActiveSketchMode)
    int  m_selPoints = 0;        // sketch points currently selected (see setSketchSelectionCounts)
    int  m_selLines = 0;         // sketch lines currently selected
    int  m_selCircles = 0;       // sketch circles currently selected
    int  m_selArcs = 0;          // sketch arcs currently selected
    int  m_sketchSolverState = -1; // -1=none, 0=Fully, 1=Under, 2=Over
    int  m_sketchSolverDof = 0;
    int  m_inferenceLevel = 0; // 0=Full, 1=Reduced, 2=Off (see setInferenceLevel)
    int  m_requestedPolygonSides = 6; // last pick from the Polygon popout
    int  m_rectMode = 0;       // 0=Corner, 1=Center (see setRectMode)
    int  m_circleMode = 0;     // 0=Center, 1=2-Point (see setCircleMode)
    bool m_showInferenceToggle = true; // see setShowInferenceToggle

    ToolAction renderSketchTools();
    ToolAction renderSketchSelectedTools();
    ToolAction renderPlaneSelectedTools();
    ToolAction renderAxisSelectedTools();
    ToolAction renderSketchRegionTools();
    ToolAction renderNoSelectionTools();
    // includePluginButtons=false suppresses HasBodies plugin contributions
    // (Split / Duplicate / Pattern / etc.) when the body tools are rendered
    // as a fallback under a Face selection — those are whole-body operations
    // that don't make sense while the user is interacting with a face.
    ToolAction renderBodyTools(bool includePluginButtons = true);
    ToolAction renderFaceTools();
    ToolAction renderEdgeTools();

    void renderPluginButtons(int contextMask);
    // Single "Add Plane…" button + dropdown listing the construction-plane
    // creation modes the current selection supports. Shared across the face /
    // plane / edge / axis context renderers; renders nothing when no mode
    // applies.
    // OCCT primitives (Box / Cylinder / Sphere / Cone / Torus) — a single
    // "Primitives..." button that opens a popup with one menu item per kind.
    // Each item fires a requestInteractiveOp the PrimitivesPlugin set up
    // and Application opens the per-kind parameter popup. Keeps the empty-
    // canvas toolbar from sprouting five sibling buttons.
    void renderPrimitivesMenu();
    void renderAddPlaneMenu();
    // Single "Add Axis…" button + dropdown listing the construction-axis
    // creation modes the current selection supports (cylinder centreline,
    // straight edge, two vertices, face normal, two-plane intersection).
    void renderAddAxisMenu();

    void tip(const char* text) const;
};

} // namespace materializr

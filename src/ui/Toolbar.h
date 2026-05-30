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
    SketchOnFace, SelectSketch, Line, Circle, Rectangle, Arc, Spline, Polygon, Trim,
    FinishSketch, ExitSketchDiscard, EditSketch, ExtrudeSketch, SubtractSketch, PushPull, LookAtSketch,
    SketchCopy, SketchMirror, SketchLinearPattern, SketchRadialPattern,
    // 3D tools that still need the old dispatch path. (Face extrude is owned by
    // ExtrudePlugin's toolbar button; the inline interactive extrude is reached
    // from sketch-extrude and the viewport context menu, not via a ToolAction.)
    Fillet, Chamfer, EditFilletChamfer, EditDiameter, Shell,
    // Gizmo modes + Mirror
    Move, Rotate, Scale, Mirror,
    // General
    Measure, ResetCamera
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

    // Set each frame by Application from a cheap face/body inspection: shows
    // the "Edit Diameter" button in Face Operations when the picked face is a
    // cylinder on a recognized cylinder-or-tube body.
    void setCanEditDiameter(bool b) { m_canEditDiameter = b; }

    // Active SketchToolMode (int — Toolbar avoids depending on SketchTool.h).
    // Matches SketchToolMode enum: 0=None, 1=Select, 2=Line, 3=Circle,
    // 4=Rectangle, 5=Arc, 6=Spline, 7=Polygon, 8=Trim. Used to draw a
    // highlight border around the matching button so the active tool is
    // unambiguous at a glance.
    void setActiveSketchMode(int mode) { m_activeSketchMode = mode; }

    // When true (the default) every toolbar button shows a hover tooltip
    // describing what it does. Off via Settings → Interface for users who
    // don't want them. Settable any frame; takes effect on the next frame.
    void setShowTooltips(bool b) { m_showTooltips = b; }

private:
    const SelectionManager* m_selection = nullptr;
    const ::History* m_history = nullptr;
    PluginContext* m_pluginCtx = nullptr;
    bool m_sketchMode = false;
    float m_gridStep = 1.0f;
    bool m_cameraOrtho = true;
    bool m_snapToGrid = true;
    bool m_canEditDiameter = false;
    bool m_showTooltips = true;
    int  m_activeSketchMode = 0; // SketchToolMode (see setActiveSketchMode)

    ToolAction renderSketchTools();
    ToolAction renderSketchSelectedTools();
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

    void tip(const char* text) const;
};

} // namespace materializr

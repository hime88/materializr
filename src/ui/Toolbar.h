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
    FinishSketch, EditSketch, ExtrudeSketch, SubtractSketch, PushPull, LookAtSketch,
    SketchCopy, SketchMirror,
    // 3D tools that still need the old dispatch path. (Face extrude is owned by
    // ExtrudePlugin's toolbar button; the inline interactive extrude is reached
    // from sketch-extrude and the viewport context menu, not via a ToolAction.)
    Fillet, Chamfer, EditFilletChamfer, EditDiameter,
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

private:
    const SelectionManager* m_selection = nullptr;
    const ::History* m_history = nullptr;
    PluginContext* m_pluginCtx = nullptr;
    bool m_sketchMode = false;
    float m_gridStep = 1.0f;
    bool m_cameraOrtho = true;
    bool m_snapToGrid = true;
    bool m_canEditDiameter = false;

    ToolAction renderSketchTools();
    ToolAction renderSketchSelectedTools();
    ToolAction renderSketchRegionTools();
    ToolAction renderNoSelectionTools();
    ToolAction renderBodyTools();
    ToolAction renderFaceTools();
    ToolAction renderEdgeTools();

    void renderPluginButtons(int contextMask);
    void renderGeneralSection();
};

} // namespace materializr

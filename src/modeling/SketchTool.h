#pragma once
#include "Sketch.h"
#include "SketchSolver.h"
#include <glm/glm.hpp>
#include <functional>
#include <set>

namespace materializr {

enum class SketchToolMode { None, Select, Line, Circle, Rectangle, Arc, Spline, Polygon, Trim };

class SketchTool {
public:
    SketchTool();

    void setSketch(Sketch* sketch);
    void setSolver(SketchSolver* solver);

    void setMode(SketchToolMode mode);
    SketchToolMode getMode() const;

    // Input events (in sketch 2D coordinates). `addToSel` is the modifier state
    // for Select-mode multi-pick (Ctrl held); ignored by other tools.
    void onMouseDown(glm::vec2 pos, bool addToSel = false);
    void onMouseMove(glm::vec2 pos);
    void onMouseUp(glm::vec2 pos);

    // --- Sketch-element selection (Select mode) ---
    const std::set<int>& getSelectedPoints() const { return m_selectedPoints; }
    const std::set<int>& getSelectedLines()  const { return m_selectedLines; }
    void clearElementSelection() {
        m_selectedPoints.clear();
        m_selectedLines.clear();
    }
    bool hasElementSelection() const {
        return !m_selectedPoints.empty() || !m_selectedLines.empty();
    }
    // Select every element in the active sketch (used by Ctrl+A / double-click).
    void selectAll();
    // Replace the current selection with the given ids.
    void setSelection(const std::set<int>& pointIds, const std::set<int>& lineIds) {
        m_selectedPoints = pointIds;
        m_selectedLines = lineIds;
    }
    void onConfirm(); // Enter/double-click to finish
    void onCancel();  // Escape to cancel

    // Type a dimension during placement: completes the current shape using `value` as the
    // primary dimension (line length, circle radius, polygon radius, rectangle half-side)
    // anchored at the first click, in the direction of the current cursor.
    // Returns true if the shape was created.
    bool applyDimension(float value);

    // Anchor point of the current placement (the first click). Used by the dimension input UI.
    glm::vec2 getFirstClick() const { return m_firstClick; }
    glm::vec2 getCurrentPos() const { return m_currentPos; }
    glm::vec2 getSecondClick() const { return m_secondClick; }
    int getClickCount() const { return m_clickCount; }
    // Current side count for the polygon tool. Modifiable from the dimension
    // popup without committing the placement, so the user can preview a new
    // count before clicking.
    int getPolygonSides() const { return m_polygonSides; }
    void setPolygonSides(int n) { m_polygonSides = (n < 3) ? 3 : n; }

    // True while the tool has an in-progress placement (first click made,
    // second pending) — used by the host to give Escape two-step semantics:
    // first Esc cancels just the in-progress shape, second Esc exits the
    // sketch mode entirely.
    bool isPlacing() const { return m_isPlacing; }

    // Grid step (in sketch-plane mm). Used for both visual grid and snap-to-line.
    // 0 disables grid snap entirely.
    void setGridStep(float step) { m_gridStep = step; }
    float getGridStep() const { return m_gridStep; }

    // Current state for rendering preview
    bool hasPreview() const;
    glm::vec2 getPreviewStart() const;
    glm::vec2 getPreviewEnd() const;
    SketchToolMode getPreviewType() const;

    // Trim hover: densified 2D points outlining the segment that would be
    // removed on the next click. Empty when nothing is hovered in Trim mode.
    const std::vector<glm::vec2>& getTrimHoverPoints() const { return m_trimHoverPoints; }

    // Is the tool actively placing something?
    bool isActive() const;

private:
    SketchToolMode m_mode = SketchToolMode::None;
    Sketch* m_sketch = nullptr;
    SketchSolver* m_solver = nullptr;

    // State for multi-click tools
    bool m_isPlacing = false;
    int m_clickCount = 0;
    glm::vec2 m_firstClick{0};
    glm::vec2 m_secondClick{0};
    glm::vec2 m_currentPos{0};

    // For line chaining
    int m_lastPointId = -1;

    // Snap to grid/points
    glm::vec2 snap(glm::vec2 pos) const;

    // Auto-constrain a newly created line (horizontal/vertical/coincident)
    void autoConstrain(int lineId);

    // Find an existing point near the given position (returns -1 if none)
    int findCoincidentPoint(glm::vec2 pos, int excludeId = -1) const;

    void handleLineTool(glm::vec2 pos);
    void handleCircleTool(glm::vec2 pos);
    void handleRectangleTool(glm::vec2 pos);
    void handleArcTool(glm::vec2 pos);
    void handleSelectTool(glm::vec2 pos);
    void handleSplineTool(glm::vec2 pos);
    void handlePolygonTool(glm::vec2 pos);
    void handleTrimTool(glm::vec2 pos);
    void computeTrimHover(glm::vec2 pos); // updates m_trimHoverPoints (no mutation)

    // Select/drag state
    int m_dragPointId = -1;
    bool m_isDragging = false;
    bool m_lastDownAddedToSel = false; // Ctrl state for the current click
    std::set<int> m_selectedPoints;
    std::set<int> m_selectedLines;

    std::vector<int> m_splinePoints; // temp storage during spline creation
    int m_polygonSides = 6; // default hexagon

    float m_gridStep = 1.0f; // default 1 mm grid

    // Updated each frame in Trim mode so the renderer can outline the segment
    // that would be deleted on click.
    std::vector<glm::vec2> m_trimHoverPoints;
};

} // namespace materializr

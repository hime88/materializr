#include "UiTheme.h"
#include "Toolbar.h"
#include "../core/SelectionManager.h"
#include "../core/History.h"
#include "../core/Operation.h"
#include "../plugin/PluginRegistry.h"
#include "../plugin/PluginContext.h"
#include "../plugin/Contributions.h"
#include <imgui.h>
#include <cmath>
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <Geom_Surface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopoDS.hxx>

namespace materializr {

Toolbar::Toolbar() = default;

// Tooltip helper. Wraps long descriptions across multiple lines instead of
// the single-line behaviour ImGui::SetItemTooltip gives by default — tooltip
// strings can run to a couple of sentences and used to truncate awkwardly.
// BeginItemTooltip handles the hover-delay; PushTextWrapPos gives us the
// width cap (in pixels, roughly 28em at the current font size).
void Toolbar::tip(const char* text) const {
    if (!m_showTooltips) return;
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void Toolbar::setSelectionManager(const SelectionManager* sel) {
    m_selection = sel;
}

ToolAction Toolbar::render() {
    ToolAction action = ToolAction::None;

    ImGui::Begin("Tools", nullptr, ImGuiWindowFlags_NoCollapse);

    if (m_sketchMode) {
        action = renderSketchTools();
    } else if (!m_selection || !m_selection->hasSelection()) {
        action = renderNoSelectionTools();
    } else if (m_selection->hasSelectedSketchRegions()) {
        action = renderSketchRegionTools();
    } else if (m_selection->primaryType() == SelectionType::Plane) {
        action = renderPlaneSelectedTools();
    } else if (m_selection->primaryType() == SelectionType::Axis) {
        action = renderAxisSelectedTools();
    } else if (m_selection->hasSelectedSketches()) {
        action = renderSketchSelectedTools();
    } else if (m_selection->hasSelectedFaces()) {
        action = renderFaceTools();
        if (action == ToolAction::None) {
            // Body tools (gizmos + Mirror) stay available when a face is
            // selected so the user can move/rotate/scale the whole body, but
            // the whole-body plugin contributions (Split / Duplicate / Pattern)
            // are skipped — they don't apply in face-selection context.
            action = renderBodyTools(/*includePluginButtons=*/false);
        }
    } else if (m_selection->hasSelectedBodies()) {
        action = renderBodyTools();
    } else if (m_selection->hasSelectedEdges()) {
        action = renderEdgeTools();
    } else {
        action = renderNoSelectionTools();
    }

    ImGui::End();
    return action;
}

void Toolbar::setSketchMode(bool active) {
    m_sketchMode = active;
}

bool Toolbar::isSketchMode() const {
    return m_sketchMode;
}

// Render plugin-contributed buttons matching any of the given contexts.
// contextMask is a bitmask: bit N = SelectionContext(N).
void Toolbar::renderPluginButtons(int contextMask) {
    if (!m_pluginCtx) return;
    auto& contribs = PluginRegistry::instance().toolbarContributions();
    std::string lastSection;
    for (size_t i = 0; i < contribs.size(); ++i) {
        auto& c = contribs[i];
        if (!((1 << static_cast<int>(c.context)) & contextMask)) continue;
        if (c.section != lastSection) {
            if (!lastSection.empty()) ImGui::Separator();
            ImGui::TextColored(materializr::accentText(), "%s", c.section.c_str());
            ImGui::Separator();
            lastSection = c.section;
        }
        ImGui::PushID(static_cast<int>(i + 10000));
        if (ImGui::Button(c.name.c_str(), ImVec2(-1, bh(30)))) {
            if (c.toolFactory) {
                PluginRegistry::instance().activateTool(c.toolFactory(), *m_pluginCtx);
            } else if (c.action) {
                c.action(*m_pluginCtx);
            }
        }
        if (!c.tooltip.empty()) tip(c.tooltip.c_str());
        ImGui::PopID();
    }
}

void Toolbar::renderPrimitivesMenu() {
    if (!m_pluginCtx) return;
    if (ImGui::Button("Primitives...", ImVec2(-1, bh(30))))
        ImGui::OpenPopup("PrimitivesMenu");
    tip("Create a stock OCCT primitive (box / cylinder / sphere / cone / "
        "torus). Picking one opens its parameter popup — defaults land a "
        "10 mm / R5 mm shape at the world origin.");
    if (ImGui::BeginPopup("PrimitivesMenu")) {
        if (ImGui::MenuItem("Box"))
            m_pluginCtx->requestInteractiveOp("PrimitiveBox");
        if (ImGui::MenuItem("Cylinder"))
            m_pluginCtx->requestInteractiveOp("PrimitiveCylinder");
        if (ImGui::MenuItem("Sphere"))
            m_pluginCtx->requestInteractiveOp("PrimitiveSphere");
        if (ImGui::MenuItem("Cone"))
            m_pluginCtx->requestInteractiveOp("PrimitiveCone");
        if (ImGui::MenuItem("Torus"))
            m_pluginCtx->requestInteractiveOp("PrimitiveTorus");
        ImGui::EndPopup();
    }
}

void Toolbar::renderAddPlaneMenu() {
    if (!m_selection || !m_pluginCtx) return;

    // Detect which construction-plane modes the current selection supports.
    int  planarFaces = 0, planeCount = 0;
    bool haveCyl = false, straightEdge = false, haveAxis = false;
    for (const auto& e : m_selection->getSelection()) {
        if (e.type == SelectionType::Plane) { ++planeCount; continue; }
        if (e.type == SelectionType::Axis)  { haveAxis = true; continue; }
        if (e.shape.IsNull()) continue;
        try {
            if (e.type == SelectionType::Face) {
                Handle(Geom_Surface) s = BRep_Tool::Surface(TopoDS::Face(e.shape));
                if (!s.IsNull()) {
                    if (s->IsKind(STANDARD_TYPE(Geom_Plane))) ++planarFaces;
                    else if (!Handle(Geom_CylindricalSurface)::DownCast(s).IsNull())
                        haveCyl = true;
                }
            } else if (e.type == SelectionType::Edge) {
                BRepAdaptor_Curve ad(TopoDS::Edge(e.shape));
                if (ad.GetType() == GeomAbs_Line) straightEdge = true;
            }
        } catch (...) {}
    }

    const bool midplane = (planarFaces >= 2) || (planeCount >= 2);
    if (!(midplane || haveCyl || haveAxis || straightEdge)) return;

    ImGui::Separator();
    if (ImGui::Button("Add Plane...", ImVec2(-1, bh(30))))
        ImGui::OpenPopup("AddPlaneMenu");
    tip("Create a construction plane derived from the current selection.");
    if (ImGui::BeginPopup("AddPlaneMenu")) {
        if (midplane && ImGui::MenuItem("Midplane (between the 2 selected)"))
            m_pluginCtx->requestInteractiveOp("Midplane");
        if (haveCyl) {
            if (ImGui::MenuItem("Tangent to cylinder"))
                m_pluginCtx->requestInteractiveOp("TangentPlane");
            if (ImGui::MenuItem("Perpendicular to cylinder axis"))
                m_pluginCtx->requestInteractiveOp("PlaneNormalToAxis");
            if (ImGui::MenuItem("Through cylinder axis (longitudinal)"))
                m_pluginCtx->requestInteractiveOp("PlaneThroughAxis");
        } else if (haveAxis || straightEdge) {
            if (ImGui::MenuItem(straightEdge ? "Normal to edge" : "Normal to axis"))
                m_pluginCtx->requestInteractiveOp("PlaneNormalToAxis");
        }
        ImGui::EndPopup();
    }
}

void Toolbar::renderAddAxisMenu() {
    if (!m_selection || !m_pluginCtx) return;

    int  planarFaces = 0, planeCount = 0, vertexCount = 0;
    bool haveCyl = false, straightEdge = false;
    for (const auto& e : m_selection->getSelection()) {
        if (e.type == SelectionType::Plane)  { ++planeCount;  continue; }
        if (e.type == SelectionType::Vertex) { ++vertexCount; continue; }
        if (e.shape.IsNull()) continue;
        try {
            if (e.type == SelectionType::Face) {
                Handle(Geom_Surface) s = BRep_Tool::Surface(TopoDS::Face(e.shape));
                if (!s.IsNull()) {
                    if (s->IsKind(STANDARD_TYPE(Geom_Plane))) ++planarFaces;
                    else if (!Handle(Geom_CylindricalSurface)::DownCast(s).IsNull())
                        haveCyl = true;
                }
            } else if (e.type == SelectionType::Edge) {
                BRepAdaptor_Curve ad(TopoDS::Edge(e.shape));
                if (ad.GetType() == GeomAbs_Line) straightEdge = true;
            }
        } catch (...) {}
    }

    const bool twoPlanes  = (planeCount >= 2) || (planarFaces >= 2);
    const bool twoVerts   = (vertexCount >= 2);
    const bool faceNormal = (planarFaces >= 1);
    if (!(haveCyl || straightEdge || twoVerts || faceNormal || twoPlanes)) return;

    ImGui::Separator();
    if (ImGui::Button("Add Axis...", ImVec2(-1, bh(30))))
        ImGui::OpenPopup("AddAxisMenu");
    tip("Create a construction axis derived from the current selection.");
    if (ImGui::BeginPopup("AddAxisMenu")) {
        if (haveCyl && ImGui::MenuItem("From cylinder axis"))
            m_pluginCtx->requestInteractiveOp("AxisFromCylinder");
        if (straightEdge && ImGui::MenuItem("Along edge"))
            m_pluginCtx->requestInteractiveOp("AxisAlongEdge");
        if (twoVerts && ImGui::MenuItem("Through two vertices"))
            m_pluginCtx->requestInteractiveOp("AxisTwoPoints");
        if (faceNormal && ImGui::MenuItem("Normal to face"))
            m_pluginCtx->requestInteractiveOp("AxisNormalToFace");
        if (twoPlanes && ImGui::MenuItem("Intersection of two planes"))
            m_pluginCtx->requestInteractiveOp("AxisTwoPlanes");
        ImGui::EndPopup();
    }
}

ToolAction Toolbar::renderSketchTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(materializr::accentText(), "Sketch Tools");
    // Constraint status badge — only appears once the sketch has constraints.
    // Green = Fully constrained, blue = Under (free DOF), red = Over
    // (contradictory). Hover shows the precise degree-of-freedom count.
    if (m_sketchSolverState >= 0) {
        ImVec4 col;
        const char* label = "";
        switch (m_sketchSolverState) {
            case 0: col = ImVec4(0.20f, 0.85f, 0.35f, 1.0f); label = "Fully constrained"; break;
            case 1: col = ImVec4(0.30f, 0.65f, 1.00f, 1.0f); label = "Under-constrained"; break;
            case 2: col = ImVec4(0.95f, 0.30f, 0.30f, 1.0f); label = "Over-constrained";   break;
            default: col = ImVec4(0.7f,0.7f,0.7f,1.0f);      label = "";                    break;
        }
        ImGui::TextColored(col, "● %s", label);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Degrees of freedom: %d\n"
                              "Negative = contradictory constraints, "
                              "zero = uniquely determined, "
                              "positive = free to drag.",
                              m_sketchSolverDof);
        }
    }
    ImGui::Separator();

    // Snap on/off + step both live in the corner widget next to the ViewCube
    // now — single source of truth. The duplicate grid-step row used to sit
    // here but was removed once the corner widget proved sufficient.

    // Render a sketch-tool button with a thick light-grey border when it's
    // the currently active mode. Caller checks the return value to set
    // `action`. Mode id matches SketchToolMode enum (see Toolbar.h).
    auto skBtn = [&](const char* label, int modeId) -> bool {
        bool active = (m_activeSketchMode == modeId);
        if (active) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        }
        bool clicked = ImGui::Button(label, ImVec2(-1, bh(30)));
        if (active) {
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }
        return clicked;
    };

    // Draw-origin toggle, rendered directly beneath whichever of Circle /
    // Rectangle is active so it reads as part of that tool (not stranded at the
    // bottom of the list).
    auto drawOriginToggle = [&](bool isRect) {
        const char* label = isRect
            ? (m_rectMode == 0 ? "Draw from: Corner" : "Draw from: Center")
            : (m_circleMode == 0 ? "Draw from: Center" : "Draw from: 2-Point");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.38f, 0.55f, 1.0f));
        if (ImGui::Button(label, ImVec2(-1, bh(22))))
            action = ToolAction::SketchToggleDrawOrigin;
        ImGui::PopStyleColor();
        tip(isRect
            ? "Rectangle origin: Corner = click a corner, drag to the opposite; "
              "Center = click the centre, drag to a corner. Click to toggle."
            : "Circle origin: Center = click the centre, drag the radius; "
              "2-Point = the two clicks are opposite ends of the diameter "
              "(rim passes through the first click). Click to toggle.");
    };

    if (skBtn("Select / Move", 1)) action = ToolAction::SelectSketch;
    tip("Pick sketch elements (points, lines, regions). Drag selection to move.");
    if (skBtn("Line",      2))     action = ToolAction::Line;
    tip("Draw straight line segments. Click to add vertices, Esc to finish.");
    if (skBtn("Circle",    3))     action = ToolAction::Circle;
    tip("Draw a circle: click centre, drag to radius.");
    if (m_activeSketchMode == 3)   drawOriginToggle(false);
    if (skBtn("Rectangle", 4))     action = ToolAction::Rectangle;
    tip("Draw an axis-aligned rectangle: click one corner, drag to the opposite.");
    if (m_activeSketchMode == 4)   drawOriginToggle(true);
    if (skBtn("Arc",       5))     action = ToolAction::Arc;
    tip("Three-point arc: click start, end, then a point on the curve.");
    if (skBtn("Spline",    6))     action = ToolAction::Spline;
    tip("Multi-point spline. Click control points, Enter to finish.");
    // Polygon: a popout to pick the regular-polygon side count by name (like
    // the Primitives menu), instead of typing it into a dialog. The chosen
    // count sets the tool's sides and activates polygon placement.
    {
        bool active = (m_activeSketchMode == 7);
        if (active) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        }
        if (ImGui::Button("Polygon", ImVec2(-1, bh(30))))
            ImGui::OpenPopup("PolygonSidesMenu");
        if (active) { ImGui::PopStyleColor(); ImGui::PopStyleVar(); }
        tip("Regular polygon: pick the number of sides, then click the centre "
            "and drag for size / rotation.");
        if (ImGui::BeginPopup("PolygonSidesMenu")) {
            struct PolyChoice { const char* name; int sides; };
            static const PolyChoice choices[] = {
                {"Triangle (3)", 3}, {"Square (4)", 4}, {"Pentagon (5)", 5},
                {"Hexagon (6)", 6}, {"Heptagon (7)", 7}, {"Octagon (8)", 8}};
            for (const auto& ch : choices)
                if (ImGui::MenuItem(ch.name)) {
                    m_requestedPolygonSides = ch.sides;
                    action = ToolAction::Polygon;
                }
            ImGui::EndPopup();
        }
    }
    tip("Regular polygon: click centre, drag to size. Side count in properties.");
    if (skBtn("Text",      9))     action = ToolAction::SketchText;
    tip("Insert text as real outline geometry: set string, font and letter "
        "height in the popup, then click to place. Letters become closed "
        "regions - extrude them or engrave them onto a face.");
    if (skBtn("Import SVG", 10))   action = ToolAction::SketchSvg;
    tip("Import an SVG file as sketch outlines: pick the file, set the "
        "width in the popup, click to place. Paths become closed regions - "
        "extrude a logo or engrave it onto a face.");
    if (skBtn("Trim",      8))     action = ToolAction::Trim;
    tip("Trim a sketch segment at the nearest intersections.");

    // Transforms operate on the current sketch-element selection (Select tool +
    // click/Ctrl+click on points and lines). No-op if nothing is selected.
    // Rotate is the gizmo's ring handle (drag = 15° snap, popup for exact value),
    // so it doesn't get its own button.
    ImGui::Separator();
    if (ImGui::Button("Copy",   ImVec2(-1, bh(28)))) action = ToolAction::SketchCopy;
    tip("Duplicate the selected sketch elements at an offset.");
    if (ImGui::Button("Mirror", ImVec2(-1, bh(28)))) action = ToolAction::SketchMirror;
    tip("Mirror selected elements across a sketch line you'll pick next.");
    if (ImGui::Button("Linear Pattern", ImVec2(-1, bh(28)))) action = ToolAction::SketchLinearPattern;
    tip("Copy the selected sketch elements N times along the sketch X axis.");
    if (ImGui::Button("Circular Pattern", ImVec2(-1, bh(28)))) action = ToolAction::SketchRadialPattern;
    tip("Copy the selected sketch elements around an origin you specify.");

    // Drawing-inference level — a live Full → Reduced → Off toggle. Lets the
    // user calm the ghost guides (and the hover-charged references) in a busy
    // area without leaving the sketch. Constraints now live exclusively on the
    // sketch-viewport right-click "Add Constraint" menu. Hidden when the user
    // has set the level once in Settings and prefers not to see the live
    // toggle (Settings → Sketch → "Show level toggle in sketch toolbar").
    if (m_showInferenceToggle) {
        ImGui::Separator();
        const char* lbl = m_inferenceLevel == 0 ? "Inferences: Full"
                        : m_inferenceLevel == 1 ? "Inferences: Reduced"
                        : m_inferenceLevel == 2 ? "Inferences: Off"
                                                : "Inferences: Max";
        ImVec4 col = m_inferenceLevel == 0 ? ImVec4(0.20f, 0.45f, 0.65f, 1.0f)
                   : m_inferenceLevel == 1 ? ImVec4(0.60f, 0.42f, 0.15f, 1.0f)
                   : m_inferenceLevel == 2 ? ImVec4(0.34f, 0.34f, 0.34f, 1.0f)
                                           : ImVec4(0.16f, 0.52f, 0.48f, 1.0f); // Max = teal (strongest)
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        if (ImGui::Button(lbl, ImVec2(-1, bh(26)))) action = ToolAction::SketchCycleInference;
        ImGui::PopStyleColor();
        tip("How many drawing guides to show, and how eagerly they snap. "
            "Max = Full plus wider catch ranges tuned for fingertips (touch). "
            "Full = the classic guides PLUS hover-to-charge references (dwell on "
            "a point to align from it). Reduced = the classic guides only, no "
            "hover-charging. Off = grid + endpoint only. Click to cycle.");
    }

    ImGui::Separator();
    if (ImGui::Button("Measure", ImVec2(-1, bh(28)))) action = ToolAction::Measure;
    tip("Measure distance / length between picked sketch elements.");

    if (!m_cameraOrtho) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.85f, 1.0f));
        if (ImGui::Button("Look at Sketch", ImVec2(-1, bh(30))))
            action = ToolAction::LookAtSketch;
        tip("Snap the camera to look straight down the sketch plane (orthographic).");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    if (ImGui::Button("Finish Sketch", ImVec2(-1, bh(30))))
        action = ToolAction::FinishSketch;
    tip("Leave sketch mode and return to the 3D viewport. Keeps the sketch.");
    if (ImGui::Button("Exit Sketch (discard)", ImVec2(-1, bh(30))))
        action = ToolAction::ExitSketchDiscard;
    tip("Discard the current sketch entirely and leave sketch mode. Rewinds "
        "history to before the sketch was entered; the body returns to its "
        "pre-sketch state. Useful when you've started a sketch you don't "
        "want to keep — Esc-while-placing only cancels the in-progress shape, "
        "this clears everything.");

    // Plugin buttons for InSketchMode context
    renderPluginButtons(1 << static_cast<int>(SelectionContext::InSketchMode));

    return action;
}

ToolAction Toolbar::renderNoSelectionTools() {
    ToolAction action = ToolAction::None;

    // Start a sketch on a base plane — lets you model from scratch with no body.
    ImGui::TextColored(materializr::accentText(), "Create");
    ImGui::Separator();
    if (ImGui::Button("Sketch on XY", ImVec2(-1, bh(30)))) action = ToolAction::StartSketchXY;
    tip("Start a new sketch on the world XY (floor) plane.");
    if (ImGui::Button("Sketch on XZ", ImVec2(-1, bh(30)))) action = ToolAction::StartSketchXZ;
    tip("Start a new sketch on the world XZ (front) plane.");
    if (ImGui::Button("Sketch on YZ", ImVec2(-1, bh(30)))) action = ToolAction::StartSketchYZ;
    tip("Start a new sketch on the world YZ (side) plane.");

    // OCCT primitives (Box / Cylinder / Sphere / Cone / Torus) under a
    // single fold-out button so the empty-canvas toolbar stays compact.
    // Each menu item fires a requestInteractiveOp the PrimitivesPlugin
    // wired up; Application opens the per-kind parameter popup.
    // (Steve: "Primitives button, pop-out side menu, then continue as
    //  normal — keeps the Create section uncluttered".)
    renderPrimitivesMenu();

    // Axis from a vertex selection (two vertices → through-points axis). This
    // is the fallback context vertices land in; renders nothing otherwise.
    renderAddAxisMenu();

    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "Inspect");
    ImGui::Separator();
    if (ImGui::Button("Measure", ImVec2(-1, bh(30)))) action = ToolAction::Measure;
    tip("Measure distance, length, or angle between picked features.");

    // Plugin buttons: NoSelection + Always
    int mask = (1 << static_cast<int>(SelectionContext::NoSelection))
             | (1 << static_cast<int>(SelectionContext::Always));
    renderPluginButtons(mask);

    return action;
}

ToolAction Toolbar::renderBodyTools(bool includePluginButtons) {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(materializr::accentText(), "Transform");
    ImGui::Separator();

    // Gizmo modes side by side, then Mirror.
    float third = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    if (ImGui::Button("Move", ImVec2(third, bh(30))))   action = ToolAction::Move;
    tip("Show the translate gizmo. Drag axes / planes to move. (W)");
    ImGui::SameLine();
    if (ImGui::Button("Rotate", ImVec2(third, bh(30)))) action = ToolAction::Rotate;
    tip("Show the rotate gizmo. Drag rings to rotate around each axis. (E)");
    ImGui::SameLine();
    if (ImGui::Button("Scale", ImVec2(third, bh(30))))  action = ToolAction::Scale;
    tip("Show the scale gizmo. Drag handles to resize. (R)");
    // Mirror + Revolve share the row so they read as the "uses an
    // already-created primitive" pair (mirror plane / construction axis).
    float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Mirror", ImVec2(half, bh(30))))    action = ToolAction::Mirror;
    tip("Mirror the selected bodies across a plane you pick next.");
    ImGui::SameLine();
    // Context-sensitive: a selected sketch lathes (spin its profile into a
    // solid); otherwise the same button revolves the selected body around an
    // axis (a fan, a hinge). beginRevolve() picks the matching mode from the
    // selection, so both share one action.
    bool sketchSel = m_selection && m_selection->hasSelectedSketches();
    if (ImGui::Button(sketchSel ? "Lathe" : "Revolve", ImVec2(half, bh(30))))
        action = ToolAction::Revolve;
    if (sketchSel)
        tip("Lathe: spin the selected sketch's profile around a Construction "
            "Axis into a new solid. Pick the axis next.");
    else
        tip("Revolve the selected body/bodies around a Construction Axis (a fan, "
            "a hinge). Pick the axis next; multi-body selection rotates as a group.");

    // Plugin buttons: always include HasBodies (1+ bodies), and only include
    // MultipleBodies (2+ bodies, e.g. Union / Subtract / Intersect) when at
    // least two bodies are actually selected. Previously OR-ing them both
    // unconditionally meant boolean ops appeared with a single body picked,
    // which can't do anything.
    if (includePluginButtons) {
        int mask = (1 << static_cast<int>(SelectionContext::HasBodies));
        if (m_selection && m_selection->selectedBodyCount() >= 2) {
            mask |= (1 << static_cast<int>(SelectionContext::MultipleBodies));
        }
        renderPluginButtons(mask);
    }

    // Fabrication: flatten the selected body into a 2D pattern (laser / CNC /
    // cut-out templates). Shown for a single selected body.
    if (m_selection && m_selection->selectedBodyCount() == 1) {
        ImGui::Spacing();
        ImGui::TextColored(materializr::accentText(), "Fabrication");
        ImGui::Separator();
        if (ImGui::Button("Unfold / Flatten", ImVec2(-1, bh(30))))
            action = ToolAction::Unfold;
        tip("Lay the body flat into a 2D pattern (cut + fold lines) for a laser "
            "cutter, CNC, or printed template. Mark it as foam board / sheet "
            "metal / wood to set how folds are processed.");
    }

    return action;
}

ToolAction Toolbar::renderFaceTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(materializr::accentText(), "Face Operations");
    ImGui::Separator();

    if (ImGui::Button("Sketch on Face", ImVec2(-1, bh(30))))
        action = ToolAction::SketchOnFace;
    tip("Start a new sketch lying on the picked face.");
    if (ImGui::Button("Push / Pull", ImVec2(-1, bh(30))))
        action = ToolAction::PushPull;
    tip("Drag the face along its normal to extrude (+) or cut (−) into the body.");
    // (Move Face / Taper / Scale Face moved onto the Transform buttons —
    // with a face selected, Move = slide, Rotate = tilt, Scale = scale face.)
    // Extrude From a face → make a new body that's the face's silhouette
    // swept along its normal. Push/Pull modifies the source body; Extrude
    // always creates a separate body. Same ToolAction the sketch toolbar
    // uses; the handler dispatches by selection type.
    if (ImGui::Button("Extrude From", ImVec2(-1, bh(30))))
        action = ToolAction::ExtrudeSketch;
    tip("Make a NEW body by extruding this face's silhouette (source body unchanged).");
    if (ImGui::Button("Shell", ImVec2(-1, bh(30))))
        action = ToolAction::Shell;
    tip("Hollow the body, removing the picked face. Wall thickness in the popup.");
    if (ImGui::Button("Repair Geometry", ImVec2(-1, bh(30))))
        action = ToolAction::RemoveFace;
    tip("Delete the picked face(s) and heal the surrounding faces back together "
        "— take a baked fillet/chamfer back to a sharp edge so it can be "
        "re-applied, or clean a round/hole off an imported part.");
    if (ImGui::Button("Projection", ImVec2(-1, bh(30))))
        action = ToolAction::ProjectSketch;
    tip("Project a sketch onto this face along the sketch's normal, then "
        "engrave (cut in) or emboss (raise out) to a depth - wrap a logo "
        "or text onto a cylinder. Sketch, mode and depth in the popup; "
        "click sketch regions in the viewport to project only those.");
    if (m_canEditDiameter &&
        ImGui::Button("Edit Diameter", ImVec2(-1, bh(30))))
        action = ToolAction::EditDiameter;
    tip("Resize a cylindrical hole / pin to an exact diameter.");
    if (m_canEditDiameter &&
        ImGui::Button("Thread", ImVec2(-1, bh(30))))
        action = ToolAction::Thread;
    tip("Cut a helical screw thread into the picked cylindrical face — "
        "external on a boss, internal in a hole. Pitch / depth / handedness "
        "in the popup.");

    if (ImGui::Button("Unfold Faces", ImVec2(-1, bh(30))))
        action = ToolAction::Unfold;
    tip("Flatten the SELECTED faces into a 2D pattern (cut + fold lines) for a "
        "laser/CNC/printed template. Pick the faces of one panel (e.g. a skin) "
        "— unfolding a whole closed body rarely makes sense.");

    // "Edit Fillet / Chamfer" appears only when the picked face was actually
    // produced by a fillet or chamfer op. We ask each Operation via
    // ownsFace() — the same hook the History panel uses elsewhere.
    if (m_selection && m_history) {
        TopoDS_Shape pickedFace;
        for (const auto& e : m_selection->getSelection()) {
            if (e.type == SelectionType::Face && !e.shape.IsNull()) {
                pickedFace = e.shape; break;
            }
        }
        if (!pickedFace.IsNull()) {
            const auto& ops = m_history->operations();
            for (const auto& op : ops) {
                if (op && op->isEnabled() && op->ownsFace(pickedFace)) {
                    const char* label = (op->typeId() == "fillet")
                                            ? "Edit Fillet"
                                            : (op->typeId() == "chamfer")
                                                  ? "Edit Chamfer"
                                                  : nullptr;
                    if (label && ImGui::Button(label, ImVec2(-1, bh(30))))
                        action = ToolAction::EditFilletChamfer;
                    tip(op->typeId() == "fillet"
                            ? "Change this fillet's radius without re-picking edges."
                            : "Change this chamfer's distance without re-picking edges.");
                    break;
                }
            }
        }
    }

    // Frozen-round hint: a fillet-shaped face with no editable op behind it
    // (an older save's baked geometry). "Edit Fillet" can't appear for it, so
    // point the user at Repair Geometry — restore the edge, then re-fillet.
    if (m_selFrozenRound) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
        ImGui::TextColored(materializr::dimText(),
            "This round is frozen (saved before edit support). Use Repair "
            "Geometry above to restore the sharp edge, then re-fillet.");
        ImGui::PopTextWrapPos();
    }

    // Construction-plane / -axis creation from the selected face(s).
    renderAddPlaneMenu();
    renderAddAxisMenu();

    // Plugin buttons for HasFaces context
    renderPluginButtons(1 << static_cast<int>(SelectionContext::HasFaces));

    return action;
}

ToolAction Toolbar::renderSketchSelectedTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(materializr::accentText(), "Sketch");
    ImGui::Separator();
    ImGui::TextWrapped("Tip: hover a sketch region to highlight it, click to select, Ctrl+click to add to selection.");
    ImGui::Separator();

    if (ImGui::Button("Edit Sketch", ImVec2(-1, bh(30))))
        action = ToolAction::EditSketch;
    tip("Re-enter sketch mode to revise this sketch's geometry.");
    if (ImGui::Button("Extrude From", ImVec2(-1, bh(30))))
        action = ToolAction::ExtrudeSketch;
    tip("Make a new body by extruding the sketch's closed regions. "
        "Whole-sketch extrude assumes ONE outer boundary - for multi-shape "
        "sketches (SVG imports, text), click individual regions in the "
        "viewport (Ctrl+click for several) and extrude those instead.");
    if (ImGui::Button("Subtract Sketch", ImVec2(-1, bh(30))))
        action = ToolAction::SubtractSketch;
    tip("Cut the extruded regions out of the body the sketch was drawn on.");
    ImGui::TextWrapped("Subtract cuts the extruded profile into the body the "
                       "sketch was drawn on (preview shown in red).");

    // Move / Rotate gizmo modes — appear here so a selected sketch behaves
    // like a movable construction plane. Bodies have these in renderBodyTools;
    // sketches need their own entry point. The Transform header matches the
    // "Sketch" / "Loft" section-label convention so the toolbar reads as a
    // sequence of clearly-titled groups.
    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "Transform");
    ImGui::Separator();
    if (ImGui::Button("Move", ImVec2(-1, bh(30))))
        action = ToolAction::Move;
    tip("Show the Move gizmo on the selected sketch. Drag an axis to reposition "
        "the sketch in 3D - its geometry rides along, so this effectively turns "
        "the sketch into a movable construction plane. Only available outside "
        "ortho view and sketch-edit mode.");
    if (ImGui::Button("Rotate", ImVec2(-1, bh(30))))
        action = ToolAction::Rotate;
    tip("Show the Rotate gizmo on the selected sketch. Drag a ring to spin the "
        "sketch around its centroid.");

    // Plugin buttons for HasSketches context
    renderPluginButtons(1 << static_cast<int>(SelectionContext::HasSketches));

    return action;
}

ToolAction Toolbar::renderPlaneSelectedTools() {
    ToolAction action = ToolAction::None;
    ImGui::TextColored(materializr::accentText(), "Construction Plane");
    ImGui::Separator();
    if (ImGui::Button("Sketch on this Plane", ImVec2(-1, bh(30))))
        action = ToolAction::SketchOnFace; // dispatched on Plane in handler
    tip("Start a new sketch lying on this construction plane — same workflow as "
        "Sketch on Face, just with the plane as the host.");

    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "Transform");
    ImGui::Separator();
    if (ImGui::Button("Move", ImVec2(-1, bh(30))))   action = ToolAction::Move;
    tip("Show the Move gizmo on this plane. Drag an axis arrow to nudge it; "
        "the live readout pinned to the cursor shows the offset along the "
        "plane's own normal.");
    if (ImGui::Button("Rotate", ImVec2(-1, bh(30)))) action = ToolAction::Rotate;
    tip("Show the Rotate gizmo. Drag a ring to spin the plane around its "
        "origin; snap is 5° increments when snap-to-grid is on.");

    // Midplane between two selected construction planes; axis from their
    // intersection.
    renderAddPlaneMenu();
    renderAddAxisMenu();
    return action;
}

ToolAction Toolbar::renderAxisSelectedTools() {
    ToolAction action = ToolAction::None;
    ImGui::TextColored(materializr::accentText(), "Construction Axis");
    ImGui::Separator();
    ImGui::TextWrapped("Axes are 1-D primitives — they'll feed Revolve and "
                       "future Pattern-Around-Axis ops. For now you can "
                       "move them; rotate isn't meaningful on a line.");

    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "Transform");
    ImGui::Separator();
    if (ImGui::Button("Move", ImVec2(-1, bh(30)))) action = ToolAction::Move;
    tip("Show the Move gizmo on this axis. Drag an arrow to translate "
        "the axis origin; the direction is preserved.");

    renderAddPlaneMenu();
    return action;
}

ToolAction Toolbar::renderSketchRegionTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(materializr::accentText(), "Region");
    ImGui::Separator();
    int n = m_selection ? m_selection->selectedSketchRegionCount() : 0;
    ImGui::Text("%d region%s selected", n, n == 1 ? "" : "s");
    ImGui::Spacing();

    // Push/Pull routes through the app's interactive arrow gizmo (default 0,
    // drag to extrude/cut) — same as a body face.
    if (ImGui::Button("Push / Pull", ImVec2(-1, bh(30))))
        action = ToolAction::PushPull;
    tip("Drag the arrow to extrude this region into a body, or cut it into the parent.");

    if (ImGui::Button("Extrude From", ImVec2(-1, bh(30))))
        action = ToolAction::ExtrudeSketch;
    tip("Make a NEW body from this region (Ctrl+click several regions to "
        "extrude them together). The source sketch/body is left unchanged.");

    // Subtract: cut this region out of the body the sketch sits on, with a red
    // preview of the removed volume. Disabled when the sketch has no source body.
    if (ImGui::Button("Subtract", ImVec2(-1, bh(30))))
        action = ToolAction::SubtractSketch;
    tip("Cut this region into the body the sketch was drawn on (preview in red).");

    // Any remaining HasSketchRegions plugin buttons.
    renderPluginButtons(1 << static_cast<int>(SelectionContext::HasSketchRegions));

    // Edit the sketch this region belongs to — re-enter sketch mode to revise it.
    if (ImGui::Button("Edit Sketch", ImVec2(-1, bh(30))))
        action = ToolAction::EditSketch;
    tip("Re-enter sketch mode to revise this region's parent sketch.");

    // Move / Rotate the region's PARENT sketch in 3D — same gizmo path as
    // the whole-sketch case. A region selection is just a finger pointing at
    // its sketch for these ops. Hidden in ortho view (gizmo's own rule) but
    // the buttons stay visible so the user understands the action exists.
    ImGui::Separator();
    ImGui::TextColored(materializr::accentText(), "Transform");
    ImGui::Separator();
    if (ImGui::Button("Move", ImVec2(-1, bh(30))))
        action = ToolAction::Move;
    tip("Show the Move gizmo on the parent sketch. Drag an axis to reposition "
        "the sketch in 3D - geometry follows, so the sketch becomes a movable "
        "construction plane. Outside ortho view only.");
    if (ImGui::Button("Rotate", ImVec2(-1, bh(30))))
        action = ToolAction::Rotate;
    tip("Show the Rotate gizmo on the parent sketch. Drag a ring to spin the "
        "sketch around its centroid.");

    ImGui::Spacing();
    ImGui::TextWrapped("Drag positive distance to extrude, negative to cut into the body the sketch sits on.");

    return action;
}

ToolAction Toolbar::renderEdgeTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(materializr::accentText(), "Edge Ops");
    ImGui::Separator();
    if (ImGui::Button("Fillet", ImVec2(-1, bh(30))))  action = ToolAction::Fillet;
    tip("Round the picked edge(s). Set radius in the popup.");
    if (ImGui::Button("Chamfer", ImVec2(-1, bh(30)))) action = ToolAction::Chamfer;
    tip("Bevel the picked edge(s). Set distance in the popup.");
    if (m_canEditDiameter &&
        ImGui::Button("Edit Diameter", ImVec2(-1, bh(30))))
        action = ToolAction::EditDiameter;
    tip("Resize the cylindrical face this edge belongs to.");

    // Construction plane / axis from this edge.
    renderAddPlaneMenu();
    renderAddAxisMenu();

    // Plugin buttons for HasEdges context
    renderPluginButtons(1 << static_cast<int>(SelectionContext::HasEdges));

    return action;
}

} // namespace materializr

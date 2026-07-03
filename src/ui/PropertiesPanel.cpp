#include "UiTheme.h"
#include "PropertiesPanel.h"
#include "../core/History.h"
#include "../core/Document.h"
#include "../core/SelectionManager.h"
#include "../core/Operation.h"
#include "../modeling/Sketch.h"
#include "../modeling/SketchSolver.h"
#include "../modeling/SketchEditOp.h"
#include "../modeling/SketchTool.h"
#include "../modeling/SketchConstraints.h"
#include "../modeling/TransformOp.h"
#include "../core/EventBus.h"
#include "../core/Events.h"
#include "../core/Verbose.h"
#include "../core/NumParse.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <gp_Ax3.hxx>
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <Geom_Surface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopoDS.hxx>
#include <TopAbs_ShapeEnum.hxx>

namespace materializr {

PropertiesPanel::PropertiesPanel() = default;

void PropertiesPanel::setHistory(History* history) {
    m_history = history;
}

void PropertiesPanel::setDocument(Document* doc) {
    m_document = doc;
}

void PropertiesPanel::setSelectionManager(const SelectionManager* sel) {
    m_selection = sel;
}

void PropertiesPanel::setEditingStep(int step) {
    m_editingStep = step;
}

int PropertiesPanel::getEditingStep() const {
    return m_editingStep;
}

bool PropertiesPanel::render() {
    bool modified = false;

    ImGui::Begin("Properties", nullptr, ImGuiWindowFlags_NoCollapse);

    // Case 0: In sketch mode — show the editable size of the selected element.
    // Takes priority: while sketching, the panel is about the sketch, not the
    // history step or a 3D selection.
    if (m_inSketchMode && m_activeSketch && m_sketchTool) {
        renderSketchElementPanel(modified);
        ImGui::End();
        return modified;
    }

    // Case 1: Editing a history operation
    if (m_history && m_editingStep >= 0 && m_editingStep < m_history->stepCount()) {
        const Operation* op = m_history->getStep(m_editingStep);
        if (op) {
            // Operation header
            ImGui::TextColored(materializr::accentText(), "%s", op->name().c_str());
            ImGui::Separator();

            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", op->description().c_str());
            ImGui::Spacing();

            // Render the operation's parameter controls
            const_cast<Operation*>(op)->renderProperties();

            // Enter commits the edit directly — the Apply button stays as the
            // mouse-driven alternative.
            bool enterCommits =
                ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
                (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                 ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));

            ImGui::Spacing();
            ImGui::Separator();

            if (ImGui::Button("Apply Changes", ImVec2(-1, 0)) || enterCommits) {
                if (m_document) {
                    // Carry any inline circle-diameter edit into later snapshots
                    // of the same sketch before replaying (see HistoryPanel).
                    m_history->propagateSketchValueEdits(m_editingStep, *m_document);
                    // Transactional: a failed replay restores the whole model
                    // rather than leaving it half-built.
                    m_history->editStep(m_editingStep, *m_document,
                                        /*transactional=*/true);
                    modified = true;
                }
            }

            ImGui::Spacing();

            // Enabled/disabled toggle
            bool enabled = op->isEnabled();
            if (ImGui::Checkbox("Enabled", &enabled)) {
                if (m_document) {
                    // In-place toggle — preserves base bodies the op modifies
                    // (replayAll's doc.clear() would delete them).
                    m_history->setStepEnabled(m_editingStep, enabled, *m_document);
                    modified = true;
                }
            }

            // Step info
            ImGui::Spacing();
            ImGui::Separator();
            char stepInfo[64];
            std::snprintf(stepInfo, sizeof(stepInfo), "Step %d of %d",
                          m_editingStep + 1, m_history->stepCount());
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", stepInfo);

            // Clear selection button
            if (ImGui::Button("Deselect", ImVec2(-1, 0))) {
                m_editingStep = -1;
            }
        }
    }
    // Case 2: A body is selected (but no operation being edited)
    else if (m_selection && m_selection->hasSelection() && m_document &&
             m_selection->primaryType() == SelectionType::Body) {
        const auto& sel = m_selection->getSelection();
        int bodyId = sel[0].bodyId;

        // Header
        ImGui::TextColored(materializr::accentText(), "Body Properties");
        ImGui::Separator();

        // Body name (editable)
        std::string bodyName = m_document->getBodyName(bodyId);
        static char nameBuffer[128];
        std::strncpy(nameBuffer, bodyName.c_str(), sizeof(nameBuffer) - 1);
        nameBuffer[sizeof(nameBuffer) - 1] = '\0';

        ImGui::Text("Name:");
        ImGui::SameLine();
        if (ImGui::InputText("##BodyName", nameBuffer, sizeof(nameBuffer),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            m_document->setBodyName(bodyId, nameBuffer);
        }

        // Body ID
        char idText[32];
        std::snprintf(idText, sizeof(idText), "ID: %d", bodyId);
        ImGui::Text("%s", idText);

        // Visibility toggle
        bool visible = m_document->isBodyVisible(bodyId);
        if (ImGui::Checkbox("Visible", &visible)) {
            m_document->setBodyVisible(bodyId, visible);
        }

        // Parametric-link hint (which sketch drives this body, and whether the
        // link is live or was broken by moving one of them independently).
        if (m_linkInfo) {
            std::string hint = m_linkInfo(/*isBody=*/true, bodyId);
            if (!hint.empty()) {
                ImGui::Spacing();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", hint.c_str());
                ImGui::PopTextWrapPos();
                if (hint.rfind("Detached", 0) == 0 && m_relink &&
                    ImGui::SmallButton("Re-link sketch")) {
                    m_relink(/*isBody=*/true, bodyId);
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        // Bounding-box readout (display only). Editing dimensions used to
        // live here as an inline editor, but it was functionally a glorified
        // Scale — same TransformOp, same anchor, same ellipse-from-cylinder
        // surprise. Editing now lives on the Scale gizmo popup, which has a
        // % / mm toggle and shows live dimensions in mm mode.
        ImGui::TextColored(materializr::accentText(), "Dimensions");
        const TopoDS_Shape& shape = m_document->getBody(bodyId);
        if (!shape.IsNull()) {
            // BRepBndLib::AddOptimal here used to run every frame, costing
            // 80-150ms on a complex NURBS body and pinning idle FPS to ~10
            // any time a body was selected. Cache by TShape pointer so the
            // recompute only happens when the topology actually rebuilds.
            const void* tsh = shape.TShape().get();
            auto it = m_bboxExtentCache.find(bodyId);
            std::array<double, 3> extents{};
            bool haveExtents = false;
            if (it != m_bboxExtentCache.end() && it->second.first == tsh) {
                extents = it->second.second;
                haveExtents = true;
            } else {
                Bnd_Box bbox;
                // Plain Add instead of AddOptimal: AddOptimal densely samples
                // every NURBS surface for a perfectly-tight box, costing
                // ~10x the time vs Add which reuses each face's already-
                // computed surface bbox. For a dimensions readout the few-
                // percent looseness is invisible to the user, and during
                // modification (TShape changes per frame → cache misses)
                // the cheaper recompute keeps interactive FPS reasonable.
                BRepBndLib::Add(shape, bbox);
                if (!bbox.IsVoid()) {
                    Standard_Real xmin, ymin, zmin, xmax, ymax, zmax;
                    bbox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
                    // user-Z-up remap: user X = world X,
                    // user Y = world Z, user Z = world Y.
                    extents = { xmax - xmin, zmax - zmin, ymax - ymin };
                    m_bboxExtentCache[bodyId] = {tsh, extents};
                    haveExtents = true;
                }
            }
            if (haveExtents) {
                ImGui::Text("Size: %.2f x %.2f x %.2f mm",
                            extents[0], extents[1], extents[2]);
                ImGui::TextDisabled("Edit dimensions via the Scale gizmo (R).");
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Empty shape");
            }
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No shape data");
        }

        // If multiple bodies selected, show count
        if (m_selection->selectedBodyCount() > 1) {
            ImGui::Spacing();
            ImGui::Separator();
            char multiText[64];
            std::snprintf(multiText, sizeof(multiText), "%d bodies selected",
                          m_selection->selectedBodyCount());
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", multiText);
        }
    }
    // Case 3: Other selection types
    else if (m_selection && m_selection->hasSelection()) {
        const char* typeName = "Object";
        int count = static_cast<int>(m_selection->getSelection().size());

        // A SketchRegion entry is the user pointing at its parent sketch, so
        // we treat the two interchangeably for the constraint editor below.
        bool sketchLike = false;
        int  parentSketchId = -1;
        switch (m_selection->primaryType()) {
            case SelectionType::Face:
                typeName = "Face";
                count = m_selection->selectedFaceCount();
                break;
            case SelectionType::Edge:
                typeName = "Edge";
                count = m_selection->selectedEdgeCount();
                break;
            case SelectionType::Vertex:
                typeName = "Vertex";
                break;
            case SelectionType::Sketch:
            case SelectionType::SketchRegion:
                typeName = (m_selection->primaryType() == SelectionType::SketchRegion)
                            ? "Region" : "Sketch";
                count = (m_selection->primaryType() == SelectionType::SketchRegion)
                            ? m_selection->selectedSketchRegionCount()
                            : m_selection->selectedSketchCount();
                sketchLike = true;
                for (const auto& e : m_selection->getSelection()) {
                    if ((e.type == SelectionType::Sketch ||
                         e.type == SelectionType::SketchRegion) && e.sketchId >= 0) {
                        parentSketchId = e.sketchId; break;
                    }
                }
                break;
            case SelectionType::Plane:
                typeName = "Plane";
                break;
            case SelectionType::Axis:
                typeName = "Axis";
                break;
            default:
                break;
        }

        char selText[128];
        std::snprintf(selText, sizeof(selText), "%d %s(s) selected", count, typeName);
        ImGui::TextColored(materializr::accentText(), "%s", selText);
        ImGui::Separator();

        // Parametric-link hint for a selected sketch (what it drives, and whether
        // moving it independently has detached it from that body).
        if (sketchLike && parentSketchId >= 0 && m_linkInfo) {
            std::string hint = m_linkInfo(/*isBody=*/false, parentSketchId);
            if (!hint.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", hint.c_str());
                ImGui::PopTextWrapPos();
                if (hint.rfind("Detached", 0) == 0 && m_relink &&
                    ImGui::SmallButton("Re-link to body")) {
                    m_relink(/*isBody=*/false, parentSketchId);
                }
                ImGui::Spacing();
            }
        }

        if (sketchLike && m_document && m_history && parentSketchId >= 0) {
            renderSketchConstraintsPanel(parentSketchId, modified);
        } else if (m_document) {
            // A single selected construction plane gets its orientation panel.
            int planeCount = 0, firstPlaneId = -1, axisCount = 0, firstAxisId = -1;
            for (const auto& e : m_selection->getSelection()) {
                if (e.type == SelectionType::Plane && e.planeId >= 0) {
                    ++planeCount;
                    if (firstPlaneId < 0) firstPlaneId = e.planeId;
                } else if (e.type == SelectionType::Axis && e.axisId >= 0) {
                    ++axisCount;
                    if (firstAxisId < 0) firstAxisId = e.axisId;
                }
            }
            if (planeCount == 1 && m_document->getPlane(firstPlaneId)) {
                renderPlanePanel(firstPlaneId, modified);
            } else if (axisCount == 1 && m_document->getAxis(firstAxisId)) {
                renderAxisPanel(firstAxisId, modified);
            } else {
                // Construction-plane CREATION actions (Midplane / Tangent /
                // Normal-to-axis) live in the Tools panel, alongside the other
                // create operations — see Toolbar's context renderers.
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                   "Sub-shape properties not yet available.");
            }
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "Sub-shape properties not yet available.");
        }
    }
    // Case 4: Nothing selected
    else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "Select an object or operation");
    }

    ImGui::End();
    return modified;
}

// Orientation readout + actions for a selected construction plane. Values are
// shown in the app's user Z-up convention (user Y = world Z, user Z = world Y)
// to match every other coordinate readout. Flip Normal mutates the document
// directly (marks dirty); Rotate About Axis… routes to Application's hinge
// popup, which records an undoable PlaneTransformOp on Apply.
void PropertiesPanel::renderPlanePanel(int planeId, bool& modified) {
    const auto* pe = m_document->getPlane(planeId);
    if (!pe) return;

    const gp_Ax3& ax = pe->plane.Position();
    gp_Pnt o = ax.Location();
    gp_Dir n = ax.Direction();
    gp_Dir u = ax.XDirection();

    // World→user display swap (Y/Z) so "up" reads as the user's Z.
    ImGui::Text("Origin:  %.2f, %.2f, %.2f mm", o.X(), o.Z(), o.Y());
    ImGui::Text("Normal:  %.3f, %.3f, %.3f",     n.X(), n.Z(), n.Y());
    ImGui::Text("In-plane X: %.3f, %.3f, %.3f",  u.X(), u.Z(), u.Y());

    // Tilt of the plane away from horizontal = angle of its normal from the
    // world up axis (world +Y). 0° = floor-parallel, 90° = vertical wall.
    double tilt = std::acos(std::min(1.0, std::fabs(n.Y()))) * 180.0 / M_PI;
    ImGui::Text("Tilt from horizontal: %.1f°", tilt);

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Flip Normal")) {
        m_document->flipPlaneNormal(planeId);
        if (m_markDirty) m_markDirty();
        modified = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Rotate About Axis...")) {
        if (m_rotatePlane) m_rotatePlane(planeId);
    }
}

// Orientation readout + Flip Direction for a selected construction axis.
// Values shown in user Z-up convention (user Y = world Z, user Z = world Y).
void PropertiesPanel::renderAxisPanel(int axisId, bool& modified) {
    const auto* ae = m_document->getAxis(axisId);
    if (!ae) return;

    const gp_Pnt& o = ae->origin;
    const gp_Dir& d = ae->direction;
    ImGui::Text("Origin:    %.2f, %.2f, %.2f mm", o.X(), o.Z(), o.Y());
    ImGui::Text("Direction: %.3f, %.3f, %.3f",     d.X(), d.Z(), d.Y());
    ImGui::Text("Length:    %.1f mm", ae->halfLength * 2.0);

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Flip Direction")) {
        m_document->flipAxisDirection(axisId);
        if (m_markDirty) m_markDirty();
        modified = true;
    }
}

// Edits the live sketch's constraints in place. Differs from
// SketchEditOp::renderProperties (which works on the m_after snapshot of
// the step you clicked in History): that path doesn't survive a project
// save/load because reloaded steps become parameterless ReplayOps. This
// panel reads/writes the current sketch directly, so the workflow works
// across sessions.
//
void PropertiesPanel::renderSketchElementPanel(bool& modified) {
    Sketch* sk = m_activeSketch;
    if (!sk) return;

    const auto& selC = m_sketchTool->getSelectedCircles();
    const auto& selA = m_sketchTool->getSelectedArcs();
    const auto& selL = m_sketchTool->getSelectedLines();
    const auto& selP = m_sketchTool->getSelectedPoints();
    const size_t total = selC.size() + selA.size() + selL.size() + selP.size();

    ImGui::TextDisabled("Sketch");
    ImGui::Separator();
    if (total == 0) {
        ImGui::TextWrapped("Select a sketch element to edit its size.");
        return;
    }
    if (total > 1) {
        ImGui::Text("%zu elements selected", total);
        ImGui::TextDisabled("Select a single circle or arc to edit its size.");
        return;
    }

    // Apply a size edit through the host so it's snapshot/undoable + cascades.
    auto apply = [&](const std::function<void()>& mut) {
        if (m_sketchMutate) { m_sketchMutate(mut); modified = true; }
    };

    // Resolve what was clicked to an editable element. Clicking a circle near
    // its CENTRE grabs the centre point (so you can drag-move it), so a selected
    // point that is a circle/arc centre still exposes that curve's size — you
    // can edit a circle by clicking anywhere on it, centre included.
    int circleId = -1, arcId = -1;
    if (!selC.empty()) circleId = *selC.begin();
    else if (!selA.empty()) arcId = *selA.begin();
    else if (!selP.empty()) {
        int pid = *selP.begin();
        for (const auto& cc : sk->getCircles()) if (cc.centerPointId == pid) { circleId = cc.id; break; }
        if (circleId < 0)
            for (const auto& aa : sk->getArcs()) if (aa.centerPointId == pid) { arcId = aa.id; break; }
    }

    if (circleId >= 0) {
        const SketchCircle* c = nullptr;
        for (const auto& cc : sk->getCircles()) if (cc.id == circleId) { c = &cc; break; }
        if (!c) return;
        ImGui::Text("Circle");
        double dia = c->radius * 2.0;
        ImGui::SetNextItemWidth(140);
        if (ImGui::InputDouble("Diameter (mm)", &dia, 0.0, 0.0, "%.3f",
                               ImGuiInputTextFlags_EnterReturnsTrue)) {
            double r = std::max(dia, 1e-6) * 0.5;
            apply([sk, circleId, r]() { sk->setCircleRadius(circleId, r); });
        }
        ImGui::TextDisabled("Centre stays put. Press Enter to apply.");
    } else if (arcId >= 0) {
        const SketchArc* a = nullptr;
        for (const auto& aa : sk->getArcs()) if (aa.id == arcId) { a = &aa; break; }
        if (!a) return;
        ImGui::Text("Arc");
        // Radius: centre fixed, endpoints slide radially (sweep preserved).
        double rad = a->radius;
        ImGui::SetNextItemWidth(140);
        if (ImGui::InputDouble("Radius (mm)", &rad, 0.0, 0.0, "%.3f",
                               ImGuiInputTextFlags_EnterReturnsTrue)) {
            double r = std::max(rad, 1e-6);
            apply([sk, arcId, r]() { sk->resizeArc(arcId, r); });
        }
        const SketchPoint* c = sk->getPoint(a->centerPointId);
        const SketchPoint* s = sk->getPoint(a->startPointId);
        const SketchPoint* e = sk->getPoint(a->endPointId);
        if (c && s && e) {
            // Chord: straight distance between the endpoints. Keeps the sweep
            // angle (same arc shape, just scaled), so it reads as "how far apart
            // are the ends" while preserving the curve's character.
            double chord = std::sqrt((e->pos.x - s->pos.x) * (e->pos.x - s->pos.x) +
                                     (e->pos.y - s->pos.y) * (e->pos.y - s->pos.y));
            ImGui::SetNextItemWidth(140);
            if (ImGui::InputDouble("Chord (mm)", &chord, 0.0, 0.0, "%.3f",
                                   ImGuiInputTextFlags_EnterReturnsTrue)) {
                double ch = std::max(chord, 1e-6);
                apply([sk, arcId, ch]() { sk->setArcChord(arcId, ch); });
            }
            // Sweep angle in degrees: start fixed, end point moves to the angle.
            double aS = std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
            double aE = std::atan2(e->pos.y - c->pos.y, e->pos.x - c->pos.x);
            double sweep = aE - aS;
            while (sweep <= 0.0)       sweep += 2.0 * M_PI;
            while (sweep > 2.0 * M_PI) sweep -= 2.0 * M_PI;
            double deg = sweep * 180.0 / M_PI;
            ImGui::SetNextItemWidth(140);
            if (ImGui::InputDouble("Sweep (\xC2\xB0)", &deg, 0.0, 0.0, "%.2f",
                                   ImGuiInputTextFlags_EnterReturnsTrue)) {
                double rad2 = deg * M_PI / 180.0;
                apply([sk, arcId, rad2]() { sk->setArcSweep(arcId, rad2); });
            }
        }
        ImGui::TextDisabled("Chord & Radius scale the arc (sweep kept); "
                            "Sweep changes the angle. Press Enter to apply.");
    } else if (!selL.empty()) {
        int lid = *selL.begin();
        const SketchLine* l = nullptr;
        for (const auto& ll : sk->getLines()) if (ll.id == lid) { l = &ll; break; }
        if (!l) return;
        // If this line is a side of an axis-aligned rectangle, edit the whole
        // rectangle (Width × Height) instead of a single side — that's what the
        // user means by "make the rectangle editable".
        Sketch::RectInfo rect;
        if (sk->findAxisAlignedRect(lid, rect)) {
            ImGui::Text("Rectangle");
            double w = rect.width, h = rect.height;
            ImGui::SetNextItemWidth(140);
            bool w_ed = ImGui::InputDouble("Width (mm)", &w, 0.0, 0.0, "%.3f",
                                           ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SetNextItemWidth(140);
            bool h_ed = ImGui::InputDouble("Height (mm)", &h, 0.0, 0.0, "%.3f",
                                           ImGuiInputTextFlags_EnterReturnsTrue);
            if (w_ed || h_ed) {
                double nw = std::max(w, 1e-6), nh = std::max(h, 1e-6);
                apply([sk, lid, nw, nh]() { sk->setRectangleSize(lid, nw, nh); });
            }
            ImGui::TextDisabled("Centre stays put. Press Enter to apply.");
        } else {
            const SketchPoint* p1 = sk->getPoint(l->startPointId);
            const SketchPoint* p2 = sk->getPoint(l->endPointId);
            ImGui::Text("Line");
            double len = 0.0;
            if (p1 && p2)
                len = std::sqrt((p2->pos.x - p1->pos.x) * (p2->pos.x - p1->pos.x) +
                                (p2->pos.y - p1->pos.y) * (p2->pos.y - p1->pos.y));
            ImGui::SetNextItemWidth(140);
            if (ImGui::InputDouble("Length (mm)", &len, 0.0, 0.0, "%.3f",
                                   ImGuiInputTextFlags_EnterReturnsTrue)) {
                double nl = std::max(len, 1e-6);
                apply([sk, lid, nl]() { sk->setLineLength(lid, nl); });
            }
            ImGui::TextDisabled("Grows from its centre; attached arcs keep their "
                                "angle. Press Enter to apply.");
        }
    } else {
        // A lone point that isn't a circle/arc centre (a line endpoint, etc.).
        ImGui::Text("Point");
        ImGui::TextDisabled("Drag to move; no editable size.");
    }
}

// Commit policy: text edits commit on Enter or focus-out (the
// IsItemDeactivatedAfterEdit signal). On commit we snapshot the pre-edit
// sketch, apply the value, run the solver, and push a SketchEditOp
// covering both states — so the change is undoable AND shows up as a
// proper step in history.
void PropertiesPanel::renderSketchConstraintsPanel(int sketchId, bool& modified) {
    auto sk = m_document->getSketch(sketchId);
    if (!sk) return;
    // One-shot diagnostic (--verbose only): log when the panel first opens on
    // a sketch so the constraint editor's reachability can be confirmed in a
    // field log. Suppress repeat logs for the same sketch on later frames.
    static int s_lastLoggedSketchId = -1;
    if (materializr::isVerbose() && sketchId != s_lastLoggedSketchId) {
        std::fprintf(stderr, "[Cascade] PropertiesPanel opened on sketchId=%d "
                             "(constraints=%zu",
                     sketchId, sk->getConstraints().size());
        for (const auto& c : sk->getConstraints()) {
            const char* tn = "?";
            switch (c.type) {
                case ConstraintType::Coincident:    tn = "Coincident";    break;
                case ConstraintType::Horizontal:    tn = "Horizontal";    break;
                case ConstraintType::Vertical:      tn = "Vertical";      break;
                case ConstraintType::Distance:      tn = "Distance";      break;
                case ConstraintType::Radius:        tn = "Radius";        break;
                case ConstraintType::Parallel:      tn = "Parallel";      break;
                case ConstraintType::Perpendicular: tn = "Perpendicular"; break;
                case ConstraintType::Fixed:         tn = "Fixed";         break;
                case ConstraintType::Tangent:       tn = "Tangent";       break;
                case ConstraintType::Equal:         tn = "Equal";         break;
                case ConstraintType::Concentric:    tn = "Concentric";    break;
                case ConstraintType::Angle:         tn = "Angle";         break;
            }
            std::fprintf(stderr, " %s=%.2f", tn, c.value);
        }
        std::fprintf(stderr, ")\n");
        s_lastLoggedSketchId = sketchId;
    }

    // Switching to a different sketch: throw away buffered edits from the
    // previous one so we don't show stale text in the inputs.
    if (m_constraintPanelSketchId != sketchId) {
        m_constraintEdits.clear();
        m_constraintPanelSketchId = sketchId;
    }

    auto& cs = sk->getMutableConstraints();
    if (cs.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "No constraints on this sketch.");
        ImGui::TextWrapped("Add one by right-clicking a sketch element in "
                           "sketch-edit mode and picking \"Add Constraint\".");
        return;
    }

    ImGui::TextColored(materializr::accentText(), "Constraints");
    ImGui::Separator();

    // Friendly type names for the non-editable bullet rows.
    auto typeName = [](ConstraintType t) -> const char* {
        switch (t) {
            case ConstraintType::Coincident:    return "Coincident";
            case ConstraintType::Horizontal:    return "Horizontal";
            case ConstraintType::Vertical:      return "Vertical";
            case ConstraintType::Parallel:      return "Parallel";
            case ConstraintType::Perpendicular: return "Perpendicular";
            case ConstraintType::Fixed:         return "Fix Position";
            case ConstraintType::Tangent:       return "Tangent";
            case ConstraintType::Equal:         return "Equal length";
            case ConstraintType::Concentric:    return "Concentric";
            default:                            return "Constraint";
        }
    };

    // Helper: commit a value change.
    //  - mutate the live constraint
    //  - re-solve so dependent geometry follows
    //  - push a SketchEditOp(before, after) covering the change
    auto commitEdit = [&](Constraint& c, double newValue, ConstraintEdit& edit) {
        if (!edit.beforeSnap) edit.beforeSnap = std::make_shared<Sketch>(*sk);
        c.value = newValue;
        SketchSolver solver;
        solver.solve(*sk);
        auto after = std::make_shared<Sketch>(*sk);
        auto op = std::make_unique<SketchEditOp>(sk, edit.beforeSnap, after);
        m_history->pushExecuted(std::move(op));
        edit.beforeSnap.reset();
        edit.focused = false;
        modified = true;
        // Cascade trigger: Application listens for this and re-executes any
        // ExtrudeOp downstream of `sketchId` so the body follows the new
        // constraint value. No-op when nobody's subscribed.
        if (m_eventBus) {
            if (materializr::isVerbose())
                std::fprintf(stderr, "[Cascade] PropertiesPanel publish SketchEdited sketchId=%d\n", sketchId);
            m_eventBus->publish(SketchEditedEvent{sketchId});
        } else if (materializr::isVerbose()) {
            std::fprintf(stderr, "[Cascade] PropertiesPanel has no event bus\n");
        }
    };

    int anyDim = 0;
    for (size_t i = 0; i < cs.size(); ++i) {
        Constraint& c = cs[i];
        ImGui::PushID(static_cast<int>(c.id));

        // Render dimensional ones (Distance, Radius/Diameter, Angle) inline.
        // Non-dimensional ones get a single muted bullet — there's nothing to
        // tune, but listing them confirms what's actually applied.
        bool isDim = (c.type == ConstraintType::Distance ||
                      c.type == ConstraintType::Radius   ||
                      c.type == ConstraintType::Angle);
        if (isDim) {
            ++anyDim;
            auto& edit = m_constraintEdits[c.id];

            // Display value: Radius shown as diameter (matches sketch popup).
            double shown = (c.type == ConstraintType::Radius) ? (c.value * 2.0)
                          : (c.type == ConstraintType::Angle)
                                ? (c.value * 180.0 / M_PI)
                                : c.value;

            // Refill the buffer when the user is NOT actively editing this
            // field, so external changes (solver runs, undo/redo) propagate
            // into the visible text. While focused we leave the buffer alone
            // so we don't trample the user's keystrokes.
            const char* unit = (c.type == ConstraintType::Angle) ? "\xC2\xB0" : "mm";
            const char* label =
                c.type == ConstraintType::Distance ? "Distance"
              : c.type == ConstraintType::Radius   ? "\xC3\x98 (diameter)"
                                                   : "Angle";
            if (!edit.focused) {
                std::snprintf(edit.buf, sizeof(edit.buf), "%.3f", shown);
            }

            ImGui::TextUnformatted(label);
            ImGui::SameLine(120);
            ImGui::SetNextItemWidth(110);
            ImGui::InputText("##val", edit.buf, sizeof(edit.buf),
                             ImGuiInputTextFlags_CharsDecimal |
                             ImGuiInputTextFlags_AutoSelectAll |
                             ImGuiInputTextFlags_EnterReturnsTrue);
            bool justActivated   = ImGui::IsItemActivated();
            bool justDeactivated = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine(); ImGui::Text("%s", unit);

            // Snapshot the pre-edit sketch the moment the user starts typing
            // so we can use it as the "before" of the eventual SketchEditOp.
            if (justActivated) {
                edit.focused = true;
                edit.beforeSnap = std::make_shared<Sketch>(*sk);
            }
            // Commit on Enter / focus-out (whichever fires first).
            if (justDeactivated) {
                // parseFinite: a non-finite constraint value would wedge the
                // solver; garbage entry commits nothing (typed stays == value).
                double typed = c.value;
                (void)materializr::parseFinite(edit.buf, typed);
                double newRaw = (c.type == ConstraintType::Radius)
                                    ? typed * 0.5
                              : (c.type == ConstraintType::Angle)
                                    ? typed * M_PI / 180.0
                                    : typed;
                if (std::abs(newRaw - c.value) > 1e-6) {
                    commitEdit(c, newRaw, edit);
                } else {
                    edit.focused = false;
                    edit.beforeSnap.reset();
                }
            }
        } else {
            ImGui::TextDisabled("\xE2\x80\xA2 %s", typeName(c.type));
        }
        ImGui::PopID();
    }

    if (!anyDim) {
        ImGui::Spacing();
        ImGui::TextWrapped("This sketch has no dimensional constraints — only "
                           "Horizontal / Parallel / etc., which have nothing "
                           "to tune.");
    } else {
        ImGui::Spacing();
        ImGui::TextDisabled("Press Enter or click elsewhere to commit a value.");
    }
}

} // namespace materializr

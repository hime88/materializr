#include "PropertiesPanel.h"
#include "../core/History.h"
#include "../core/Document.h"
#include "../core/SelectionManager.h"
#include "../core/Operation.h"
#include "../modeling/Sketch.h"
#include "../modeling/SketchSolver.h"
#include "../modeling/SketchEditOp.h"
#include "../modeling/SketchConstraints.h"
#include "../modeling/TransformOp.h"
#include "../core/EventBus.h"
#include "../core/Events.h"
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

    // Case 1: Editing a history operation
    if (m_history && m_editingStep >= 0 && m_editingStep < m_history->stepCount()) {
        const Operation* op = m_history->getStep(m_editingStep);
        if (op) {
            // Operation header
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", op->name().c_str());
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
                const_cast<Operation*>(op)->setEnabled(enabled);
                if (m_document) {
                    m_history->replayAll(*m_document);
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
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Body Properties");
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

        ImGui::Spacing();
        ImGui::Separator();

        // Bounding-box readout (display only). Editing dimensions used to
        // live here as an inline editor, but it was functionally a glorified
        // Scale — same TransformOp, same anchor, same ellipse-from-cylinder
        // surprise. Editing now lives on the Scale gizmo popup, which has a
        // % / mm toggle and shows live dimensions in mm mode.
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Dimensions");
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
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", selText);
        ImGui::Separator();

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
// Commit policy: text edits commit on Enter or focus-out (the
// IsItemDeactivatedAfterEdit signal). On commit we snapshot the pre-edit
// sketch, apply the value, run the solver, and push a SketchEditOp
// covering both states — so the change is undoable AND shows up as a
// proper step in history.
void PropertiesPanel::renderSketchConstraintsPanel(int sketchId, bool& modified) {
    auto sk = m_document->getSketch(sketchId);
    if (!sk) return;
    // One-shot diagnostic: log when the panel first opens on a sketch so we
    // can confirm the constraint editor is being reached. Suppress repeat
    // logs for the same sketch on subsequent frames.
    static int s_lastLoggedSketchId = -1;
    if (sketchId != s_lastLoggedSketchId) {
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

    ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "Constraints");
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
            std::fprintf(stderr, "[Cascade] PropertiesPanel publish SketchEdited sketchId=%d\n", sketchId);
            m_eventBus->publish(SketchEditedEvent{sketchId});
        } else {
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
                double typed = std::atof(edit.buf);
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

#include "Toolbar.h"
#include "../core/SelectionManager.h"
#include "../core/History.h"
#include "../core/Operation.h"
#include "../plugin/PluginRegistry.h"
#include "../plugin/PluginContext.h"
#include "../plugin/Contributions.h"
#include <imgui.h>
#include <cmath>

namespace materializr {

Toolbar::Toolbar() = default;

// Tooltip helper. ImGui::SetItemTooltip auto-handles the show delay + hovered
// check, so we just gate on the user's "show toolbar tooltips" preference.
// Called immediately after each button.
void Toolbar::tip(const char* text) const {
    if (m_showTooltips) ImGui::SetItemTooltip("%s", text);
}

void Toolbar::setSelectionManager(const SelectionManager* sel) {
    m_selection = sel;
}

ToolAction Toolbar::render() {
    ToolAction action = ToolAction::None;

    ImGui::Begin("Tools");

    if (m_sketchMode) {
        action = renderSketchTools();
    } else if (!m_selection || !m_selection->hasSelection()) {
        action = renderNoSelectionTools();
    } else if (m_selection->hasSelectedSketchRegions()) {
        action = renderSketchRegionTools();
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
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", c.section.c_str());
            ImGui::Separator();
            lastSection = c.section;
        }
        ImGui::PushID(static_cast<int>(i + 10000));
        if (ImGui::Button(c.name.c_str(), ImVec2(-1, 30))) {
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

ToolAction Toolbar::renderSketchTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Sketch Tools");
    ImGui::Separator();

    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Grid:");
    const float steps[] = { 0.1f, 0.5f, 1.0f, 10.0f };
    const char* labels[] = { "0.1", "0.5", "1", "10" };
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine();
        bool selected = std::abs(m_gridStep - steps[i]) < 1e-6f;
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
        if (ImGui::Button(labels[i], ImVec2(34, 24))) m_gridStep = steps[i];
        if (selected) ImGui::PopStyleColor();
    }
    ImGui::Separator();

    // Render a sketch-tool button with a thick light-grey border when it's
    // the currently active mode. Caller checks the return value to set
    // `action`. Mode id matches SketchToolMode enum (see Toolbar.h).
    auto skBtn = [&](const char* label, int modeId) -> bool {
        bool active = (m_activeSketchMode == modeId);
        if (active) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        }
        bool clicked = ImGui::Button(label, ImVec2(-1, 30));
        if (active) {
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }
        return clicked;
    };

    if (skBtn("Select / Move", 1)) action = ToolAction::SelectSketch;
    tip("Pick sketch elements (points, lines, regions). Drag selection to move.");
    if (skBtn("Line",      2))     action = ToolAction::Line;
    tip("Draw straight line segments. Click to add vertices, Esc to finish.");
    if (skBtn("Circle",    3))     action = ToolAction::Circle;
    tip("Draw a circle: click centre, drag to radius.");
    if (skBtn("Rectangle", 4))     action = ToolAction::Rectangle;
    tip("Draw an axis-aligned rectangle: click one corner, drag to the opposite.");
    if (skBtn("Arc",       5))     action = ToolAction::Arc;
    tip("Three-point arc: click start, end, then a point on the curve.");
    if (skBtn("Spline",    6))     action = ToolAction::Spline;
    tip("Multi-point spline. Click control points, Enter to finish.");
    if (skBtn("Polygon",   7))     action = ToolAction::Polygon;
    tip("Regular polygon: click centre, drag to size. Side count in properties.");
    if (skBtn("Trim",      8))     action = ToolAction::Trim;
    tip("Trim a sketch segment at the nearest intersections.");

    // Transforms operate on the current sketch-element selection (Select tool +
    // click/Ctrl+click on points and lines). No-op if nothing is selected.
    // Rotate is the gizmo's ring handle (drag = 15° snap, popup for exact value),
    // so it doesn't get its own button.
    ImGui::Separator();
    if (ImGui::Button("Copy",   ImVec2(-1, 28))) action = ToolAction::SketchCopy;
    tip("Duplicate the selected sketch elements at an offset.");
    if (ImGui::Button("Mirror", ImVec2(-1, 28))) action = ToolAction::SketchMirror;
    tip("Mirror selected elements across a sketch line you'll pick next.");
    if (ImGui::Button("Linear Pattern", ImVec2(-1, 28))) action = ToolAction::SketchLinearPattern;
    tip("Copy the selected sketch elements N times along the sketch X axis.");
    if (ImGui::Button("Radial Pattern", ImVec2(-1, 28))) action = ToolAction::SketchRadialPattern;
    tip("Copy the selected sketch elements around an origin you specify.");

    ImGui::Separator();
    if (ImGui::Button("Measure", ImVec2(-1, 28))) action = ToolAction::Measure;
    tip("Measure distance / length between picked sketch elements.");

    if (!m_cameraOrtho) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.85f, 1.0f));
        if (ImGui::Button("Look at Sketch", ImVec2(-1, 30)))
            action = ToolAction::LookAtSketch;
        tip("Snap the camera to look straight down the sketch plane (orthographic).");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    if (ImGui::Button("Finish Sketch", ImVec2(-1, 30)))
        action = ToolAction::FinishSketch;
    tip("Leave sketch mode and return to the 3D viewport. Keeps the sketch.");
    if (ImGui::Button("Exit Sketch (discard)", ImVec2(-1, 30)))
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
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Create");
    ImGui::Separator();
    if (ImGui::Button("Sketch on XY", ImVec2(-1, 30))) action = ToolAction::StartSketchXY;
    tip("Start a new sketch on the world XY (floor) plane.");
    if (ImGui::Button("Sketch on XZ", ImVec2(-1, 30))) action = ToolAction::StartSketchXZ;
    tip("Start a new sketch on the world XZ (front) plane.");
    if (ImGui::Button("Sketch on YZ", ImVec2(-1, 30))) action = ToolAction::StartSketchYZ;
    tip("Start a new sketch on the world YZ (side) plane.");

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Inspect");
    ImGui::Separator();
    if (ImGui::Button("Measure", ImVec2(-1, 30))) action = ToolAction::Measure;
    tip("Measure distance, length, or angle between picked features.");

    // Plugin buttons: NoSelection + Always
    int mask = (1 << static_cast<int>(SelectionContext::NoSelection))
             | (1 << static_cast<int>(SelectionContext::Always));
    renderPluginButtons(mask);

    return action;
}

ToolAction Toolbar::renderBodyTools(bool includePluginButtons) {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Transform");
    ImGui::Separator();

    // Gizmo modes side by side, then Mirror.
    float third = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    if (ImGui::Button("Move", ImVec2(third, 30)))   action = ToolAction::Move;
    tip("Show the translate gizmo. Drag axes / planes to move. (W)");
    ImGui::SameLine();
    if (ImGui::Button("Rotate", ImVec2(third, 30))) action = ToolAction::Rotate;
    tip("Show the rotate gizmo. Drag rings to rotate around each axis. (E)");
    ImGui::SameLine();
    if (ImGui::Button("Scale", ImVec2(third, 30)))  action = ToolAction::Scale;
    tip("Show the scale gizmo. Drag handles to resize. (R)");
    if (ImGui::Button("Mirror", ImVec2(-1, 30)))    action = ToolAction::Mirror;
    tip("Mirror the selected bodies across a plane you pick next.");

    ImGui::Checkbox("Snap to grid", &m_snapToGrid);
    const float gridSteps[] = { 0.1f, 0.5f, 1.0f, 10.0f };
    const char* gridLabels[] = { "0.1", "0.5", "1", "10" };
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine();
        bool selected = std::abs(m_gridStep - gridSteps[i]) < 1e-6f;
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
        ImGui::PushID(i);
        if (ImGui::SmallButton(gridLabels[i])) m_gridStep = gridSteps[i];
        ImGui::PopID();
        if (selected) ImGui::PopStyleColor();
    }

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

    return action;
}

ToolAction Toolbar::renderFaceTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Face Operations");
    ImGui::Separator();

    if (ImGui::Button("Sketch on Face", ImVec2(-1, 30)))
        action = ToolAction::SketchOnFace;
    tip("Start a new sketch lying on the picked face.");
    if (ImGui::Button("Push / Pull", ImVec2(-1, 30)))
        action = ToolAction::PushPull;
    tip("Drag the face along its normal to extrude (+) or cut (−) into the body.");
    // Extrude From a face → make a new body that's the face's silhouette
    // swept along its normal. Push/Pull modifies the source body; Extrude
    // always creates a separate body. Same ToolAction the sketch toolbar
    // uses; the handler dispatches by selection type.
    if (ImGui::Button("Extrude From", ImVec2(-1, 30)))
        action = ToolAction::ExtrudeSketch;
    tip("Make a NEW body by extruding this face's silhouette (source body unchanged).");
    if (ImGui::Button("Shell", ImVec2(-1, 30)))
        action = ToolAction::Shell;
    tip("Hollow the body, removing the picked face. Wall thickness in the popup.");
    if (m_canEditDiameter &&
        ImGui::Button("Edit Diameter", ImVec2(-1, 30)))
        action = ToolAction::EditDiameter;
    tip("Resize a cylindrical hole / pin to an exact diameter.");

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
                    if (label && ImGui::Button(label, ImVec2(-1, 30)))
                        action = ToolAction::EditFilletChamfer;
                    tip(op->typeId() == "fillet"
                            ? "Change this fillet's radius without re-picking edges."
                            : "Change this chamfer's distance without re-picking edges.");
                    break;
                }
            }
        }
    }

    // Plugin buttons for HasFaces context
    renderPluginButtons(1 << static_cast<int>(SelectionContext::HasFaces));

    return action;
}

ToolAction Toolbar::renderSketchSelectedTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Sketch");
    ImGui::Separator();
    ImGui::TextWrapped("Tip: hover a sketch region to highlight it, click to select, Ctrl+click to add to selection.");
    ImGui::Separator();

    if (ImGui::Button("Edit Sketch", ImVec2(-1, 30)))
        action = ToolAction::EditSketch;
    tip("Re-enter sketch mode to revise this sketch's geometry.");
    if (ImGui::Button("Extrude From", ImVec2(-1, 30)))
        action = ToolAction::ExtrudeSketch;
    tip("Make a new body by extruding the sketch's closed regions.");
    if (ImGui::Button("Subtract Sketch", ImVec2(-1, 30)))
        action = ToolAction::SubtractSketch;
    tip("Cut the extruded regions out of the body the sketch was drawn on.");
    ImGui::TextWrapped("Subtract cuts the extruded profile into the body the "
                       "sketch was drawn on (preview shown in red).");

    // Plugin buttons for HasSketches context
    renderPluginButtons(1 << static_cast<int>(SelectionContext::HasSketches));

    return action;
}

ToolAction Toolbar::renderSketchRegionTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Region");
    ImGui::Separator();
    int n = m_selection ? m_selection->selectedSketchRegionCount() : 0;
    ImGui::Text("%d region%s selected", n, n == 1 ? "" : "s");
    ImGui::Spacing();

    // Push/Pull routes through the app's interactive arrow gizmo (default 0,
    // drag to extrude/cut) — same as a body face.
    if (ImGui::Button("Push / Pull", ImVec2(-1, 30)))
        action = ToolAction::PushPull;
    tip("Drag the arrow to extrude this region into a body, or cut it into the parent.");

    // Subtract: cut this region out of the body the sketch sits on, with a red
    // preview of the removed volume. Disabled when the sketch has no source body.
    if (ImGui::Button("Subtract", ImVec2(-1, 30)))
        action = ToolAction::SubtractSketch;
    tip("Cut this region into the body the sketch was drawn on (preview in red).");

    // Any remaining HasSketchRegions plugin buttons.
    renderPluginButtons(1 << static_cast<int>(SelectionContext::HasSketchRegions));

    // Edit the sketch this region belongs to — re-enter sketch mode to revise it.
    if (ImGui::Button("Edit Sketch", ImVec2(-1, 30)))
        action = ToolAction::EditSketch;
    tip("Re-enter sketch mode to revise this region's parent sketch.");

    ImGui::Spacing();
    ImGui::TextWrapped("Drag positive distance to extrude, negative to cut into the body the sketch sits on.");

    return action;
}

ToolAction Toolbar::renderEdgeTools() {
    ToolAction action = ToolAction::None;

    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Edge Ops");
    ImGui::Separator();
    if (ImGui::Button("Fillet", ImVec2(-1, 30)))  action = ToolAction::Fillet;
    tip("Round the picked edge(s). Set radius in the popup.");
    if (ImGui::Button("Chamfer", ImVec2(-1, 30))) action = ToolAction::Chamfer;
    tip("Bevel the picked edge(s). Set distance in the popup.");
    if (m_canEditDiameter &&
        ImGui::Button("Edit Diameter", ImVec2(-1, 30)))
        action = ToolAction::EditDiameter;
    tip("Resize the cylindrical face this edge belongs to.");

    // Plugin buttons for HasEdges context
    renderPluginButtons(1 << static_cast<int>(SelectionContext::HasEdges));

    return action;
}

} // namespace materializr

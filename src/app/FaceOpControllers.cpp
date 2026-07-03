#include "FaceOpControllers.h"
#include "UserAxes.h"
#include "../core/Document.h"
#include "../core/SelectionManager.h"
#include "../core/NumParse.h"
#include "../modeling/ShellOp.h"
#include "../modeling/TaperOp.h"
#include "../modeling/ScaleFaceOp.h"
#include "../modeling/ProjectSketchOp.h"
#include "../modeling/DefeatureOp.h"
#include "../modeling/Sketch.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <imgui.h>
#include <BRep_Tool.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <GProp_GProps.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <Geom_Surface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_ConicalSurface.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

namespace materializr {

// ─── Shell ───────────────────────────────────────────────────────────────────

int ShellController::onBegin(const IopContext& ctx) {
    for (const auto& e : ctx.selection.getSelection()) {
        if (e.type == SelectionType::Face && e.bodyId >= 0 &&
            !e.shape.IsNull()) {
            m_face = TopoDS::Face(e.shape);
            m_thickness = 1.0f;
            std::snprintf(m_inputBuf, sizeof(m_inputBuf), "%.2f",
                          m_thickness);
            m_inputFocus = true;
            return e.bodyId;
        }
    }
    return -1;
}

std::unique_ptr<Operation> ShellController::buildOp(const IopContext&) {
    if (m_thickness <= 0.0f) return nullptr;
    auto op = std::make_unique<ShellOp>();
    op->setBody(bodyId());
    op->setThickness(static_cast<double>(m_thickness));
    op->addFaceToRemove(m_face);
    return op;
}

void ShellController::panelBody(const IopContext& ctx, bool& changed) {
    ImGui::TextDisabled("Hollows the body, opening a face.");

    if (m_inputFocus) {
        ImGui::SetKeyboardFocusHere();
        m_inputFocus = false;
    }
    ImGui::SetNextItemWidth(140);
    // parseFinite: non-finite input keeps the previous thickness rather
    // than feeding inf into MakeThickSolid.
    if (ImGui::InputText("##shellThickness", m_inputBuf, sizeof(m_inputBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue |
                         ImGuiInputTextFlags_CharsDecimal)) {
        (void)materializr::parseFinite(m_inputBuf, m_thickness);
        requestCommit();
    } else {
        float parsed = m_thickness;
        if (materializr::parseFinite(m_inputBuf, parsed) &&
            std::abs(parsed - m_thickness) > 0.001f) {
            m_thickness = parsed;
            changed = true;
        }
    }
    ImGui::SameLine();
    ImGui::Text("mm");

    if (ImGui::SliderFloat("##shellSlider", &m_thickness, 0.1f, 20.0f,
                           "%.2f mm")) {
        // Snap to 0.1 mm — wall thicknesses are almost always in tenths, and a
        // free-floating 3.47 mm slider value is just noise.
        m_thickness = std::round(m_thickness * 10.0f) / 10.0f;
        std::snprintf(m_inputBuf, sizeof(m_inputBuf), "%.2f", m_thickness);
        changed = true;
    }

    if (!previewOk()) {
        const ImVec4 warn(1.0f, 0.6f, 0.3f, 1.0f);
        // If the wall lands near one of the body's rounded-edge radii, THAT'S
        // the cause: a fillet offset inward by ~its own radius collapses to a
        // zero-radius edge (singular for any join type), so the shell can't be
        // built there — but a clearly thinner or thicker wall works. Name it.
        // roundedFaceRadii is a full-face scan and panelBody runs every frame,
        // so cache it keyed on the body shape (recompute only when it changes).
        const TopoDS_Shape& body = ctx.doc.getBody(bodyId());
        if (m_radiiCacheShape.IsNull() || !m_radiiCacheShape.IsEqual(body)) {
            m_radiiCache = ShellOp::roundedFaceRadii(body);
            m_radiiCacheShape = body;
        }
        double nearR = -1.0, bestD = 1e18;
        for (double r : m_radiiCache) {
            double d = std::abs(r - static_cast<double>(m_thickness));
            if (d < bestD) { bestD = d; nearR = r; }
        }
        if (nearR > 0.0 && bestD < 0.5) {
            ImGui::TextColored(warn,
                "Shell failed: %.2f mm is too close to this body's %.2f mm\n"
                "rounded edge - a wall near a fillet radius can't be offset.\n"
                "Try a wall clearly thinner or thicker than %.2f mm.",
                static_cast<double>(m_thickness), nearR, nearR);
        } else {
            ImGui::TextColored(warn,
                "Shell failed - try a thinner wall, or\n"
                "this body's faces can't be shelled.");
        }
    }
}

void ShellController::onCleanup() {
    m_face.Nullify();
    m_radiiCache.clear();
    m_radiiCacheShape.Nullify();
}

// ─── Taper ───────────────────────────────────────────────────────────────────

int TaperController::onBegin(const IopContext& ctx) {
    // Collect every selected face on ONE body — multi-select all four
    // sides of a box to pyramid it in one go.
    m_faces.clear();
    int body = -1;
    for (const auto& e : ctx.selection.getSelection()) {
        if (e.type != SelectionType::Face || e.bodyId < 0 ||
            e.shape.IsNull())
            continue;
        if (body < 0) body = e.bodyId;
        if (e.bodyId != body) continue; // one body per op
        m_faces.push_back(TopoDS::Face(e.shape));
    }
    if (m_faces.empty()) return -1;
    m_angle = 10.0f;
    m_axisIdx = 0;
    m_flipBase = false;
    return body;
}

bool TaperController::resolveFrame(const IopContext& ctx, glm::vec3& dirOut,
                                   glm::vec3& neutralOut) const {
    if (bodyId() < 0 || m_faces.empty()) return false;

    // Pull direction. Auto: a cylindrical/conical face drafts along its own
    // axis; a planar face drafts along the world axis most PERPENDICULAR to
    // its normal (preferring up). Manual: the user-convention X/Y/Z radios.
    glm::vec3 dir(0.0f, 1.0f, 0.0f);
    if (m_axisIdx == 0) {
        try {
            const TopoDS_Face& f = m_faces.front();
            Handle(Geom_Surface) s = BRep_Tool::Surface(f);
            Handle(Geom_CylindricalSurface) cyl =
                Handle(Geom_CylindricalSurface)::DownCast(s);
            Handle(Geom_ConicalSurface) cone =
                Handle(Geom_ConicalSurface)::DownCast(s);
            if (!cyl.IsNull() || !cone.IsNull()) {
                gp_Dir a = !cyl.IsNull()
                               ? cyl->Cylinder().Position().Direction()
                               : cone->Cone().Position().Direction();
                dir = glm::vec3(static_cast<float>(a.X()),
                                static_cast<float>(a.Y()),
                                static_cast<float>(a.Z()));
            } else {
                BRepGProp_Face prop(f);
                double u1, u2, v1, v2;
                prop.Bounds(u1, u2, v1, v2);
                gp_Pnt c;
                gp_Vec nv;
                prop.Normal(0.5 * (u1 + u2), 0.5 * (v1 + v2), c, nv);
                glm::vec3 n(static_cast<float>(nv.X()),
                            static_cast<float>(nv.Y()),
                            static_cast<float>(nv.Z()));
                if (glm::length(n) > 1e-6f) n = glm::normalize(n);
                const glm::vec3 axes[3] = {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}};
                float best = 2.0f;
                for (const auto& a : axes) {
                    float d = std::abs(glm::dot(n, a));
                    if (d < best - 1e-4f) { best = d; dir = a; }
                }
            }
        } catch (...) {}
    } else {
        dir = userAxisToWorldVec(m_axisIdx - 1);
    }
    if (glm::length(dir) < 1e-6f) return false;
    dir = glm::normalize(dir);

    // Neutral plane: perpendicular to the pull direction, through the
    // body's extreme along it — the BASE stays fixed and the far end
    // tilts. Flip moves the fixed plane to the other extreme.
    try {
        Bnd_Box bb;
        BRepBndLib::Add(ctx.doc.getBody(bodyId()), bb);
        if (bb.IsVoid()) return false;
        double x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        glm::vec3 corners[8] = {
            {(float)x0, (float)y0, (float)z0}, {(float)x1, (float)y0, (float)z0},
            {(float)x0, (float)y1, (float)z0}, {(float)x1, (float)y1, (float)z0},
            {(float)x0, (float)y0, (float)z1}, {(float)x1, (float)y0, (float)z1},
            {(float)x0, (float)y1, (float)z1}, {(float)x1, (float)y1, (float)z1}};
        float lo = 1e30f, hi = -1e30f;
        for (const auto& c : corners) {
            float p = glm::dot(c, dir);
            lo = std::min(lo, p);
            hi = std::max(hi, p);
        }
        glm::vec3 center(0.5f * (float)(x0 + x1), 0.5f * (float)(y0 + y1),
                         0.5f * (float)(z0 + z1));
        float proj = m_flipBase ? hi : lo;
        neutralOut = center + dir * (proj - glm::dot(center, dir));
        dirOut = dir;
        return true;
    } catch (...) { return false; }
}

std::unique_ptr<Operation> TaperController::buildOp(const IopContext& ctx) {
    if (std::abs(m_angle) < 0.1f) return nullptr;
    glm::vec3 dir, np;
    if (!resolveFrame(ctx, dir, np)) return nullptr;
    auto op = std::make_unique<TaperOp>();
    op->setBody(bodyId());
    for (const auto& f : m_faces) op->addFace(f);
    op->setDirection(dir.x, dir.y, dir.z);
    op->setNeutralPoint(np.x, np.y, np.z);
    op->setAngleDeg(static_cast<double>(m_angle));
    return op;
}

void TaperController::panelBody(const IopContext&, bool& changed) {
    ImGui::TextDisabled("%zu face(s) tilt about the body's base.",
                        m_faces.size());
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
    ImGui::TextDisabled("Tip: pick SIDE walls — a cylinder wall becomes a "
                        "cone, box sides become a pyramid.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    if (previewOk()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
                           "Previewing %.1f deg", m_angle);
    } else if (std::abs(m_angle) < 0.1f) {
        // buildOp() short-circuits at ~0° so no preview is computed —
        // but the face is fine. Don't flash the "can't taper" warning
        // when the user is just sitting on the slider's zero stop.
        ImGui::TextDisabled("Move the angle slider to preview.");
    } else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                           "No preview: this face can't taper along the "
                           "current Pull axis. Try another axis, Flip base, "
                           "or pick a side face.");
        ImGui::TextDisabled("Note: only flat / cylindrical / conical walls "
                            "can be drafted (a kernel limit) - for freeform "
                            "shapes like wing skins, use Scale Face on the "
                            "END face instead.");
        ImGui::PopTextWrapPos();
    }

    if (ImGui::SliderFloat("Angle", &m_angle, -45.0f, 45.0f, "%.1f deg"))
        changed = true;

    ImGui::Text("Pull axis");
    ImGui::SameLine();
    const char* axisNames[4] = {"Auto", "X", "Y", "Z"};
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::RadioButton(axisNames[i], m_axisIdx == i)) {
            m_axisIdx = i;
            changed = true;
        }
    }
    if (ImGui::Checkbox("Flip base (fixed end)", &m_flipBase))
        changed = true;
}

void TaperController::onCleanup() { m_faces.clear(); }

// ─── Remove Face (defeature) ─────────────────────────────────────────────────

int DefeatureController::onBegin(const IopContext& ctx) {
    // Gather every selected face on ONE body — multi-select a few faces to
    // remove them together.
    m_faces.clear();
    int body = -1;
    for (const auto& e : ctx.selection.getSelection()) {
        if (e.type != SelectionType::Face || e.bodyId < 0 || e.shape.IsNull())
            continue;
        if (body < 0) body = e.bodyId;
        if (e.bodyId != body) continue; // one body per op
        m_faces.push_back(TopoDS::Face(e.shape));
    }
    if (m_faces.empty()) return -1;
    return body;
}

std::unique_ptr<Operation> DefeatureController::buildOp(const IopContext&) {
    if (m_faces.empty()) return nullptr;
    auto op = std::make_unique<DefeatureOp>();
    op->setBody(bodyId());
    for (const auto& f : m_faces) op->addFace(f);
    return op;
}

void DefeatureController::panelBody(const IopContext&, bool&) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
    ImGui::TextDisabled("Removes the selected face(s) and heals the surrounding "
                        "faces back together — e.g. take a baked fillet back to "
                        "a sharp edge so you can re-fillet it.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    ImGui::Text("%zu face(s) selected", m_faces.size());

    if (previewOk()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Previewing removal");
    } else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                           "Can't remove: the neighbouring faces can't be "
                           "extended to close the gap. Try a different face — a "
                           "single fillet / round usually works.");
        ImGui::PopTextWrapPos();
    }
}

void DefeatureController::onCleanup() { m_faces.clear(); }

// ─── Project Sketch ──────────────────────────────────────────────────────────

int ProjectSketchController::onBegin(const IopContext& ctx) {
    m_face.Nullify();
    m_sketchIds.clear();
    m_regionFilter.clear();
    m_depth = 1.0f;
    m_mode = 0;

    int body = -1;
    int pickedSketch = -1;
    for (const auto& e : ctx.selection.getSelection()) {
        if (e.type == SelectionType::Face && e.bodyId >= 0 &&
            !e.shape.IsNull() && body < 0) {
            m_face = TopoDS::Face(e.shape);
            body = e.bodyId;
        }
        // Regions Ctrl+clicked beforehand narrow the projection to just
        // those; all of them must come from one sketch.
        if (e.type == SelectionType::SketchRegion && e.sketchId >= 0) {
            if (pickedSketch < 0) pickedSketch = e.sketchId;
            if (e.sketchId == pickedSketch)
                m_regionFilter.push_back(e.subShapeIndex);
        }
    }
    if (body < 0) return -1;

    m_sketchIds = ctx.doc.getAllSketchIds();
    if (m_sketchIds.empty()) {
        std::fprintf(stderr, "[ProjectSketch] no sketches in document\n");
        return -1;
    }
    // Default to the selection's sketch, else the newest one.
    m_sketchPick = static_cast<int>(m_sketchIds.size()) - 1;
    if (pickedSketch >= 0) {
        for (size_t i = 0; i < m_sketchIds.size(); ++i)
            if (m_sketchIds[i] == pickedSketch)
                m_sketchPick = static_cast<int>(i);
    }
    return body;
}

std::unique_ptr<Operation> ProjectSketchController::buildOp(
    const IopContext&) {
    if (m_face.IsNull() || m_sketchIds.empty() || m_depth < 0.01f)
        return nullptr;
    auto op = std::make_unique<ProjectSketchOp>();
    op->setBody(bodyId());
    op->setTargetFace(m_face);
    op->setSketchId(m_sketchIds[m_sketchPick]);
    op->setRegionFilter(m_regionFilter);
    op->setDepth(static_cast<double>(m_depth));
    op->setMode(m_mode == 1 ? ProjectSketchOp::Mode::Emboss
                            : ProjectSketchOp::Mode::Engrave);
    return op;
}

void ProjectSketchController::panelBody(const IopContext& ctx,
                                        bool& changed) {
    ImGui::TextDisabled("Projects the sketch onto this face along the\n"
                        "sketch's normal, then cuts in or raises out.");
    ImGui::TextWrapped("Click the sketch elements you want projected — click "
                       "each to add or remove. Use Select all / Clear below.");

    // Live region scoping: clicking sketch regions in the viewport while this
    // panel is open narrows the projection to just those (each click toggles —
    // no modifier needed while this step is active); clicking empty space goes
    // back to the whole sketch. A clicked region also drives the sketch choice,
    // so picking "the relevant sketch" is literally clicking it.
    {
        int selSketch = -1;
        std::vector<int> live;
        for (const auto& e : ctx.selection.getSelection()) {
            if (e.type != SelectionType::SketchRegion || e.sketchId < 0)
                continue;
            if (selSketch < 0) selSketch = e.sketchId;
            if (e.sketchId == selSketch)
                live.push_back(e.subShapeIndex);
        }
        if (selSketch >= 0 &&
            selSketch != m_sketchIds[m_sketchPick]) {
            for (size_t i = 0; i < m_sketchIds.size(); ++i) {
                if (m_sketchIds[i] == selSketch) {
                    m_sketchPick = static_cast<int>(i);
                    changed = true;
                }
            }
        }
        std::sort(live.begin(), live.end());
        std::vector<int> cur = m_regionFilter;
        std::sort(cur.begin(), cur.end());
        if (live != cur) {
            m_regionFilter = live;
            changed = true;
        }
    }

    std::string current =
        ctx.doc.getSketchName(m_sketchIds[m_sketchPick]);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##projSketch", current.c_str())) {
        for (size_t i = 0; i < m_sketchIds.size(); ++i) {
            ImGui::PushID(static_cast<int>(i)); // names may repeat
            bool sel = static_cast<int>(i) == m_sketchPick;
            std::string label = ctx.doc.getSketchName(m_sketchIds[i]);
            if (ImGui::Selectable(label.c_str(), sel)) {
                if (static_cast<int>(i) != m_sketchPick) {
                    m_sketchPick = static_cast<int>(i);
                    m_regionFilter.clear(); // filter was for the old sketch
                    changed = true;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    // Select all → then click the few you DON'T want to drop them (easier than
    // hand-picking every letter of a long inscription). Clear → back to none.
    if (ImGui::SmallButton("Select all")) {
        if (auto sk = ctx.doc.getSketch(m_sketchIds[m_sketchPick])) {
            const int sid = m_sketchIds[m_sketchPick];
            const int n = static_cast<int>(sk->buildRegions().size());
            for (int i = 0; i < n; ++i) {
                bool already = false;
                for (const auto& s : ctx.selection.getSelection())
                    if (s.type == SelectionType::SketchRegion &&
                        s.sketchId == sid && s.subShapeIndex == i) {
                        already = true;
                        break;
                    }
                if (already) continue;
                SelectionEntry e;
                e.type = SelectionType::SketchRegion;
                e.sketchId = sid;
                e.subShapeIndex = i;
                ctx.selection.toggleSelection(e); // adds (absent after the check)
            }
            changed = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        ctx.selection.clear();
        changed = true;
    }
    // Smart guess: auto-nesting of a dense logo is imperfect, so cycle the
    // selection through loops-only / islands-only / all. One press usually
    // lands close to what you want (loops-only = letters, counters hollow);
    // fix the stragglers by clicking. An "island" is a region whose interior
    // sits inside another region's solid (a counter that should be a hole).
    if (ImGui::SmallButton("Cycle loops/islands")) {
        if (auto sk = ctx.doc.getSketch(m_sketchIds[m_sketchPick])) {
            auto regions = sk->buildRegions();
            const int sid = m_sketchIds[m_sketchPick];
            std::vector<bool> island(regions.size(), false);
            for (size_t i = 0; i < regions.size(); ++i)
                for (size_t j = 0; j < regions.size(); ++j)
                    if (i != j && sk->isPointInRegion(
                                      regions[j], regions[i].representativePoint)) {
                        island[i] = true;
                        break;
                    }
            m_cycleMode = (m_cycleMode + 1) % 3; // press 1=loops, 2=islands, 3=all
            ctx.selection.clear();
            for (size_t i = 0; i < regions.size(); ++i) {
                const bool want = m_cycleMode == 0 ||
                                  (m_cycleMode == 1 && !island[i]) ||
                                  (m_cycleMode == 2 && island[i]);
                if (!want) continue;
                SelectionEntry e;
                e.type = SelectionType::SketchRegion;
                e.sketchId = sid;
                e.subShapeIndex = static_cast<int>(i);
                ctx.selection.toggleSelection(e);
            }
            changed = true;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", m_cycleMode == 0 ? "all"
                              : m_cycleMode == 1 ? "loops" : "islands");
    if (!m_regionFilter.empty()) {
        ImGui::TextDisabled("%d region(s) selected - click any to add or\n"
                            "remove. Use Clear to reset.",
                            static_cast<int>(m_regionFilter.size()));
    } else {
        ImGui::TextDisabled("All regions. Click elements to project only\n"
                            "those (click each to add or remove).");
    }

    if (!wantsLivePreview(ctx)) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                           "%d regions - live preview is off here.\n"
                           "Confirm to apply (may take a moment).",
                           effectiveRegionCount(ctx));
    }

    if (ImGui::RadioButton("Engrave", &m_mode, 0)) changed = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("Emboss", &m_mode, 1)) changed = true;

    if (ImGui::SliderFloat("##projDepth", &m_depth, 0.1f, 10.0f,
                           "%.2f mm")) {
        changed = true;
    }

    if (!previewOk()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                           "Projection failed - the selected region(s) couldn't\n"
                           "be applied (off the face, or too thin/degenerate).");
    }
}

void ProjectSketchController::onCleanup() {
    m_face.Nullify();
    m_sketchIds.clear();
    m_regionFilter.clear();
}

int ProjectSketchController::effectiveRegionCount(const IopContext& ctx) const {
    if (m_sketchIds.empty()) return 0;
    if (!m_regionFilter.empty()) return static_cast<int>(m_regionFilter.size());
    if (auto sk = ctx.doc.getSketch(m_sketchIds[m_sketchPick]))
        return static_cast<int>(sk->buildRegions().size());
    return 0;
}

bool ProjectSketchController::wantsLivePreview(const IopContext& ctx) const {
    return effectiveRegionCount(ctx) <= kPreviewRegionCap;
}

// ─── Scale Face ──────────────────────────────────────────────────────────────

int ScaleFaceController::onBegin(const IopContext& ctx) {
    int body = -1;
    m_face.Nullify();
    for (const auto& e : ctx.selection.getSelection()) {
        if (e.type == SelectionType::Face && e.bodyId >= 0 &&
            !e.shape.IsNull()) {
            m_face = TopoDS::Face(e.shape);
            body = e.bodyId;
            break;
        }
    }
    if (body < 0 || m_face.IsNull()) return -1;

    m_pctU = m_pctV = 30.0f;
    m_uniform = true;
    m_dragAxis = -1;
    m_mode = 1; // Pinch: "scale this face, the body follows"
    m_len = 10.0f;
    m_lenMax = 100.0f;
    try {
        TopoDS_Shape bodyShape = ctx.doc.getBody(body);

        BRepGProp_Face gpf(m_face);
        double u1, u2, v1, v2;
        gpf.Bounds(u1, u2, v1, v2);
        gp_Pnt onFace;
        gp_Vec nv;
        gpf.Normal(0.5 * (u1 + u2), 0.5 * (v1 + v2), onFace, nv);
        Bnd_Box bb;
        BRepBndLib::Add(bodyShape, bb);
        if (nv.Magnitude() > 1e-9 && !bb.IsVoid()) {
            gp_Dir n(nv);
            double x0, y0, z0, x1, y1, z1;
            bb.Get(x0, y0, z0, x1, y1, z1);
            gp_Pnt corners[8] = {
                gp_Pnt(x0, y0, z0), gp_Pnt(x1, y0, z0),
                gp_Pnt(x0, y1, z0), gp_Pnt(x1, y1, z0),
                gp_Pnt(x0, y0, z1), gp_Pnt(x1, y0, z1),
                gp_Pnt(x0, y1, z1), gp_Pnt(x1, y1, z1)};
            double depth = 0.0;
            for (const auto& c : corners) {
                double d = gp_Vec(c, onFace).Dot(gp_Vec(n));
                depth = std::max(depth, d);
            }
            if (depth > 1e-3) {
                // Default = the FULL depth of the body behind the face, so
                // scaling a box top re-slopes the sides from the BASE.
                m_lenMax = static_cast<float>(depth);
                m_len = m_lenMax;
            }
        }
        // Gizmo frame: the face plane's own axes + the face's half-extents
        // along them. COPY the plane — Pln() returns a temporary, and a
        // reference into it dangles (the red-line-to-infinity bug).
        Handle(Geom_Plane) gpl =
            Handle(Geom_Plane)::DownCast(BRep_Tool::Surface(m_face));
        if (!gpl.IsNull()) {
            const gp_Pln fpln = gpl->Pln();
            const gp_Ax3& fax = fpln.Position();
            gp_Dir ud = fax.XDirection(), vd2 = fax.YDirection();
            m_axisU = glm::vec3((float)ud.X(), (float)ud.Y(), (float)ud.Z());
            m_axisV = glm::vec3((float)vd2.X(), (float)vd2.Y(),
                                (float)vd2.Z());
            GProp_GProps fpr;
            BRepGProp::SurfaceProperties(m_face, fpr);
            gp_Pnt fc = fpr.CentreOfMass();
            m_center = glm::vec3((float)fc.X(), (float)fc.Y(), (float)fc.Z());
            Bnd_Box fbb;
            BRepBndLib::Add(m_face, fbb);
            if (!fbb.IsVoid()) {
                double fx0, fy0, fz0, fx1, fy1, fz1;
                fbb.Get(fx0, fy0, fz0, fx1, fy1, fz1);
                gp_Pnt fcs[8] = {
                    gp_Pnt(fx0, fy0, fz0), gp_Pnt(fx1, fy0, fz0),
                    gp_Pnt(fx0, fy1, fz0), gp_Pnt(fx1, fy1, fz0),
                    gp_Pnt(fx0, fy0, fz1), gp_Pnt(fx1, fy0, fz1),
                    gp_Pnt(fx0, fy1, fz1), gp_Pnt(fx1, fy1, fz1)};
                float hu = 1.0f, hv = 1.0f;
                for (const auto& cpt : fcs) {
                    glm::vec3 d((float)cpt.X() - m_center.x,
                                (float)cpt.Y() - m_center.y,
                                (float)cpt.Z() - m_center.z);
                    hu = std::max(hu, std::abs(glm::dot(d, m_axisU)));
                    hv = std::max(hv, std::abs(glm::dot(d, m_axisV)));
                }
                m_halfU = hu;
                m_halfV = hv;
            }
        }
    } catch (...) {}
    return body;
}

std::unique_ptr<Operation> ScaleFaceController::buildOp(const IopContext&) {
    auto op = std::make_unique<ScaleFaceOp>();
    op->setBody(bodyId());
    op->setFace(m_face);
    op->setScaleUV(static_cast<double>(m_pctU), static_cast<double>(m_pctV));
    op->setLength(static_cast<double>(m_len));
    op->setMode(m_mode == 1 ? ScaleFaceOp::Mode::Pinch
                            : ScaleFaceOp::Mode::Extend);
    return op;
}

void ScaleFaceController::applyHandleDrag(int axis, float dPct,
                                          const IopContext& ctx) {
    float& pct = (axis == 0) ? m_pctU : m_pctV;
    pct = std::min(200.0f, std::max(5.0f, pct + dPct));
    if (m_uniform) {
        m_pctU = pct;
        m_pctV = pct;
    }
    update(ctx);
}

void ScaleFaceController::panelBody(const IopContext&, bool& changed) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
    ImGui::TextDisabled("Scale this face; the body re-slopes to follow. "
                        "Full length = sides follow from the base; shorter "
                        "= blend only near the face.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    if (previewOk()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
                           "Previewing %.0f%% x %.0f%% over %.1f mm",
                           m_pctU, m_pctV, m_len);
    } else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                           "No preview: needs a FLAT end face (and 100%% is "
                           "a no-op). Try another face or tweak values.");
        ImGui::PopTextWrapPos();
    }

    if (ImGui::Checkbox("Uniform", &m_uniform) && m_uniform) {
        m_pctV = m_pctU;
        changed = true;
    }
    if (m_uniform) {
        if (ImGui::SliderFloat("Scale", &m_pctU, 5.0f, 200.0f, "%.0f %%")) {
            m_pctV = m_pctU;
            changed = true;
        }
    } else {
        // Each slider's text label is shown ABOVE the bar in the colour of
        // the matching face-gizmo arrow, with a hidden "##" slider label.
        // (Steve: "U" and "V" mean nothing here, and the narrow panel was
        //  truncating "Scale U" / "Scale V" to just "S" at the right edge.)
        const ImVec4 redCol (0.92f, 0.35f, 0.35f, 1.0f); // matches the red arrow
        const ImVec4 blueCol(0.35f, 0.59f, 0.92f, 1.0f); // matches the blue arrow
        ImGui::TextColored(redCol, "Red line");
        if (ImGui::SliderFloat("##scaleU", &m_pctU, 5.0f, 200.0f, "%.0f %%"))
            changed = true;
        ImGui::TextColored(blueCol, "Blue line");
        if (ImGui::SliderFloat("##scaleV", &m_pctV, 5.0f, 200.0f, "%.0f %%"))
            changed = true;
    }
    ImGui::TextDisabled("Or drag the two arrows on the face.");
    if (ImGui::SliderFloat("Length", &m_len, 0.5f,
                           std::max(m_lenMax, 1.0f), "%.1f mm"))
        changed = true;
    ImGui::Text("Mode");
    ImGui::SameLine();
    if (ImGui::RadioButton("Extend tip", m_mode == 0)) {
        m_mode = 0;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Pinch existing", m_mode == 1)) {
        m_mode = 1;
        changed = true;
    }
}

void ScaleFaceController::onCleanup() {
    m_face.Nullify();
    m_dragAxis = -1;
}

} // namespace materializr

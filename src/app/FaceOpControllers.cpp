#include "FaceOpControllers.h"
#include "UserAxes.h"
#include "../core/Document.h"
#include "../core/SelectionManager.h"
#include "../modeling/ShellOp.h"
#include "../modeling/TaperOp.h"
#include "../modeling/ScaleFaceOp.h"
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

void ShellController::panelBody(const IopContext&, bool& changed) {
    ImGui::TextDisabled("Hollows the body and removes the picked face.");

    if (m_inputFocus) {
        ImGui::SetKeyboardFocusHere();
        m_inputFocus = false;
    }
    ImGui::SetNextItemWidth(140);
    if (ImGui::InputText("##shellThickness", m_inputBuf, sizeof(m_inputBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue |
                         ImGuiInputTextFlags_CharsDecimal)) {
        m_thickness = static_cast<float>(std::atof(m_inputBuf));
        requestCommit();
    } else {
        float parsed = static_cast<float>(std::atof(m_inputBuf));
        if (std::abs(parsed - m_thickness) > 0.001f) {
            m_thickness = parsed;
            changed = true;
        }
    }
    ImGui::SameLine();
    ImGui::Text("mm");

    if (ImGui::SliderFloat("##shellSlider", &m_thickness, 0.1f, 20.0f,
                           "%.2f mm")) {
        std::snprintf(m_inputBuf, sizeof(m_inputBuf), "%.2f", m_thickness);
        changed = true;
    }
}

void ShellController::onCleanup() { m_face.Nullify(); }

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
        if (ImGui::SliderFloat("Scale U", &m_pctU, 5.0f, 200.0f, "%.0f %%"))
            changed = true;
        if (ImGui::SliderFloat("Scale V", &m_pctV, 5.0f, 200.0f, "%.0f %%"))
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

#include "MoveFaceOp.h"
#include "SubShapeIndex.h"
#include "Sketch.h"
#include <gp_Trsf.hxx>
#include <gp_Pln.hxx>

#include <imgui.h>
#include <cstdio>
#include <cstring>

#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp_Face.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs.hxx>
#include <TopoDS.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Pnt.hxx>
#include <Standard_ErrorHandler.hxx>
#include <Standard_Failure.hxx>
#include <algorithm>

bool MoveFaceOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_face.IsNull() || m_move.Magnitude() < 1e-6) return false;

    try {
        OCC_CATCH_SIGNALS // turn an OCCT kernel fault here into a catch below
        m_previousShape = doc.getBody(m_bodyId);
        if (m_previousShape.IsNull()) return false;

        // Outward face normal N and a point P0 on the face (BRepGProp_Face's
        // Normal is orientation-corrected, so it points out of the body).
        BRepGProp_Face prop(m_face);
        double u1, u2, v1, v2;
        prop.Bounds(u1, u2, v1, v2);
        gp_Pnt c; gp_Vec nv;
        prop.Normal((u1 + u2) * 0.5, (v1 + v2) * 0.5, c, nv);
        if (nv.Magnitude() < 1e-9) return false;
        gp_Vec N = nv.Normalized();
        gp_Vec P0(c.X(), c.Y(), c.Z());

        // Keep the move strictly in the face's plane (slide, never push/pull).
        gp_Vec V = m_move - N * m_move.Dot(N);
        if (V.Magnitude() < 1e-6) return false; // nothing to slide

        // Body extent along N. The shear pins the far end (min) and shifts the
        // selected face's plane (near max) by V, linear in between.
        Bnd_Box bb;
        BRepBndLib::Add(m_previousShape, bb);
        if (bb.IsVoid()) return false;
        double x1, y1, z1, x2, y2, z2;
        bb.Get(x1, y1, z1, x2, y2, z2);
        double minH = 1e300, maxH = -1e300;
        for (int ix = 0; ix < 2; ++ix)
            for (int iy = 0; iy < 2; ++iy)
                for (int iz = 0; iz < 2; ++iz) {
                    gp_Vec corner(ix ? x2 : x1, iy ? y2 : y1, iz ? z2 : z1);
                    double h = (corner - P0).Dot(N);
                    minH = std::min(minH, h);
                    maxH = std::max(maxH, h);
                }
        double L = maxH - minH;
        if (L < 1e-6) return false;

        // shift(Q) = V * ((Q-P0)·N - minH) / L  — an affine map.
        //   linear M = I + (1/L) V⊗N ;  translation T_i = V_i * (-(P0·N + minH)/L)
        const double invL = 1.0 / L;
        const double tConst = -invL * (P0.Dot(N) + minH);
        const double Vc[3] = { V.X(), V.Y(), V.Z() };
        const double Nc[3] = { N.X(), N.Y(), N.Z() };
        gp_GTrsf gt;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j)
                gt.SetValue(i + 1, j + 1, (i == j ? 1.0 : 0.0) + invL * Vc[i] * Nc[j]);
            gt.SetValue(i + 1, 4, Vc[i] * tConst);
        }

        BRepBuilderAPI_GTransform xf(m_previousShape, gt, Standard_True);
        if (!xf.IsDone()) return false;
        TopoDS_Shape result = xf.Shape();

        // Affine maps preserve topology, but validate anyway (degenerate input).
        if (result.IsNull() || !BRepCheck_Analyzer(result).IsValid()) {
            std::fprintf(stderr, "[MoveFace] sheared result invalid — refusing\n");
            return false;
        }
        int nsolids = 0;
        for (TopExp_Explorer sx(result, TopAbs_SOLID); sx.More(); sx.Next()) ++nsolids;
        if (nsolids < 1) return false;

        m_resultShape = result;
        doc.updateBody(m_bodyId, result);

        // Slide on-face sketches by the same in-plane move (translate their
        // plane), so they stay glued to the moved face instead of floating.
        m_appliedMove = V;
        gp_Trsf slide;
        slide.SetTranslation(V);
        for (int sid : m_sketchIds) {
            if (auto sk = doc.getSketch(sid)) {
                gp_Pln pln = sk->getPlane();
                pln.Transform(slide);
                sk->setPlane(pln);
                // The cached host face is used to build the sketch's regions —
                // move it too (copy=true forces a fresh TShape so the region
                // cache, keyed on it, invalidates), or its stale geometry
                // highlights at the OLD position when the region is clicked.
                TopoDS_Face sf = sk->getSourceFace();
                if (!sf.IsNull()) {
                    TopoDS_Shape mv = BRepBuilderAPI_Transform(sf, slide, Standard_True).Shape();
                    if (!mv.IsNull() && mv.ShapeType() == TopAbs_FACE)
                        sk->setSourceFace(TopoDS::Face(mv));
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool MoveFaceOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) return false;
    try {
        doc.updateBody(m_bodyId, m_previousShape);
        // Slide the on-face sketches back to where they were.
        gp_Trsf back;
        back.SetTranslation(m_appliedMove.Reversed());
        for (int sid : m_sketchIds) {
            if (auto sk = doc.getSketch(sid)) {
                gp_Pln pln = sk->getPlane();
                pln.Transform(back);
                sk->setPlane(pln);
                TopoDS_Face sf = sk->getSourceFace();
                if (!sf.IsNull()) {
                    TopoDS_Shape mv = BRepBuilderAPI_Transform(sf, back, Standard_True).Shape();
                    if (!mv.IsNull() && mv.ShapeType() == TopAbs_FACE)
                        sk->setSourceFace(TopoDS::Face(mv));
                }
            }
        }
        return true;
    } catch (...) { return false; }
}

std::string MoveFaceOp::description() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Move Face by (%.2f, %.2f, %.2f)",
                  m_move.X(), m_move.Y(), m_move.Z());
    return buf;
}

void MoveFaceOp::renderProperties() {
    ImGui::Text("Move Face");
    ImGui::Separator();
    ImGui::Text("Body ID: %d", m_bodyId);
    ImGui::Text("Move: (%.2f, %.2f, %.2f) mm", m_move.X(), m_move.Y(), m_move.Z());
}

OperationDiff MoveFaceOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}

std::string MoveFaceOp::serializeParams() const {
    std::string blob;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "body=%d;mx=%.6f;my=%.6f;mz=%.6f",
                  m_bodyId, m_move.X(), m_move.Y(), m_move.Z());
    blob += buf;
    if (!m_previousShape.IsNull() && !m_face.IsNull()) {
        std::vector<TopoDS_Shape> faces{m_face};
        std::string idx = SubShapeIndex::serialize(m_previousShape, faces, TopAbs_FACE);
        if (!idx.empty()) blob += ";face=" + idx;
    }
    return blob;
}

bool MoveFaceOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "body") { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "mx")   { m_move.SetX(std::atof(val.c_str())); any = true; }
        else if (key == "my")   { m_move.SetY(std::atof(val.c_str())); any = true; }
        else if (key == "mz")   { m_move.SetZ(std::atof(val.c_str())); any = true; }
        else if (key == "face") { m_faceIndices = SubShapeIndex::parse(val); any = true; }
        pos = end + 1;
    }
    return any;
}

bool MoveFaceOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0 || m_faceIndices.empty()) return false;
    m_previousShape.Nullify();
    m_resultShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    for (const auto& [id, shp] : state.modifiedAfter)
        if (id == m_bodyId) { m_resultShape = shp; break; }
    if (m_previousShape.IsNull()) return false;

    std::vector<TopoDS_Shape> resolved;
    if (!SubShapeIndex::resolveAll(m_previousShape, m_faceIndices, TopAbs_FACE, resolved) ||
        resolved.empty()) {
        return false;
    }
    m_face = TopoDS::Face(resolved.front());
    return true;
}

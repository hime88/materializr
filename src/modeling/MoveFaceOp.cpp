#include "MoveFaceOp.h"
#include "SubShapeIndex.h"
#include "Sketch.h"
#include <gp_Trsf.hxx>
#include <gp_Pln.hxx>

#include <imgui.h>
#include <cstdio>
#include <cstring>

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <gp_Ax1.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <Geom_Surface.hxx>
#include <Geom_Plane.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <cmath>
#include <gp_Pnt.hxx>
#include <Standard_ErrorHandler.hxx>
#include <Standard_Failure.hxx>
#include <algorithm>

bool MoveFaceOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_face.IsNull()) return false;
    // Nothing-to-do guard, per kind.
    if (m_kind == Kind::Translate && m_move.Magnitude() < 1e-6) return false;
    if (m_kind == Kind::Rotate    && std::abs(m_rotAngle) < 1e-6) return false;
    if (m_kind == Kind::Scale     && std::abs(m_scaleFactor - 1.0) < 1e-6) return false;

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

        // Pivot = the face's area centroid (the "invisible point in the middle"
        // that Rotate/Scale turn/scale about).
        gp_Pnt pivot;
        {
            GProp_GProps gp; BRepGProp::SurfaceProperties(m_face, gp);
            pivot = gp.CentreOfMass();
        }

        // The single transform applied to the moving (top) loops — translate,
        // rotate-about-pivot, or scale-about-pivot. Everything downstream (loft,
        // sketch follow, undo) just applies this one gp_Trsf.
        gp_Vec V = m_move - N * m_move.Dot(N); // in-plane slide (Translate)
        gp_Trsf topT;
        switch (m_kind) {
            case Kind::Translate:
                if (V.Magnitude() < 1e-6) return false;
                topT.SetTranslation(V);
                break;
            case Kind::Rotate:
                topT.SetRotation(gp_Ax1(pivot, m_rotAxis), m_rotAngle);
                break;
            case Kind::Scale:
                topT.SetScale(pivot, m_scaleFactor);
                break;
        }

        // LOFT REBUILD (replaces the old whole-body gp_GTrsf shear, which made
        // OCCT NURBS-convert the entire body and segfault on freeform/boolean
        // geometry). A prism is bounded by the selected face and an OPPOSITE
        // PARALLEL face. We loft a capped solid between the base OUTER wire and
        // the moved top outer wire, then subtract a loft of each HOLE loop
        // (base inner -> moved top inner). Every loop moves by V here (the whole
        // face slides, holes included); per-loop control (move a hole on its
        // own) layers on top later. All geometry is local wires — no whole-body
        // convert, so it survives bodies the shear crashed on. Non-prism bodies
        // refuse here instead of crashing.

        // Find the opposite parallel planar face (the prism's far cap).
        TopoDS_Face baseFace;
        double bestDist = -1e300;
        for (TopExp_Explorer fx(m_previousShape, TopAbs_FACE); fx.More(); fx.Next()) {
            TopoDS_Face f = TopoDS::Face(fx.Current());
            if (f.IsSame(m_face)) continue;
            Handle(Geom_Surface) s = BRep_Tool::Surface(f);
            if (s.IsNull() || !s->IsKind(STANDARD_TYPE(Geom_Plane))) continue;
            BRepGProp_Face fp(f);
            double uu1, uu2, vv1, vv2; fp.Bounds(uu1, uu2, vv1, vv2);
            gp_Pnt fc; gp_Vec fn; fp.Normal((uu1 + uu2) * 0.5, (vv1 + vv2) * 0.5, fc, fn);
            if (fn.Magnitude() < 1e-9) continue;
            if (fn.Normalized().Dot(N) > -0.999) continue; // not antiparallel
            double dist = -(gp_Vec(fc.X(), fc.Y(), fc.Z()) - P0).Dot(N);
            if (dist > bestDist) { bestDist = dist; baseFace = f; }
        }
        if (baseFace.IsNull()) {
            std::fprintf(stderr, "[MoveFace] refused: no opposite face to loft from\n");
            return false;
        }

        // Split each cap into its outer wire + inner (hole) loops.
        auto collect = [](const TopoDS_Face& f, TopoDS_Wire& outer,
                          std::vector<TopoDS_Wire>& inners) {
            outer = BRepTools::OuterWire(f);
            for (TopExp_Explorer wx(f, TopAbs_WIRE); wx.More(); wx.Next()) {
                TopoDS_Wire w = TopoDS::Wire(wx.Current());
                if (!w.IsSame(outer)) inners.push_back(w);
            }
        };
        auto centroid = [](const TopoDS_Wire& w) {
            gp_XYZ acc(0, 0, 0); int n = 0;
            for (TopExp_Explorer vx(w, TopAbs_VERTEX); vx.More(); vx.Next()) {
                acc += BRep_Tool::Pnt(TopoDS::Vertex(vx.Current())).XYZ(); ++n;
            }
            if (n) acc /= n;
            return gp_Pnt(acc);
        };

        TopoDS_Wire topOuter, baseOuter;
        std::vector<TopoDS_Wire> topInners, baseInners;
        collect(m_face, topOuter, topInners);
        collect(baseFace, baseOuter, baseInners);
        if (topOuter.IsNull() || baseOuter.IsNull()) return false;

        auto moved = [&](const TopoDS_Wire& w) {
            return TopoDS::Wire(BRepBuilderAPI_Transform(w, topT, Standard_True).Shape());
        };
        auto loftSolid = [](TopoDS_Wire a, TopoDS_Wire b, TopoDS_Shape& out) -> bool {
            BRepOffsetAPI_ThruSections ts(Standard_True /*solid*/, Standard_True /*ruled*/);
            ts.AddWire(a); ts.AddWire(b); ts.Build();
            if (!ts.IsDone()) return false;
            out = ts.Shape();
            return !out.IsNull();
        };

        // A wire's lofted end is the MOVED one only if that ring slides.
        auto endFor = [&](const TopoDS_Wire& w, bool slides) {
            return slides ? moved(w) : w;
        };

        // Outer prism: base outer (reversed to match orientation) -> top
        // (moved iff the outline slides).
        TopoDS_Wire bO = baseOuter; bO.Reverse();
        TopoDS_Shape result;
        if (!loftSolid(bO, endFor(topOuter, m_moveOuter), result)) {
            std::fprintf(stderr, "[MoveFace] outer loft failed — refusing\n");
            return false;
        }

        // Subtract a loft for each hole. Its TOP ring rides the face (moves when
        // the outline moves) OR moves on its own when the hole is vertical; its
        // BOTTOM ring moves only when the hole is vertical (a straight tube).
        for (size_t hi = 0; hi < topInners.size(); ++hi) {
            const TopoDS_Wire& tw = topInners[hi];
            bool slant    = (hi < m_holeSlant.size())    && m_holeSlant[hi];
            bool vertical = (hi < m_holeVertical.size()) && m_holeVertical[hi];
            // On a TILT the top ring is part of the rotating face — it must ride
            // it (stay coplanar) or the face can't close (the "half cover" bug);
            // the hole then continues as a slanted tube. On Move/Scale the ring
            // can stay put (three-state), so opt-in only.
            bool topRidesFace = m_moveOuter && m_kind == Kind::Rotate;
            bool topSlides = topRidesFace || slant || vertical;
            bool botSlides = vertical;
            gp_Pnt tc = centroid(tw);
            int best = -1; double bd = 1e300;
            for (size_t i = 0; i < baseInners.size(); ++i) {
                gp_Pnt bc = centroid(baseInners[i]);
                gp_Vec dv(bc.X() - tc.X(), bc.Y() - tc.Y(), bc.Z() - tc.Z());
                double d = (dv - N * dv.Dot(N)).Magnitude(); // in-plane only
                if (d < bd) { bd = d; best = static_cast<int>(i); }
            }
            if (best < 0) continue;
            TopoDS_Wire bi = baseInners[best]; bi.Reverse();
            TopoDS_Shape holeSolid;
            if (!loftSolid(endFor(bi, botSlides), endFor(tw, topSlides), holeSolid))
                continue;
            BRepAlgoAPI_Cut cut(result, holeSolid);
            if (cut.IsDone() && !cut.Shape().IsNull()) result = cut.Shape();
        }

        if (result.IsNull() || !BRepCheck_Analyzer(result).IsValid()) {
            std::fprintf(stderr, "[MoveFace] lofted result invalid — refusing\n");
            return false;
        }
        int nsolids = 0;
        for (TopExp_Explorer sx(result, TopAbs_SOLID); sx.More(); sx.Next()) ++nsolids;
        if (nsolids < 1) return false;

        m_resultShape = result;
        doc.updateBody(m_bodyId, result);

        // Move on-face sketches by the SAME transform (slide / tilt / scale), so
        // they stay glued to the face — but only when the face OUTLINE moves
        // (sketches ride the face, not a hole). Stored for undo.
        m_appliedXform = m_moveOuter ? topT : gp_Trsf();
        if (m_moveOuter)
        for (int sid : m_sketchIds) {
            if (auto sk = doc.getSketch(sid)) {
                gp_Pln pln = sk->getPlane();
                pln.Transform(m_appliedXform);
                sk->setPlane(pln);
                // The cached host face is used to build the sketch's regions —
                // move it too (copy=true forces a fresh TShape so the region
                // cache, keyed on it, invalidates), or its stale geometry
                // highlights at the OLD position when the region is clicked.
                TopoDS_Face sf = sk->getSourceFace();
                if (!sf.IsNull()) {
                    TopoDS_Shape mv = BRepBuilderAPI_Transform(sf, m_appliedXform, Standard_True).Shape();
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
        // Move the on-face sketches back by the inverse transform.
        gp_Trsf back = m_appliedXform.Inverted();
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

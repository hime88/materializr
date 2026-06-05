#include "ThreadOp.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <algorithm>
#include <vector>
#include <Geom_CylindricalSurface.hxx>
#include <Geom2d_Line.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepLib.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <Geom2d_Ellipse.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <GCE2d_MakeSegment.hxx>
#include <gp_Ax2d.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <ShapeFix_Shape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Dir2d.hxx>
#include <imgui.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

ThreadOp::ThreadOp() = default;

void ThreadOp::setAxis(const gp_Ax2& axis) {
    m_axis = axis;
    m_axOX = axis.Location().X();
    m_axOY = axis.Location().Y();
    m_axOZ = axis.Location().Z();
    m_axDX = axis.Direction().X();
    m_axDY = axis.Direction().Y();
    m_axDZ = axis.Direction().Z();
    m_axXX = axis.XDirection().X();
    m_axXY = axis.XDirection().Y();
    m_axXZ = axis.XDirection().Z();
}

TopoDS_Shape ThreadOp::buildResult(const TopoDS_Shape& body) const {
    if (body.IsNull() || m_pitch <= 0.05 || m_depth <= 0.0 ||
        m_length <= 0.0 || m_radius <= m_depth) {
        return {};
    }
    double turns = m_length / m_pitch;
    // Runaway guard: a 0.1 mm pitch over a long rod would sweep thousands of
    // turns and lock the compute for minutes. 300 turns is far beyond any
    // sane model at this app's scale (+2 below for the runout extensions).
    if (turns > 300.0) return {};

    // Geometric sanity: a depth beyond ~0.65·pitch merges adjacent grooves
    // and leaves paper-thin helical fins instead of crests (ISO depth is
    // 0.6134·P); beyond ~45% of the radius it eats the core. Clamp rather
    // than fail — the UI clamps too, but reloaded files / old params must
    // never produce garbage solids.
    const double depth = std::min({m_depth, 0.65 * m_pitch, 0.45 * m_radius});
    if (depth <= 0.0) return {};

    std::fprintf(stderr, "[Thread] buildResult: pitch=%.3f depth=%.3f r=%.3f "
                         "len=%.3f hole=%d\n",
                 m_pitch, m_depth, m_radius, m_length, m_isHole ? 1 : 0);
    try {
        // Rebuild the axis from the serialisable components (identical for
        // fresh and reloaded ops).
        gp_Pnt loc(m_axOX, m_axOY, m_axOZ);
        gp_Dir zd(m_axDX, m_axDY, m_axDZ);
        gp_Dir xd(m_axXX, m_axXY, m_axXZ);
        gp_Ax3 ax3(loc, zd, xd);

        auto pt = [&](double rad, double dz) {
            return gp_Pnt(loc.X() + zd.X() * dz + xd.X() * rad,
                          loc.Y() + zd.Y() * dz + xd.Y() * rad,
                          loc.Z() + zd.Z() * dz + xd.Z() * rad);
        };

        // ---- Thread runout: extend the helix one turn past each FREE end so
        // the groove runs off the cylinder instead of stopping in a blunt
        // wall. An end is free when a probe point just beyond it (at
        // mid-groove radius) lies outside the body — a rod tip or a hole
        // mouth extends; a boss rooted in a plate or a blind hole bottom
        // stays exact so the cutter can't gouge surrounding material.
        double probeR = m_isHole ? (m_radius + 0.5 * depth)
                                 : (m_radius - 0.5 * depth);
        auto endIsFree = [&](double vBeyond) {
            try {
                BRepClass3d_SolidClassifier cls(body, pt(probeR, vBeyond), 1e-6);
                return cls.State() == TopAbs_OUT;
            } catch (...) { return false; }
        };
        double vLo = endIsFree(-0.6 * m_pitch) ? -0.5 * m_pitch : 0.0;
        double vHi = m_length +
                     (endIsFree(m_length + 0.6 * m_pitch) ? 0.5 * m_pitch : 0.0);
        turns = (vHi - vLo) / m_pitch;
        std::fprintf(stderr, "[Thread] runout: vLo=%.3f vHi=%.3f turns=%.1f\n",
                     vLo, vHi, turns);

        // Build the groove cutter for a helix span [lo, hi] along the axis
        // using the canonical OCCT threading construction (the "bottle
        // tutorial"): ThruSections between two half-ellipse wires drawn in
        // UV space on coaxial cylindrical surfaces. Unlike a MakePipeShell
        // sweep (whose helical solids the boolean classified erratically —
        // four variants, four different wrong answers), these tools cut
        // consistently, and the ellipse tips taper to nothing, giving
        // natural thread runout at both ends.
        // Half-ellipse wire on a cylindrical surface, SEGMENTED per turn:
        // the long UV ellipse is trimmed into ~one-turn arcs (and the seam
        // into matching chunks) so ThruSections lofts a row of tame patches
        // instead of one 30-turn B-spline — the difference between booleans
        // that cut reliably and booleans that silently remove nothing
        // (proven via the headless volume harness on both prism-made and
        // primitive bodies, both handednesses).
        auto ellipseWire = [&](const Handle(Geom_CylindricalSurface)& surf,
                               const gp_Ax2d& ax2d, double major, double minor,
                               int nSeg) -> TopoDS_Wire {
            Handle(Geom2d_Ellipse) ell = new Geom2d_Ellipse(ax2d, major, minor);
            BRepBuilderAPI_MakeWire mw;
            for (int i = 0; i < nSeg; ++i) {
                double t0 = M_PI * i / nSeg, t1 = M_PI * (i + 1) / nSeg;
                Handle(Geom2d_TrimmedCurve) a =
                    new Geom2d_TrimmedCurve(ell, t0, t1);
                TopoDS_Edge e = BRepBuilderAPI_MakeEdge(a, surf).Edge();
                BRepLib::BuildCurves3d(e);
                mw.Add(e);
            }
            gp_Pnt2d p1 = ell->Value(M_PI), p2 = ell->Value(0);
            for (int i = 0; i < nSeg; ++i) {
                gp_Pnt2d a(p1.X() + (p2.X() - p1.X()) * i / nSeg,
                           p1.Y() + (p2.Y() - p1.Y()) * i / nSeg);
                gp_Pnt2d b(p1.X() + (p2.X() - p1.X()) * (i + 1) / nSeg,
                           p1.Y() + (p2.Y() - p1.Y()) * (i + 1) / nSeg);
                Handle(Geom2d_TrimmedCurve) sgm = GCE2d_MakeSegment(a, b).Value();
                TopoDS_Edge e = BRepBuilderAPI_MakeEdge(sgm, surf).Edge();
                BRepLib::BuildCurves3d(e);
                mw.Add(e);
            }
            return mw.Wire();
        };

        auto buildCutter = [&](double lo, double hi) -> TopoDS_Shape {
            try {
                // Outer surface 0.1 mm on the material-free side of the face
                // (clean detachment without near-tangency); inner surface at
                // the groove apex. (For a hole, "outer" is inside the void.)
                double rOut = m_isHole ? (m_radius - 0.10) : (m_radius + 0.10);
                double rIn  = m_isHole ? (m_radius + depth) : (m_radius - depth);
                Handle(Geom_CylindricalSurface) sOut =
                    new Geom_CylindricalSurface(ax3, rOut);
                Handle(Geom_CylindricalSurface) sIn =
                    new Geom_CylindricalSurface(ax3, rIn);

                // One long, thin ellipse in (U, V) space whose major axis runs
                // along the helix; the U sign sets the handedness (chirality
                // is invariant under axis flip, so u+ is right-handed for any
                // face axis orientation).
                double t = (hi - lo) / m_pitch;
                double uMax = 2.0 * M_PI * t;
                double uSign = m_rightHanded ? 1.0 : -1.0;
                gp_Pnt2d centre(uSign * uMax * 0.5, (lo + hi) * 0.5);
                gp_Dir2d along(uSign * 2.0 * M_PI, m_pitch);
                gp_Ax2d ax2d(centre, along);
                double major = 0.5 * std::hypot(uMax, hi - lo);
                double minor = std::min(0.57735 * depth, 0.45 * m_pitch);
                int nSeg = std::max(4, static_cast<int>(std::ceil(t)));

                TopoDS_Wire w1 = ellipseWire(sOut, ax2d, major, minor, nSeg);
                TopoDS_Wire w2 = ellipseWire(sIn, ax2d, major, minor * 0.25, nSeg);

                BRepOffsetAPI_ThruSections tool(Standard_True);
                tool.AddWire(w1);
                tool.AddWire(w2);
                tool.CheckCompatibility(Standard_False);
                tool.Build();
                if (!tool.IsDone()) return {};
                // NOTE: do NOT "fix" the orientation even if GProp reports a
                // negative volume — the integrator mis-reads the helical
                // seam, but the boolean classifies this solid correctly.
                return tool.Shape();
            } catch (...) { return {}; }
        };

        auto tryCut = [&](const TopoDS_Shape& tool) -> TopoDS_Shape {
            if (tool.IsNull()) return {};
            try {
                BRepAlgoAPI_Cut cut;
                TopTools_ListOfShape args, tools;
                args.Append(body);
                tools.Append(tool);
                cut.SetArguments(args);
                cut.SetTools(tools);
                cut.SetFuzzyValue(1.0e-3);
                cut.Build();
                if (!cut.IsDone()) return {};
                TopoDS_Shape res = cut.Shape();
                // The cut result regularly carries tolerance nits — heal it
                // so downstream ops (fillets, further booleans, save) get a
                // clean solid.
                if (!BRepCheck_Analyzer(res).IsValid()) {
                    ShapeFix_Shape fixer(res);
                    fixer.Perform();
                    res = fixer.Shape();
                }
                return res;
            } catch (...) { return {}; }
        };

        std::fprintf(stderr, "[Thread] cutting (span %.2f..%.2f)...\n", vLo, vHi);
        TopoDS_Shape result = tryCut(buildCutter(vLo, vHi));
        if (result.IsNull() && (vLo < 0.0 || vHi > m_length)) {
            std::fprintf(stderr,
                         "[Thread] extended cut failed — retrying exact span\n");
            result = tryCut(buildCutter(0.0, m_length));
        }
        if (result.IsNull()) {
            std::fprintf(stderr, "[Thread] boolean cut FAILED\n");
            return {};
        }
        std::fprintf(stderr, "[Thread] buildResult OK\n");
        return result;
    } catch (...) {
        std::fprintf(stderr, "[Thread] buildResult threw\n");
        return {};
    }
}

bool ThreadOp::execute(Document& doc) {
    if (m_bodyId < 0) return false;
    try {
        m_previousShape = doc.getBody(m_bodyId);
        if (m_previousShape.IsNull()) return false;

        // The popup's worker thread may have already computed the result —
        // consume it; redo / editStep recompute synchronously as usual.
        TopoDS_Shape result;
        if (!m_precomputed.IsNull()) {
            result = m_precomputed;
            m_precomputed.Nullify();
        } else {
            result = buildResult(m_previousShape);
        }
        if (result.IsNull()) return false;

        doc.updateBody(m_bodyId, result);
        return true;
    } catch (...) {
        return false;
    }
}

bool ThreadOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) return false;
    try {
        doc.updateBody(m_bodyId, m_previousShape);
        return true;
    } catch (...) {
        return false;
    }
}

std::string ThreadOp::description() const {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s thread Ø%.1f, pitch %.2f mm%s",
                  m_isHole ? "Internal" : "External",
                  m_radius * 2.0, m_pitch, m_rightHanded ? "" : " (LH)");
    return buf;
}

void ThreadOp::renderProperties() {
    ImGui::Text("%s Thread", m_isHole ? "Internal" : "External");
    ImGui::Separator();
    ImGui::InputDouble("Pitch (mm)", &m_pitch, 0.1, 0.5, "%.2f");
    if (m_pitch < 0.1) m_pitch = 0.1;
    ImGui::InputDouble("Depth (mm)", &m_depth, 0.05, 0.2, "%.2f");
    if (m_depth < 0.05) m_depth = 0.05;
    // Past ~0.65·pitch the grooves merge and shred the crests into floating
    // helical fins (Steve found this empirically — "it's jumping lol").
    double maxDepth = std::min(0.65 * m_pitch, 0.45 * m_radius);
    if (m_depth > maxDepth) m_depth = maxDepth;
    ImGui::TextDisabled("Depth caps at 0.65 \xC3\x97 pitch (ISO is 0.61).");
    bool rh = m_rightHanded;
    if (ImGui::Checkbox("Right-handed", &rh)) m_rightHanded = rh;
    ImGui::Text("Diameter: %.2f mm   Length: %.2f mm", m_radius * 2.0, m_length);
}

OperationDiff ThreadOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}

std::string ThreadOp::serializeParams() const {
    char buf[420];
    std::snprintf(buf, sizeof(buf),
        "body=%d;radius=%.6f;length=%.6f;pitch=%.6f;depth=%.6f;hole=%d;rh=%d;"
        "ox=%.9g;oy=%.9g;oz=%.9g;dx=%.9g;dy=%.9g;dz=%.9g;"
        "xx=%.9g;xy=%.9g;xz=%.9g",
        m_bodyId, m_radius, m_length, m_pitch, m_depth,
        m_isHole ? 1 : 0, m_rightHanded ? 1 : 0,
        m_axOX, m_axOY, m_axOZ, m_axDX, m_axDY, m_axDZ,
        m_axXX, m_axXY, m_axXZ);
    return buf;
}

bool ThreadOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        double d = std::atof(val.c_str());
        int    i = std::atoi(val.c_str());
        if      (key == "body")   { m_bodyId = i; any = true; }
        else if (key == "radius") { m_radius = d; any = true; }
        else if (key == "length") { m_length = d; any = true; }
        else if (key == "pitch")  { m_pitch = d; any = true; }
        else if (key == "depth")  { m_depth = d; any = true; }
        else if (key == "hole")   { m_isHole = (i != 0); any = true; }
        else if (key == "rh")     { m_rightHanded = (i != 0); any = true; }
        else if (key == "ox") { m_axOX = d; any = true; }
        else if (key == "oy") { m_axOY = d; any = true; }
        else if (key == "oz") { m_axOZ = d; any = true; }
        else if (key == "dx") { m_axDX = d; any = true; }
        else if (key == "dy") { m_axDY = d; any = true; }
        else if (key == "dz") { m_axDZ = d; any = true; }
        else if (key == "xx") { m_axXX = d; any = true; }
        else if (key == "xy") { m_axXY = d; any = true; }
        else if (key == "xz") { m_axXZ = d; any = true; }
        pos = end + 1;
    }
    return any;
}

bool ThreadOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0) return false;
    m_previousShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    return !m_previousShape.IsNull();
}

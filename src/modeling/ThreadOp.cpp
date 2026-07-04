#include "ThreadOp.h"
#include "../core/Verbose.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <algorithm>
#include <vector>
#include <Geom_CylindricalSurface.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Ax3.hxx>
#include <gp_Vec.hxx>
#include <Geom2d_Line.hxx>
#include <Geom_Plane.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepBuilderAPI_Transform.hxx>
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
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Face.hxx>
#include <Geom_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <ShapeFix_Shape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Dir2d.hxx>
#include <imgui.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ─── Swept-rod fast path ("the twist idea") ─────────────────────────────────
// For the most common case — an EXTERNAL thread covering a PLAIN full
// cylinder (sketch circle → extrude → thread) — don't cut grooves with a
// boolean at all. Build the threaded rod NATIVELY: sweep the notched
// cross-section along the axis while an auxiliary helix spine twists it
// (BRepOffsetAPI_MakePipeShell curvilinear equivalence). One solid, ~6 smooth
// helicoid faces, validity by construction, and ~200ms where the boolean
// compound/per-turn path took MINUTES on a 35-turn rod (Steve's 14x70).
// Anything else (holes, bolts with heads, partial spans, interrupted
// cylinders) returns null here and takes the proven boolean paths below.
//
// Cross-section (notch centred at phi=0, radius band [R-depth .. R]):
//   root flat  arc  phi in [-45, +45]  at R-depth   (0.25 of the period)
//   flank      arc  rising to the crest edge        (0.25)
//   crest flat arc  phi in [135, 225]  at R         (0.25)
//   flank      arc  falling back to the root        (0.25)
// Flanks are 3-point arcs staying in the band — a straight chord across 90°
// of arc sags to ~0.7R and gouges the rod (probe-proven).
static TopoDS_Shape sweptRodThread(const gp_Ax3& ax3, double R, double len,
                                   double pitch, double depth,
                                   bool rightHanded) {
    try {
        OCC_CATCH_SIGNALS
        const double rr = R - depth;
        auto polar = [&](double r, double phiDeg) {
            double a = phiDeg * M_PI / 180.0;
            return gp_Pnt(r * std::cos(a), r * std::sin(a), 0.0);
        };
        gp_Pnt rootA = polar(rr, -45), rootM = polar(rr, 0), rootB = polar(rr, 45);
        gp_Pnt crestA = polar(R, 135), crestM = polar(R, 180), crestB = polar(R, 225);
        TopoDS_Edge eRoot = BRepBuilderAPI_MakeEdge(
            GC_MakeArcOfCircle(rootA, rootM, rootB).Value()).Edge();
        TopoDS_Edge eUp = BRepBuilderAPI_MakeEdge(
            GC_MakeArcOfCircle(rootB, polar(0.5 * (rr + R), 90), crestA).Value()).Edge();
        TopoDS_Edge eCrest = BRepBuilderAPI_MakeEdge(
            GC_MakeArcOfCircle(crestA, crestM, crestB).Value()).Edge();
        TopoDS_Edge eDown = BRepBuilderAPI_MakeEdge(
            GC_MakeArcOfCircle(crestB, polar(0.5 * (rr + R), 270), rootA).Value()).Edge();
        BRepBuilderAPI_MakeWire mkProfile(eRoot, eUp, eCrest, eDown);
        if (!mkProfile.IsDone()) return {};

        TopoDS_Edge eSpine =
            BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(0, 0, len)).Edge();
        TopoDS_Wire spine = BRepBuilderAPI_MakeWire(eSpine).Wire();

        // Helix on the crest cylinder (2D line in UV space → 3D curve).
        Handle(Geom_CylindricalSurface) cylSurf = new Geom_CylindricalSurface(
            gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), R);
        const double uSign = rightHanded ? 1.0 : -1.0;
        Handle(Geom2d_Line) l2d = new Geom2d_Line(
            gp_Pnt2d(0.0, 0.0), gp_Dir2d(uSign * 2.0 * M_PI, pitch));
        const double turns = len / pitch;
        const double segLen =
            std::sqrt(4.0 * M_PI * M_PI + pitch * pitch) * turns;
        TopoDS_Edge eHelix =
            BRepBuilderAPI_MakeEdge(l2d, cylSurf, 0.0, segLen).Edge();
        BRepLib::BuildCurves3d(eHelix);
        TopoDS_Wire helix = BRepBuilderAPI_MakeWire(eHelix).Wire();

        BRepOffsetAPI_MakePipeShell pipe(spine);
        pipe.SetMode(helix, Standard_True);  // twist follows the helix
        pipe.Add(mkProfile.Wire());
        pipe.Build();
        if (!pipe.IsDone() || !pipe.MakeSolid()) return {};
        TopoDS_Shape rod = pipe.Shape();

        // Move the canonical (origin, +Z) rod onto the op's axis frame.
        gp_Trsf tr;
        tr.SetDisplacement(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1),
                                  gp_Dir(1, 0, 0)), ax3);
        rod = BRepBuilderAPI_Transform(rod, tr, Standard_True).Shape();

        // Guards: valid solid, volume matching the intended profile within
        // 6% (integrated analytically: quarter root flat + quarter crest +
        // two ~linear ramps). Any miss → boolean fallback.
        if (rod.IsNull() || !BRepCheck_Analyzer(rod).IsValid()) return {};
        double A = 0.0;
        const int NI = 720;
        for (int i = 0; i < NI; ++i) {
            double phi = 360.0 * i / NI;
            double r;
            if (phi >= 315 || phi < 45) r = rr;
            else if (phi < 135) r = rr + depth * (phi - 45) / 90.0;
            else if (phi < 225) r = R;
            else r = R - depth * (phi - 225) / 90.0;
            A += 0.5 * r * r * (2.0 * M_PI / NI);
        }
        GProp_GProps g;
        BRepGProp::VolumeProperties(rod, g);
        if (std::abs(g.Mass() - A * len) > 0.06 * A * len) return {};
        return rod;
    } catch (...) {
        return {};
    }
}

// Extract the thread frame from a cylindrical face: axis at the face's V_min
// end (origin = surface location + v0·axis, direction along the cylinder), and
// the radius. Length is deliberately NOT taken from the face — the user's
// chosen thread span is kept; only the cylinder's position and diameter follow
// an edit. Returns false if the face isn't a plain cylinder.
static bool cylFaceToThread(const TopoDS_Face& face, gp_Ax2& ax2, double& radius) {
    Handle(Geom_Surface) gs = BRep_Tool::Surface(face);
    Handle(Geom_CylindricalSurface) cs =
        Handle(Geom_CylindricalSurface)::DownCast(gs);
    if (cs.IsNull()) return false;
    const gp_Cylinder cyl = cs->Cylinder();
    radius = cyl.Radius();
    Standard_Real u0, u1, v0, v1;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    const gp_Ax3 pos = cyl.Position();
    const gp_Dir axisDir = pos.Direction();
    const gp_Pnt origin =
        pos.Location().Translated(gp_Vec(axisDir) * std::min(v0, v1));
    ax2 = gp_Ax2(origin, axisDir, pos.XDirection());
    return true;
}

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

    if (materializr::isVerbose())
        std::fprintf(stderr, "[Thread] buildResult: pitch=%.3f depth=%.3f r=%.3f "
                             "len=%.3f hole=%d\n",
                     m_pitch, m_depth, m_radius, m_length, m_isHole ? 1 : 0);

    // PRE-FLIGHT STRATEGY CHECK. The single compound band tool only cuts
    // reliably against FULL, CLOSED cylinders — after ~40 harness
    // experiments, any boolean between it and a PARTIAL or INTERRUPTED
    // cylinder (the half of a lengthwise split, a rod with a cross-hole)
    // coin-flips between no-ops, inverted removal, and plausible-volume cuts
    // of the WRONG material (the "stacked poker chips" body), and those
    // garbage solids go on to crash the tessellator. Such bodies take the
    // PER-TURN SEQUENTIAL path below instead: one boolean per turn, each
    // validated by removed volume, failed turns retried through a variant
    // ladder (micro-jitters that break the surface coincidences that make
    // the kernel misclassify). Proven in the volume harness: lengthwise
    // halves thread 21/21 turns, holed rods ~20/21.
    bool fullCylinder = false;
    {
        gp_Pnt axLoc(m_axOX, m_axOY, m_axOZ);
        gp_Dir axDir(m_axDX, m_axDY, m_axDZ);
        for (TopExp_Explorer fx(body, TopAbs_FACE);
             fx.More() && !fullCylinder; fx.Next()) {
            TopoDS_Face f = TopoDS::Face(fx.Current());
            Handle(Geom_CylindricalSurface) cs =
                Handle(Geom_CylindricalSurface)::DownCast(BRep_Tool::Surface(f));
            if (cs.IsNull()) continue;
            gp_Cylinder cyl = cs->Cylinder();
            if (std::abs(cyl.Radius() - m_radius) > 1e-4) continue;
            if (std::abs(cyl.Position().Direction().Dot(axDir)) < 0.9999)
                continue;
            // axis line coincidence: location must lie on our axis
            gp_Vec d(axLoc, cyl.Position().Location());
            if (d.Magnitude() > 1e-6 &&
                d.Crossed(gp_Vec(axDir)).Magnitude() > 1e-3) continue;
            double u1, u2, v1, v2;
            BRepTools::UVBounds(f, u1, u2, v1, v2);
            // A face with an inner wire (cross-hole mouth) keeps full outer
            // UV bounds, so "full 2π wrap" here really means "uninterrupted
            // enough for the compound tool to have a chance" — the volume
            // guard in tryCut still arbitrates, and per-turn is the net.
            if (std::abs((u2 - u1) - 2.0 * M_PI) < 1e-3) fullCylinder = true;
        }
    }
    try {
        // Rebuild the axis from the serialisable components (identical for
        // fresh and reloaded ops).
        gp_Pnt loc(m_axOX, m_axOY, m_axOZ);
        gp_Dir zd(m_axDX, m_axDY, m_axDZ);
        gp_Dir xd(m_axXX, m_axXY, m_axXZ);
        gp_Ax3 ax3(loc, zd, xd);

        // ---- Swept-rod fast path: an EXTERNAL thread covering a PLAIN
        // full-cylinder body end to end builds natively (no boolean, ~200ms
        // vs minutes; see sweptRodThread). Detection is strict — exactly one
        // matching full-2pi cylinder + two planar caps perpendicular to the
        // axis, and the thread span covering the body's whole axial extent.
        // Anything else falls through to the proven boolean paths.
        if (!m_isHole && fullCylinder) {
            int nFaces = 0;
            bool shapeOk = true;
            for (TopExp_Explorer fx(body, TopAbs_FACE); fx.More(); fx.Next()) {
                ++nFaces;
                TopoDS_Face f = TopoDS::Face(fx.Current());
                Handle(Geom_Surface) gs = BRep_Tool::Surface(f);
                Handle(Geom_CylindricalSurface) cs =
                    Handle(Geom_CylindricalSurface)::DownCast(gs);
                Handle(Geom_Plane) pl = Handle(Geom_Plane)::DownCast(gs);
                if (!cs.IsNull()) {
                    if (std::abs(cs->Cylinder().Radius() - m_radius) > 1e-4 ||
                        std::abs(cs->Cylinder().Position().Direction()
                                     .Dot(zd)) < 0.9999)
                        shapeOk = false;
                } else if (!pl.IsNull()) {
                    if (std::abs(pl->Pln().Axis().Direction().Dot(zd)) < 0.9999)
                        shapeOk = false;
                } else {
                    shapeOk = false;
                }
            }
            // Axial extent from the body's vertices.
            double hMin = 1e300, hMax = -1e300;
            for (TopExp_Explorer vx(body, TopAbs_VERTEX); vx.More(); vx.Next()) {
                gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()));
                double h = gp_Vec(loc, p).Dot(gp_Vec(zd));
                hMin = std::min(hMin, h);
                hMax = std::max(hMax, h);
            }
            const bool fullSpan = hMin > -0.55 * m_pitch &&
                                  std::abs(hMax - m_length) < 0.55 * m_pitch;
            // MakePipeShell is exact through ~40 turns and degrades beyond
            // (probe: 40 OK, 50 fails to build, 60+ builds garbage the
            // volume guard rejects). Gate rather than waste the attempt.
            const bool turnsOk = (hMax - hMin) / m_pitch <= 40.0;
            if (nFaces == 3 && shapeOk && fullSpan && turnsOk && hMax > hMin) {
                gp_Ax3 base(gp_Pnt(loc.X() + zd.X() * hMin,
                                   loc.Y() + zd.Y() * hMin,
                                   loc.Z() + zd.Z() * hMin), zd, xd);
                TopoDS_Shape rod = sweptRodThread(base, m_radius, hMax - hMin,
                                                  m_pitch, depth,
                                                  m_rightHanded);
                if (!rod.IsNull()) {
                    if (materializr::isVerbose())
                        std::fprintf(stderr, "[Thread] swept-rod fast path\n");
                    return rod;
                }
                std::fprintf(stderr, "[Thread] swept-rod declined — falling "
                                     "back to boolean cut\n");
            }
        }

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
        if (materializr::isVerbose())
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
        // Closed UV band wire on a cylindrical surface, SYMMETRIC about the
        // helix line: tip → taper to the lower offset line (−off) → chunked
        // run parallel to the helix → taper to the far tip → taper up to the
        // upper line (+off) → chunked run back → taper to the tip. Both
        // flanks of the lofted groove slant equally (a seam ON the helix
        // lofts into a flat radial wall — Steve's "flat top, tapered
        // bottom"). All edges are straight 2D lines on the surface; long
        // runs are CHUNKED per turn — the tame-patch segmentation that makes
        // the boolean cut reliably (a single 30-turn lofted spline removes
        // nothing or worse; proven via the headless volume harness).
        // `vRef` anchors the helix PHASE: u = 0 at v = vRef (shifted by whole
        // turns so u stays near 0 within [lo, hi] — the surface is periodic
        // and phase mod 2π is what aligns the groove). The compound path
        // passes vRef = lo (the historical anchor); the per-turn path passes
        // the same vRef for every turn so all its tools lie on ONE helix.
        // `tlLo`/`tlHi` are the taper lengths at each end of the band; a
        // value <= 0 produces a SQUARE end (the band stops on a straight
        // axial edge instead of tapering to a tip). Interior per-turn tools
        // use square ends — their end walls sit INSIDE the neighbouring
        // void, away from every existing surface. The interlocking 0.5P
        // tapers were inherited from the compound recipe's runout, and they
        // are exactly the surfaces that made every neighbouring-void
        // boolean coin-flip (the whole shrink/debt/repair saga, all of
        // whose repairs OCCT inverted). Tapers remain only at the thread's
        // real ends, where runout belongs.
        auto bandWire = [&](const Handle(Geom_CylindricalSurface)& surf,
                            double uSign, double vRef, double lo, double hi,
                            double off, int nSeg, double tlLo,
                            double tlHi) -> TopoDS_Wire {
            double kTurn = std::floor((lo - vRef) / m_pitch + 1e-9);
            auto onHelix = [&](double v) {
                return gp_Pnt2d(
                    uSign * 2.0 * M_PI * ((v - vRef) / m_pitch - kTurn), v);
            };
            auto offHelix = [&](double v, double dv) {
                gp_Pnt2d p = onHelix(v);
                return gp_Pnt2d(p.X(), p.Y() + dv);
            };
            bool sqLo = tlLo <= 0.0, sqHi = tlHi <= 0.0;
            gp_Pnt2d L1 = offHelix(lo + std::max(tlLo, 0.0), -off);
            gp_Pnt2d L2 = offHelix(hi - std::max(tlHi, 0.0), -off);
            gp_Pnt2d U2 = offHelix(hi - std::max(tlHi, 0.0), off);
            gp_Pnt2d U1 = offHelix(lo + std::max(tlLo, 0.0), off);
            BRepBuilderAPI_MakeWire mw;
            auto addChunked = [&](gp_Pnt2d p, gp_Pnt2d q, int n) {
                for (int i = 0; i < n; ++i) {
                    gp_Pnt2d a(p.X() + (q.X() - p.X()) * i / n,
                               p.Y() + (q.Y() - p.Y()) * i / n);
                    gp_Pnt2d b(p.X() + (q.X() - p.X()) * (i + 1) / n,
                               p.Y() + (q.Y() - p.Y()) * (i + 1) / n);
                    Handle(Geom2d_TrimmedCurve) sg =
                        GCE2d_MakeSegment(a, b).Value();
                    TopoDS_Edge e = BRepBuilderAPI_MakeEdge(sg, surf).Edge();
                    BRepLib::BuildCurves3d(e);
                    mw.Add(e);
                }
            };
            addChunked(L1, L2, nSeg);
            if (sqHi) {
                addChunked(L2, U2, 1); // square top: straight axial edge
            } else {
                gp_Pnt2d D = onHelix(hi);
                addChunked(L2, D, 1);
                addChunked(D, U2, 1);
            }
            addChunked(U2, U1, nSeg);
            if (sqLo) {
                addChunked(U1, L1, 1); // square bottom
            } else {
                gp_Pnt2d A = onHelix(lo);
                addChunked(U1, A, 1);
                addChunked(A, L1, 1);
            }
            return mw.Wire();
        };

        // Full-parameter cutter builder. `outClear`/`wFac`/`dJit` are the
        // per-turn variant ladder's micro-jitters (clearance, groove width
        // factor, extra depth) — they break the exact surface coincidences
        // between a turn's tool and the surfaces the previous turn's cut
        // created, which is what makes the kernel misclassify. `nSeg` is the
        // chunk count of the long runs; one chunk PER TURN, exactly.
        auto buildCutterEx = [&](double lo, double hi, double vRef,
                                 double outClear, double wFac, double dJit,
                                 int nSeg, double tlLo,
                                 double tlHi) -> TopoDS_Shape {
            try {
                // Outer surface on the material-free side of the face (clean
                // detachment without near-tangency); inner surface at the
                // groove apex. (For a hole, "outer" is inside the void.)
                double d   = depth + dJit;
                double rOut = m_isHole ? (m_radius - outClear)
                                       : (m_radius + outClear);
                double rIn  = m_isHole ? (m_radius + d) : (m_radius - d);
                Handle(Geom_CylindricalSurface) sOut =
                    new Geom_CylindricalSurface(ax3, rOut);
                Handle(Geom_CylindricalSurface) sIn =
                    new Geom_CylindricalSurface(ax3, rIn);

                // u+ is a right-handed screw for any face axis orientation
                // (chirality is invariant under axis flip).
                double uSign = m_rightHanded ? 1.0 : -1.0;
                // Groove opening = 7/8 of the pitch, leaving an ISO-like
                // crest land of P/8. The old 0.45·pitch cap (a leftover sweep
                // -era safety) left two-thirds of the pitch as flat land —
                // Steve: "looks more like a leadscrew than an actual screw".
                double halfW = 0.4375 * m_pitch * wFac;

                TopoDS_Wire w1 = bandWire(sOut, uSign, vRef, lo, hi,
                                          halfW, nSeg, tlLo, tlHi);
                TopoDS_Wire w2 = bandWire(sIn, uSign, vRef, lo, hi,
                                          halfW * 0.25, nSeg, tlLo, tlHi);

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
        // The historical compound-tool builder (full-cylinder fast path) —
        // parameters bit-identical to every shipped release.
        auto buildCutter = [&](double lo, double hi) -> TopoDS_Shape {
            double t = (hi - lo) / m_pitch;
            int nSeg = std::max(4, static_cast<int>(std::ceil(t)));
            return buildCutterEx(lo, hi, lo, 0.10, 1.0, 0.0, nSeg,
                                 0.5 * m_pitch, 0.5 * m_pitch);
        };

        auto shapeVol = [](const TopoDS_Shape& s) -> double {
            try {
                GProp_GProps p;
                BRepGProp::VolumeProperties(s, p);
                return p.Mass();
            } catch (...) { return 0.0; }
        };
        const double bodyVol = shapeVol(body);

        auto cutOn = [&](const TopoDS_Shape& target, const TopoDS_Shape& tool,
                         double fuzz) -> TopoDS_Shape {
            try {
                BRepAlgoAPI_Cut cut;
                TopTools_ListOfShape args, tools;
                args.Append(target);
                tools.Append(tool);
                cut.SetArguments(args);
                cut.SetTools(tools);
                cut.SetFuzzyValue(fuzz);
                // The per-turn path runs dozens of these sequentially on
                // the worker thread; let each boolean use the machine
                // (Steve watched a 27-turn rod pin ONE of 16 cores for a
                // minute).
                cut.SetRunParallel(Standard_True);
                cut.Build();
                if (!cut.IsDone()) return {};
                return cut.Shape();
            } catch (...) { return {}; }
        };
        auto cutOnce = [&](const TopoDS_Shape& tool) -> TopoDS_Shape {
            return cutOn(body, tool, 1.0e-3);
        };

        // ---- Shape-aware cut validation. Volume bands alone cannot tell a
        // real groove from a WRONG-MATERIAL cut at similar volume (Steve's
        // stacked-disc bodies: the boolean removes a full-circumference slab
        // between grooves instead of the groove — comparable volume, garbage
        // shape, tessellator crash). Classifier probes can: after a turn's
        // cut, points in the GROOVE band must be void and points in the
        // CREST band must still be solid. A disc cut eats the crest; an
        // imprint no-op leaves the groove solid. Both fail instantly.
        gp_Vec ydv = gp_Vec(zd).Crossed(gp_Vec(xd));
        auto cylPt = [&](double rad, double theta, double dz) {
            double c = std::cos(theta), s = std::sin(theta);
            return gp_Pnt(
                loc.X() + zd.X() * dz + (xd.X() * c + ydv.X() * s) * rad,
                loc.Y() + zd.Y() * dz + (xd.Y() * c + ydv.Y() * s) * rad,
                loc.Z() + zd.Z() * dz + (xd.Z() * c + ydv.Z() * s) * rad);
        };
        auto isIn = [&](const TopoDS_Shape& s, const gp_Pnt& p) {
            try {
                BRepClass3d_SolidClassifier c(s, p, 1e-7);
                return c.State() == TopAbs_IN;
            } catch (...) { return false; }
        };
        // Score the helix span [lo, hi] of `post` against pre-cut state
        // `pre`: along the groove helix, the groove point must have gone
        // OUT and the crest point (half a pitch up, same angle) stayed IN.
        // The probe angle is derived from v directly (θ = uSign·2π·(v−vLo)
        // /P), so the probes are GRID-INDEPENDENT — they follow the helix
        // wherever the zone boundaries sit. Only samples where BOTH points
        // were solid in `pre` count (split-away regions and cross-holes
        // are legitimately void); taper/runout spans at the very ends are
        // skipped. Returns {good, considered}.
        struct ProbeScore { int good = 0, considered = 0; };
        auto turnProbes = [&](const TopoDS_Shape& pre, const TopoDS_Shape& post,
                              double lo, double hi) -> ProbeScore {
            const int K = 16;
            double uSign = m_rightHanded ? 1.0 : -1.0;
            double rG = m_isHole ? (m_radius + 0.5 * depth)
                                 : (m_radius - 0.5 * depth);
            double rC = m_isHole ? (m_radius + 0.25 * depth)
                                 : (m_radius - 0.25 * depth);
            ProbeScore sc;
            for (int k = 0; k < K; ++k) {
                double vG = lo + (hi - lo) * (k + 0.5) / K;
                if (vG < vLo + 0.75 * m_pitch || vG > vHi - 0.75 * m_pitch)
                    continue; // taper/runout — groove intentionally shallow
                double vC = vG + 0.5 * m_pitch;
                double th = uSign * 2.0 * M_PI * (vG - vLo) / m_pitch;
                gp_Pnt pg = cylPt(rG, th, vG), pc = cylPt(rC, th, vC);
                if (!isIn(pre, pg) || !isIn(pre, pc)) continue;
                ++sc.considered;
                bool okSample = !isIn(post, pg) && isIn(post, pc);
                // WIDTH check, mid-span samples only (so the probes stay
                // inside this cut's own territory): a full-width groove is
                // void at ±0.30P off the centreline at this depth (design
                // half-opening ≈0.355P there); a TAPER-NARROWED section is
                // still solid. Centre-only probes passed those and Steve
                // saw "flatter/narrower channel grooves" on half the rod.
                if (okSample && k * 10 >= 3 * K && k * 10 <= 7 * K) {
                    gp_Pnt w1 = cylPt(rC, th, vG - 0.30 * m_pitch);
                    gp_Pnt w2 = cylPt(rC, th, vG + 0.30 * m_pitch);
                    if (isIn(pre, w1) && isIn(post, w1)) okSample = false;
                    if (isIn(pre, w2) && isIn(post, w2)) okSample = false;
                }
                if (okSample) ++sc.good;
            }
            return sc;
        };

        auto tryCut = [&](const TopoDS_Shape& tool) -> TopoDS_Shape {
            if (tool.IsNull()) return {};
            TopoDS_Shape res = cutOnce(tool);
            // Validate: a CUT must shrink the body. Against partial bodies
            // (e.g. the half-rod a reflow re-threads) the helical tool's
            // classification can invert and the "cut" ADDS the groove volume
            // — retry with the reversed tool, which flips it back.
            if (!res.IsNull() && shapeVol(res) > bodyVol + 1e-3) {
                std::fprintf(stderr, "[Thread] cut inverted (vol grew) — "
                                     "retrying with reversed tool\n");
                TopoDS_Shape rev = tool;
                rev.Reverse();
                res = cutOnce(rev);
            }
            if (res.IsNull()) return {};
            double v = shapeVol(res);
            // A real groove removes meaningful material. Accept only
            // 0 < v < body − ε: rejects growth (inverted classification) AND
            // no-op cuts that merely imprint edges (volume unchanged — the
            // reversed-tool retry can produce these, which read as "edges
            // but the surface looks solid").
            double minRemoval = std::max(1e-2, bodyVol * 1e-4);
            if (v > bodyVol - minRemoval || v < 0.0) {
                std::fprintf(stderr, "[Thread] cut removed nothing or grew "
                                     "(%.2f vs body %.2f) — rejecting\n",
                             v, bodyVol);
                return {};
            }
            // Shape-aware gate: every turn must probe PERFECTLY as a real
            // groove (groove band void, crest band solid, all sampled
            // angles). Wrong-material cuts with plausible total volume —
            // the kind that used to pass here, reach the tessellator, and
            // SEGFAULT — die on this; so do partial cuts that leave groove
            // arcs filled near a slot (Steve's "half groove filled in",
            // which a 75% threshold waved through). Any imperfection
            // demotes to the per-turn path, which retries turn-by-turn for
            // a perfect cut before settling.
            int nTurns = static_cast<int>(std::ceil((vHi - vLo) / m_pitch));
            for (int t = 0; t < nTurns; ++t) {
                ProbeScore sc = turnProbes(body, res, vLo + t * m_pitch,
                                           vLo + (t + 1) * m_pitch);
                // < 5 measurable samples = a partial end turn that's mostly
                // runout — INCONCLUSIVE, not damning. (A 1/4 verdict on the
                // last fractional turn demoted a perfectly good compound
                // cut on Steve's 16mm rod.)
                if (sc.considered >= 5 && sc.good != sc.considered) {
                    std::fprintf(stderr, "[Thread] compound cut imperfect at "
                                         "turn %d (%d/%d probes) — demoting "
                                         "to per-turn\n", t, sc.good,
                                 sc.considered);
                    return {};
                }
            }
            // The cut result regularly carries tolerance nits — heal it so
            // downstream ops (fillets, further booleans, save) get a clean
            // solid.
            if (!BRepCheck_Analyzer(res).IsValid()) {
                ShapeFix_Shape fixer(res);
                fixer.Perform();
                res = fixer.Shape();
            }
            return res;
        };

        // PER-TURN SEQUENTIAL fallback: one boolean per turn, each validated
        // by removed volume against the analytic per-turn groove volume,
        // failed turns retried through a variant ladder of micro-jitters.
        // This is how partial cylinders (lengthwise split halves) and
        // interrupted cylinders (cross-holes) get threaded — the compound
        // tool coin-flips on those, but single-turn cuts with validation
        // land 20-21/21 turns in the harness, and a turn that fails every
        // variant is SKIPPED (a short groove gap), never garbage.
        auto perTurnCut = [&]() -> TopoDS_Shape {
            int nT = static_cast<int>(std::ceil((vHi - vLo) / m_pitch));
            if (nT < 1 || nT > 120) return {};
            // Analytic groove volume of one turn: trapezoid cross-section
            // swept at mid-groove radius.
            double halfW = 0.4375 * m_pitch;
            double area = 0.5 * (2.0 * halfW + 2.0 * halfW * 0.25) * depth;
            double rMid = m_isHole ? (m_radius + 0.5 * depth)
                                   : (m_radius - 0.5 * depth);
            double analytic = area * 2.0 * M_PI * rMid;
            if (analytic <= 1e-9) return {};

            struct Variant {
                double fuzz, clear_, wFac, dJit, ovl;
                bool rev, taper;
            };
            // TWO complementary tool families (matrix-proven):
            //   SQUARE (taper=false): square-ended band segments, `ovl`
            //     reaching into the previous void. Perfect + natively
            //     VALID on full and cross-holed rods; no-ops/inverts on
            //     lengthwise halves.
            //   TAPER (taper=true): classic [zone-0.5P, zone+0.5P] tools
            //     with 0.5P tapers. Perfect on halves; coin-flips every
            //     3rd turn on full rods.
            // The engine starts square-first and flips its preference for
            // the rest of the body the first time the taper family wins a
            // turn (bodies are homogeneous in which family works).
            const Variant variants[] = {
                // square family
                {1e-3, 0.10, 1.000, 0.000, 0.06, false, false},
                {1e-3, 0.10, 0.995, 0.000, 0.06, false, false},
                {1e-3, 0.10, 1.000, 0.005, 0.06, false, false},
                {1e-3, 0.10, 1.000, 0.000, 0.11, false, false},
                {1e-3, 0.10, 1.000, 0.000, 0.06, true,  false},
                {2e-3, 0.15, 0.990, 0.010, 0.13, false, false},
                // taper family
                {1e-3, 0.10, 1.000, 0.000, 0.00, false, true},
                {1e-3, 0.10, 0.995, 0.000, 0.00, false, true},
                {1e-3, 0.10, 1.000, 0.005, 0.00, false, true},
                {1e-3, 0.10, 1.000, 0.000, 0.00, true,  true},
                {5e-4, 0.10, 0.995, 0.005, 0.00, false, true},
            };
            const int nV = sizeof(variants) / sizeof(variants[0]);
            bool taperFirst = false;

            TopoDS_Shape cur = body;
            double curVol = bodyVol;
            int failed = 0, skipped = 0;
            // STRICTLY bottom-up: each square tool's blunt top wall (cut
            // into material at zoneHi) is erased by the NEXT turn's
            // overlapping bottom.
            //
            // CREST-ALIGNED ZONE GRID: grooves sit at v = vLo + n·P (the
            // helix anchor), so zone boundaries go at vLo + (n±0.5)·P —
            // mid-crest — and each square tool owns ONE whole groove. The
            // original grid put boundaries exactly ON the grooves: every
            // square end wall sliced through a groove, stacking half-open
            // groove stubs at each boundary until the booleans collapsed
            // outright (Steve's 16mm rod: turns 0-8 OK, everything after
            // failed). nT+1 zones cover [vLo, vHi] with clipped ends.
            // End zones: when a thread end is FREE (vLo/vHi already carry
            // the runout extension), the end zone may reach 0.5P further
            // into the air so its runout taper has somewhere to live.
            // Clamping the first zone to [vLo, vLo+0.5P] made it exactly
            // as long as its own taper — a degenerate tool that cut
            // nothing, deleting the first half-turn of groove (Steve's
            // "weird non-tapered end").
            bool botFree = vLo < -1e-9;
            bool topFree = vHi > m_length + 1e-9;
            double gridLo = botFree ? vLo - 0.5 * m_pitch : vLo;
            double gridHi = topFree ? vHi + 0.5 * m_pitch : vHi;
            // Build the zone list, then MERGE face-crossing end zones into
            // their material-anchored neighbours. An end zone whose groove
            // centre sits beyond the rod face is void-dominated — OCCT
            // no-ops the cut and the face-exit flank sliver never gets
            // removed (the blunt triangular pocket below the rim in
            // Steve's screenshot). Merged, the sliver rides along on a
            // tool that owns a full in-material groove.
            std::vector<std::pair<double, double>> zones;
            for (int i = 0; i <= nT; ++i) {
                double zlo = std::max(gridLo, vLo + (i - 0.5) * m_pitch);
                double zhi = std::min(gridHi, vLo + (i + 0.5) * m_pitch);
                if (zhi - zlo < 0.05 * m_pitch) continue;
                zones.push_back({zlo, zhi});
            }
            while (zones.size() > 1 &&
                   zones[0].second <= 0.5 * m_pitch + 1e-9) {
                zones[1].first = zones[0].first;
                zones.erase(zones.begin());
            }
            while (zones.size() > 1 &&
                   zones.back().first >= m_length - 0.5 * m_pitch - 1e-9) {
                zones[zones.size() - 2].second = zones.back().second;
                zones.pop_back();
            }
            for (size_t zi = 0; zi < zones.size(); ++zi) {
                int i = static_cast<int>(zi);
                double zoneLo = zones[zi].first;
                double zoneHi = zones[zi].second;
                bool first = (zi == 0);
                bool last = (zi + 1 == zones.size());
                bool ok = false;
                bool noMaterial = false;
                // Per-variant failure accounting — printed when a whole
                // turn fails so a field log is diagnosable without a
                // rebuild (finding the last regression cost Steve a CPU
                // fan and me a blind guess).
                int rjNull = 0, rjGrew = 0, rjBig = 0, rjProbe = 0;
                int lastGood = -1, lastDen = -1;
                // Best imperfect candidate across the ladder: a variant
                // that cleared ≥75% of probes but left an arc of the groove
                // filled (Steve's slotted rod: "one of the half grooves got
                // filled in"). Prefer ANY perfect variant; adopt the best
                // imperfect one only after the whole ladder has had a shot.
                TopoDS_Shape bestRes;
                double bestVol = 0.0;
                int bestScore = -1, bestDen = 1, bestVar = -1;
                for (int pass = 0; pass < 2 && !ok; ++pass) {
                    // pass 0 = preferred family, pass 1 = the other
                    bool wantTaper = (pass == 0) ? taperFirst : !taperFirst;
                for (int v = 0; v < nV && !ok; ++v) {
                    const Variant& va = variants[v];
                    if (va.taper != wantTaper) continue;
                    double toolLo, toolHi, tlLo, tlHi;
                    if (va.taper) {
                        toolLo = zoneLo - 0.5 * m_pitch;
                        toolHi = zoneHi + 0.5 * m_pitch;
                        tlLo = tlHi = 0.5 * m_pitch;
                    } else {
                        // Square ends, bottom overlapping the previous
                        // void. Real thread ends keep runout tapers —
                        // shortened when the zone is clamped (non-free
                        // end) so the tool never degenerates to nothing.
                        toolLo = first ? zoneLo : zoneLo - va.ovl * m_pitch;
                        toolHi = zoneHi;
                        double span = toolHi - toolLo;
                        tlLo = first ? std::min(0.5 * m_pitch, 0.6 * span)
                                     : 0.0;
                        tlHi = last ? std::min(0.5 * m_pitch,
                                               0.6 * span - tlLo * 0.5)
                                    : 0.0;
                        if (tlHi < 0.0) tlHi = 0.0;
                    }
                    int nSeg = std::max(1, static_cast<int>(std::lround(
                                               (toolHi - toolLo) / m_pitch)));
                    TopoDS_Shape tool = buildCutterEx(
                        toolLo, toolHi, vLo, va.clear_, va.wFac, va.dJit,
                        nSeg, tlLo, tlHi);
                    if (tool.IsNull()) continue;
                    if (va.rev) tool.Reverse();
                    TopoDS_Shape res = cutOn(cur, tool, va.fuzz);
                    if (res.IsNull()) { ++rjNull; continue; }
                    double after = shapeVol(res);
                    if (after <= 0.0) { ++rjNull; continue; }
                    double removed = curVol - after;
                    // Inverted / wrong-material cuts remove far more than
                    // one groove turn ever could (band hi proven at 2.5×:
                    // a turn after a skipped neighbour legitimately
                    // catches up its missed groove too)...
                    if (removed > 2.5 * analytic) { ++rjBig; continue; }
                    // ...and a GROWN body is an inverted classification.
                    if (removed < -1e-6) { ++rjGrew; continue; }
                    // FACE-EXIT zones legitimately remove a tiny sliver —
                    // the runout flank wedge where the helix crosses the
                    // rod's end face (NOT necessarily the grid's first/
                    // last zone). The normal 2% floor swallowed it ("no
                    // material") and left the groove ending in a blunt
                    // triangular pocket below the rim (Steve's screenshot).
                    bool faceExit = zoneLo < 0.75 * m_pitch ||
                                    zoneHi > m_length - 0.75 * m_pitch;
                    double matFloor = faceExit ? 0.001 * analytic
                                               : 0.02 * analytic;
                    if (removed < matFloor) {
                        // A clean boolean that removed (almost) nothing.
                        // Either this turn's groove isn't in this body
                        // (split-away region) — or the cut no-op'd the way
                        // awkward bodies do and a LATER VARIANT will land
                        // it. Remember the benign outcome but keep trying;
                        // do NOT adopt `res` (imprint edges pollute).
                        noMaterial = true;
                        continue;
                    }
                    // Volume is in band — but at single-turn scale a
                    // wrong-material disc cut removes a volume comparable
                    // to a real groove (Steve's stacked-disc rod). Shape
                    // probes arbitrate.
                    ProbeScore sc = turnProbes(cur, res, zoneLo, zoneHi);
                    if (sc.considered == 0) {
                        // No probeable material yet volume moved: distrust.
                        noMaterial = true;
                        continue;
                    }
                    // < 5 measurable samples = partial end turn, mostly
                    // runout: probes are INCONCLUSIVE — trust the volume
                    // band that already passed.
                    if (sc.good == sc.considered || sc.considered < 5) {
                        cur = res;
                        curVol = after;
                        ok = true;
                        // Adapt: if the non-preferred family won, prefer it
                        // for the rest of this body.
                        if (va.taper != taperFirst) taperFirst = va.taper;
                    } else if (sc.good * 4 >= sc.considered * 3 &&
                               sc.good * bestDen > bestScore * sc.considered) {
                        bestRes = res;
                        bestVol = after;
                        bestScore = sc.good;
                        bestDen = sc.considered;
                        bestVar = v;
                    } else {
                        ++rjProbe;
                        lastGood = sc.good;
                        lastDen = sc.considered;
                    }
                }
                }
                if (!ok && !bestRes.IsNull()) {
                    std::fprintf(stderr, "[Thread] turn %d: best variant %d "
                                         "imperfect (%d/%d probes) — "
                                         "adopting\n",
                                 i, bestVar, bestScore, bestDen);
                    cur = bestRes;
                    curVol = bestVol;
                    ok = true;
                }
                if (!ok) {
                    if (noMaterial) ++skipped;
                    else {
                        ++failed;
                        std::fprintf(stderr, "[Thread] turn %d failed: "
                                             "null=%d grew=%d big=%d "
                                             "probe=%d (last %d/%d)\n",
                                     i, rjNull, rjGrew, rjBig, rjProbe,
                                     lastGood, lastDen);
                    }
                }
                // Bail as soon as the failure budget is blown — each failed
                // turn burns the FULL variant ladder (9 booleans), and a
                // body that fails this often isn't going to be saved by the
                // remaining turns. This runs synchronously during reflow
                // replays; keep the doomed case bounded.
                if (failed > std::max(2, nT / 5)) {
                    std::fprintf(stderr, "[Thread] per-turn: aborting at "
                                         "turn %d (%d failures)\n", i, failed);
                    return {};
                }
            }
            if (materializr::isVerbose())
                std::fprintf(stderr, "[Thread] per-turn: %d turns, %d skipped "
                                     "(no material), %d failed\n",
                             nT, skipped, failed);
            // The whole pass must have removed SOMETHING — a body that no
            // groove intersects is a no-op, and no-ops suspend (same rule
            // as the compound path's volume guard).
            double minRemoval = std::max(1e-2, bodyVol * 1e-4);
            if (curVol > bodyVol - minRemoval) return {};
            if (!BRepCheck_Analyzer(cur).IsValid()) {
                ShapeFix_Shape fixer(cur);
                fixer.Perform();
                cur = fixer.Shape();
            }
            return cur;
        };

        TopoDS_Shape result;
        if (fullCylinder) {
            if (materializr::isVerbose())
                std::fprintf(stderr, "[Thread] cutting (span %.2f..%.2f)...\n",
                             vLo, vHi);
            result = tryCut(buildCutter(vLo, vHi));
            // NO exact-span compound retry here. When the extended compound
            // cut inverts, the exact-span retry historically "succeeded"
            // with plausible-volume WRONG-MATERIAL removals (the poker-chip
            // bodies) that pass the coarse whole-body volume guard and then
            // SEGFAULT the tessellator. The per-turn fallback below retries
            // with per-turn validation instead — garbage can't pass it.
        }
        if (result.IsNull()) {
            std::fprintf(stderr, "[Thread] %s — per-turn sequential cut\n",
                         fullCylinder ? "compound cut failed"
                                      : "partial/interrupted cylinder");
            result = perTurnCut();
        }
        if (result.IsNull()) {
            std::fprintf(stderr, "[Thread] boolean cut FAILED\n");
            return {};
        }
        if (materializr::isVerbose())
            std::fprintf(stderr, "[Thread] buildResult OK\n");
        return result;
    } catch (...) {
        std::fprintf(stderr, "[Thread] buildResult threw\n");
        return {};
    }
}

namespace {
std::function<bool(ThreadOp&, Document&)> s_asyncRecut;
} // namespace

void ThreadOp::setAsyncRecutHook(std::function<bool(ThreadOp&, Document&)> h) {
    s_asyncRecut = std::move(h);
}

bool ThreadOp::execute(Document& doc) {
    if (m_bodyId < 0) return false;
    try {
        m_previousShape = doc.getBody(m_bodyId);
        if (m_previousShape.IsNull()) return false;

        // Follow an upstream edit: on the recompute path (editStep / redo, i.e.
        // no worker-precomputed result), re-resolve the target cylinder face
        // against the current body and adopt its new axis + radius. The stored
        // absolute params are the fallback when there's no ref or it can't
        // resolve — today's behaviour, so nothing regresses for old files.
        if (m_precomputed.IsNull() && !m_faceRef.empty()) {
            materializr::topo::Context ctx;
            ctx.doc = &doc;
            ctx.shape = m_previousShape;
            ctx.type = TopAbs_FACE;
            ctx.crossRebuild = true;   // body may have been rebuilt upstream
            TopoDS_Shape f;
            if (materializr::topo::resolve(m_faceRef, ctx, f) &&
                !f.IsNull() && f.ShapeType() == TopAbs_FACE) {
                gp_Ax2 ax2; double r = 0.0;
                if (cylFaceToThread(TopoDS::Face(f), ax2, r) && r > 1e-6) {
                    setAxis(ax2);   // updates m_axis + serialized components
                    m_radius = r;
                }
            }
        }

        // Recompute path with the app hook installed: hand the multi-second
        // re-cut to the worker (body stays at its pre-thread state until the
        // result lands) so a cascade replay never freezes the UI.
        if (m_precomputed.IsNull() && s_asyncRecut && s_asyncRecut(*this, doc))
            return true;

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
    std::string s = buf;
    // Target-face name LAST so its (delimiter-free) blob runs to end-of-string;
    // absent in old files, which just keep their absolute axis/radius.
    if (!m_faceRef.empty()) s += ";faceref=" + m_faceRef.serialize();
    return s;
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
        // faceref carries an opaque length-prefixed blob and is written last,
        // so read it to end-of-string (not to the next ';').
        if (key == "faceref") {
            m_faceRef = materializr::topo::Ref::parse(blob.substr(eq + 1));
            any = true;
            break;
        }
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

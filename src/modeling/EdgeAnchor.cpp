#include "EdgeAnchor.h"
#include "Sketch.h"
#include <BRepAdaptor_Curve.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace EdgeAnchor {
namespace {

constexpr double kUVTol = 1e-3;   // sketch-plane position match tolerance
constexpr double kAxisDot = 0.999; // |dir·axis| above this = parallel

struct Frame {
    gp_Pnt o; gp_Dir axis, xd, yd;
    double u(const gp_Pnt& p) const { return gp_Vec(o, p).Dot(gp_Vec(xd)); }
    double v(const gp_Pnt& p) const { return gp_Vec(o, p).Dot(gp_Vec(yd)); }
    double h(const gp_Pnt& p) const { return gp_Vec(o, p).Dot(gp_Vec(axis)); }
};

Frame frameOf(const materializr::Sketch& sk) {
    const gp_Pln& pln = sk.getPlane();
    return { pln.Location(), pln.Axis().Direction(),
             pln.XAxis().Direction(), pln.YAxis().Direction() };
}

bool asLine(const TopoDS_Edge& e, BRepAdaptor_Curve& c) {
    try { c.Initialize(e); return c.GetType() == GeomAbs_Line; }
    catch (...) { return false; }
}

// Corner = line parallel to the extrude axis. Sets its midpoint (u,v).
bool cornerUV(const TopoDS_Edge& e, const Frame& f, double& u, double& v) {
    BRepAdaptor_Curve c;
    if (!asLine(e, c)) return false;
    if (std::abs(c.Line().Direction().Dot(f.axis)) < kAxisDot) return false;
    gp_Pnt m = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
    u = f.u(m); v = f.v(m);
    return true;
}

// Rim = straight edge NOT parallel to the axis (lies at a cap height). Returns
// its two endpoints in (u,v) and its height h along the axis.
bool rimEnds(const TopoDS_Edge& e, const Frame& f,
             double& u1, double& v1, double& u2, double& v2, double& h) {
    BRepAdaptor_Curve c;
    if (!asLine(e, c)) return false;
    if (std::abs(c.Line().Direction().Dot(f.axis)) >= kAxisDot) return false; // that's a corner
    gp_Pnt p1 = c.Value(c.FirstParameter());
    gp_Pnt p2 = c.Value(c.LastParameter());
    u1 = f.u(p1); v1 = f.v(p1);
    u2 = f.u(p2); v2 = f.v(p2);
    h = 0.5 * (f.h(p1) + f.h(p2));
    return true;
}

bool near2(double ax, double ay, double bx, double by, double tol) {
    return std::hypot(ax - bx, ay - by) < tol;
}

// Do unordered endpoint pairs {A,B} and {X,Y} coincide?
bool endsMatch(double au, double av, double bu, double bv,
               double xu, double xv, double yu, double yv, double tol) {
    return (near2(au, av, xu, xv, tol) && near2(bu, bv, yu, yv, tol)) ||
           (near2(au, av, yu, yv, tol) && near2(bu, bv, xu, xv, tol));
}

} // namespace

std::vector<Anchor> compute(const std::vector<TopoDS_Edge>& edges,
                            const materializr::Sketch& sk) {
    std::vector<Anchor> out(edges.size());
    Frame f = frameOf(sk);
    const auto& pts = sk.getPoints();
    const auto& lines = sk.getLines();

    for (size_t i = 0; i < edges.size(); ++i) {
        double u, v;
        if (cornerUV(edges[i], f, u, v)) {
            for (const auto& p : pts)
                if (near2(u, v, p.pos.x, p.pos.y, kUVTol)) {
                    out[i] = { Anchor::Corner, p.id, 0.0 };
                    break;
                }
            if (out[i].kind != Anchor::None) continue;
        }
        double u1, v1, u2, v2, h;
        if (rimEnds(edges[i], f, u1, v1, u2, v2, h)) {
            for (const auto& L : lines) {
                const auto* a = sk.getPoint(L.startPointId);
                const auto* b = sk.getPoint(L.endPointId);
                if (!a || !b) continue;
                if (endsMatch(u1, v1, u2, v2,
                              a->pos.x, a->pos.y, b->pos.x, b->pos.y, kUVTol)) {
                    out[i] = { Anchor::Rim, L.id, h };
                    break;
                }
            }
        }
    }
    return out;
}

bool resolve(const std::vector<Anchor>& anchors, const materializr::Sketch& sk,
             const TopoDS_Shape& base, std::vector<TopoDS_Edge>& out) {
    out.clear();
    if (anchors.empty()) return false;
    Frame f = frameOf(sk);

    for (const Anchor& a : anchors) {
        TopoDS_Edge found;
        if (a.kind == Anchor::Corner) {
            const auto* p = sk.getPoint(a.elemId);
            if (!p) return false;
            for (TopExp_Explorer ex(base, TopAbs_EDGE); ex.More(); ex.Next()) {
                double u, v;
                if (cornerUV(TopoDS::Edge(ex.Current()), f, u, v) &&
                    near2(u, v, p->pos.x, p->pos.y, kUVTol)) {
                    found = TopoDS::Edge(ex.Current()); break;
                }
            }
        } else if (a.kind == Anchor::Rim) {
            const materializr::SketchLine* L = nullptr;
            for (const auto& l : sk.getLines()) if (l.id == a.elemId) { L = &l; break; }
            if (!L) return false;
            const auto* pa = sk.getPoint(L->startPointId);
            const auto* pb = sk.getPoint(L->endPointId);
            if (!pa || !pb) return false;
            for (TopExp_Explorer ex(base, TopAbs_EDGE); ex.More(); ex.Next()) {
                double u1, v1, u2, v2, h;
                if (rimEnds(TopoDS::Edge(ex.Current()), f, u1, v1, u2, v2, h) &&
                    std::abs(h - a.h) < kUVTol &&
                    endsMatch(u1, v1, u2, v2,
                              pa->pos.x, pa->pos.y, pb->pos.x, pb->pos.y, kUVTol)) {
                    found = TopoDS::Edge(ex.Current()); break;
                }
            }
        } else {
            return false; // Kind::None — can't resolve
        }
        if (found.IsNull()) return false;
        out.push_back(found);
    }
    return true;
}

std::string serialize(int sketchId, const std::vector<Anchor>& anchors) {
    if (sketchId < 0) return "";
    bool any = false;
    for (const auto& a : anchors) if (a.kind != Anchor::None) { any = true; break; }
    if (!any) return "";
    std::string s = std::to_string(sketchId);
    char buf[64];
    for (const auto& a : anchors) {
        s += '~';
        if (a.kind == Anchor::Corner) { s += "C,"; s += std::to_string(a.elemId); }
        else if (a.kind == Anchor::Rim) {
            std::snprintf(buf, sizeof(buf), "R,%d,%.6f", a.elemId, a.h);
            s += buf;
        } else s += "N";
    }
    return s;
}

bool parse(const std::string& blob, int& sketchId, std::vector<Anchor>& anchors) {
    anchors.clear();
    if (blob.empty()) return false;
    size_t pos = 0; bool first = true;
    while (pos <= blob.size()) {
        size_t t = blob.find('~', pos);
        if (t == std::string::npos) t = blob.size();
        std::string tok = blob.substr(pos, t - pos);
        pos = t + 1;
        if (first) { sketchId = std::atoi(tok.c_str()); first = false; }
        else if (!tok.empty()) {
            Anchor a;
            if (tok[0] == 'C') { a.kind = Anchor::Corner; a.elemId = std::atoi(tok.c_str() + 2); }
            else if (tok[0] == 'R') { a.kind = Anchor::Rim; std::sscanf(tok.c_str(), "R,%d,%lf", &a.elemId, &a.h); }
            else a.kind = Anchor::None;
            anchors.push_back(a);
        }
        if (t == blob.size()) break;
    }
    return !first;
}

} // namespace EdgeAnchor

#include "FilletOp.h"
#include "SubShapeIndex.h"
#include "EdgeAnchor.h"
#include "../core/Verbose.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <cmath>
#include <imgui.h>

namespace {
// Representative point on a face (midpoint of its UV bounds). Stable for the
// same face geometry, so it survives re-tessellation between picks.
bool faceCenter(const TopoDS_Face& face, gp_Pnt& out) {
    try {
        BRepGProp_Face gp(face);
        Standard_Real u0, u1, v0, v1;
        gp.Bounds(u0, u1, v0, v1);
        gp_Vec n;
        gp.Normal((u0 + u1) * 0.5, (v0 + v1) * 0.5, out, n);
        return true;
    } catch (...) { return false; }
}

// Blend radius of a fillet face, if it is a recognisable analytic blend
// surface (cylinder on a straight edge, torus/sphere where edges curve or
// meet). Returns <0 when the face isn't such a surface — those we can't
// discriminate by radius and must fall back to the saved indices.
double faceBlendRadius(const TopoDS_Face& face) {
    try {
        BRepAdaptor_Surface s(face);
        switch (s.GetType()) {
            case GeomAbs_Cylinder: return s.Cylinder().Radius();
            case GeomAbs_Torus:    return s.Torus().MinorRadius();
            case GeomAbs_Sphere:   return s.Sphere().Radius();
            default:               return -1.0;
        }
    } catch (...) { return -1.0; }
}
} // namespace

// Every sketch in the document, as EdgeAnchor references. Real bodies are
// carved by several sketches (base extrude + profile cuts), so anchoring
// consults them all; sketches unrelated to this body simply never match.
// Prefers the cascade override (the edited sketch's FINAL state) over the
// live sketch: during a history replay the live sketch is rolled back through
// its SketchEditOp snapshots, so it holds a stale state exactly when this op
// re-executes — while the extrude below was rebuilt from the final one.
// `keep` extends the overrides' lifetime to the caller's scope.
static std::vector<EdgeAnchor::SketchRef> anchorSketches(
        Document& doc, std::vector<std::shared_ptr<materializr::Sketch>>& keep) {
    std::vector<EdgeAnchor::SketchRef> refs;
    for (int sid : doc.getAllSketchIds()) {
        if (auto ov = doc.cascadeSketchOverride(sid)) {
            keep.push_back(ov);
            refs.push_back({ sid, ov.get() });
        } else if (auto sk = doc.getSketch(sid)) {
            keep.push_back(sk);
            refs.push_back({ sid, sk.get() });
        }
    }
    return refs;
}

void FilletOp::computeAnchors(Document& doc) {
    m_edgeAnchors.clear();
    std::vector<std::shared_ptr<materializr::Sketch>> keep;
    m_edgeAnchors = EdgeAnchor::compute(m_edges, anchorSketches(doc, keep));
    // Success trace is --verbose only: execute() (and thus this) runs per
    // PREVIEW FRAME while a fillet is being dragged — an always-on stderr
    // flush per frame is real drag cost. Failure paths below stay loud.
    if (materializr::isVerbose()) {
        int corners = 0, rims = 0, arcs = 0, none = 0;
        for (const auto& a : m_edgeAnchors)
            (a.kind == EdgeAnchor::Anchor::Corner ? corners :
             a.kind == EdgeAnchor::Anchor::Rim    ? rims :
             a.kind == EdgeAnchor::Anchor::None   ? none : arcs)++;
        std::fprintf(stderr,
            "[Fillet] anchored %zu edges: %d corner, %d rim, %d arc, %d none\n",
            m_edges.size(), corners, rims, arcs, none);
    }
}

bool FilletOp::resolveAnchors(Document& doc, const TopoDS_Shape& base) {
    if (m_edgeAnchors.size() != m_edges.size()) return false;
    std::vector<TopoDS_Edge> resolved;
    std::vector<std::shared_ptr<materializr::Sketch>> keep;
    if (!EdgeAnchor::resolve(m_edgeAnchors, anchorSketches(doc, keep), base, resolved))
        return false;
    m_edges = std::move(resolved);
    if (materializr::isVerbose())
        std::fprintf(stderr, "[Fillet] resolved %zu edge(s) via generative anchors\n",
                     m_edges.size());
    return true;
}

FilletOp::FilletOp() = default;

void FilletOp::setBody(int bodyId) {
    m_bodyId = bodyId;
}

void FilletOp::setEdges(const std::vector<TopoDS_Edge>& edges) {
    m_edges = edges;
}

void FilletOp::setRadius(double radius) {
    m_radius = radius;
}

bool FilletOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_edges.empty() || m_radius <= 0.0) {
        return false;
    }

    try {
        // Store previous shape for undo
        m_previousShape = doc.getBody(m_bodyId);

        // If an upstream edit regenerated the body, our stored edges have
        // stale TShapes — re-bind them to their successors by carrier
        // geometry so editing (say) a neighbouring fillet's radius doesn't
        // kill this op. Fails (loudly, via editStep) only when an edge was
        // genuinely consumed by the upstream change.
        if (!SubShapeIndex::rebindEdges(m_previousShape, m_edges)) {
            // Ordinal/carrier matching failed — the edges moved (e.g. a sketch
            // DIMENSION edit relocated a filleted corner). Try re-finding them
            // by the sketch vertex they sit over (generative anchoring).
            if (!resolveAnchors(doc, m_previousShape)) {
                // LAST RESORT: topological names. Seam edges from a boolean
                // sit over no sketch feature (anchors fail by construction);
                // their gen-lineage refs resolve through the producing op's
                // ledger, republished on the body by the upstream replay.
                bool topoOk = false;
                if (!m_edgeRefs.empty() &&
                    m_edgeRefs.size() == m_edges.size()) {
                    materializr::topo::Context rc;
                    rc.doc = &doc;
                    rc.shape = m_previousShape;
                    rc.type = TopAbs_EDGE;
                    rc.gen = doc.bodyLedger(m_bodyId);
                    rc.crossRebuild = true;
                    std::vector<TopoDS_Shape> out;
                    if (materializr::topo::resolveSet(m_edgeRefs, rc, out) &&
                        out.size() == m_edges.size()) {
                        for (size_t i = 0; i < out.size(); ++i)
                            m_edges[i] = TopoDS::Edge(out[i]);
                        topoOk = true;
                        std::fprintf(stderr, "[Fillet] edges re-found by "
                                             "topo refs (gen/seam path)\n");
                    }
                }
                if (!topoOk) {
                    std::fprintf(stderr,
                        "[Fillet] rebindEdges + anchors + topo refs failed "
                        "(R=%.2f, %zu edges) — selected edge isn't in the "
                        "current body's edge map.\n",
                        m_radius, m_edges.size());
                    return false;
                }
            }
        }

        // Capture generative anchors from the (now-valid) edges the first time
        // we run — so a later dimension edit can re-find them by sketch feature.
        if (m_edgeAnchors.empty()) computeAnchors(doc);
        // And topological names (with the body's producing ledger in context,
        // so a SEAM edge gets its gen-lineage name).
        if (m_edgeRefs.empty() && !m_edges.empty()) {
            materializr::topo::Context mc;
            mc.doc = &doc;
            mc.shape = m_previousShape;
            mc.type = TopAbs_EDGE;
            mc.gen = doc.bodyLedger(m_bodyId);
            for (const auto& e : m_edges)
                m_edgeRefs.push_back(materializr::topo::mint(e, mc));
        }

        // Create fillet on the body shape
        BRepFilletAPI_MakeFillet fillet(m_previousShape);

        for (const auto& edge : m_edges) {
            fillet.Add(m_radius, edge);
        }

        fillet.Build();
        if (!fillet.IsDone()) {
            std::fprintf(stderr,
                "[Fillet] BRepFilletAPI.IsDone() returned false (R=%.2f) "
                "— OCCT refused to build the fillet at this radius.\n",
                m_radius);
            return false;
        }

        TopoDS_Shape candidate = fillet.Shape();
        if (candidate.IsNull()) {
            std::fprintf(stderr, "[Fillet] result shape is null (R=%.2f).\n",
                         m_radius);
            return false;
        }

        // IsDone() is necessary but NOT sufficient: when fillet radii on
        // adjacent edges overlap (the classic many-edges-at-once case), OCCT
        // happily returns IsDone()==true with a topologically INVALID solid —
        // self-intersecting blends or dropped faces — which is exactly the
        // "faces disappear / garbage geometry" failure. BRepCheck_Analyzer is
        // the authoritative validity test; reject anything it flags so a
        // corrupt body never gets committed to the document/history. (The bbox
        // and volume checks below catch grosser blow-outs but pass plenty of
        // invalid-but-plausibly-sized results.)
        if (!BRepCheck_Analyzer(candidate).IsValid()) {
            std::fprintf(stderr,
                "[Fillet] result failed BRepCheck_Analyzer (R=%.2f, %zu edges) "
                "— invalid topology, refusing to commit.\n",
                m_radius, m_edges.size());
            return false;
        }

        // OCCT's fillet API is permissive — IsDone() returns true even when
        // the radius exceeds what the geometry can support, and the result
        // is then a self-intersecting / overlapping mess instead of a clean
        // refusal. Two narrow sanity checks reject those without flagging
        // legitimate concave fillets (which ADD material and so make the
        // upper-bound volume check we used to have backwards):
        //   • Bounding box: a fillet should never GROW the body's bbox by
        //     more than a hair. Garbled-cube case (radius > half-extent)
        //     produces inverted shells whose bbox blows out — that's the
        //     signal we catch.
        //   • Volume: must be strictly > 0. Truly degenerate output (zero
        //     or negative volume) is the other failure mode.
        // (Steve: a coffee-cup rim could only fillet to 1.5 mm on the
        //  inside, and not at all on the outside — the old "volume must
        //  not exceed input × 1.01" rule rejected the inside concave
        //  fillets even when geometrically fine.)
        {
            // AddOptimal walks the actual geometry rather than the looser
            // tolerance-padded extents the plain Add uses. Shelled bodies
            // tend to land in OCCT with face seams at ~1e-3 tolerance,
            // which inflated the result bbox by ~8 mm on a 100 mm cup and
            // tripped the growth gate even on 0.1 mm fillets.
            Bnd_Box bbIn, bbOut;
            BRepBndLib::AddOptimal(m_previousShape, bbIn);
            BRepBndLib::AddOptimal(candidate,       bbOut);
            if (!bbIn.IsVoid() && !bbOut.IsVoid()) {
                Standard_Real ix0, iy0, iz0, ix1, iy1, iz1;
                Standard_Real ox0, oy0, oz0, ox1, oy1, oz1;
                bbIn .Get(ix0, iy0, iz0, ix1, iy1, iz1);
                bbOut.Get(ox0, oy0, oz0, ox1, oy1, oz1);
                const double slop = 1.01; // 1% tolerance for fp noise
                if (ox1 - ox0 > (ix1 - ix0) * slop ||
                    oy1 - oy0 > (iy1 - iy0) * slop ||
                    oz1 - oz0 > (iz1 - iz0) * slop) {
                    std::fprintf(stderr,
                        "[Fillet] bbox grew past slop (R=%.2f): "
                        "%.2fx%.2fx%.2f -> %.2fx%.2fx%.2f mm.\n",
                        m_radius,
                        ix1 - ix0, iy1 - iy0, iz1 - iz0,
                        ox1 - ox0, oy1 - oy0, oz1 - oz0);
                    return false;
                }
            }

            GProp_GProps gpOut;
            BRepGProp::VolumeProperties(candidate, gpOut);
            if (gpOut.Mass() < 1e-6) {
                std::fprintf(stderr,
                    "[Fillet] result volume ~= 0 (R=%.2f mm).\n",
                    m_radius);
                return false;
            }
        }

        // Publish the generation map (input edge -> blend faces) so the "gen"
        // naming strategy can name a blend face by its generating edge — the
        // general-kernel path for op-produced faces. Captured on every execute,
        // so a rebuild's ledger reflects the current geometry.
        m_ledger.capture(fillet, m_previousShape, TopAbs_EDGE);

        // Record the blend faces generated from each input edge so a later face
        // click can be traced back to this fillet for re-editing.
        m_generatedFaces.clear();
        for (const auto& edge : m_edges) {
            try {
                const TopTools_ListOfShape& gen = fillet.Generated(edge);
                // Range-based loop instead of TopTools_ListIteratorOfListOfShape,
                // whose header was removed in OCCT 8.0 (still works on 7.x).
                for (const TopoDS_Shape& s : gen) {
                    if (s.ShapeType() == TopAbs_FACE)
                        m_generatedFaces.push_back(s);
                }
            } catch (...) {}
        }

        // Update the body with the filleted shape (kept on the op too, so
        // serializeParams can index the generated faces against the result).
        m_resultShape = candidate;
        doc.updateBody(m_bodyId, m_resultShape);
        doc.setBodyLedger(m_bodyId, &m_ledger);
        return true;
    } catch (...) {
        return false;
    }
}

bool FilletOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) {
        return false;
    }

    try {
        doc.updateBody(m_bodyId, m_previousShape);
        return true;
    } catch (...) {
        return false;
    }
}

std::string FilletOp::description() const {
    return "Fillet R" + std::to_string(m_radius) + " on " +
           std::to_string(m_edges.size()) + " edge(s)";
}

void FilletOp::renderProperties() {
    ImGui::Text("Fillet");
    ImGui::Separator();

    ImGui::InputDouble("Radius", &m_radius, 0.1, 1.0, "%.3f");

    ImGui::Text("Edges: %d selected", static_cast<int>(m_edges.size()));
    ImGui::Text("Body ID: %d", m_bodyId);
}

OperationDiff FilletOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}

std::string FilletOp::serializeParams() const {
    // The edge set is persisted as ordinal indices into the INPUT shape's
    // canonical sub-shape map (see SubShapeIndex.h) — BREP round-trips the
    // shape byte-identically, so the indices resolve on reload. Generated
    // blend faces are indexed against the RESULT shape for click-to-edit.
    std::string blob;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "body=%d;radius=%.6f", m_bodyId, m_radius);
    blob += buf;
    if (!m_previousShape.IsNull() && !m_edges.empty()) {
        std::vector<TopoDS_Shape> edges(m_edges.begin(), m_edges.end());
        std::string idx = SubShapeIndex::serialize(m_previousShape, edges,
                                                   TopAbs_EDGE);
        if (!idx.empty()) blob += ";edges=" + idx;
    }
    if (!m_resultShape.IsNull() && !m_generatedFaces.empty()) {
        std::string idx = SubShapeIndex::serialize(m_resultShape,
                                                   m_generatedFaces,
                                                   TopAbs_FACE);
        if (!idx.empty()) blob += ";gen=" + idx;
    }
    // Generative anchors (additive; old readers ignore the key). See EdgeAnchor.
    std::string anc = EdgeAnchor::serialize(m_edgeAnchors);
    if (!anc.empty()) blob += ";anchor=" + anc;
    // Topological edge names (additive, LAST — length-prefixed opaque blobs
    // read to end-of-string). Persisting them keeps a SEAM fillet/chamfer
    // re-derivable after reload; absent in old files.
    if (!m_edgeRefs.empty()) {
        bool any = false;
        std::string rb;
        for (const auto& r : m_edgeRefs) {
            std::string b = r.serialize();
            rb += std::to_string(b.size()) + ":" + b;
            if (!r.empty()) any = true;
        }
        if (any) blob += ";edgerefs=" + rb;
    }
    return blob;
}

bool FilletOp::deserializeParams(const std::string& blob) {
    // Tolerant key=value parser. Unknown keys are ignored; missing keys keep
    // current defaults. Returns true if at least one key was understood.
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        // edgerefs holds length-prefixed opaque blobs, written last — read to
        // end-of-string (not to the next ';').
        if (key == "edgerefs") {
            std::string rest = blob.substr(eq + 1);
            m_edgeRefs.clear();
            size_t p = 0;
            while (p < rest.size()) {
                size_t c = rest.find(':', p);
                if (c == std::string::npos) break;
                size_t n = (size_t)std::atoll(rest.substr(p, c - p).c_str());
                if (c + 1 + n > rest.size()) break;
                m_edgeRefs.push_back(
                    materializr::topo::Ref::parse(rest.substr(c + 1, n)));
                p = c + 1 + n;
            }
            any = true;
            break;
        }
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "radius") { m_radius = std::atof(val.c_str()); any = true; }
        else if (key == "body")   { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "edges")  { m_edgeIndices = SubShapeIndex::parse(val); any = true; }
        else if (key == "gen")    { m_genFaceIndices = SubShapeIndex::parse(val); any = true; }
        else if (key == "anchor") {
            EdgeAnchor::parse(val, m_edgeAnchors);
            any = true;
        }
        pos = end + 1;
    }
    return any;
}

bool FilletOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0 || m_edgeIndices.empty()) return false;

    // Bind the before/after shapes for our body from the saved step.
    m_previousShape.Nullify();
    m_resultShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    for (const auto& [id, shp] : state.modifiedAfter)
        if (id == m_bodyId) { m_resultShape = shp; break; }
    if (m_previousShape.IsNull()) return false;

    // Re-resolve the filleted edges against the input shape. ALL must resolve
    // — a partial set would fillet the wrong geometry, so decline to ReplayOp.
    std::vector<TopoDS_Shape> resolved;
    if (!SubShapeIndex::resolveAll(m_previousShape, m_edgeIndices,
                                   TopAbs_EDGE, resolved)) {
        return false;
    }
    m_edges.clear();
    for (const auto& s : resolved) m_edges.push_back(TopoDS::Edge(s));

    // Blend faces (click-to-edit mapping) resolve against the result —
    // best-effort: their absence only disables face-click mapping.
    m_generatedFaces.clear();
    if (!m_resultShape.IsNull() && !m_genFaceIndices.empty()) {
        std::vector<TopoDS_Shape> gen;
        if (SubShapeIndex::resolveAll(m_resultShape, m_genFaceIndices,
                                      TopAbs_FACE, gen)) {
            m_generatedFaces = std::move(gen);
        }
    }
    return true;
}

void FilletOp::refreshGeneratedFaces(const TopoDS_Shape& currentBody) {
    if (currentBody.IsNull()) return;

    // The saved indices were captured against THIS fillet's local result shape;
    // resolving them against the final body (which may have more faces from
    // later fillets, and may have been moved by downstream Transforms) can drift
    // onto a neighbouring fillet's faces. So we rebind by geometry instead:
    // a constant-radius fillet's blend faces are analytic surfaces of radius
    // ≈ m_radius. Matching on radius keeps a 3 mm fillet from ever claiming a
    // neighbouring 4 mm fillet's faces, and is invariant under rigid moves.
    const double rtol = std::max(1e-3, 1e-2 * m_radius);

    std::vector<TopoDS_Shape> result;
    for (TopExp_Explorer ex(currentBody, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face& f = TopoDS::Face(ex.Current());
        double r = faceBlendRadius(f);
        if (r >= 0.0 && std::fabs(r - m_radius) <= rtol)
            result.push_back(f);
    }

    // Add any index-resolved faces the radius scan can't classify (free-form
    // blends), but exclude index faces whose radius clearly belongs to a
    // DIFFERENT fillet — those are the drift this rebind exists to reject.
    std::vector<TopoDS_Shape> idxFaces;
    if (!m_genFaceIndices.empty() &&
        SubShapeIndex::resolveAll(currentBody, m_genFaceIndices, TopAbs_FACE, idxFaces)) {
        for (const auto& s : idxFaces) {
            double r = faceBlendRadius(TopoDS::Face(s));
            if (r >= 0.0 && std::fabs(r - m_radius) > rtol) continue; // wrong fillet
            bool dup = false;
            for (const auto& g : result) if (g.IsSame(s)) { dup = true; break; }
            if (!dup) result.push_back(s);
        }
    }

    if (!result.empty())
        m_generatedFaces = std::move(result);
    else if (!idxFaces.empty())
        m_generatedFaces = std::move(idxFaces); // last-resort: trust the indices
}

bool FilletOp::ownsFace(const TopoDS_Shape& face) const {
    if (face.IsNull() || face.ShapeType() != TopAbs_FACE) return false;
    // A fillet blend is NEVER a plane (straight edges blend to cylinders,
    // curved/corner cases to tori/spheres/bsplines). Rehydrated generated-face
    // indices can mis-resolve after an old-save reload (ordinal drift) and
    // claim a big planar neighbour — clicking the slab top then opened the
    // fillet editor instead of the face's own properties.
    try {
        BRepAdaptor_Surface bs(TopoDS::Face(face));
        if (bs.GetType() == GeomAbs_Plane) return false;
    } catch (...) {}
    for (const auto& f : m_generatedFaces) {
        if (f.IsSame(face)) return true;
    }
    // Geometric fallback for when the body's faces were rebuilt (e.g. after a
    // replay) and are no longer IsSame to the stored ones.
    gp_Pnt q;
    if (!faceCenter(TopoDS::Face(face), q)) return false;
    for (const auto& f : m_generatedFaces) {
        gp_Pnt p;
        if (faceCenter(TopoDS::Face(f), p) && p.Distance(q) < 1e-4) return true;
    }
    return false;
}

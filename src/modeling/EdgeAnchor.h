#pragma once
#include <TopoDS_Shape.hxx>
#include <TopoDS_Edge.hxx>
#include <vector>
#include <string>

namespace materializr { class Sketch; }

// Generative edge tracking (experiment/generative-edges).
//
// A fillet/chamfer stores WHICH SKETCH FEATURE produced each edge it operates
// on, instead of an absolute position or an ordinal index. After a sketch
// DIMENSION edit relocates the edge, it is re-found from the sketch element's
// CURRENT position — surviving resizes that break ordinal/carrier matching.
//
// Two edge kinds of a sketch-extrude are handled (Phase 1 + 2):
//   Corner — a straight edge parallel to the extrude axis, sitting over a
//            single sketch VERTEX (point id).
//   Rim    — a straight top/bottom edge from a sketch LINE, at a fixed height
//            along the axis (line id + that height picks top vs bottom).
namespace EdgeAnchor {

struct Anchor {
    enum Kind { None, Corner, Rim } kind = None;
    int    elemId = -1;   // Corner: sketch point id;  Rim: sketch line id
    double h      = 0.0;  // Rim: signed height along the extrude axis
};

// One anchor per input edge (Kind::None for edges we can't attribute).
std::vector<Anchor> compute(const std::vector<TopoDS_Edge>& edges,
                            const materializr::Sketch& sk);

// Re-find each anchored edge in `base` at the sketch element's CURRENT
// position. Returns false (out cleared) unless EVERY anchor is non-None and
// resolves — a partial result would fillet the wrong geometry.
bool resolve(const std::vector<Anchor>& anchors, const materializr::Sketch& sk,
             const TopoDS_Shape& base, std::vector<TopoDS_Edge>& out);

// Serialize as "<sketchId>~<a0>~<a1>..." where each edge token is
// "C,<pid>" | "R,<lid>,<h>" | "N". Empty if there is nothing useful to store.
std::string serialize(int sketchId, const std::vector<Anchor>& anchors);
bool parse(const std::string& blob, int& sketchId, std::vector<Anchor>& anchors);

} // namespace EdgeAnchor

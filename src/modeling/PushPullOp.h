#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "TopoName.h"
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Vec.hxx>
#include <gp_Pnt.hxx>
#include <vector>
#include <string>

// Flip `normal` to point OUT of `solid`, but ONLY when the solid classifier
// gives an UNAMBIGUOUS antipodal verdict on a planar face: a point ε along
// +normal is INSIDE the solid while a point ε along −normal is OUTSIDE. That
// is the genuine "orientation inverted, normal points into material" case
// (bug #5). Any other reading — the correct OUT/IN pair, both IN, both OUT,
// ON, a thin-body overshoot, or a classifier throw — returns `normal`
// UNCHANGED, so the cases the PushPullOp war story protects (pockets, cavity
// walls, thin bodies, curved/axis faces) are provably untouched.
//
// `face` is the planar face being probed: the classifier samples a point that
// genuinely lies on its MATERIAL, because a holed/annular face's parametric
// centre can fall inside a hole — there both ±ε probes read OUTSIDE, the verdict
// is "ambiguous", and a reversed face would be left inverted (push/pull then
// runs backwards on it). `center` is used as the probe point when it's on the
// material, else a sampled interior point is used.
gp_Vec correctedOutwardNormal(const TopoDS_Shape& solid, const TopoDS_Face& face,
                              const gp_Pnt& center, const gp_Vec& normal);

class PushPullOp : public Operation {
public:
    struct Target {
        TopoDS_Face profile;
        int sourceBodyId = -1; // -1 means create as a new body
    };

    PushPullOp();
    ~PushPullOp() override = default;

    void setTargets(std::vector<Target> targets);
    void setDistance(double d); // signed: positive = extrude, negative = cut
    // Symmetric: sweep ±distance about the profile plane as ONE prism —
    // both sides in a single body, no mid-plane seam (the fused-halves
    // union keeps a seam edge on spline walls; OCCT can't unify those).
    void setSymmetric(bool s) { m_symmetric = s; }
    bool isSymmetric() const { return m_symmetric; }
    // Free-space (sourceBodyId < 0) targets only: when set, the swept prism is
    // SUBTRACTED from every VISIBLE body it intersects (each separately, hidden
    // bodies skipped, overlap cut and the rest discarded) instead of becoming a
    // new body. Falls back to the new-body behaviour when it intersects nothing
    // or a cut would be invalid. Off by default → identical to prior behaviour.
    void setCutIntersecting(bool c) { m_cutIntersecting = c; }
    bool getCutIntersecting() const { return m_cutIntersecting; }

    double getDistance() const { return m_distance; }

    // Cascade plumbing: remember which sketch + region(s) each target came
    // from when Push/Pull was triggered from sketch regions. The two arrays
    // are zip-aligned with m_targets: m_sketchSourceIds[i] is the sketch
    // that produced m_targets[i].profile, m_sketchSourceRegions[i] the
    // specific region index in that sketch (or -1 = "use first region").
    // -1 sketch id means a face-driven Push/Pull (no source sketch; that
    // target stays as-is during cascade).
    void setSketchSource(int targetIndex, int sketchId, int regionIndex = -1);
    bool hasAnySketchSource() const;
    int getSketchIdAt(int targetIndex) const;
    int targetCount() const { return static_cast<int>(m_targets.size()); }
    bool rebuildProfileFromSketch(Document& doc, int sketchId);

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Push/Pull"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "pushpull"; }
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override {
        std::vector<int> ids;
        for (const auto& t : m_targets)
            if (t.sourceBodyId >= 0) ids.push_back(t.sourceBodyId);
        return ids;
    }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    std::vector<Target> m_targets;
    double m_distance = 1.0;
    bool m_cutIntersecting = false; // free-space prism cuts intersecting visible bodies

    // Undo state
    std::vector<std::pair<int, TopoDS_Shape>> m_previousBodies; // sourceBody mutations
    bool m_symmetric = false;
    std::vector<int> m_createdBodyIds;                          // NewBody additions

    // Persisted across undo so the next execute (redo) reinserts free-floating
    // bodies under their previous ids, letting Document's tombstone restore
    // bring folder / colour / visibility / name back.
    std::vector<int> m_reuseBodyIds;
    size_t m_reuseIdx = 0;

    // Cascade plumbing — see setSketchSource() in the public section.
    std::vector<int> m_sketchSourceIds;     // sketch id per target (-1 = none)
    std::vector<int> m_sketchSourceRegions; // region index per target (-1 = first)
    // Reload support for FACE-driven targets: per-target ordinal index of the
    // profile face within the source body's pre-op shape (see
    // SubShapeIndex.h). 0 = not a face target / not resolvable.
    std::vector<int> m_faceIndices;
    // Per-target topological name of a FACE-driven target's profile (zip-aligned
    // with m_targets; empty for sketch/free targets). Minted on first execute,
    // re-resolved when the profile handle goes stale after an upstream edit
    // rebuilt the source body — so a pushed/pulled face FOLLOWS the edit.
    // Serialized additively as per-target `tr<i>=`.
    std::vector<materializr::topo::Ref> m_targetRefs;
    // Capture/refresh face-driven target profiles against the current bodies.
    void refreshFaceTargets(Document& doc);
};

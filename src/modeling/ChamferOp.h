#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "EdgeAnchor.h"
#include <TopoDS_Shape.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <vector>
#include <string>

class ChamferOp : public Operation {
public:
    ChamferOp();
    ~ChamferOp() override = default;

    // Parameters
    void setBody(int bodyId);
    void setEdges(const std::vector<TopoDS_Edge>& edges);
    void setDistance(double distance);
    // Second setback (along the OTHER face of each edge). <= 0 means symmetric:
    // both faces use setDistance(). > 0 makes an asymmetric chamfer.
    void setDistance2(double distance) { m_distance2 = distance; }

    // Generative edge tracking (see FilletOp / EdgeAnchor.h): source sketch so
    // a chamfered corner/rim edge follows a later dimension edit.
    void setSourceSketch(int sketchId) { m_sourceSketchId = sketchId; }
    int  getSourceSketch() const { return m_sourceSketchId; }

    // Getters
    int getBodyId() const { return m_bodyId; }
    double getDistance() const { return m_distance; }
    double getDistance2() const { return m_distance2; }
    bool isAsymmetric() const { return m_distance2 > 0.0; }

    // A single face adjacent to EVERY edge in `edges` (the loop's shared face),
    // or a null face if they don't all border one common face. Used as the
    // distance-1 reference so an asymmetric chamfer is consistent across a
    // multi-edge selection. For a single edge, returns one of its two faces.
    static TopoDS_Face sharedReferenceFace(const TopoDS_Shape& body,
                                           const std::vector<TopoDS_Edge>& edges);
    const std::vector<TopoDS_Edge>& getEdges() const { return m_edges; }
    // Body shape from the last execute()'s pre-state — used by the interactive
    // edit-by-clicking-face flow to preview an updated distance against the
    // body as it stood BEFORE this chamfer was applied.
    const TopoDS_Shape& getPreviousShape() const { return m_previousShape; }

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Chamfer"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "chamfer"; }
    bool ownsFace(const TopoDS_Shape& face) const override;
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override { return {m_bodyId}; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;
    // Re-resolve generated-face indices against the body's CURRENT shape (e.g.
    // after downstream transforms moved the geometry). Called by the loader
    // after all ops are rehydrated so ownsFace() works on the final body.
    void refreshGeneratedFaces(const TopoDS_Shape& currentBody);

private:
    int m_bodyId = -1;
    std::vector<TopoDS_Edge> m_edges;
    double m_distance = 1.0;
    double m_distance2 = -1.0; // <=0 = symmetric (use m_distance for both faces)
    TopoDS_Shape m_previousShape; // for undo
    // Chamfer faces produced by the last execute(), so a clicked face can be
    // mapped back to this op for re-editing.
    std::vector<TopoDS_Shape> m_generatedFaces;
    // Result shape + parsed sub-shape indices — same reload scheme as
    // FilletOp (see SubShapeIndex.h).
    TopoDS_Shape m_resultShape;
    std::vector<int> m_edgeIndices;
    std::vector<int> m_genFaceIndices;

    // Generative anchors (EdgeAnchor.h) — same scheme as FilletOp.
    int m_sourceSketchId = -1;
    std::vector<EdgeAnchor::Anchor> m_edgeAnchors;
    void computeAnchors(Document& doc);
    bool resolveAnchors(Document& doc, const TopoDS_Shape& base);

public:
    // Retrofit anchors for a chamfer loaded from a pre-anchoring project.
    // Anchoring consults every sketch in the document (see FilletOp).
    void ensureAnchors(Document& doc) {
        if (m_edgeAnchors.empty() && !m_edges.empty())
            computeAnchors(doc);
    }
};

#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "EdgeAnchor.h"
#include <TopoDS_Shape.hxx>
#include <TopoDS_Edge.hxx>
#include <vector>
#include <string>

class FilletOp : public Operation {
public:
    FilletOp();
    ~FilletOp() override = default;

    // Parameters
    void setBody(int bodyId);
    void setEdges(const std::vector<TopoDS_Edge>& edges);
    void setRadius(double radius);

    // Generative edge tracking (experiment/generative-edges): remember which
    // SKETCH generated this body so a filleted CORNER edge can be re-found by
    // the sketch VERTEX it sits over — surviving a dimension edit that
    // relocates the corner, where ordinal/carrier matching fails. -1 = unknown
    // (falls back to today's behaviour).
    void setSourceSketch(int sketchId) { m_sourceSketchId = sketchId; }
    int  getSourceSketch() const { return m_sourceSketchId; }

    // Getters
    int getBodyId() const { return m_bodyId; }
    double getRadius() const { return m_radius; }
    const std::vector<TopoDS_Edge>& getEdges() const { return m_edges; }
    // Body shape from the last execute()'s pre-state — needed by the
    // interactive edit-by-clicking-face flow so it can preview an updated
    // radius against the body as it stood BEFORE this fillet was applied.
    const TopoDS_Shape& getPreviousShape() const { return m_previousShape; }

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Fillet"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "fillet"; }
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
    double m_radius = 1.0;
    TopoDS_Shape m_previousShape; // for undo
    // Fillet (blend) faces produced by the last execute(), so a clicked face can
    // be mapped back to this op for re-editing.
    std::vector<TopoDS_Shape> m_generatedFaces;
    // The filleted result, captured by execute(). serializeParams indexes the
    // generated faces against it (they're sub-shapes of the result, not the
    // input); rehydrate restores it from the reload's after-state.
    TopoDS_Shape m_resultShape;
    // Sub-shape indices parsed from a saved project (see SubShapeIndex.h);
    // resolved against the before/after shapes in rehydrateFromReload.
    std::vector<int> m_edgeIndices;
    std::vector<int> m_genFaceIndices;

    // Generative anchors: one per m_edges entry, tagging the sketch feature
    // (corner vertex / rim line) each edge came from (see EdgeAnchor.h).
    int m_sourceSketchId = -1;
    std::vector<EdgeAnchor::Anchor> m_edgeAnchors;

public:
    // Capture anchors NOW if they're missing — used to retrofit fillets loaded
    // from a project made before anchoring existed, while their edges are
    // still valid (before any edit breaks the rebind). Anchoring consults
    // every sketch in the document, so no source-sketch setup is needed.
    void ensureAnchors(Document& doc) {
        if (m_edgeAnchors.empty() && !m_edges.empty())
            computeAnchors(doc);
    }

private:
    // Capture anchors from the current m_edges + source sketch (first execute).
    void computeAnchors(Document& doc);
    // Replace m_edges by re-finding each anchor at the sketch feature's CURRENT
    // position in `base`. Returns false unless EVERY edge resolves.
    bool resolveAnchors(Document& doc, const TopoDS_Shape& base);
};

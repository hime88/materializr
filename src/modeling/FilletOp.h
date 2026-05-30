#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
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
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;

private:
    int m_bodyId = -1;
    std::vector<TopoDS_Edge> m_edges;
    double m_radius = 1.0;
    TopoDS_Shape m_previousShape; // for undo
    // Fillet (blend) faces produced by the last execute(), so a clicked face can
    // be mapped back to this op for re-editing.
    std::vector<TopoDS_Shape> m_generatedFaces;
};

#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <TopTools_ListOfShape.hxx>
#include <string>

class ShellOp : public Operation {
public:
    ShellOp();
    ~ShellOp() override = default;

    // Parameters
    void setBody(int id);
    void setThickness(double t);
    void addFaceToRemove(const TopoDS_Face& face);
    void clearFacesToRemove();

    // Getters
    int getBodyId() const { return m_bodyId; }
    double getThickness() const { return m_thickness; }

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Shell"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "shell"; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;

private:
    int m_bodyId = -1;
    double m_thickness = 1.0;
    TopTools_ListOfShape m_facesToRemove;
    TopoDS_Shape m_previousShape;
};

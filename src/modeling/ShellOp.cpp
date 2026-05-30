#include "ShellOp.h"
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <TopoDS.hxx>
#include <imgui.h>

ShellOp::ShellOp() = default;

void ShellOp::setBody(int id) {
    m_bodyId = id;
}

void ShellOp::setThickness(double t) {
    m_thickness = t;
}

void ShellOp::addFaceToRemove(const TopoDS_Face& face) {
    m_facesToRemove.Append(face);
}

void ShellOp::clearFacesToRemove() {
    m_facesToRemove.Clear();
}

bool ShellOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_thickness <= 0.0) {
        return false;
    }

    try {
        // Store previous shape for undo
        m_previousShape = doc.getBody(m_bodyId);

        BRepOffsetAPI_MakeThickSolid shellMaker;
        shellMaker.MakeThickSolidByJoin(
            m_previousShape,
            m_facesToRemove,
            -m_thickness,     // negative = inward shell
            1.0e-3            // tolerance
        );
        shellMaker.Build();
        if (!shellMaker.IsDone()) {
            return false;
        }

        doc.updateBody(m_bodyId, shellMaker.Shape());
        return true;
    } catch (...) {
        return false;
    }
}

bool ShellOp::undo(Document& doc) {
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

std::string ShellOp::description() const {
    int faceCount = m_facesToRemove.Size();
    return "Shell thickness " + std::to_string(m_thickness) +
           " (" + std::to_string(faceCount) + " open face(s))";
}

void ShellOp::renderProperties() {
    ImGui::Text("Shell");
    ImGui::Separator();

    ImGui::InputDouble("Thickness", &m_thickness, 0.1, 1.0, "%.3f");

    int faceCount = m_facesToRemove.Size();
    ImGui::Text("Open faces: %d selected", faceCount);
    ImGui::Text("Body ID: %d", m_bodyId);
}

std::string ShellOp::serializeParams() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "thickness=%.6f", m_thickness);
    return buf;
}

bool ShellOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if (key == "thickness") { m_thickness = std::atof(val.c_str()); any = true; }
        pos = end + 1;
    }
    return any;
}

#include "PatternOp.h"
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <imgui.h>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

PatternOp::PatternOp() = default;

void PatternOp::setBody(int id) {
    m_bodyId = id;
}

void PatternOp::setType(PatternType t) {
    m_type = t;
}

void PatternOp::setCount(int n) {
    m_count = n;
}

void PatternOp::setLinearSpacing(double x, double y, double z) {
    m_spacingX = x;
    m_spacingY = y;
    m_spacingZ = z;
}

void PatternOp::setRadialAxis(double ax, double ay, double az) {
    m_axisX = ax;
    m_axisY = ay;
    m_axisZ = az;
}

void PatternOp::setRadialOrigin(double ox, double oy, double oz) {
    m_originX = ox;
    m_originY = oy;
    m_originZ = oz;
}

void PatternOp::setTotalAngle(double deg) {
    m_totalAngle = deg;
}

bool PatternOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_count < 2) {
        return false;
    }

    try {
        TopoDS_Shape sourceShape = doc.getBody(m_bodyId);
        if (sourceShape.IsNull()) {
            return false;
        }

        m_createdBodyIds.clear();

        for (int i = 1; i < m_count; ++i) {
            gp_Trsf trsf;

            if (m_type == PatternType::Linear) {
                // Translate by i * spacing
                gp_Vec offset(
                    i * m_spacingX,
                    i * m_spacingY,
                    i * m_spacingZ
                );
                trsf.SetTranslation(offset);
            } else {
                // Radial: rotate by i * (totalAngle / count) around axis
                double stepAngle = (m_totalAngle / m_count) * i;
                double stepAngleRad = stepAngle * M_PI / 180.0;
                gp_Ax1 axis(gp_Pnt(m_originX, m_originY, m_originZ),
                            gp_Dir(m_axisX, m_axisY, m_axisZ));
                trsf.SetRotation(axis, stepAngleRad);
            }

            BRepBuilderAPI_Transform transform(sourceShape, trsf, true);
            transform.Build();
            if (!transform.IsDone()) {
                // Undo any bodies created so far
                for (int createdId : m_createdBodyIds) {
                    doc.removeBody(createdId);
                }
                m_createdBodyIds.clear();
                return false;
            }

            int newId = doc.addBody(transform.Shape(), "Pattern " + std::to_string(i));
            m_createdBodyIds.push_back(newId);
        }

        return true;
    } catch (...) {
        // Clean up on exception
        for (int createdId : m_createdBodyIds) {
            try { doc.removeBody(createdId); } catch (...) {}
        }
        m_createdBodyIds.clear();
        return false;
    }
}

bool PatternOp::undo(Document& doc) {
    try {
        // Remove all created pattern copies
        for (int createdId : m_createdBodyIds) {
            doc.removeBody(createdId);
        }
        m_createdBodyIds.clear();
        return true;
    } catch (...) {
        return false;
    }
}

std::string PatternOp::description() const {
    std::string typeStr = (m_type == PatternType::Linear) ? "Linear" : "Radial";
    return typeStr + " Pattern, " + std::to_string(m_count) + " copies";
}

void PatternOp::renderProperties() {
    ImGui::Text("Pattern");
    ImGui::Separator();

    const char* typeItems[] = { "Linear", "Radial" };
    int typeIndex = static_cast<int>(m_type);
    if (ImGui::Combo("Type", &typeIndex, typeItems, 2)) {
        m_type = static_cast<PatternType>(typeIndex);
    }

    ImGui::InputInt("Count", &m_count);
    if (m_count < 2) {
        m_count = 2;
    }

    ImGui::Text("Body ID: %d", m_bodyId);

    if (m_type == PatternType::Linear) {
        ImGui::InputDouble("Spacing X", &m_spacingX, 0.1, 1.0, "%.3f");
        ImGui::InputDouble("Spacing Y", &m_spacingY, 0.1, 1.0, "%.3f");
        ImGui::InputDouble("Spacing Z", &m_spacingZ, 0.1, 1.0, "%.3f");
    } else {
        ImGui::InputDouble("Axis X", &m_axisX, 0.1, 1.0, "%.3f");
        ImGui::InputDouble("Axis Y", &m_axisY, 0.1, 1.0, "%.3f");
        ImGui::InputDouble("Axis Z", &m_axisZ, 0.1, 1.0, "%.3f");
        ImGui::InputDouble("Total Angle", &m_totalAngle, 1.0, 15.0, "%.1f");
    }

    if (!m_createdBodyIds.empty()) {
        ImGui::Text("Created %d copies", static_cast<int>(m_createdBodyIds.size()));
    }
}

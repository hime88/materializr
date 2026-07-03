#include "DimensionInput.h"
#include "../core/NumParse.h"
#include <imgui.h>
#include <cstdio>
#include <cstdlib>

namespace materializr {

DimensionInput::DimensionInput() = default;

void DimensionInput::show(const std::string& label, double currentValue, glm::vec2 screenPos) {
    m_label = label;
    m_value = currentValue;
    m_screenPos = screenPos;
    m_visible = true;
    m_confirmed = false;
    m_showAngle = false;
    m_firstFrame = true;
    std::snprintf(m_valueBuffer, sizeof(m_valueBuffer), "%.4f", currentValue);
}

void DimensionInput::hide() {
    m_visible = false;
    m_confirmed = false;
}

bool DimensionInput::isVisible() const {
    return m_visible;
}

void DimensionInput::showWithAngle(double length, double angleDeg, glm::vec2 screenPos) {
    show("Length", length, screenPos);
    m_showAngle = true;
    m_angle = angleDeg;
    std::snprintf(m_angleBuffer, sizeof(m_angleBuffer), "%.2f", angleDeg);
}

double DimensionInput::getValue() const {
    return m_value;
}

double DimensionInput::getAngle() const {
    return m_angle;
}

bool DimensionInput::render() {
    if (!m_visible)
        return false;

    m_confirmed = false;

    ImGui::SetNextWindowPos(ImVec2(m_screenPos.x + 15.0f, m_screenPos.y + 15.0f),
                            ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_AlwaysAutoResize
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoSavedSettings;

    ImGui::Begin("##DimensionInput", nullptr, flags);

    ImGui::Text("%s", m_label.c_str());
    ImGui::SetNextItemWidth(120.0f);

    if (m_firstFrame) {
        ImGui::SetKeyboardFocusHere();
        m_firstFrame = false;
    }

    bool valueEntered = ImGui::InputText("##value", m_valueBuffer, sizeof(m_valueBuffer),
                                         ImGuiInputTextFlags_EnterReturnsTrue);

    if (m_showAngle) {
        ImGui::Text("Angle");
        ImGui::SetNextItemWidth(120.0f);
        bool angleEntered = ImGui::InputText("##angle", m_angleBuffer, sizeof(m_angleBuffer),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (angleEntered)
            valueEntered = true;
    }

    if (valueEntered) {
        // parseFinite: garbage / non-finite entry leaves the previous value
        // (NaN/inf here flowed into sketch geometry).
        (void)materializr::parseFinite(m_valueBuffer, m_value);
        if (m_showAngle) {
            (void)materializr::parseFinite(m_angleBuffer, m_angle);
        }
        m_confirmed = true;
        m_visible = false;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_visible = false;
        m_confirmed = false;
    }

    ImGui::End();

    return m_confirmed;
}

} // namespace materializr

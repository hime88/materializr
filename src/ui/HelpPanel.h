#pragma once

namespace materializr {

// A docked panel that walks new users through the basics: navigation, sketches,
// modelling, history. Toggled from Help → User Guide.
class HelpPanel {
public:
    HelpPanel() = default;

    void setVisible(bool v) { m_visible = v; }
    void toggle()           { m_visible = !m_visible; }
    bool isVisible() const  { return m_visible; }

    void render();

private:
    bool m_visible = false;
};

} // namespace materializr

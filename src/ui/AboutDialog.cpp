#include "AboutDialog.h"
#include <imgui.h>

#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

#ifndef MATERIALIZR_VERSION
#define MATERIALIZR_VERSION "0.0.0"
#endif

namespace materializr {

namespace {

// Open a URL in the user's default browser. POSIX uses xdg-open; Windows uses
// ShellExecuteA. No-op (and silently ignored) if the call fails — used only
// for the GitHub link from the About dialog, which is a convenience.
void openInBrowser(const char* url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = std::string("xdg-open ") + "\"" + url + "\" >/dev/null 2>&1 &";
    [[maybe_unused]] int rc = std::system(cmd.c_str());
#endif
}

} // namespace

AboutDialog::AboutDialog() = default;

void AboutDialog::setVisible(bool vis) {
    m_visible = vis;
}

bool AboutDialog::isVisible() const {
    return m_visible;
}

void AboutDialog::render() {
    if (!m_visible) return;

    ImGui::OpenPopup("About Materializr");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 340), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("About Materializr", &m_visible,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {

        // App name — slightly larger via font scaling.
        float origScale = ImGui::GetFont()->Scale;
        ImGui::GetFont()->Scale = 2.0f;
        ImGui::PushFont(ImGui::GetFont());
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("Materializr").x) * 0.5f);
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Materializr");
        ImGui::GetFont()->Scale = origScale;
        ImGui::PopFont();

        std::string verLine = std::string("Version ") + MATERIALIZR_VERSION;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(verLine.c_str()).x) * 0.5f);
        ImGui::Text("%s", verLine.c_str());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const char* desc = "Open-source parametric 3D CAD";
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(desc).x) * 0.5f);
        ImGui::Text("%s", desc);

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.65f, 0.75f, 0.95f, 1.0f), "Credits");
        ImGui::BulletText("R4stl1n — original project");
        ImGui::BulletText("stevebushwa — design, testing, direction");
        ImGui::BulletText("Claude (Anthropic) — pair-coding collaborator");

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           "Built with OpenCASCADE, Dear ImGui, GLFW, GLM, libcurl.");
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "License: MIT");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const char* repoUrl = "https://github.com/materializr-cad/materializr";
        float btnW = 170.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnW) * 0.5f);
        if (ImGui::Button("Open Project on GitHub", ImVec2(btnW, 0))) {
            openInBrowser(repoUrl);
        }
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(repoUrl).x) * 0.5f);
        ImGui::TextDisabled("%s", repoUrl);

        ImGui::Spacing();
        float closeW = 100.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - closeW) * 0.5f);
        if (ImGui::Button("Close", ImVec2(closeW, 0))) {
            m_visible = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace materializr

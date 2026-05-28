#include "FileDialogs.h"
#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <filesystem>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace materializr {

// True if `p` is an existing directory. Platform-split so the Linux path keeps
// its original POSIX stat() behaviour untouched.
static bool dlgIsDir(const std::string& p) {
#ifdef _WIN32
    std::error_code ec;
    return std::filesystem::is_directory(p, ec);
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static struct {
    bool open = false;
    bool isSave = false;
    std::string title;
    char pathBuf[1024] = {};
    char nameBuf[256] = {};
    std::string currentDir;
    std::vector<std::string> entries;
    std::vector<bool> isDirVec;
    std::vector<FileFilter> filters;
    int selectedFilter = 0;
    int selectedEntry = -1;
    std::function<void(const std::string&)> callback;

    // Add a file entry, applying the active extension filter. Shared by both
    // platform listing paths below.
    void considerFile(std::vector<std::pair<std::string, bool>>& items,
                      const std::string& name) {
        if (!filters.empty() && selectedFilter < (int)filters.size()) {
            if (!matchesFilter(name, filters[selectedFilter].pattern)) return;
        }
        items.push_back({name, false});
    }

    void refresh() {
        entries.clear();
        isDirVec.clear();
        selectedEntry = -1;

        std::vector<std::pair<std::string, bool>> items;

#ifdef _WIN32
        // std::filesystem doesn't yield "." / "..", so add parent navigation.
        items.push_back({"..", true});
        std::error_code ec;
        for (std::filesystem::directory_iterator it(currentDir, ec), end;
             it != end && !ec; it.increment(ec)) {
            std::string name = it->path().filename().string();
            if (name.empty()) continue;
            if (it->is_directory(ec)) items.push_back({name, true});
            else considerFile(items, name);
        }
#else
        DIR* dir = opendir(currentDir.c_str());
        if (!dir) return;

        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            std::string name = ent->d_name;
            if (name == ".") continue;

            std::string fullPath = currentDir + "/" + name;
            struct stat st;
            bool isDirectory = false;
            if (stat(fullPath.c_str(), &st) == 0) {
                isDirectory = S_ISDIR(st.st_mode);
            }

            if (isDirectory) {
                items.push_back({name, true});
            } else {
                considerFile(items, name);
            }
        }
        closedir(dir);
#endif

        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });

        for (auto& item : items) {
            entries.push_back(item.first);
            isDirVec.push_back(item.second);
        }
    }

    static bool matchesFilter(const std::string& name, const std::string& pattern) {
        std::string nameLower = name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

        size_t pos = 0;
        while (pos < pattern.size()) {
            size_t next = pattern.find_first_of(" ;", pos);
            std::string ext = pattern.substr(pos, next - pos);
            if (ext.size() > 1 && ext[0] == '*') ext = ext.substr(1);
            if (!ext.empty()) {
                std::string extLower = ext;
                std::transform(extLower.begin(), extLower.end(), extLower.begin(), ::tolower);
                if (nameLower.size() >= extLower.size() &&
                    nameLower.substr(nameLower.size() - extLower.size()) == extLower) {
                    return true;
                }
            }
            if (next == std::string::npos) break;
            pos = next + 1;
        }
        return false;
    }

    void init(const std::string& t, bool save, const std::string& defaultName,
              const std::vector<FileFilter>& f,
              std::function<void(const std::string&)> cb) {
        open = true;
        isSave = save;
        title = t;
        filters = f;
        callback = cb;
        selectedFilter = 0;
        selectedEntry = -1;
        std::memset(nameBuf, 0, sizeof(nameBuf));

#ifdef _WIN32
        // Use forward slashes throughout so the navigation logic (rfind('/'),
        // currentDir + "/" + name) works; the Win32/filesystem APIs accept them.
        std::error_code ec;
        currentDir = std::filesystem::current_path(ec).string();
        std::replace(currentDir.begin(), currentDir.end(), '\\', '/');
        if (currentDir.empty()) currentDir = "C:/";
#else
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {
            currentDir = cwd;
        } else {
            currentDir = "/home";
        }
#endif
        std::strncpy(pathBuf, currentDir.c_str(), sizeof(pathBuf) - 1);

        if (!defaultName.empty()) {
            std::strncpy(nameBuf, defaultName.c_str(), sizeof(nameBuf) - 1);
        }

        refresh();
    }
} s_state;

void FileDialogs::openFile(const std::string& title,
                            const std::vector<FileFilter>& filters,
                            std::function<void(const std::string&)> callback) {
    s_state.init(title, false, "", filters, callback);
}

void FileDialogs::saveFile(const std::string& title,
                            const std::string& defaultName,
                            const std::vector<FileFilter>& filters,
                            std::function<void(const std::string&)> callback) {
    s_state.init(title, true, defaultName, filters, callback);
}

bool FileDialogs::isOpen() {
    return s_state.open;
}

void FileDialogs::render() {
    if (!s_state.open) return;

    ImGui::SetNextWindowSize(ImVec2(600, 450), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));

    bool stillOpen = true;
    ImGui::Begin(s_state.title.c_str(), &stillOpen,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking);

    if (!stillOpen) {
        s_state.open = false;
        if (s_state.callback) s_state.callback("");
        ImGui::End();
        return;
    }

    // Path bar
    ImGui::Text("Location:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##path", s_state.pathBuf, sizeof(s_state.pathBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (dlgIsDir(s_state.pathBuf)) {
            s_state.currentDir = s_state.pathBuf;
            s_state.refresh();
        }
    }

    // Filter
    if (!s_state.filters.empty()) {
        ImGui::Text("Filter:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(300);
        if (ImGui::BeginCombo("##filter",
                s_state.filters[s_state.selectedFilter].description.c_str())) {
            for (int i = 0; i < (int)s_state.filters.size(); i++) {
                if (ImGui::Selectable(s_state.filters[i].description.c_str(),
                                      i == s_state.selectedFilter)) {
                    s_state.selectedFilter = i;
                    s_state.refresh();
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    // File list
    float bottomHeight = s_state.isSave ? 65.0f : 35.0f;
    ImGui::BeginChild("##files", ImVec2(0, -bottomHeight), true);
    for (int i = 0; i < (int)s_state.entries.size(); i++) {
        const auto& name = s_state.entries[i];
        bool dir = s_state.isDirVec[i];

        std::string label = dir ? "[DIR]  " + name : "       " + name;
        bool selected = (i == s_state.selectedEntry);

        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            s_state.selectedEntry = i;

            if (!dir) {
                std::strncpy(s_state.nameBuf, name.c_str(), sizeof(s_state.nameBuf) - 1);
            }

            if (ImGui::IsMouseDoubleClicked(0)) {
                if (dir) {
                    if (name == "..") {
                        size_t slash = s_state.currentDir.rfind('/');
                        if (slash != std::string::npos && slash > 0) {
                            s_state.currentDir = s_state.currentDir.substr(0, slash);
                        } else {
                            s_state.currentDir = "/";
                        }
                    } else {
                        s_state.currentDir += "/" + name;
                    }
                    std::strncpy(s_state.pathBuf, s_state.currentDir.c_str(),
                                 sizeof(s_state.pathBuf) - 1);
                    s_state.refresh();
                } else {
                    std::string result = s_state.currentDir + "/" + name;
                    s_state.open = false;
                    if (s_state.callback) s_state.callback(result);
                }
            }
        }
    }
    ImGui::EndChild();

    // Save: filename input
    if (s_state.isSave) {
        ImGui::Text("Name:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-160);
        ImGui::InputText("##name", s_state.nameBuf, sizeof(s_state.nameBuf));
        ImGui::SameLine();
    }

    if (ImGui::Button(s_state.isSave ? "Save" : "Open", ImVec2(70, 0))) {
        std::string result;
        if (s_state.isSave && std::strlen(s_state.nameBuf) > 0) {
            result = s_state.currentDir + "/" + s_state.nameBuf;
        } else if (!s_state.isSave && s_state.selectedEntry >= 0 &&
                   !s_state.isDirVec[s_state.selectedEntry]) {
            result = s_state.currentDir + "/" + s_state.entries[s_state.selectedEntry];
        }
        s_state.open = false;
        if (s_state.callback) s_state.callback(result);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(70, 0))) {
        s_state.open = false;
        if (s_state.callback) s_state.callback("");
    }

    ImGui::End();
}

} // namespace materializr

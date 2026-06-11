#include "FileDialogs.h"
#include <imgui.h>
#include <memory>
#include <filesystem>
#if defined(__ANDROID__)
// Android has no native file-picker helper (zenity/kdialog/WinAPI), so pfd is
// excluded; openFile/saveFile drive the in-app ImGui browser instead.
#include <SDL.h>   // SDL_AndroidGetExternalStoragePath
#else
#include "portable-file-dialogs.h"
#endif

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

#if defined(__ANDROID__)
// True if the directory can actually be listed (read permission), not just that
// it exists — distinguishes "All-files access granted" from "not yet".
static bool dlgCanList(const char* p) {
    DIR* d = opendir(p);
    if (!d) return false;
    closedir(d);
    return true;
}
#endif

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

namespace {

// portable-file-dialogs takes filter strings in the form
//   { "label", "pattern1 pattern2 ...", "label2", "pattern3 ...", ... }
// flattened into a single vector. Translate FileFilter into that shape.
#if !defined(__ANDROID__)
std::vector<std::string> pfdFilters(const std::vector<FileFilter>& filters) {
    std::vector<std::string> v;
    v.reserve(filters.size() * 2);
    for (const auto& f : filters) {
        v.push_back(f.description);
        v.push_back(f.pattern);
    }
    return v;
}
#endif

// Async pfd dialog state. FileDialogs::render() polls .ready(0) every
// frame — without that the main thread blocks inside .result() until the
// picker closes and the WM throws a "not responding" warning on top of
// our window. Polling keeps the frame loop running so the OS keeps
// seeing input / draw activity. (Steve: "while the file explorer is
// open I get a 'materializr is not responding' popup".)
#if !defined(__ANDROID__)
struct AsyncDlgState {
    std::unique_ptr<pfd::open_file> openH;
    std::unique_ptr<pfd::save_file> saveH;
    std::function<void(const std::string&)> callback;
    bool active() const { return openH != nullptr || saveH != nullptr; }
    void clear() {
        openH.reset();
        saveH.reset();
        callback = nullptr;
    }
};
static AsyncDlgState s_async;
#endif
// Last directory the picker landed in. Application syncs to/from
// AppSettings::lastFileDir at load + save time so it survives a relaunch.
static std::string s_lastDir;

} // namespace

void FileDialogs::setLastDir(const std::string& dir) { s_lastDir = dir; }
const std::string& FileDialogs::getLastDir() { return s_lastDir; }

#if defined(__ANDROID__)
// Open the in-app ImGui browser (s_state). No native picker on Android, so this
// is the file UI. Rooted at the last-used dir, falling back to the app's
// writable external-storage path (/sdcard/Android/data/<pkg>/files).
static void launchInAppBrowser(const std::string& title, bool isSave,
                               const std::string& defaultName,
                               const std::vector<FileFilter>& filters,
                               std::function<void(const std::string&)> cb) {
    std::string start = s_lastDir;
    if (start.empty() || !dlgIsDir(start)) {
        // Prefer the user-visible storage root (needs All-files access); fall
        // back to the app's own external dir, which is always readable.
        if (dlgCanList("/storage/emulated/0")) {
            start = "/storage/emulated/0";
        } else {
            const char* ext = SDL_AndroidGetExternalStoragePath();
            start = (ext && dlgIsDir(ext)) ? ext : "/";
        }
    }
    s_state.open = true;
    s_state.isSave = isSave;
    s_state.title = title.empty() ? (isSave ? "Save File" : "Open File") : title;
    s_state.currentDir = start;
    std::strncpy(s_state.pathBuf, start.c_str(), sizeof(s_state.pathBuf) - 1);
    s_state.pathBuf[sizeof(s_state.pathBuf) - 1] = '\0';
    std::strncpy(s_state.nameBuf, defaultName.c_str(), sizeof(s_state.nameBuf) - 1);
    s_state.nameBuf[sizeof(s_state.nameBuf) - 1] = '\0';
    s_state.filters = filters;
    s_state.selectedFilter = 0;
    s_state.selectedEntry = -1;
    s_state.callback = std::move(cb);
    s_state.refresh();
}
#endif

void FileDialogs::openFile(const std::string& title,
                            const std::vector<FileFilter>& filters,
                            std::function<void(const std::string&)> callback) {
#if defined(__ANDROID__)
    if (s_state.open) return; // one picker at a time
    launchInAppBrowser(title, /*isSave=*/false, "", filters, std::move(callback));
#else
    if (s_async.active()) return; // one picker at a time
    // Seed pfd with the directory the user last picked in so they don't
    // have to re-navigate from ~ every time. zenity / kdialog interpret
    // their --filename arg as a FILE path; without a trailing slash,
    // "/home/kevin/Documents" is read as "filename = Documents in folder
    // /home/kevin" and the picker lands at $HOME instead of inside
    // Documents. (Steve: "the dialog returns me to my home directory,
    // not the last folder".)
    std::string seed = s_lastDir;
    if (!seed.empty() && seed.back() != '/' && seed.back() != '\\') {
        seed += '/';
    }
    s_async.openH = std::make_unique<pfd::open_file>(
        title, seed, pfdFilters(filters));
    s_async.callback = std::move(callback);
#endif
}

void FileDialogs::saveFile(const std::string& title,
                            const std::string& defaultName,
                            const std::vector<FileFilter>& filters,
                            std::function<void(const std::string&)> callback) {
#if defined(__ANDROID__)
    if (s_state.open) return;
    launchInAppBrowser(title, /*isSave=*/true, defaultName, filters, std::move(callback));
#else
    if (s_async.active()) return;
    // pfd's save_file wants a path-ish default — concat the last-used dir
    // with the supplied filename so the picker opens IN that folder with
    // the suggested filename already in the field.
    std::string seed = defaultName;
    if (!s_lastDir.empty()) {
        std::filesystem::path p(s_lastDir);
        if (!defaultName.empty()) p /= defaultName;
        seed = p.string();
    }
    s_async.saveH = std::make_unique<pfd::save_file>(
        title, seed, pfdFilters(filters),
        pfd::opt::force_overwrite);
    s_async.callback = std::move(callback);
#endif
}

bool FileDialogs::isOpen() {
#if defined(__ANDROID__)
    return s_state.open;
#else
    return s_async.active() || s_state.open;
#endif
}

void FileDialogs::render() {
#if !defined(__ANDROID__)
    // Native (pfd) path: poll the spawned helper subprocess each frame.
    // ready(0) is a non-blocking check; when it returns true we fetch the
    // path and fire the callback exactly once, then clear the state.
    if (s_async.active()) {
        bool done = false;
        std::string result;
        if (s_async.openH) {
            if (s_async.openH->ready(0)) {
                done = true;
                auto r = s_async.openH->result();
                if (!r.empty()) result = r.front();
            }
        } else if (s_async.saveH) {
            if (s_async.saveH->ready(0)) {
                done = true;
                result = s_async.saveH->result();
            }
        }
        if (done) {
            // Remember the directory of the successful pick so the next
            // open / save lands here. Empty result = the user cancelled —
            // we leave s_lastDir as it was.
            if (!result.empty()) {
                try {
                    std::filesystem::path p(result);
                    if (p.has_parent_path())
                        s_lastDir = p.parent_path().string();
                } catch (...) {}
            }
            auto cb = std::move(s_async.callback);
            s_async.clear();
            if (cb) cb(result);
        }
        return; // legacy in-app dialog stays dormant while pfd owns the picker
    }
#endif // !__ANDROID__

    // In-app ImGui dialog. On desktop this is the legacy fall-back; on Android
    // (no native picker) it is the file UI, driven by openFile/saveFile above.
    // above; kept around as a fall-back wire-point if a no-helper Linux
    // box ever needs it.
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

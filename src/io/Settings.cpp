#include "Settings.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <filesystem>

namespace materializr {

namespace {

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

// Pull an int from the map if present and parseable; otherwise leave `out`.
void readInt(const std::map<std::string, std::string>& kv, const char* key, int& out) {
    auto it = kv.find(key);
    if (it == kv.end()) return;
    try { out = std::stoi(it->second); } catch (...) { /* keep default */ }
}

void readBool(const std::map<std::string, std::string>& kv, const char* key, bool& out) {
    auto it = kv.find(key);
    if (it == kv.end()) return;
    std::string v = it->second;
    for (auto& c : v) c = static_cast<char>(::tolower(c));
    if (v == "1" || v == "true" || v == "yes" || v == "on")  out = true;
    else if (v == "0" || v == "false" || v == "no" || v == "off") out = false;
    // anything else: keep default
}

} // namespace

std::string SettingsIO::defaultPath() {
#ifdef _WIN32
    // %APPDATA%\materializr\settings.cfg (fall back to the user profile).
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
        return std::string(appdata) + "\\materializr\\settings.cfg";
    if (const char* up = std::getenv("USERPROFILE"); up && *up)
        return std::string(up) + "\\materializr\\settings.cfg";
    return "materializr-settings.cfg"; // last resort: current directory
#else
    std::string base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        base = xdg;
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        base = std::string(home) + "/.config";
    } else {
        base = "."; // last resort: current directory
    }
    return base + "/materializr/settings.cfg";
#endif
}

AppSettings SettingsIO::load(const std::string& path) {
    AppSettings s; // defaults

    std::ifstream ifs(path);
    if (!ifs.is_open()) return s; // no file yet: use defaults

    // Parse every `key = value` line into a map. Blank lines and `#`/`;`
    // comments are skipped; malformed lines are ignored.
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(ifs, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (!key.empty()) kv[key] = val;
    }

    // Map known keys onto the struct. Unknown keys are simply never read.
    readInt (kv, "theme",                s.theme);
    readInt (kv, "orbitButton",          s.orbitButton);
    readInt (kv, "panButton",            s.panButton);
    readBool(kv, "levelOrbit",           s.levelOrbit);
    readBool(kv, "autosaveEnabled",      s.autosaveEnabled);
    readInt (kv, "autosaveIntervalSec",  s.autosaveIntervalSec);

    return s;
}

bool SettingsIO::save(const std::string& path, const AppSettings& s) {
    // Make sure the parent directory exists.
    try {
        std::filesystem::path p(path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
    } catch (...) { /* fall through and let the open fail if it must */ }

    std::ofstream ofs(path, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) return false;

    ofs << "# Materializr settings. Unknown keys are ignored; missing keys use\n"
           "# defaults. Safe to edit by hand or to carry across versions.\n";
    ofs << "theme = "               << s.theme               << "\n";
    ofs << "orbitButton = "         << s.orbitButton         << "\n";
    ofs << "panButton = "           << s.panButton           << "\n";
    ofs << "levelOrbit = "          << (s.levelOrbit ? "true" : "false") << "\n";
    ofs << "autosaveEnabled = "     << (s.autosaveEnabled ? "true" : "false") << "\n";
    ofs << "autosaveIntervalSec = " << s.autosaveIntervalSec << "\n";

    return ofs.good();
}

} // namespace materializr

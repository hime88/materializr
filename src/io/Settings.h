#pragma once
#include <string>

namespace materializr {

// User-facing application preferences that persist between launches. Defaults
// here are the out-of-the-box behaviour and are also the fallback whenever a
// key is missing or unreadable in the settings file.
struct AppSettings {
    int  theme              = 0;    // 0 = Dark, 1 = Light
    int  orbitButton        = 2;    // ImGuiMouseButton: 0=Left, 1=Right, 2=Middle
    int  panButton          = 1;
    bool levelOrbit         = true; // turntable (level) vs free trackball orbit
    // Uniform multiplier on camera input deltas (orbit / pan / zoom). 1.0 =
    // the hard-coded baseline; below 1 calms a touchy trackpad, above 1
    // snappier. Persisted so the user doesn't have to re-set it every launch.
    float mouseSensitivity  = 1.0f;
    bool autosaveEnabled    = false;
    int  autosaveIntervalSec = 120;
    bool invertCubeDrag     = false; // ViewCube drag-to-orbit direction
    // Max seconds between two clicks for them to register as a double-click
    // (ImGui default 0.30). Trackpad users often double-tap slower than a
    // mouse; raising this stops a slow body double-click from being seen as
    // two single clicks (which would cycle-select past the body to the sketch
    // behind it).
    float doubleClickTimeSec = 0.30f;

    // --- Rendering ---
    float lightAmbient   = 0.40f; // 0..1 base illumination; higher = softer shadows
    bool  lightHeadlight = false; // key light tracks the camera (no large shadows)
    bool  lightFill      = true;  // soft opposing fill light to lift dark sides
    int   msaaSamples    = 4;     // viewport anti-aliasing: 0=off, 2, 4, 8
    int   meshQuality    = 1;     // tessellation density: 0=Low,1=Medium,2=High,3=Ultra
    float selectionLineWidth = 3.0f; // px width of highlighted edges/body outlines (1..10)
    bool  showToolbarTooltips = true; // hover-tip describing each toolbar button

    // --- Session ---
    bool  autoOpenLastProject = false;     // re-open the most recent project on launch
    // Path of the project currently open. Updated on save / load; cleared on
    // File → Close Project. On launch (with autoOpenLastProject on) this is
    // read and the file is loaded if it still exists — so "I closed the
    // project before quitting" produces an empty launch next time, and
    // "I just quit while working" reopens where you left off.
    std::string lastProjectPath;

    // Hit the GitHub releases API at startup and pop a "newer release
    // available" dialog if the running build is older. Off in --safe-mode
    // (no surprise network calls when the user is recovering from a crash).
    bool  checkForUpdatesOnLaunch = true;

    // --- Snap / grid (persisted) ---
    // Snap-to-grid toggle and step (mm) shared by the sketch grid and the
    // body/sketch gizmo translate. Persisting these means the user doesn't
    // have to re-enable snap and re-pick a 1 mm step every launch.
    bool  snapToGrid    = true;
    float sketchGridStep = 1.0f;

    // --- Sketch inferences ---
    // Default inference level applied at launch and whenever no project
    // override exists. 0 = Full (includes hover-to-charge), 1 = Reduced
    // (classic guides), 2 = Off (grid + endpoint only). Mirrors
    // SketchTool::InferenceLevel; kept as int to keep this header free of
    // a sketch-tool dependency.
    int  inferenceLevel = 0;
    // Whether the sketch toolbar shows the live Full/Reduced/Off cycle
    // button. Off lets users who set the level once in Settings declutter
    // the toolbar; on (default) keeps the per-session live toggle visible.
    bool showInferenceToolbarToggle = true;
    // Line angle-snap increment in degrees (0 = off). The line tool snaps its
    // direction to multiples of this from the segment anchor. Default 15.
    int  angleSnapDeg = 15;
};

// Reads/writes AppSettings as a simple `key = value` text file. The reader is
// intentionally tolerant: unknown keys are ignored and missing/garbled keys
// fall back to defaults, so a settings file written by a newer or older build
// never prevents the app from starting. The writer always emits the full set
// of currently-known keys, so new settings are added to the file automatically.
namespace SettingsIO {
    // Default location: $XDG_CONFIG_HOME/materializr/settings.cfg
    // (or ~/.config/materializr/settings.cfg).
    std::string defaultPath();

    AppSettings load(const std::string& path);
    bool        save(const std::string& path, const AppSettings& s);

    // Portable import/export as JSON, for backing up preferences or moving them
    // between machines (File → Import/Export Settings). Same tolerance contract
    // as load(): unknown keys are ignored and missing keys keep their defaults.
    // `lastProjectPath` is deliberately omitted — it is machine-specific session
    // state, not a portable preference. On a read error or unparseable file,
    // importJson returns defaults and sets *ok (if provided) to false.
    bool        exportJson(const std::string& path, const AppSettings& s);
    AppSettings importJson(const std::string& path, bool* ok = nullptr);
}

} // namespace materializr

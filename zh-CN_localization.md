# Simplified Chinese Localization (简体中文汉化)

Contribution summary for the Materializr maintainers.

## What this change does

Adds a complete Simplified Chinese (简体中文) UI translation covering all
visible strings across every layout (Classic / Modern / im-touch), dialogs,
toasts, status bar, tooltips, viewport labels and the help guide.

The i18n mechanism is untouched: **the English string is the key**
(see `src/i18n.h`). This change only (a) fills the Chinese column of the
translation catalogue and (b) wraps previously-unwrapped call sites with
`tr()`.

## Scope / size

- Catalogue entries: `1052 → 1346` keys × 6 languages (+294 new zh entries).
- Files touched: 1 data source, 1 regenerated header, ~20 source files.
- No business logic, variables, function names, widget IDs (`##id`) or
  license files were modified.

## Translation mechanism notes

- Single source of truth: `tools/i18n_catalogue.py` — **do not hand-edit
  `src/i18n_catalogue.h`**; regenerate it with:
  ```
  python tools/i18n_catalogue.py
  ```
- `cstr()` escapes all non-ASCII bytes as `\xNN`, so the header stays pure
  ASCII and compiles identically on MSVC (even without `/utf-8`), Clang and
  GCC.
- Language switching is live: ImGui rebuilds strings every frame, glyphs are
  rasterised on demand.

## Files changed

### Data
- `tools/i18n_catalogue.py` — ~294 new zh translations in the `CAT` array.
- `src/i18n_catalogue.h` — regenerated output (submit it, or let maintainers
  regenerate; either works).

### Source (tr() wrapping / new call sites)
- `src/app/Application.cpp` — unsaved-changes modal, toasts (sketch-in-progress,
  thread re-cut, merge-faces failure), project display name (`New project`),
  `#include "../i18n.h"` relative-path fix.
- `src/app/Application_Dialogs.cpp` — pattern popup titles, section/world-plane
  names, primitive dialog labels (`Width (X)`…, invalid-reason texts), Revolve
  popup title & axis labels, mirror hint, shortcut legend rows, layout names,
  thread profiles, export format/page combos (rewritten to BeginCombo/Selectable
  because the old `\0`-separated Combo strings cannot be dictionary keys).
- `src/app/Application_Viewport.cpp` — angle-snap label, dimension hint texts,
  `mm dia` viewport label, `(controls geometry)` / `(measures only)`.
- `src/app/layout/LayoutCommon.cpp` — `Untitled` tab fallback, polygon choices.
- `src/app/layout/modern/ModernLayout.cpp` — rail section headers, focus-mode tips.
- `src/app/layout/imtouch/ImTouchLayout.cpp` — model-tree toggle tips.
- `src/modeling/ThreadOp.cpp` — thread profile names.
- `src/plugins/TutorialPlugin.cpp` — layout-card names/descriptions.
- `src/ui/Toolbar.cpp` — sketch-constraint status, Revolve/Lathe buttons,
  fully/under/over-constrained labels.
- `src/ui/HelpPanel.cpp` — `section()` now translates title+body internally.
- `src/ui/ItemsPanel.cpp`, `StatusBar.cpp`, `VersionPanel.cpp` — `Bodies: %d`.
- `src/ui/PropertiesPanel.cpp` — `ID: %d`, `%d bodies selected`.
- `src/ui/HistoryPanel.cpp` — `Step %d/%d`.
- `src/ui/MeasureTool.cpp` — `body` / `bodies` plural args.
- `src/ui/WelcomeScreen.cpp` — title, `Version ` prefix, supporter link.
- `src/ui/AboutDialog.cpp` — title, `Version ` prefix, tagline.
- `src/ui/LandingPage.cpp` — `New Project` tile + hint.
- `src/touch_mode.h` — `btnCreate()` now wrapped in `tr()`.

## Terminology (CAD convention)

| English        | 简体中文 |
|----------------|----------|
| Extrude        | 拉伸     |
| Loft           | 放样     |
| Shell          | 抽壳     |
| Draft          | 拔模     |
| Fillet/Chamfer | 圆角/倒角 |
| Revolve        | 旋转     |
| Sweep          | 扫掠     |
| Boolean        | 布尔     |
| Unfold         | 展开     |
| Pattern        | 阵列     |

## Verification

- MSVC Release build passes (incremental + full source set).
- `tests/test_i18n.cpp` validates catalogue format, printf placeholders,
  UTF-8 legality and per-language key parity.
- `tr()` hit-tests (g++ / Clang-family): key lookup, `##id` retention, format
  strings (`%d`, `%s`, `%.*s`, `%%`) all correct.

## Known notes for reviewers

- ViewCube face labels (`Top`/`Bottom`/`L`/`R`) and 2D drawing-view direction
  names are intentionally **not** translated (3D/2D CAD convention).
- `"TEXT"` in the text tool is a sample/business value the user overwrites, not
  a UI label; left as-is.
- A handful of orphan `"Visible##id"` catalogue keys exist (~10, e.g.
  `Orbit##touchSens`): `tr()` looks up by the visible part, so these full-key
  entries are redundant but harmless — a maintainer may clean them up.
- `%`-containing English strings (`100%`, `Scale U (%)`, `100%%`) are the
  upstream originals; zh mirrors them exactly and all call sites are
  `TextUnformatted`/label-safe.

## Suggested review flow

```
git diff --stat                     # overview
git diff tools/i18n_catalogue.py    # translations (the bulk)
python tools/i18n_catalogue.py      # regenerate the header
cmake --build build                 # rebuild
```

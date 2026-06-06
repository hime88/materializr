# Materializr

**Open-source parametric 3D CAD for makers** — constraint sketches, solid
modeling, threads, SVG & text engraving, STL/STEP export.

## Download

**[⬇ Get the latest release](https://github.com/materializr-cad/materializr/releases/latest)**

| Platform | File | How |
|---|---|---|
| Linux (x86_64 / aarch64) | `Materializr-*.AppImage` | `chmod +x` it and run — no install |
| Windows | `Materializr-Setup.exe` | run the installer |
| Windows (portable) | `Materializr-windows-x64.zip` | unzip anywhere, run `materializr.exe` |

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-blue)
![License](https://img.shields.io/badge/License-MIT-green)

Built on the [OpenCASCADE](https://dev.opencascade.org/) geometry kernel —
real B-rep solids, not meshes — with a Dear ImGui interface. Sketch on any
face or construction plane, pull it into a solid, keep editing any step of
the history later (even after closing the project).

## What it does

**Sketch** — lines, circles, arcs, splines, polygons, rectangles with
SketchUp-style inference snapping (endpoints, midpoints, perpendicular,
tangent, 15° increments) and opt-in dimensions & constraints. **Text** as
real outline geometry (three bundled fonts) and **SVG import** with live
placement preview — both become ordinary closed regions you can extrude.

**Model** — push/pull, extrude, revolve, sweep, loft, booleans,
fillet/chamfer, shell, mirror, linear & radial patterns, split, align.
Direct face editing: **taper** (draft), **scale face** (pinch a wing tip
into a winglet), edit a hole or boss to an exact diameter.

**Detail** — validated **screw threads** (internal & external, standard
coarse defaults from the diameter), and **Projection**: engrave or emboss
any sketch onto a flat *or curved* face — wrap a logo around a cylinder in
three clicks.

**Stay in control** — every operation is an editable history step;
projects reload with the history still editable. Construction planes &
axes, **Section View** with any cutting plane, version snapshots with
auto-save, undo everywhere.

**Exchange** — STEP and IGES import/export, STL and glTF export
(Z-up corrected for printing), SVG import, PNG viewport export, and a
compact native `.materializr` format that stores bodies, sketches, and
the full history.

## Documentation

- **[Getting Started](docs/getting-started.md)** — install + your first sketch in five minutes.
- **[Features](docs/features.md)** — full list of what every tool does.
- **[Usage Guide](docs/usage.md)** — workflow recipes and keyboard shortcuts.
- **[Building from Source](docs/building.md)** — native Linux, Docker AppImage, Windows (MSVC + vcpkg).
- **[Architecture](docs/architecture.md)** — code layout, design patterns, tech stack.
- **[Changelog](docs/changelog.md)** — release notes and known issues.

The app ships an in-app **Help → User Guide**, a **Keyboard Shortcuts**
panel, and **Help → Check for Updates**.

## License

MIT — see [LICENSE](LICENSE).

## Contributing

Contributions welcome — bug reports and missing-workflow notes especially,
as the road to 1.0 is field testing. Open an issue first for substantial
changes; small fixes can go straight to a PR.

## Credits

- **R4stl1n** — original project.
- **stevebushwa** — design, testing, direction.
- **Claude (Anthropic)** — pair-coding collaborator.

## Acknowledgments

- [OpenCASCADE Technology](https://dev.opencascade.org/) — geometry kernel
- [Dear ImGui](https://github.com/ocornut/imgui) — immediate-mode GUI
- [GLFW](https://www.glfw.org/) — windowing and input
- [GLM](https://github.com/g-truc/glm) — math library
- [nanosvg](https://github.com/memononen/nanosvg) — SVG parsing
- [libcurl](https://curl.se/libcurl/) — update checking

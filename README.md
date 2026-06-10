# Materializr

**Open-source parametric 3D CAD for makers** — constraint sketches, solid
modeling, threads, SVG & text engraving, STL/STEP export.

![Materializr modeling a coffee mug with filleted rim, handle, and embossed logo wrapped around the cylindrical face](docs/hero-mug.png)

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

## What it is (and isn't)

Materializr **isn't trying to replace** SolidWorks, Fusion 360, FreeCAD, or any
other CAD program. It aims for the **middle ground between dead-simple and
fully-featured** — enough genuine parametric solid modeling to make real parts,
without a steep learning curve, a subscription, or an account. If you've ever
found beginner tools too limiting and pro tools too heavy, that gap is what
this is for.

It's also **young software built quickly, so expect rough edges — there are
bugs we haven't found yet.** The good news: operations validate their results
and *refuse* rather than silently produce garbage, so a failed action leaves
your model untouched instead of corrupting it. Still: **save often**, and if
something behaves oddly, a bug report is the most useful thing you can send.

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

Materializr is built on a stack of excellent open-source projects — none of
this would exist without them.

**Geometry & math**

- [OpenCASCADE Technology](https://dev.opencascade.org/) — B-rep solid
  modelling kernel (LGPL with Open CASCADE exception).
- [GLM](https://github.com/g-truc/glm) — OpenGL-friendly C++ math (MIT).

**Graphics & windowing**

- [Dear ImGui](https://github.com/ocornut/imgui) — immediate-mode GUI,
  used for every panel and overlay (MIT).
- [GLFW](https://www.glfw.org/) — window, input, and OpenGL context
  creation (zlib).
- [GLEW](https://glew.sourceforge.net/) — OpenGL extension loading on
  Windows (modified BSD / MIT).

**File I/O & exchange**

- [nanosvg](https://github.com/memononen/nanosvg) — SVG parser for the
  sketch SVG-import tool (zlib).
- [libcurl](https://curl.se/libcurl/) — HTTPS GET for Help → Check for
  Updates (curl license).
- [zlib](https://zlib.net/) — gzip stream for the v3 `.materializr`
  project format (zlib license).
- [portable-file-dialogs](https://github.com/samhocevar/portable-file-dialogs)
  — single-header bridge to the host's native Open / Save dialog
  (WTFPL). Lets you save to SMB / NFS / cloud mounts the OS file manager
  already knows about.

**Bundled fonts**

- [JetBrains Mono](https://www.jetbrains.com/lp/mono/) — UI font
  (SIL Open Font License 1.1).
- [DejaVu Sans](https://dejavu-fonts.github.io/) and DejaVu Serif —
  shipped as choices for the sketch Text tool (DejaVu Fonts License,
  derived from Bitstream Vera).

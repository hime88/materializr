# Changelog

All notable changes to Materializr are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow SemVer.

## [1.3.0] — 2026-07-02

The stable rollup of the whole `1.3.0-beta.1 … beta.11` line, plus one fix that
landed after the last beta. Per-beta detail is in the entries below; the
headline additions since **1.2.8**:

### Highlights

- **Unfold / flatten to 2D cut patterns.** Lay a 3D body flat into a developable
  net and export it as **SVG** or **tiled PDF** — with a live page-break preview,
  magenta **registration crosses** in the overlaps for precise multi-sheet
  assembly, rotate / auto-fit, and a conformal option for double-curved shells.
  Straight from solid to laser cutter, vinyl, or paper template.
- **STL import.** Bring in a mesh (accuracy slider, wireframe toggle) and sketch
  on its flat faces — reference or rework printed/scanned parts.
- **Twist Face** and direct face editing — the Rotate gizmo's third ring spins a
  face about its normal to spiral the walls; alongside taper and scale-face.
- **Interactive Mirror** — place and rotate a mirror line, snap, and mirror
  points / lines / circles / arcs / splines with coincident welding.
- **Navigation that tracks 1:1.** Pan, push/pull, extrude and scale-face drags
  move exactly with the cursor at any zoom; **orbit spins around the object**
  (the pivot re-anchors onto the geometry at the view centre).
- **Crisp UI on scaled Windows displays.** The app is now per-monitor DPI-aware,
  so at 125–200% scaling it renders at native resolution (sharp) instead of a
  blurry bitmap-upscale. *(New in stable — landed after beta.11.)*
- **Smarter sketch snapping** — snaps onto the host body's adjacent-face edges
  and continuously along in-plane hole rims / fillet arcs; grid-relative snap
  bands so fine grids draw short segments; post-draw resize of lines, rects and
  arcs by exact dimension.
- **Opt-in beta channel** — Settings toggle to receive pre-release builds early.
- A broad **stability & performance pass**: multi-instance crash-recovery fix,
  GPU-cached sketch/selection rendering, and a backend audit — smoother on
  complex models and tablets.

APK versionCode 28.

## [1.3.0-beta.11] — 2026-07-02 (pre-release)

Feature-complete cut for 1.3.0. APK versionCode 27.

### Added

- **Twist Face.** The direct face-editing gizmo's Rotate mode gains its third
  ring — the blue one, lying *in* the face plane (about the face normal). Grab it
  to **spin a face relative to the opposite cap**, spiralling the walls between
  them; the panel also takes an exact twist angle. Built as a layered ruled loft,
  so any total angle works (a single loft would cap at ~45°), and an over-twist
  that self-intersects is refused cleanly rather than producing garbage. Tilt and
  twist are separate operations — one gesture does either, not both.

### Changed

- **Navigation tracks 1:1 at any zoom.** Pan, and the push/pull, extrude, and
  scale-face drag handles, now move the world/face exactly one pixel's worth per
  pixel dragged, instead of a fixed scale that felt sluggish zoomed in and jumpy
  zoomed out. The grabbed point stays under the cursor.
- **Orbit spins around the object.** The pivot re-anchors onto the geometry at
  the view centre at the start of each orbit (with a ring-sample fallback that
  lands between two parts when you're looking down the gap between them), instead
  of a point that had drifted behind the model — which used to read as a blend of
  pan and rotate. The image doesn't jump; only the pivot moves.
- **Sketch snapping scales with the grid.** At a fine grid (e.g. 0.1 mm) the snap
  bands are now grid-relative, so you can draw short segments and place points one
  increment apart. Point/endpoint snapping — including loop closure — is gated
  behind the inference toggle: with inferences off, nothing captures the cursor.
  The live length readout shows hundredths (trailing zeros trimmed).
- **Isolate & visibility.** Right-click **Isolate** now actually hides the other
  bodies from the viewport menu, a new **Show All Bodies** brings them back, and
  the redundant "Hide Others" entry was removed.

### Fixed

- **Crash-recovery race between two running instances** (a `SIGBUS` / wrong-session
  restore) — each instance now claims its own recovery slot via an OS file lock,
  so they never fight over one autosave file.
- **"Unhide sketch → not responding"** on heavy projects — unhiding a complex
  sketch no longer forces a full re-tessellation or an OCCT region rebuild on the
  hover path.
- **Push/pull on an unlinked sketch** no longer fuses the new material into the
  body the sketch was *originally* drawn on; a detached sketch behaves as
  free-floating.
- **Couldn't draw short lines/arcs** — a segment no longer welds its second point
  back onto its own start, so sub-0.3 mm geometry commits.
- **Selection outline / gizmo lagging** at the pre-move position after a
  multi-body move.
- **Non-finite numeric input** (e.g. `1e999` → infinity) is rejected at every
  entry box instead of flowing into the geometry kernel; pattern instance counts
  are clamped so a huge typed number can't hang the app.

### Performance

- Big pass for complex projects and tablets: **gizmo Move/Rotate/Scale drags are
  GPU-only** (no per-frame remesh), **static sketches and selection highlights
  render from cached GPU buffers**, and several per-frame document walks (the
  sketch↔body link map, the toolbar's face scans, history/items panel work) are
  now memoized. Noticeably smoother orbit and drag with many bodies on screen.
- Leftover per-interaction debug output is now gated behind `--verbose`, and a
  round of dead code was removed.

## [1.3.0-beta.10] — 2026-06-30 (pre-release)

### Fixed

- **Oversized window on small / scaled Windows displays.** On a 1080p (or
  smaller) Windows screen at the OS-default 125–150% display scaling, the fixed
  1600×900 window opened larger than the virtualised desktop and spilled past
  every edge — hiding the taskbar, the title-bar close button, and the sides of
  both dock panels. The window now clamps its initial size to the display's
  usable work area (taskbar excluded) and starts **maximized** when the screen is
  too small; the clamped size becomes the restore size, so un-maximizing or a
  minimize→restore no longer overruns the display. 2K+ Windows screens, Linux,
  macOS, and Android were unaffected.

### Changed

- **Android: OCCT geometry kernel 7.8.1 → 7.9.3**, matching the desktop/Linux
  build so behaviour stays consistent across platforms. APK versionCode 26.

## [1.3.0-beta.9] — 2026-06-30 (pre-release)

### Added

- **Flat-pattern viewer: rotate + print preview.** The Unfold dialog can now
  **rotate** the whole pattern — a slider, **−/+ 1°** fine steps, a **+90°**
  button, and **Auto-fit**, which snaps to the orientation that needs the fewest
  pages. A single **Export** button with an **SVG / PDF** dropdown drives the
  preview: SVG shows a clean 1:1 layout, PDF overlays a live **page-break grid**
  with a page count so you can see exactly how it tiles before exporting.
- **PDF alignment marks.** Tiled PDFs get magenta **registration crosses** in the
  page overlaps — the same point prints on both adjacent sheets, so you overlay
  and slide until the crosses coincide for precise assembly. A **Marks** dropdown
  (None / Sparse / Normal / Dense) sets their density.

### Fixed

- **Conformal unwrap of a cone** came out a wrapped full disk (~12000% stretch);
  it now slits the apex and unwraps to a clean **sector** like the developable
  net. Conformal on a closed solid (which can't flatten to one piece) now shows a
  clear warning steering you to untick Conformal for the developable net.

## [1.3.0-beta.8] — 2026-06-30 (pre-release)

### Added

- **Unfold / flatten surfaces into 2D cut patterns.** Select a body — or just
  the faces of one panel — and **Unfold / Flatten** lays the surfaces out flat
  for laser cutters, CNC, or printed templates. Flat and box-like parts unfold
  into a connected net; **developable** curved skins (cylinders, cones, extruded
  profiles, a square→round loft) unroll into one connected piece, hinging whole
  panels along their shared edges and picking the layout with the smallest flat
  area. **Doubly-curved** surfaces (spheres, funnels) flatten via an opt-in
  **Conformal unwrap** (Least-Squares Conformal Map, Blender-style) that
  auto-cuts a seam to open closed shapes. A **material** setting (Pliable /
  Semi-rigid / Rigid) and **thickness** drive the bend/score lines and mitre
  offsets (e.g. ½″ ply at a 90° corner → a 6.5 mm mitre line). Export the flat
  pattern as **SVG** (1:1 mm, for laser/CNC) or a **tiled, full-size PDF** (US
  Letter or A4) with crop marks, tile overlap, and a 50 mm scale bar to verify
  the print came out at true scale.

## [1.3.0-beta.7] — 2026-06-29 (pre-release)

### Added

- **Import STL meshes.** File ▸ Import ▸ STL brings in an STL (ASCII or binary)
  as a mesh body. An import dialog offers an **Accuracy** slider — lower
  simplifies the mesh (faster, with larger merged flat faces), higher keeps more
  detail — and a **wireframe** toggle. Genuinely near-coplanar facets are merged
  into single planar faces, so you can **pick a flat region and use "Sketch on
  Face"** to retrace it by hand — handy for recreating a model from a scan or a
  print when it can't be done automatically. Sketching on a mesh face best-fits
  the plane to the region you picked, so it stays accurate even on a simplified
  import. Imported bodies are tagged as meshes (saved with the project) and use a
  fast cached picker so selection stays smooth; the facet wireframe can be turned
  off here or in Settings ▸ Rendering.

### Fixed

- The sketch grid step now persists across launches (it was written to the
  settings file but never read back).
- Settings export/import (JSON) now round-trips the panel-visibility toggles and
  touch sensitivities, matching the on-disk settings.

## [1.3.0-beta.6] — 2026-06-29 (pre-release)

### Added

- **Snap to the host body's edges while sketching.** Starting a sketch on a
  solid's face already let the cursor snap to that face's own corners and edges;
  now it also snaps to the edges of every neighbouring face that touches it —
  the side walls and bordering edges around the face you're drawing on — so you
  can line geometry up with the existing body, not just the single face.
- **Continuous snapping along round host edges.** Hole rims and fillet arcs that
  lie in the sketch plane are now snapped along their whole perimeter (respecting
  the arc's span), instead of catching only at a few sampled points.

## [1.3.0-beta.5] — 2026-06-29 (pre-release)

### Added

- **Interactive Mirror.** The sketch Mirror tool now places a movable, rotatable
  mirror line (with a live ghost preview) instead of mirroring instantly across a
  fixed vertical axis. The line carries the same move/rotate gizmo as sketch
  elements — drag the arrows/centre to position it, the ring to rotate (snaps to
  5° increments) — plus Vertical/Horizontal presets and ±45° nudges in its panel.
  It now mirrors points, lines, circles, arcs and splines (was points + lines),
  welding coincident vertices. The line snaps to half the grid step, so the
  reflected geometry lands on whole grid increments.

### Changed

- **Copy lands offset and selected.** A sketch Copy now drops the duplicate two
  grid steps away (not exactly on the original) with the move gizmo already over
  it, so you can immediately drag it into place instead of fishing two stacked
  copies apart.
- **Box-select catches curves.** Click-and-drag selection now includes circles,
  arcs and splines, not just points and lines — and selected splines are
  highlighted like everything else.
- **Splines render in the standard cobalt** instead of green, matching every
  other sketch element.
- **Bigger move/rotate gizmo on touch.** The sketch gizmo's handles and catch
  radius scale up in touch mode so they're comfortably finger-sized.

### Fixed

- **ViewCube frames the in-progress sketch.** Clicking the ViewCube during the
  very first sketch (no committed bodies yet) snapped to a tiny default cube;
  it now encompasses the sketch you've drawn so far.
- **Two-finger pan/zoom mid-sketch leaves no debris.** Starting a two-finger
  gesture while a drawing tool is active no longer drops a stray start vertex or
  half-placed shape from the first finger — the placement is rolled back so
  navigation is clean without the Move button.
- **History item highlight is dismissable.** Clicking a sketch history step
  highlights its element; clicking the same step again now clears it.

## [1.3.0-beta.4] — 2026-06-28 (pre-release)

### Changed

- **Touch — calmer pan, faster zoom by default.** The baseline (1.0×)
  two-finger pan was twitchy and the pinch-zoom was painfully slow. The base
  sensitivities are retuned (pan ×0.5, zoom ×2.5) so the out-of-the-box feel is
  right; the Settings sliders still scale from there.

### Fixed

- **Sketch Select/Move no longer shows inference guides.** Snap markers and
  labels only appear while you're actually drawing now — hovering with the
  Select/Move tool (or with touch "Move" mode on) no longer clutters the view
  with guides it can't act on.
- **Selecting a sketch element no longer leaves it highlighted after you switch
  tools.** Picking a line/point with Select/Move and then choosing a drawing
  tool now clears the golden highlight instead of leaving it stuck on.
- **Touch — scrolling a menu no longer selects the options it passes over.** A
  drag-to-scroll over the Tools list released the press *on* the button it
  started on, registering as a tap. The cursor is now parked off-screen before
  the release so a scroll flick just scrolls.
- **Touch — panels scroll with a natural finger drag.** Dragging inside the
  Settings window (and other panels) moved the whole window instead of
  scrolling. Windows are now movable from the title bar only, so body drags fall
  through to drag-to-scroll.

## [1.3.0-beta.3] — 2026-06-28 (pre-release)

### Fixed

- **Splash screen no longer hangs until you touch the window.** The post-launch
  render grace only overrode the focus check, not the idle-skip — so at a fresh,
  idle startup the first real UI frame stayed undrawn behind the loading screen
  until a tap or mouse-move woke the loop. The grace now overrides the idle-skip
  too, so the splash→UI handoff always completes on its own.

## [1.3.0-beta.2] — 2026-06-28 (pre-release)

### Fixed

- **ViewCube — edges and corners stay selectable until a face is edge-on.** The
  new edge-click feature culled edge spots and corner dots once a face tilted
  past ~45°, even though the face itself stayed visible to 90°. Visibility is now
  derived from face adjacency (a vertex/edge is live while a face bordering it
  faces the camera), so they track the real silhouette.

## [1.3.0-beta.1] — 2026-06-28 (pre-release)

First build of the **beta channel**. Headline feature: sketch elements are now
resizable after they're drawn.

### Added

- **Resize sketch geometry from the Properties panel.** Select an element while
  editing a sketch and change its size directly:
  - **Lines** — set the **Length**; the line grows symmetrically about its
    midpoint, and anything sharing its endpoints (chained lines, rectangle
    corners, arc ends) follows along.
  - **Rectangles** — select any side to edit **Width × Height**; the centre
    stays put. Detected automatically from one selected side.
  - **Arcs** — **Radius** and **Chord** both scale the arc keeping its shape
    (chord = the straight distance between the endpoints); **Sweep** changes the
    included angle. Moving a point shared with an arc bends that arc to preserve
    its swept angle.
  - These edits are proper undoable history steps, and selecting an element
    highlights its history step (and vice-versa).
- **ViewCube edge clicks for two-face views.** The cube could snap to a single
  face or a three-face corner; now hovering near a visible edge highlights that
  seam, and clicking looks straight down it so both adjacent faces are seen at
  once (12 edge spots — 4 vertical, 4 top, 4 bottom). Hover-revealed, no
  persistent marker, so the cube stays uncluttered.
- **Beta update channel.** A new *Settings → Include pre-release (beta) builds*
  opt-in. With it on, the in-app update check also offers pre-releases (like
  this one); left off, you stay on stable releases only. The two channels are
  kept apart automatically — stable users are never shown betas.

### Fixed

- **ViewCube clicks no longer go dead.** A regression gated cube clicks on
  `WantCaptureMouse`, which is true across the whole docked viewport — so faces,
  corners, rotate arrows, roll arcs, Home, and drag-to-orbit all stopped
  responding. Clicks are now gated on window hover instead, restoring every
  cube interaction.

## [1.2.8] — 2026-06-27

macOS arrives, plus security hardening, correctness fixes, and sketch/history polish.

### Added

- **macOS (Apple Silicon) desktop build.** A native arm64 build, shipped as a
  `.dmg` — the fourth platform alongside Linux, Windows, and Android. Retina /
  HiDPI viewport, the system OpenGL backend, and a self-contained bundle.

### Fixed

- **Sketch — lines latch anywhere on an arc**, not just at its endpoints. Arcs
  now snap like circles do, with the latch confined to the arc's actual span.
- **Sketch — arcs (and splines) divide regions.** An interior arc that splits a
  face (e.g. the band between two arcs) is now its own selectable, push/pullable
  region instead of being swallowed by the surrounding area.
- **History — disabling a step no longer deletes the whole body.** Toggling a
  step rebuilds the model in place, so disabling a lone push/pull on a base body
  reverts just that move and keeps the body; re-enabling restores it cleanly
  instead of erroring. Disabled steps are also respected by undo/redo (no model
  desync), and the threaded thread-cut worker no longer races the render thread.

### Security

- **Hardened the untrusted-file parsers and the update flow** following a code
  audit: decompression-bomb caps on the project/recovery and SVG loaders,
  bounded length-prefixes and count loops, try/catch around STEP / IGES /
  project import (including OCCT kernel faults), a shell-free URL opener with
  https-pinned update URLs, and a size-/redirect-capped update checker. No
  behavior change for valid files.

### Changed

- **Docs now match reality:** de-advertised features that weren't actually
  reachable (Sweep, the 2D Drawing workspace, Align, DXF / image export) and
  relabeled SVG export as the real per-sketch path.

## [1.2.7] — 2026-06-26

Bug fixes from the first round of community reports.

### Fixed

- **Shell no longer crashes** when a hollow exceeds the body's available wall
  space (e.g. shelling one face, then another, and dragging the thickness past
  what fits). The operation now fails cleanly instead of taking the app down.
  Under the hood this re-armed OCCT's kernel-fault guard in release builds, so
  other modeling tools are more crash-resistant on degenerate geometry too.
- **Measure → Object** reports a body's size on the correct axes — height now
  reads under **Z**, not Y.
- **Touch:** a slow, deliberate two-finger pan no longer flips into a zoom.
  Pan/zoom intent is judged from net finger travel with a bias toward pan, so
  the small non-parallel wobble of two fingers can't be mistaken for a pinch.
- **Settings:** fixed an internal control-ID clash on the touch Orbit/Pan
  sensitivity sliders (a debug-build warning; harmless but real).

## [1.2.6] — 2026-06-25

Crash safety and grid polish.

### Added

- **Crash/hang recovery autosave.** The whole project — including an **unsaved**
  one with no file yet — is snapshotted to a recovery sidecar as you work, so a
  crash, hang, or kill no longer loses your model. On the next launch it offers
  to restore the recovered work. Independent of the (off-by-default) autosave,
  which only writes an already-saved file.

### Changed

- **Grid:** removed the "Grid shade" slider (it only tinted the sketch-plane
  grid and looked inert in the normal view), and gave the every-10th decade
  lines more contrast against the rest in both the ground and sketch grids.

## [1.2.5] — 2026-06-25

Modeling and viewport polish.

### Added

- **Grid line thickness.** A "Grid thickness" slider (0.1×–2×) in Settings →
  Appearance scales the grid line width, alongside the existing opacity and
  shade controls. Applies to both the ground and sketch grids.

### Fixed

- **Move Face on a plain face.** Move Face wrongly refused an ordinary face
  (e.g. a cube side) with an "only through-holes can be moved" message — the
  hole detector flagged a face with no bore as a "pocket". Plain faces now move
  normally; real hole-moving is unchanged.
- **ViewCube no longer steals panel clicks.** Clicking a panel that overlaps the
  ViewCube's corner (e.g. the Move Face *Cancel* button) registered as a
  cube-face click and snapped the camera to that face. The cube now ignores
  clicks an on-screen widget consumed.
- **Orbiting out of a Top/Bottom view.** The first orbit after leaving a
  straight-down/up view snapped the camera to a fixed right-side view; it now
  tips off the pole following the view's current orientation.
- **Splash flash.** Removed an occasional black flash on the startup splash by
  priming both swap-chain buffers on the first frame.

### Changed

- **Move Face arrows** are coloured by world axis (X red, Y green, Z blue) to
  match the main move gizmo, instead of a single yellow.
- **Thicker gizmo handles** — the move arrows and rotate rings are easier to
  grab.

## [1.2.4] — 2026-06-24

UI clarity and robustness.

### Added

- **Box / Cylinder / Sphere / Cone / Torus primitives**, authored in the Z-up UI
  convention so they stand up correctly.
- **Multi-target Subtract** — checkbox modal to subtract many tools from many
  bodies at once, with an option to keep the tool bodies.

### Fixed

- **World grid** no longer drops out at grazing/level camera angles.
- **Boolean robustness** — a tiny-fuzzy retry plus a `BRepCheck_Analyzer`
  validity gate so a Subtract never commits self-intersecting geometry.
- **Desktop startup** draws the UI without needing a click when the window
  manager is slow to focus a terminal-launched window.

### Changed

- **Radial Pattern → Circular Pattern**, and **Lathe vs Revolve**: the toolbar
  button reads "Lathe" for a selected sketch (spin a profile into a solid) and
  "Revolve" for a body (rotate around an axis — a fan or hinge).
- The **Shell** thickness slider snaps to 0.1.

## [1.2.3] — 2026-06-21

Android startup fixes (regressions/oversights from 1.2.2).

### Fixed

- **Android: the loading screen no longer hangs on "Almost there" until you tap.**
  1.2.2's render-on-demand suspended drawing while the window reported no input
  focus, but Android doesn't report focus until the first touch — so the splash
  never handed off to the UI. The background render-suspension now only applies
  on desktop (Android already suspends backgrounded apps at the OS level).
- **Android: the splash showed the wrong version (1.2.0).** The Android native
  build hardcoded the version string separately from the app version and had
  drifted. It's now passed through from the build's `versionName`, so it can't
  drift again.

## [1.2.2] — 2026-06-21

Resource-usage and autosave fixes. Backward compatible — existing projects open
unchanged.

### Fixed

- **Autosave actually runs now.** It was effectively never firing unless you
  interacted continuously, so a project you edited and then left alone was never
  saved and closing always prompted "unsaved changes." The check now runs while
  idle, on a wall-clock timer.

### Changed

- **Render on demand — much lower CPU/GPU/battery use.** The app no longer
  redraws at 60fps when there's nothing to show: it stops rendering when idle,
  when a sketch or a live preview (push/pull, fillet, move, …) is left open and
  untouched (a 1s grace covers interaction), and — most importantly — **whenever
  the window is in the background.** A backgrounded app previously kept the GPU
  busy enough to make the whole desktop's cursor stutter on shared-GPU systems;
  that's gone. Interaction and orbiting are unchanged (still smooth at 60fps).

### Added

- **AppStream metadata** in the AppImage, so AppImage managers (Gear Lever,
  AppImagePool) and software centres show the description, screenshot, links and
  release notes instead of just the name.

## [1.2.1] — 2026-06-21

Parametric-edit reliability fixes found while dogfooding multi-feature parts.
Backward compatible — existing projects open unchanged.

### Fixed

- **Editing a fillet/chamfer picks the right one.** Clicking a rounded face to
  edit it no longer grabs a neighbouring fillet of a different radius, and the
  "Edit Fillet/Chamfer" option no longer disappears after you cancel an edit.
  Face→feature mapping is now matched by blend geometry instead of fragile
  index bookkeeping, and is refreshed after every edit (commit *and* cancel).
- **Editing an early feature no longer deletes geometry.** A body built from a
  duplicated sketch (e.g. a lid) could, on a reloaded project, be cut into a
  body it happened to overlap — deleting part of that body (a box losing its
  floor) and stranding the rest of the history. A reloaded additive extrude now
  faithfully recreates its own body instead.

## [1.2.0] — 2026-06-20

Features that were long overlooked, plus a round of parametric-reliability and
viewport fixes. Backward compatible — existing projects open unchanged.

### Added

- **Move Hole** — slide a through-hole laterally across the face it pierces
  (round, square, polygon; constant, countersunk, counterbored and stepped
  holes). Reuses the Move tool + gizmo; under the hood a boolean re-cut, so
  cavities are never filled. Survives save/reload as an editable step.
- **Light theme for the viewport** — the background gradient, grid, and ViewCube
  now follow the UI theme (dark-on-light grid + ink) instead of staying dark.
- **Repair Geometry** (Face Operations) — remove a face and heal the surrounding
  faces back together: take a baked fillet/chamfer back to a sharp edge so it can
  be re-applied, or clean an unwanted round/hole off an imported part.
- **Sketch grid opacity + shade** controls (Settings → Sketch) — dim the grid and
  set its greyscale so it reads on a light/white body or a dark scene.
- **Duplicate Sketch** + edit a sketch element's size (circle Ø / arc R) straight
  from the Properties panel — for same-layout/different-holes variants.
- **Editable circle diameter from history**, and **symmetry + pre-placement
  alignment** sketch inferences.

### Changed

- **History** names each sketch step by its geometry and size ("Rectangle
  80 × 45 mm") instead of a generic "Add sketch element"; **frozen (baked) steps
  are marked amber** so they're easy to spot.
- **Section view** offset range now adapts to the model's size (was a fixed
  ±100 mm that couldn't traverse bigger parts).
- The load-time "older save" warning is honest (no impossible "re-apply" advice —
  it points at Repair Geometry) and dwells longer.

### Fixed

- **Grid no longer bleeds through bodies / punches through the face you sketch
  on** (it's drawn after geometry with depth-writes off), and it now fades
  cleanly under the opacity slider instead of leaving a grey ghost.
- **Sketch on a flat-but-non-planar-surface face** (e.g. the slanted sides of a
  scaled-down box) now works, and the grid aligns to the face edge instead of
  sitting ~45° off.
- **Parametric edits survive reload**: editing an op upstream of a boolean/delete
  now propagates; sketch ids are preserved (sketch edits no longer bake);
  circle-diameter edits reach the body; a failed sketch edit reverts cleanly
  instead of half-building. **Move-hole now reloads editable** (was always baked).
- **Push/pull no longer inverts** on a holed / annular face.
- **Startup "(not responding)" freeze** — the launch update-check runs off the
  main thread.

## [1.1.0] — 2026-06-18

Feature + fix release, with a large pass on the Android touch experience.

### Added

- **Sketch → SVG export** (1:1 mm, polyline) for laser cutters and 2.5D CNC —
  from the Items-panel sketch menu and the new viewport sketch menu.
- **Viewport sketch context menu** — right-click (or long-press) a sketch:
  Edit / Export as SVG / Move / Rotate / Delete.
- **Settings → Panels** — per-panel show/hide (Tools, Interactions, History,
  Items, Properties), persisted.
- **Touch sensitivity sliders** (orbit / pan / zoom) under Settings → Navigation.
- **AppImage delta auto-update** — the Linux AppImage now embeds gh-releases
  update info and ships a `.zsync`, so AppImageUpdate / Gear Lever can update it.
- In-app **"Join our Discord"** link in the About dialog.

### Changed

- **Boolean Union/Intersect** combine ALL selected bodies; **Subtract** asks
  which body to keep.
- **Sketch snapping** tiered by inference level: grid-vs-curve nearest-wins
  (Reduced), land on grid∩curve (Full/Max), grid+endpoint only (Off).
- Settings reorganized (Navigation tab; per-panel visibility; tidier grouping).
- Listing/README positioning: "the middle ground between Tinkercad and FreeCAD".

### Fixed

- **Push/pull direction** on faces whose orientation was inverted (the truly
  outward normal is now verified via a solid-classifier test).
- **Gizmo move** no longer drifts the un-moved axes when snapping.
- STL/STEP export enforce their file extension when the picker returns a bare name.
- **Touch:** panels resize and re-dock by drag; the Settings scrollbar drags;
  double-tap selects the body (even over a sketch); two-finger pan/zoom lock to
  one gesture at a time; box-select via Alt+drag in trackpad mode.

## [1.0.1] — 2026-06-14

Bugfix release. Android-focused: STEP import could crash the app instantly on
some files. Desktop behaviour is unchanged apart from a hardened importer.

### Fixed

- **STEP import crash on Android.** OpenCASCADE's shape-healing resource loader
  closes a file descriptor in a way Android's fd-sanitizer treats as fatal,
  aborting the process the moment a STEP file was opened. The sanitizer is now
  downgraded to warn-only at startup, so imports complete normally. (Desktop has
  no fd-sanitizer and was never affected.)
- STEP import now catches OpenCASCADE exceptions and reports a graceful error
  instead of letting a malformed or unusually complex file take down the app.

## [1.0.0] — 2026-06-12

First public 1.0 release: cross-platform (Linux desktop + Android), with a
built-in tutorial and a round of tablet dogfooding fixes. Android ships as a
release-signed arm64 APK; Linux as an AppImage.

### Added

- **Getting Started tutorial.** A skippable, step-by-step onboarding overlay
  (Help → Getting Started) that explains the basics and shows both mouse and
  touch controls, highlighting whichever the current device uses. Built as its
  own plugin on two new generic plugin hooks (per-frame overlay rendering and
  menu-item contributions).
- **Touch ergonomics.** Drag-to-scroll panels, per-side collapse tabs to fold
  the Tools / Items columns away on small screens (desktop: View → Hide Panels
  / F9), a larger finger-friendly navigation cube, and a "designed for tablets"
  notice on very small screens.
- **Storage Access Framework** on Android: system document picker for open /
  save, and a Share / Save-to-device sheet for STL / STEP / IGES / glTF export.
  No broad-storage permission requested.

### Fixed

- Edges could be selected *through* curved faces (cylinders, fillets, spheres):
  the occlusion test now uses the face normal at the actual click point.
- Sketch dimension input (line, and the rectangle's height step) clipped its
  text; the box is now a fixed size and stays on-screen.
- Push/Pull (and other face-op) click-drag now works on desktop with a
  left-orbit / trackpad binding, not just on touch.

## [0.9.9.1] — 2026-06-12

Touch sketching polish from tablet dogfooding. Everything here gates on touch
mode or the inference level, so desktop behaviour is unchanged — except the two
fixes noted as cross-platform.

### Fixed

- **Crash when tapping Undo repeatedly while a sketch was open.** Only the
  Ctrl+Z path guarded against undoing past sketch entry into the host body; the
  menu / History-panel / command paths did not, so on touch (where you tap a
  button) it could roll the body back under the live sketch and abort. A history
  undo-floor now blocks every undo path at the sketch-entry step and greys the
  buttons out there. *(All platforms.)*

### Added

- **Press-drag-release sketching on touch.** The line tool drops its first
  vertex on press, rubber-bands the segment as you drag (with a live length
  readout), and commits on release — then chains. The arc tool follows the same
  flow for its first two points (press-drag the chord), then a tap sets the
  bulge. Tap-to-place still works as a fallback.
- **Arc chord hint.** While drawing an arc's chord, a curved hint arcs over it
  so it reads as an arc-in-progress rather than a plain line. *(All platforms.)*
- **Chain controls** on the touch context bar: **Finish / Back / Cancel** for
  line and spline chains — Back drops the last segment/point, Cancel scraps the
  whole chain.
- **"Max" inference tier.** Everything Full does, plus wider snap/alignment
  catch ranges tuned for fingertips. Full and below are identical on every
  device, so mouse/keyboard users are never over-snapped. Defaults on for touch.
- **Sketch line width** setting (Settings ▸ Display, 1–6 px) so sketch geometry
  reads over the grid; vertex markers scale with it and now render on Android
  (the GL ES point-size path was missing).

### Changed

- **Touch tooltips time out** after 15 s of no interaction, so a tip left
  hanging by a finger-lift clears itself.

## [0.9.9] — 2026-06-12

### Added

- **Android support.** Materializr now builds and runs as a touch-capable
  Android app (arm64), with file open/save, immersive fullscreen, and the full
  modeling kernel. OpenCASCADE / SDL2 / FreeType are cross-compiled from pinned,
  SHA-256-verified upstream source.
- **Touch mode — a runtime setting on every platform** (Settings ▸ General),
  not a separate build. Defaults on for Android, off on desktop, and can be
  flipped either way — so a tablet with a mouse/keyboard runs the full desktop
  model, and a desktop touchscreen / convertible / Surface can switch to touch.
  When on: larger hit targets and HiDPI scaling, long-press for right-click
  context menus, press-drag-release sketch placement (circle/rectangle in one
  gesture), one-finger orbit / two-finger pan / pinch zoom, draggable push-pull
  and gizmo arrows, sub-shape multi-select (edges and faces), on-screen
  Multi-Select / Move toggles, and a soft-keyboard toggle for text fields.

### Changed

- **Windowing and input now run on SDL2 on every platform** (desktop included),
  replacing GLFW, so one backend serves Linux, Windows, and Android. No change
  to desktop mouse/keyboard behavior.

## [0.9.8.1] — 2026-06-11

### Fixed

- **Rippled curved surfaces in exported STL / glTF.** The mesh used a 0.5-radian
  (~28°) angular tolerance, so small fillets came out as a few flat steps — a
  90° fillet got ~3 facets. Export now tessellates at a print-quality 0.01mm
  chord / ~5.7° (STL) and a finer chord / ~11° (glTF), so blends come out
  smooth. Files are somewhat larger, which is the right trade for printing.

## [0.9.8] — 2026-06-11

### Added

- **Face transforms respect feature boundaries.** Move / Scale / Rotate Face
  used to loft to the opposite end of the body and rebuild the whole solid, so
  on a multi-feature part it erased everything in between (scale a funnel's
  spout and the funnel collapsed into a cone). They now find where the adjacent
  feature actually ends and loft only within it, booleaning the result back so
  every other feature survives — funnels, stepped boxes, and chained transforms.
- **SVG import, much more tolerant.** Line-art icons (stroke-only paths) are
  offset into closed ribbons; `<text>` renders to glyph outlines in your
  installed fonts; `<style>` class fills are inlined before parsing. More of
  what you grab off the web "just imports."
- **Wrap a logo around a cylinder.** Engrave/emboss onto a curved face now maps
  the sketch around the surface (no silhouette limit, correct orientation,
  hollow counters), with a click-to-toggle region picker plus Select all, Clear,
  and "Cycle loops/islands" to choose what stamps.
- **Progress bars for long operations.** Dense projections run off the main
  thread with a live, cancelable bar instead of freezing; thread cutting shows
  the same indicator.

### Fixed

- **Drag-and-drop crash on Wayland** — dragging a file over the window crashed
  the app (a GLFW Wayland bug); it now runs under X11/XWayland where that path
  is safe. A backtrace is printed on any fatal signal now.
- **Non-deterministic geometry** — GL rendering left the FPU in flush-to-zero
  mode, so imports and booleans came out subtly different run to run; the FPU is
  now restored every frame.
- **Shell on scaled/lofted faces** silently did nothing; it now retries with an
  intersection-join offset and reports failure instead of no-op-ing.
- **Sketching on a curved face** dropped onto a random tangent plane; it now
  refuses and points you at Add Plane….
- Dense-logo projection survives a stray bad region (one-shot → batched →
  per-tool) instead of failing wholesale; counter holes detect deterministically.

## [0.9.3] — 2026-06-07

### Added

- **Symmetric push/pull** — a checkbox in the push/pull panel (plane
  sketches) sweeps the region the same distance to BOTH sides of the
  sketch plane as a single body: no mid-plane seam edge, ever. The panel
  shows the per-side distance and a live total-width readout; the slider
  stays positive while symmetric. (Unioning two separately pulled halves
  keeps a seam edge on curved walls — an upstream OCCT limitation, since
  its face unifier only merges flat/elementary surfaces. Symmetric mode
  sidesteps it by construction.)

### Fixed

- **Selection highlight on prism caps** — the geometry kernel reuses one
  face object for both caps of a prism, and the highlight system drew the
  back cap's selection on the front cap (invisible from behind), so back
  faces seemed to ignore clicks. Every push/pull and extrude result now
  carries fully independent faces.
- **Thin and symmetric bodies no longer lose their caps to the sketch** —
  when a body's surface sits within tolerance of its sketch plane, clicks
  on the caps used to select the sketch region instead. Ties now go to
  the face; the region (or anything else beneath) is one same-spot click
  away via selection cycling.

## [0.9.2] — 2026-06-07

Fixes the dual-direction push/pull workflow end to end — pulling a sketch
region both ways no longer produces bodies that look unselectable, and the
interactive preview can no longer interfere with your committed work. Two
deep bugs wearing one symptom:

### Fixed

- **Bodies pulled from the same sketch are now fully independent.** OCCT's
  prism builder reuses the profile face as the prism's caps, so two pulls
  from one region produced bodies literally sharing faces (with each other
  and the sketch) — selections on them worked internally but the highlight
  and face menus didn't respond, making faces look unclickable. Every
  push/pull and extrude now builds from its own copy of the profile.
- **The push/pull preview no longer touches the history at all.** The old
  preview re-wrote the history stack every frame, which made body ids
  churn, opened click-windows where the document was momentarily empty,
  and — if anything else touched history mid-preview (the Edit menu, the
  History panel) — could permanently erase the last committed step. The
  preview now snapshots and restores directly, and commits exactly one
  step. Undo/redo and the History panel are disabled while a preview is
  live; viewport selection clicks are ignored during legacy previews.
- **Selection depth cycling**: when a sketch region and a body face
  overlap under the cursor, the nearest one is picked first and clicking
  the same spot again selects the other — nothing is ever unreachable.

### Internal

- Click/pick/renderer-slot/mesh-failure diagnostics now log to stderr
  (one line per click; detailed dumps only on misses) — field reports of
  "can't click X" are now self-explanatory from the log.

## [0.9.1] — 2026-06-06

Bugfix release — the first finds of the post-0.9.0 field-testing phase.

### Fixed

- **Push/pull works on interior and pocketed faces.** The outward
  direction came from a bounding-box heuristic that was exactly backwards
  for hollow bodies: cavity walls no-opped entirely, pocket floors
  pushed/pulled inverted. The face normal OCCT reports is already
  outward-corrected, so the heuristics (all three generations of them)
  are simply gone.
- **No more slightly-crooked "snapped" lines.** Directional sketch
  inferences (perpendicular / parallel / tangent / axis / angle) could
  place an endpoint a hair off-axis, producing lines 1° off horizontal
  that every later inference then aligned to. An inferred segment within
  ~4° of an axis now collapses exactly onto it (and back onto the grid);
  genuinely slanted inferences are untouched.

### Added

- Vector logo drafts (full-colour + mono engrave variant) in `docs/`.

## [0.9.0] — 2026-06-06

Feature-complete for the original vision: from here to 1.0 is polish and
field testing.

### Added

- **SVG import** — two ways in. The sketch toolbar's **Import SVG** tool
  places artwork interactively: live ghost preview of the actual paths at
  the cursor, width in mm, 90° rotation, click to stamp, **Backspace to
  remove the last placement** (also works in the Text tool). Or
  **File → Import → SVG** for the quick path: the file lands as a named
  sketch on the ground plane at natural size. Paths, shapes, transforms,
  groups and viewBox all handled (vendored nanosvg); imported outlines
  form true closed regions — extrude a logo or engrave it onto a
  cylinder. Stroke-only SVGs import as open centrelines (no regions);
  convert strokes to paths for full geometry.
- **Region-granular selection everywhere** — box-select now picks up the
  individual closed regions inside the rectangle (same as Ctrl+clicking
  each one), so multi-shape sketches get the same tools and the same
  correct extrudes as single picks. **Ctrl+click toggles**: clicking a
  selected region deselects it. Selected regions render with a
  **translucent fill** (hover gets a lighter wash) so the selection reads
  as surfaces, not outlines.
- **Multi-island extrude** — extruding a whole sketch (or a box-selected
  set) now groups closed wires even-odd by containment: every island
  extrudes with its proper holes. Previously the largest outline
  swallowed every other wire as a "hole", which fed OCCT inverted
  geometry and produced non-manifold bodies on SVG/text sketches. The
  parametric rebuild path uses the same construction, so reloaded
  extrudes reproduce exactly.

### Changed

- **"Project Sketch" is now "Projection"** — the noun reads one way.
  Projection scoping (click regions in the viewport while the panel is
  open) is mentioned in the tooltip so it's discoverable.

### Fixed

- **Projection of regions with several holes** failed with a misleading
  "sketch must land fully inside the face" message — a six-bladed logo
  couldn't engrave. The face assembly now builds each projected wire
  independently and subtracts holes with a boolean, which needs no wire
  winding coordination.
- **Multi-island and multi-region extrudes swept in the wrong direction**
  (flat "2D projection" bodies) when the sketch plane contained the world
  Z axis — compound profiles fell back to a default direction instead of
  the profile's own normal.
- **SVG circles vanishing from region detection** — SVG paths legally
  contain zero-length curve segments at joints; one zero-length sketch
  line made the wire builder silently discard the whole loop. Degenerate
  segments are now scrubbed at import AND skipped by the wire builder, so
  affected sketches heal on load.
- **Zoom stutter on dense scenes** — every scroll tick ray-cast the whole
  document to find the zoom focus; it now reuses the hover pick.
- **Saving while a tool preview was live** could crash, and silently
  persisted the preview body into the project file. Autosave now waits
  for tools to close; a manual save cancels live previews first.

## [0.8.5] — 2026-06-06

### Added

- **Project Sketch onto Face** — pick a body face → **Project Sketch** →
  the chosen sketch's regions project onto the face along the sketch's
  normal and **engrave** (cut in) or **emboss** (raise out) to a depth.
  The projected outline is rebuilt on the target face's own surface, so
  the result follows the curvature exactly — wrap a logo or label onto a
  cylinder. Scope it live: with the panel open, click sketch regions in
  the viewport to project only those (Ctrl+click adds; click empty space
  for all); clicking a region also picks its sketch. Depth is measured
  along the projection direction, like a stamp pressed straight in.
  Sketches that hang off the face are refused cleanly. Parametric and
  reload-editable.
- **Text in sketches** — a **Text** tool in the sketch toolbar inserts
  real TrueType outlines (three bundled fonts: JetBrains Mono, DejaVu
  Sans, DejaVu Serif) as ordinary sketch geometry: letters form closed
  regions with proper holes, so they extrude, push/pull, and project like
  anything drawn by hand. Letter height is the measured capital height in
  mm. A dashed preview box with a baseline follows the cursor before
  placing; rotate in 90° steps from the popup — the default orientation
  reads upright in the current view. Glyph outlines stay out of the way:
  no vertex markers, no snap/inference grabbing, no welding new lines
  onto letter points.
- **Per-region extrude** — click one closed region of a sketch (a single
  letter, the circle inside a rectangle) → **Extrude From** extrudes just
  that region; Ctrl+click several regions to extrude them together. A
  whole-sketch selection still extrudes the full profile.

### Fixed

- **Region picking respects nesting** — clicking anywhere inside a closed
  region now selects exactly that region; previously only a click near
  the region's centre worked, and clicks near edges selected the
  surrounding sketch. Strict containment beats boundary proximity, and
  the smallest containing region wins.
- **Concurrent tool previews no longer corrupt each other** — starting
  any interactive operation now cancels whichever other preview is live
  (and vice versa). Previously, cancelling a tool while another preview
  ran on the same body could leave phantom geometry on the body with no
  history step to undo it.
- Region detection is cached — hovering sketches with many curves (text,
  dense profiles) no longer recomputes the region layout every frame.
- **Windows packages now include the bundled fonts** — the UI font
  rendered as a fallback on Windows because the assets folder was never
  staged into the zip/installer.

### Internal

- The Linux AppImage now bundles the executable's full shared-library
  closure (minus the system GL/X11 layer) instead of a hand-maintained
  list — new library dependencies can no longer ship missing.

## [0.8.4] — 2026-06-06

### Added

- **Scale Face.** Select a flat end face → **Scale Face** → the body
  re-slopes toward a scaled copy of that face's profile. Defaults to the
  whole body following from its base (scale a box top → frustum, height
  unchanged); dial the length down for a localized blend, switch to Extend
  mode to add a tapered tip instead. **Non-uniform scaling** via two
  in-plane sliders or by **dragging the on-face arrow handles** (red/blue,
  live percentages); a Uniform checkbox links them. >100% flares. The
  winglet op: pinch a wing-tip profile and the skin follows. Fully
  parametric and reload-editable.
- **Startup splash screen** — the blank window during long project
  auto-opens is now a branded loading screen with live status.
- **Application icon on Windows** — embedded in the exe (Explorer,
  installer) and set at runtime (taskbar); X11 docks get it too.

### Changed

- **Sketch inferences keep their hands off the cursor**: directional snaps
  (perpendicular / parallel / tangent / 15° increments) are capped at
  ~1.5 mm of actual cursor deviation — their angular tolerances used to
  make capture distance grow with line length.
- Dashed inference guides extend past the anchor end only, so the
  endpoint marker at the cursor stays visible.
- The **Delete key** removes selected construction planes and axes (same
  as the Items panel's right-click).
- The Taper panel explains the kernel's face-type limit (flat /
  cylindrical / conical walls only) and points freeform cases to Scale
  Face.

### Fixed

- Two dangling-lifetime bugs producing impossible geometry: the first
  polygon per session came out scrambled (vector reallocation invalidated
  the centre pointer — also shipped in 0.8.2's notes, fully resolved
  here), and Scale Face stopped working entirely while drawing a "red
  line to infinity" (reference into a temporary plane → garbage axes).

## [0.8.3] — 2026-06-06

### Added

- **Taper Face.** Select one or more faces of a body → **Taper** (Face
  Operations) → tilt them about the body's base with a live-preview angle
  slider (−45° to +45°). A cylinder wall becomes a cone; a box's sides
  become a pyramid frustum. Pull axis is automatic (cylinders and cones
  draft along their own axis; flat faces pick a sensible upright) with
  manual X/Y/Z override and a flip-base option for which end stays fixed.
  The panel reports live whether the preview is working, with guidance when
  a face/axis combination can't be drafted. Fully parametric: undoable,
  saved, re-editable after reload.

### Fixed

- **STL exports now stand upright in slicers** — the Y-up scene rotates to
  the Z-up convention on export (a proper rotation, not a mirroring axis
  swap), matching what STEP export already did.

## [0.8.2] — 2026-06-05

The dogfood release: ten fixes straight from real from-scratch modeling
sessions.

### Fixed

- **First polygon per session came out scrambled** — vertices missing their
  centre offset or landing ~10²⁶ mm away. A dangling pointer into the
  sketch-point array across a reallocation; an undo retained the grown
  capacity, which is why retrying the same clicks always worked.
- **Arc tool committed on the wrong side of the chord, seemingly at
  random** — the mid (bulge) click was discarded after computing the
  centre while everything downstream sweeps CCW start→end. Endpoints are
  now stored in the order whose sweep passes through the clicked bulge.
- **Sketch-on-face landed the camera on the wrong side** for faces whose
  stored normal points into the body (narrow side faces) — the camera now
  stands off on the face's outward side.
- **The sketch grid vanished when zooming in** — the fade was centred on
  the orbit target, which cursor-zoom walks away from; it now centres on
  the view ray's intersection with the grid plane.
- **Construction axis from a planar face** fires from the face centroid
  instead of a corner (the surface's parametric origin).
- **Polygon dimension popup rendered half cut-off** until typing forced a
  re-measure (wrapped text inside an auto-resizing window).
- Inference guide label no longer sits on top of the measurement readout
  on short lines.

### Changed

- **Ctrl+Z during an in-progress sketch placement cancels the shape being
  drawn**; committed elements undo on the next press.
- **Backspace removes the last spline control point** during placement
  (cleaning up its dot unless something else uses the point); Esc on an
  in-progress spline removes all its placed points.
- **Picking a snap-grid step closes the snap popup** — no click-away
  needed before resuming drawing.

## [0.8.1] — 2026-06-05

Section View actually works out of the box.

### Fixed

- **Section View default plane** was the ground plane with its normal up —
  enabling it clipped everything above the floor, showing only the bottom
  face. Default is now a vertical (front) plane, and toggling Section View
  on auto-aims the cut through the centre of the visible bodies.
- **Edge wireframes now clip with the section plane** — previously the full
  body's edge overlay ghosted through the removed half.

### Changed

- The Section View panel moved to the top-centre of the viewport (it was
  pinned top-right, hidden behind the Items/Properties panels) and gained an
  **Exit Section View** button (Escape also exits).
- Offset is a slider (−100 to +100 mm; Ctrl+click to type exact values
  beyond the range).

## [0.8.0] — 2026-06-05

The "threads and curves" release: real modeled ISO threads with a validated
cutting engine, a working spline tool, and a section view for looking inside
parts without cutting them.

### Added

- **Threads.** Select a cylindrical face (rod or hole) → **Thread** → ISO
  coarse defaults are pre-filled from the diameter; pitch, depth, length and
  handedness are editable. Threads are real cut geometry — they export to
  STEP/STL and survive project reload as an editable parametric step.

  **How the cutting works:** a fast single-tool pass handles clean cylinders;
  anything harder (interrupted or partial cylinders) falls back to a
  turn-by-turn engine that cuts one thread turn at a time and **validates
  every cut** — removed volume plus classifier probes for groove depth,
  crest integrity, and full groove width — before accepting it. Two
  complementary cutter families (square-ended bands for whole/holed rods,
  tapered tools for split halves) are selected automatically per body. A cut
  that cannot be verified is skipped or the whole operation fails cleanly —
  threads can no longer produce garbage solids or crash the app.

  **Workflow rule — apply threads LAST.** Modeling operations on a threaded
  body (push/pull, boolean, split) are refused with guidance: delete the
  Thread step in History, make the change, then re-apply the thread (the
  step is parametric, so re-applying is a couple of clicks). The Thread
  panel states this.

  **What threads handle today:** plain rods, chamfered rods, rods with
  transverse holes, internal threads in holes, and lengthwise-split halves
  (split first, then thread each half). **Known limitations:** slots crossing
  the threaded length can leave gapped turns near the slot (failed cuts are
  skipped honestly, never garbled); threading long or interrupted rods takes
  up to a minute — every turn is being individually verified; the
  "operation refused on threaded body" message currently only appears in the
  console log.

- **Spline sketch tool — now real geometry.** Previously the spline drew a
  display-only polyline that couldn't extrude. Now: smooth centripetal
  Catmull-Rom curves that follow your clicks live, close into extrudable
  regions (click your first point to close, your last point or Enter to
  finish open), chain with lines and arcs into mixed profiles, and feed
  extrude/revolve/loft with exactly the curve you see — renderer and
  geometry sample the same function.
- **Section View** (View menu). A render-only clipping plane: pick a world
  plane or any construction plane, drag the offset, flip sides. Bodies open
  up visually with interior lighting and exact intersection curves traced on
  the cut — inspect thread profiles or wall thickness with zero booleans and
  zero risk to the model.

### Changed

- **Push/pull on dense bodies** (threaded rods and anything else past ~250
  faces) shows a tinted ghost of the tool volume during the drag and runs
  the real boolean once on release — live dragging no longer freezes the
  app for seconds per frame.
- Boolean cuts inside the thread engine run with OCCT's parallel mode.

### Fixed

- Threads no longer produce "stacked poker chip" bodies, inverted cuts,
  uneven groove widths, or blunt unfinished groove ends at the rod faces —
  the validated per-turn engine rejects every one of these (each was
  reproduced and beaten during development).
- Two segfault classes traced to unvalidated thread cuts reaching the
  tessellator are gone with their cause.

## [0.7.0] — 2026-06-04

The "real parametric CAD" release: a reopened project is no longer a frozen
replay — its history comes back **live and editable**.

### Added

- **Cross-session parametric editing.** History steps reload as their real,
  re-editable operations instead of baked snapshots: **Pattern** (count,
  spacing, angle, axis), **Extrude** (distance, draft), **Push/Pull** —
  sketch-region *and* bare-face targets — (distance), **Revolve** (angle,
  axis), **Fillet** (radius), **Chamfer** (distance), **Shell** (thickness,
  preserved openings), **Cylinder resize** (diameters), and **construction
  plane/axis creation** (undo/redo with stable ids). Sketch-sourced profiles
  re-derive from their sketch; edges and faces persist via a new
  sub-shape-identity scheme and survive reloads exactly.
- **Geometric edge re-binding.** Editing an upstream feature regenerates the
  body — downstream fillets/chamfers now re-locate their edges on the new
  shape by carrier geometry (same line/circle), so fillet→chamfer chains
  survive re-edits. A genuinely consumed edge still fails loudly rather than
  blending the wrong one.
- **Recompute failure handling.** A step that can't recompute is suspended
  with an explanation in the History panel and comes back automatically when
  the upstream edit is reverted; an edit the operation itself can't build
  (fillet radius larger than its host) snaps back to the last working value
  and leaves the model untouched.
- **Radial pattern around construction axes** — the rotation-axis dropdown
  lists every construction axis (plus world X/Y/Z), taking both direction and
  origin from the axis.
- **Editable cylinder-resize step** — diameter field(s) in Properties.
- **Editor ergonomics** — Enter commits a parameter edit; the History panel
  pins Apply below a scrollable parameter area; interactive fillet/chamfer
  re-edit (select a blend face → *Edit Fillet*) previews the full downstream
  replay live.

### Fixed

- **Loft with ring profiles forms tubes** — inner boundaries loft into
  channels and are cut from the outer body; loft and revolve both pick the
  outermost sketch region instead of an arbitrary one.
- **Revolve popup defaults to "Sweep Sketch profile"** when a sketch is
  selected (previously stayed on Rotate Body and committed a snapshot step).
- **History fidelity across saves**: nine operations saved empty body diffs
  (pattern, loft, sweep, copy, boolean, shell, cylinder-resize, align,
  snapshot transforms) making their reloaded steps un-undoable; re-saving a
  reloaded project no longer collapses its history; revolve steps now record
  their diff at all.
- **Autosave can no longer truncate history** — it holds off while you're
  below the history tip (mid-undo), and Close Project routes to the explicit
  prompt instead of quietly saving a gutted file.
- **Close Project clears the viewport fully** — construction planes/axes no
  longer linger as unclickable ghosts.
- Emptied sketches leave the Items panel on finish.
- Fillet/chamfer drag quantises to the displayed 0.1 mm (an on-screen "2.0"
  no longer stores 1.9948).
- **Windows build restored** (broken since 0.5.2 by a POSIX-only include).

## [0.6.3] — 2026-06-02

### Added

- **Construction axes brought to parity with construction planes.** A new
  contextual **"Add Axis…"** dropdown in the Tools panel creates axes from the
  current selection, each resolving to an (origin, direction) the host computes:
  - **From cylinder axis** — a cylindrical face's centreline
  - **Along edge** — a straight edge
  - **Through two vertices**
  - **Normal to face** — a planar face's normal
  - **Intersection of two planes** — computed via `IntAna_QuadQuadGeo`

  The plain Construction Axis button also auto-defaults to the right mode for
  whatever is selected.
- **Flip Direction for axes** — `Document::flipAxisDirection`, available from
  the Items-panel axis context menu and the Properties panel.
- **Axis Properties readout** — a selected axis shows origin / direction /
  length, plus a Flip button.

### Fixed

- **Axis transforms are now undoable across the board.** A new
  `AxisTransformOp` (before/after origin + direction) is wired into the axis
  gizmo's move branch, so `Ctrl+Z` reverts axis drags — the same fix
  `PlaneTransformOp` brought to planes.

## [0.6.2] — 2026-06-02

### Added

- **Construction planes completed into a full datum-creation system.** New
  creation modes surfaced contextually behind a single **"Add Plane…"**
  dropdown that only lists what the current selection supports: Midplane
  (centred between two parallel planes/faces), Normal to axis/edge, Tangent to
  cylinder, Perpendicular to cylinder axis, and Through cylinder axis
  (longitudinal). All reduce to "plane with normal N through point P"; the host
  computes (N, P) from the selection.
- **Flip Normal** for planes (`Document::flipPlaneNormal`) with a visible
  normal stalk drawn by `PlaneRenderer`, plus a **Rotate/hinge about axis**
  popup (hinge = plane U/V, a construction axis, an edge, or a cylinder
  centreline).
- **Undoable plane transforms** via `PlaneTransformOp`, wired into the plane
  gizmo and the rotate-about-axis Apply.
- **Plane Properties readout** — origin / normal / in-plane-X / tilt (user
  Z-up), with Flip and Rotate buttons.
- **Construction Axes** (new datum type) and **Revolve** (profile around an
  axis, 0–360°) folded in under this release.

### Fixed

- **Coplanar pick tie-break** — a construction plane sitting on a body face no
  longer steals the click; the face wins ties so coplanar faces stay
  selectable (`Picker.cpp`). The sketch path is untouched.

## [0.6.0] — 2026-06-02

### Added

- **Construction planes as first-class document objects.** Create planes via
  the Construction Plane popup (XY / XZ / YZ in user-Z-up convention, plus
  Parallel-to-Face) with a live preview, an offset slider, and a typeable
  `Rotate by N° around X/Y/Z` field. Planes survive history reload and
  render as translucent blue quads with a darker border. Selected planes
  switch to a warm amber highlight. Hidden while sketching in ortho so the
  drawing canvas stays clean.
- **Click-selectable planes.** Picker hit-tests every visible plane after
  bodies; a closer plane wins. Selection routes through a new
  `SelectionType::Plane` entry; the Tools panel switches to a Construction
  Plane group with Sketch-on-Plane / Move / Rotate.
- **Move + Rotate gizmo on planes.** Same gizmo handles that move sketch
  planes today, now for construction planes via a new `m_planeGizmoDrag`
  list. Translate dollies the plane along world axes; Rotate spins around
  the plane's own origin. Auto-armed during the placement popup, opt-in
  via W/E (or the toolbar Move/Rotate buttons) after commit — so a plane
  click alone just highlights.
- **Sketch on Plane.** The Tools panel exposes "Sketch on this Plane"
  when a plane is selected, routing through the same enter-sketch path the
  XY/XZ/YZ start-sketch actions use, just with the plane's stored
  `gp_Pln` as the host.
- **Plane gizmo readouts.** Cursor-pinned amber pill during a plane drag:
  `Δ N.NN mm | Origin M.MM mm` for Move (left = this drag, right =
  signed distance from world origin along the plane's normal), `N.N°
  about X/Y/Z` for Rotate. Both respect the corner-widget snap step.
- **Finer rotation snap for planes.** Rotation snap is 5° (hard) / soft
  15° while dragging a plane, versus the body/sketch 15° / soft 45°.
  Plane orientation is often a precise angle (15° chamfer-line, 23° draft)
  that the looser body snap fought against.
- **Plugin render-pass invocation.** `Application` now iterates registered
  `RenderPassContribution` entries each frame, after the grid/background
  and before the body/edge pass. Each pass's `initialize()` runs once on
  the GL thread. ConstructionPlanePlugin uses this to own its
  `PlaneRenderer` end-to-end (no more `m_planeRenderer` in Application).
- **`PluginContext::isInSketchMode()`** exposes the host's sketch-mode
  state so plugins can suppress visual decorations during sketch-edit.

### Changed

- **Construction-plane orientation in Z-up.** "XY" plane (the popup's first
  radio) is now the floor — normal = world +Y, offset slider drives height.
  "XZ" is the front wall, "YZ" the side wall. Reads naturally in the user's
  Z-up convention.
- **Body Dimensions section in Properties is read-only.** The editable
  dim fields moved into the Scale gizmo popup, which now has a **% / mm**
  toggle. In mm mode the X/Y/Z inputs pre-fill from the body's live bbox
  extents (Z-up); typing applies a per-axis scale anchored at bbox-min so
  the body grows along +axis only. % mode keeps the centre-pivot
  multi-axis scale.
- **Push/Pull snaps to the corner-widget grid step.** Drag the arrow,
  type a distance, or move the slider — all snap to 0.1 / 0.5 / 1 / 10 mm
  (whatever the snap widget says). Toggling snap off mid-drag immediately
  frees the distance back to fine values.
- **Fillet / Chamfer readout pinned to the cursor.** Arrow + handle still
  come out of the edge; the mm pill now sits 14 px right of the mouse
  (same UX as the arc-angle preview) instead of floating at the edge
  midpoint where your eye has to track it.
- **Toolbar grid-step row removed.** Snap on/off + step are exclusively
  the corner widget next to the ViewCube. Duplicate grid rows in the
  sketch and body toolbar groups were removed.

### Fixed

- **Plane gizmo drag no longer pushes a phantom `TransformOp` with
  `bodyId = -1`.** Previous code reached the single-body commit branch
  with `nBodies = 0` for plane-only drags, pushing a bad op that crashed
  the next launch with `Body not found: -1`. New `planeOnly` branch
  short-circuits the body/sketch commit paths since plane drags already
  write through `Document::setPlane` during the live drag. Also added a
  defensive `Document::putBody` reject for `id < 0` and a try-catch
  around the full-rebuild loop so a corrupt project can still load.
- **Construction-plane offset slider** now syncs with the live preview
  plane each frame — gizmo-driven moves update the value rather than
  leaving it stuck at zero.
- **Single rotate readout per drag.** The body/sketch rotate °-label was
  appearing alongside the plane-aware cursor-pinned pill, showing two
  different snapped values. The body/sketch readout now skips when only
  a plane is in the drag.
- **Arc angle preview readout matches the snap policy.** Reads the same
  rounded value the cursor will actually land on.

## [0.5.2] — 2026-06-01

### Added

- **Editable body dimensions in the Properties panel.** Selecting a single body
  shows X / Y / Z extent fields (Z-up convention — X/Y the floor axes, Z the
  height). Type a new value to scale the body along that axis, anchored at its
  bbox-min corner so growth is along +axis only.
- **JetBrains Mono bundled for the UI font.** Slashed zero, distinct 0 / 8 / B
  glyphs — fixes the "is that a 0 or an 8?" reading problem on the default
  ProggyClean. ~270 KB in the AppImage; loaded from
  `share/materializr/fonts/JetBrainsMono-Regular.ttf` at startup.
- **Cursor-aware mouse-wheel zoom.** Scrolling now dollies toward whatever the
  cursor is over (ray-pick onto geometry, fallback to a target-plane projection
  over empty space). Solves the "can't zoom into a small off-origin object
  without a pan-zoom-tilt dance" problem.
- **`F` key = frame selection.** Hit `F` to fit the camera to the selected
  bodies, or to all visible bodies if nothing's selected. Standard CAD shortcut;
  suppressed in sketch-edit and while typing into a text field.
- **Arc tool: live sweep angle readout + 15° snap.** While placing the third
  click, the cursor shows the inferred sweep (e.g. `90.0° (¼)`, `180.0° (½)`),
  pinned 14 px right of the cursor. Within ±5° of a 15° multiple the cursor
  jumps to the exact apex via `d = (L/2)·tan(θ/4)` — same side of the chord
  preserved, no flipping. Snap defers to the global snap toggle.
- **Angle constraint dimension arc + label.** The Angle constraint between two
  sketch lines now renders a SolidWorks-style arc at the intersection vertex,
  with a `°` label hugging the outside of the arc midpoint. Auto-detects the
  vertex from the closest endpoint pair (handles loose Coincident pairs).
- **Bold push/pull, extrude, fillet, chamfer arrows.** The dim-arrow style for
  active body operations is now amber (255, 200, 60) with a black halo, 3 px
  line, ~2× larger arrowhead, and an amber border on the mm/° label — readable
  against any body colour. Sketch inferences and the move-gizmo readout keep
  the lighter style.

### Changed

- **Snap on/off + step are now exclusively the corner widget** next to the
  ViewCube. Removed the duplicate checkboxes from the Settings dialog and from
  both Toolbar groups (sketch + body). Threshold slider stays in Settings.
- **Body dimension bbox + Measure tool bbox use `AddOptimal`.** Analytic bounds
  rather than tessellation + per-face Tolerance padding: a Ø80 cylinder reads
  exactly `80.000 / 80.000 / 20.000` instead of `80.007 / 80.005 / 20.010`.
- **Move-gizmo readout reports the body's bottom along the up axis** rather
  than its centre. A cylinder sitting on Z=0 now reads `Z 0.00` (drag up 10 mm
  → `Z 10.00`) — matches what the user sees on the grid.

### Fixed

- **Sketch push/pull no longer accumulates banding on the vertical wall during
  the slider drag.** `Document::removeBody` now publishes `BodyRemovedEvent`,
  and `Application` listens to drop the renderer's mesh + edge slots
  immediately. Previously each preview-undo deleted the body from the document
  but left its mesh in the ShapeRenderer until the next full rebuild — N
  overlapping prism previews stacking up during a drag.
- **Push/Pull on a free-floating sketch no longer marks every invisible body
  dirty every frame.** The missing-slot loop now restricts itself to visible
  bodies, so a 100+ body project doesn't burn re-tessellation budget on hidden
  geometry.
- **Snap-widget click bleed-through.** The corner widget now uses manual
  hit-testing (ViewCube pattern) and publishes its hover state to the
  viewport input gate, so clicking the widget opens the popup instead of
  starting a sketch line beneath it.
- **Snap-widget layout-cursor pollution.** Setting changes via the corner
  widget no longer make subsequent items render directly under the button —
  `InvisibleButton + SetCursorScreenPos` was tripping ImGui's
  boundary-extension assert; replaced with `IsMouseHoveringRect`.

## [0.5.1] — 2026-05-31

### Added

- **v3 project file format — `.materializr` files are ~5× smaller.** Binary
  BREP via OCCT's `BinTools` (no display triangulation), gzip-wrapped at max
  compression, and a length-prefixed `PARAMS_LEN` block for op-parameter
  blobs so a single op can carry multiline / arbitrary content. The reader
  auto-detects v2 vs v3 by the gzip magic + header version; old files load
  unchanged, new saves write v3. A real-world 16 MB project saves at ~3 MB
  in v3.
- **Constraints panel on the live sketch.** Selecting a sketch (or any
  region of one) outside sketch-edit shows every constraint in the
  Properties panel:
  - Distance / Radius (shown as Ø diameter) / Angle constraints get inline
    text editors that commit on Enter or focus-out.
  - Non-dimensional constraints (Horizontal, Parallel, Coincident, etc.)
    appear as muted bullet rows so you can see what's applied.
  - Edits run the solver immediately and push a `SketchEditOp` covering
    before+after onto history (undoable).
  - Works across sessions — operates on the live sketch, not historical
    snapshots.
- **History → Properties editor survives reload.** `SketchEditOp` now
  serializes both before+after sketch snapshots into the project file's
  per-step params blob. On load, the rehydrator reconstructs a real
  `SketchEditOp` (binding to the live sketch by id) instead of a
  parameterless `ReplayOp`, so clicking a sketch-edit step in History on
  an old project still surfaces editable constraint values.

### Changed

- **`Sketch::setCircleRadius` / `setArcRadius`** mutators on Sketch — the
  constraint solver now actually writes the constraint value back into the
  circle/arc geometry when a Radius constraint applies, instead of treating
  Radius as "informational only" (the previous comment in the solver). So a
  Radius constraint set to 30 mm now produces a 30 mm circle, not a 30-mm-
  labelled 130-mm circle.
- **Taskbar icon resolves correctly under Dash-to-Panel / GNOME.** The GLFW
  window now sets `WM_CLASS = "Materializr"` (X11) and `app-id =
  "Materializr"` (Wayland) via `glfwWindowHintString`, and the bundled
  `.desktop` includes `StartupWMClass=Materializr` so the running window
  matches its launcher entry. Without these, GLFW defaulted to `glfw` and
  Dash-to-Panel showed a generic blank.

### Fixed

- **Document::findSketchId(Sketch*)** helper added so callers can do a
  reverse lookup from a held `shared_ptr<Sketch>` back to its document id —
  used by `SketchEditOp::serializeWithDocument` to stamp the live id into
  saved snapshots.
- **Parser refactor**: extracted `parseSketchBody(istream, Sketch&,
  endTok)` out of the existing `readSketch` so the same parser serves both
  top-level sketches and embedded SketchEditOp snapshots. No behavioural
  change for v2 files.

### Known limitations

- **Downstream ops don't auto-cascade yet.** Editing a sketch's constraint
  resizes the sketch correctly, but an `ExtrudeOp` / `PushPullOp` /
  `FilletOp` that consumed that sketch (or its resulting body) doesn't
  automatically re-execute. The body that came from the old sketch keeps
  its old shape until you redo the op manually. Tracked as task #98
  "replay-on-sketch-edit"; the toponaming work needed to safely re-run
  fillets etc. is being treated as its own future release.

## [0.5.0] — 2026-05-31

### Added

- **Loft plugin.** New solid op that morphs between two closed sketch
  profiles via `BRepOffsetAPI_ThruSections`. The plugin contributes a "Loft"
  button to the Sketch panel AND the Region panel — select two whole
  sketches or two regions on different sketches and click. With only one
  selected, a non-modal banner nudges you to Ctrl-click a second profile
  (the previous modal version blocked viewport picks). On click with two
  selected, a popup opens with **Solid / Shell**, **Smooth / Ruled**, and
  **Reverse profile B vertex order** toggles, each re-pushing a live preview
  `LoftOp` onto history; Apply commits, Cancel/Esc undoes the preview.
  Tooltip explains the "parallel-plane assumption" so users understand why
  orthogonal-plane lofts produce a pyramid-tent surface.
- **Sketch-as-construction-plane** workflow. A standalone sketch can now
  serve as a movable construction plane:
  - The viewport picker reaches sketches via either their closed-region
    interior (`SelectionType::SketchRegion`) or any edge (`SelectionType::
    Sketch`) — open profiles like an arc or a spline are now selectable too.
  - Box-select also captures sketches by projecting their points to
    screen-space.
  - When a sketch is selected outside ortho / sketch-edit, clicking **Move**
    or **Rotate** in the Tools panel arms a Move/Rotate gizmo centred on
    the sketch's bbox centroid. Dragging mutates the sketch's plane (and
    its geometry rides along) via a new `SketchTransformOp`, snapshotted
    for undo. A three-input X/Y/Z popup offers exact translations.
  - Selection-change disarms the gizmo automatically, so picking a sketch
    just surfaces the toolbar options first — no surprise gizmo on top.
- **Construction Plane popup.** Replaces the no-op New Construction Plane
  button: a live-previewed popup with **XY / XZ / YZ** radio buttons, an
  optional **Parallel to selected face** mode (auto-enabled when a planar
  face is in the selection), and an **Offset** slider that pushes the plane
  along its normal. Backed by `ConstructionPlaneOp` on history. (Plane
  rendering in the viewport + Items-panel listing remain a TODO — for now
  use the sketch-as-plane workflow above.)
- **Editable dimension constraints from the History panel.** Clicking a
  sketch-edit step in History now shows every Distance / Radius / Angle
  constraint with inline `InputDouble` fields (diameter for radius,
  matching the in-sketch popup). Typing a value re-solves the snapshot
  immediately; Apply Changes propagates it through `editStep`. Non-
  dimensional constraints (Horizontal, Parallel, …) appear as read-only
  rows.
- **Snap-grid corner widget** next to the ViewCube. Small square showing
  the current grid step (0.1 / 0.5 / 1 / 10 mm). Solid-blue border when
  snap is on, faint grey when off. Left-click opens a Snap & Grid popup
  (snap toggle + step buttons); right-click quick-toggles snap. Every
  change persists to `settings.cfg`, so the choice survives launches.
- **Persistent snap & grid settings** (`snapToGrid`, `sketchGridStep`)
  written to the same `~/.config/materializr/settings.cfg` as the other
  preferences, round-tripping through the JSON import/export path too.
- **App icon + banner.** Replaced the placeholder "C" SVG with the real
  M-cube `icon.png`, resized to 256×256 + 512×512 hicolor sizes by
  ImageMagick during the Docker build. `banner.png` lives at the repo
  root for the GitHub social-preview image.

### Changed

- **Sketch on XY / XZ / YZ now produce visibly distinct camera views.**
  `alignCameraToActiveSketch`'s continuity-projection used to snap the up
  vector to the same in-plane axis for both XY and XZ when starting from
  an empty viewport, making them indistinguishable. The three explicit
  toolbar entry points now prime the camera up with the canonical
  Top / Front / Right axis before entering, so the projection lands on
  the right `faceY` for each.
- **Gizmo readout shows the dragged axis** and matches the Z-up user
  convention (red = X, green = Z, blue = Y) instead of the world internals.
  Translate label leads with the axis letter and the coord tuple is
  re-ordered to `(user X, user Y, user Z)`. Rotate label does the same
  for its axis letter.
- **Gizmo translate snap is now absolute-position** (pivot lands on grid
  intersections) instead of delta-snap. Matches the rest of the app's
  grid-snap semantics; off-grid starting positions jump onto grid on the
  first qualifying drag. Rotate snap is hard 15° when snap-to-grid is on,
  free with a 7° soft-snap near 45° when off.
- **Gizmo dimension line draws from the world origin** to the current
  pivot, with the absolute coords in the label — easier to read "where
  is this now" than "how far did I drag this".
- **Gizmo hides during any interactive op** (Push/Pull, Loft, Construction
  Plane, Pattern, Shell, Resize, sketch-edit, …). Previously the rotate
  gizmo stuck around on top of those popups' previews.
- **Toolbar tooltips wrap across multiple lines** instead of running off
  one line. Long blurbs (Loft, Move/Rotate on sketches, the parallel-plane
  caveat) now read as paragraphs.
- **Sketch selection highlight covers open profiles.** `SketchRenderer::
  renderSketchHighlight` walks every primitive (lines / circles / arcs /
  splines / polygon edges) in solid yellow at 4 px when a whole sketch is
  selected, so an arch or open polyline lights up the same way a closed
  region does.

### Fixed

- **Vertical / open sketches were unpickable** in the viewport. Two causes:
  the picker only tested closed regions, and the body-vs-sketch occlusion
  rejected sketches lying on a body face because the mesh-triangle hit and
  the analytical plane hit can disagree by microns. Picker now falls back
  to an edge-distance test (returns the parent sketch as `SelectionType::
  Sketch`) and the occlusion threshold loosened to `max(0.5 mm, 0.5 %
  view-distance)` with an automatic source-face exemption.
- **Push/pull on a reloaded sketch around an existing hole** produced a
  solid bar instead of a tube. `Sketch::buildRegions` now grafts inner
  wires from the source face onto each sketch face whose outer wire fully
  contains them, fixing BOPAlgo not partitioning when edges don't intersect.

## [0.4.0] — 2026-05-30

### Added

- **SketchUp-style drawing-time inferences.** While you place sketch points,
  ghost guide lines show what the cursor is aligned to and the cursor snaps to
  that alignment — without leaving any persistent constraint behind. Seven
  inference kinds: endpoint (yellow square), midpoint (cyan triangle), on-line
  (lavender diamond), axis-from-point red horizontal and green vertical
  dashed guides, perpendicular-to-previous (orange dashed), and parallel-to
  -previous (magenta dashed). When two inferences fire at once (e.g. the line
  is perpendicular to the previous segment AND the cursor's X is aligned with
  an earlier point) the cursor snaps to their **intersection**, which makes
  drawing a square or any axis-aligned closed shape effectively one click per
  corner. Every guide is haloed in black so it stays visible against the
  light-blue sketch face / dark grid backdrop.
- **Auto-close on chain-start.** Clicking back onto the starting point of a
  line chain commits the closing segment and ends placement. Combines with
  the endpoint inference so you don't have to aim precisely — get within snap
  range, click, done.
- **Opt-in formal sketch constraints.** Right-click any selected sketch
  element(s) and a context menu offers "Add Constraint ▸" with seven types:
  Horizontal, Vertical, Coincident, Parallel, Perpendicular, Equal length,
  Fix Position. Items are filtered by selection arity so you only ever see
  options that can actually apply. Once added, a constraint enforces itself
  during subsequent drags (e.g. dragging a Horizontal line can shorten or
  reposition it but never tilt it). Constraints round-trip through the
  project file so they survive save/reload. Geometry you never click a
  constraint on stays totally free — constraints are 100% opt-in.

### Changed

- **No more silent autoConstrain.** Newly drawn lines used to pick up
  invisible Horizontal / Vertical / Coincident constraints based on their
  drawn angle. They no longer do — the inference system above provides the
  alignment help drawing-time, and persistent constraints come only from
  explicit user clicks.
- **Sketch line palette.** Committed sketch lines (lines / circles / arcs) are
  now deep cobalt (was pastel blue, washed out against the sketch face tint).
  Preview lines are bright yellow (was white, indistinguishable from the
  dimension overlay). Dimension overlay stays grey-white. Three colours, three
  clearly distinct meanings.

## [0.3.5] — 2026-05-30

### Added

- **Per-body STL export.** Right-click any body in the viewport or its row
  in the Items panel and pick **Export STL…** to dump just that body to
  a file. The save dialog opens with the body's current name (as shown
  in the Items panel) as the default filename — `<body name>.stl`, with
  filesystem-unfriendly characters (`/ \ : * ? " < > |`) sanitised to
  `_`. Separate from File → Export STL, which still writes every visible
  body to one file; this one is for pulling individual parts out of a
  multi-body project without juggling visibility.

### Internal

- New `Application::exportBodyAsStl(int bodyId)`, called from both
  context menus.
- `ItemsPanel` gained a `setExportStlCallback(std::function<void(int)>)`
  hook so it can route the menu click without depending on the STL I/O
  module directly.

## [0.3.4] — 2026-05-30

Tiny cleanup release — drops scaffolding left over from troubleshooting in
0.3.3 that didn't end up driving any behaviour. No user-facing changes.

### Internal

- Removed the `wasOrtho` flag in `Camera::orbitLevel` (and its `(void)`
  discard). It was captured for an experiment that snapped the orbit
  target to world origin on ortho-exit; the experiment was reverted
  earlier in the same session in favour of preserving the sketch's
  grid-aligned anchor, so the variable served no purpose.
- Removed the `SavedCamera` struct, `m_savedCameraForSketch` member, and
  the `saveCameraInto` helper. The original purpose — restoring the
  pre-sketch camera in `exitSketchMode` — was deleted in 0.3.3 in
  favour of leaving the camera in place. The snapshot was still being
  captured at every sketch entry but nothing read it.

## [0.3.3] — 2026-05-30

A long session of polish: ViewCube redesign + axis triad + roll buttons,
ortho-snap respecting turntable, picker depth occlusion fix, sketch-tool
highlight, polygon preview / rotation / corner-snap, push/pull direction
correction on STEP imports, two-step Escape + Exit-Sketch discard button,
and op-parameter persistence in the project file.

### Added

- **ViewCube overhaul** to match FreeCAD's layout: smaller cube body, four
  cardinal triangle-arrow orbit buttons, two large sweeping corner arrows
  for 90° camera roll around the view axis, a home / reset-view button
  with a house glyph, and a coloured XYZ axis triad that rotates with the
  camera. Triad colours follow the world grid convention (X red,
  Y green = world up, Z blue = world forward).
- **Side-face labels** shortened to `L / F / R / B` so the smaller cube
  reads cleanly; Top / Bottom keep full labels.
- **Roll buttons** (`RollLeft` / `RollRight` actions) rotate the camera's
  up vector 90° around the view direction — a snapped ortho view stays
  snapped but spins in place.
- **Z-up turntable respect on Top / Bottom ortho snap.** The new ortho
  view's up vector is computed from the camera's horizontal forward
  direction so whatever was ahead of you before clicking the face ends
  up at the top of the screen. Front / Back / Left / Right snaps keep
  world +Y up (those are unambiguous).
- **Sketch-on-face turntable + axis snap.** When sketching on a horizontal
  face the projected camera up degenerates; we fall back to the camera's
  forward direction projected onto the plane, then snap to whichever of
  ±faceY / ±faceX is closest, so the sketch view always lands axis-aligned
  but in the quadrant the user was facing.
- **Sketch grid origin snapped to the nearest world-grid intersection**
  projected onto the sketch plane — grid lines coincide with whole-mm
  world positions on the face plane instead of being shifted by the
  face centre's fractional coords. Same snapped point becomes the camera
  target, so an orbit out of ortho pivots around the area you were
  sketching on.
- **Active sketch-tool highlight.** The current tool's button in the
  sketch toolbar gets a 2 px mid-grey (`#808080`) border so it's
  unambiguous at a glance which mode (Line / Circle / Rectangle / Arc /
  Spline / Polygon / Trim) is active.
- **Polygon tool overhaul.** Dimension popup label reads "Sides" (≥3) and
  typing a count + Enter updates the live preview without committing —
  the user can iterate on side count, rotation, and radius before clicking
  to commit. The cursor's direction from the centre sets the polygon's
  rotation so the first vertex lands exactly on the (snapped) cursor —
  same "corner snaps to grid" behaviour as the circle tool.
- **Two-step Escape in sketch mode.** First press cancels just the
  in-progress shape (line mid-stroke, polygon awaiting its second click,
  etc.); second press exits sketch mode entirely.
- **Exit Sketch (discard) button** under Finish Sketch. Rewinds history
  to before the sketch was entered, removes the sketch from the document
  if empty, and exits — leaves the body in its pre-sketch state. Useful
  for bailing on a half-built sketch.
- **Op parameter persistence in the project file.** Each saved history
  step gains an optional `PARAMS "blob"` line; FilletOp, ChamferOp,
  ResizeCylindricalOp, ShellOp, and PatternOp each implement
  `serializeParams` / `deserializeParams` to round-trip their scalar
  inputs (radius, distance, count, axis, origin, etc.). ReplayOp carries
  the blob across save / load so a future edit-after-reload pass has the
  data already on hand. Older project files without `PARAMS` lines load
  unchanged.

### Fixed

- **Picker depth occlusion: clicking edges through a face.** The previous
  view-direction depth tolerance scaled with view distance (4 % then
  0.5 %), which at typical CAD scales let edges of a 1 mm wall on the
  far side of the body remain "clickable through" the front face. Now
  the picker computes the face's surface normal at the hit point and
  rejects edge segments whose signed distance to the face's tangent
  plane is more than 0.3 mm behind the camera-side. The check is
  camera-angle-independent, so a 1 mm wall reads as 1 mm behind whether
  the view is shallow or steep.
- **Edges of small / zoomed-out faces stealing clicks** from the face
  centre. The 8 px edge-promotion threshold is now clamped to ¼ of the
  face's projected on-screen size, so a 20 px face has at most a 5 px
  edge zone (vs every interior pixel previously being within 8 px of
  some boundary edge).
- **Push / Pull direction wrong on STEP-imported horizontal faces.**
  `BRepGProp_Face::Normal` returns the geometric surface normal, which
  on imported geometry sometimes points INTO the body — so push went
  into the solid and pull cut into thin air. Push / Pull now probes the
  body's solid classifier on BOTH sides of the face at 1 mm; if forward
  is inside and back is outside it flips the normal. Same check fires
  for the live arrow display in `beginPushPull` so arrow and extrusion
  agree.
- **Sketch on STEP-imported face plane normal verified by the classifier.**
  Mirror of the push/pull fix at sketch-entry time so the sketch plane
  itself faces outward — fixes the case where push/pull on a sketch
  region had to fight an inverted underlying plane.
- **Orbit out of sketch ortho** resets the camera up vector to world +Y
  (was carrying over the sketch's chosen up, which caused the post-orbit
  view to look rotated / twisted) and preserves the snapped sketch
  anchor as the orbit target so the model stays framed.
- **Exit Sketch leaves the camera where it is.** Previously
  `exitSketchMode` force-restored the pre-sketch camera, yanking the user
  back to wherever they were before they clicked the face. Now exit
  feels like leaving ortho-snap: the area being looked at stays framed,
  only the sketch grid disappears.
- **Trackpad-mode gizmo drag suppressed camera drag.** When orbit and pan
  are both bound to the Left button (trackpad mode), the camera drag
  block fired in addition to gizmo drag — so trying to drag a gizmo
  handle also yawed the camera. The camera drag now checks
  `m_gizmoDragging` first and suppresses itself while the gizmo owns the
  interaction.
- **ViewCube click-through.** The sketch input block now also gates on
  `m_viewCube->wasHovered()`, mirroring the body picker. Clicking a roll
  arrow or face on the cube no longer leaks a line-draw to the sketch
  underneath.

### Changed

- **Side faces of the ViewCube** are labelled `L / F / R / B` (single
  letters) to fit the smaller cube comfortably.
- **`addPolygon`** gained a `rotationRad` parameter so the first vertex
  can be aligned with the cursor direction. Existing callers that don't
  pass it default to 0 rotation as before.

### Internal

- `Operation` gained `serializeParams()` / `deserializeParams()` virtuals
  (empty default). ReplayOp carries the blob across save / load via
  `setStoredParams` / `storedParams()`.
- `ProjectHistoryStep` gained an opaque `params` field. ProjectIO writes
  `PARAMS "blob"` per step when non-empty; load tolerates missing lines.

## [0.3.2] — 2026-05-30

Sketch pattern polish: circles + arcs now actually replicate, the radial
origin picker drops a yellow snap-to-grid dot inside the sketch, and the
whole popup previews live the way the body pattern popup does.

### Added

- **Live preview for sketch patterns.** Each parameter change (count,
  spacing, sweep angle, origin pick) immediately rebuilds the preview
  on the sketch. Apply commits the current state as a `SketchEditOp`;
  Cancel / Esc cleanly restores the pre-popup sketch.
- **In-sketch radial origin picker.** Replaces the coordinate text
  fields. Click *Pick origin in sketch* in the popup, then click in
  the viewport — a yellow dot with `(x, y)` follows the cursor on the
  sketch plane, snapped to the current grid step. Click commits the
  origin and re-previews; Esc bails out of pick mode.

### Fixed

- **Sketch patterns now replicate circles and arcs**, not just points
  and lines. A sketch made of (say) just circles previously patterned
  nothing visible because the apply loop only walked points / lines.
  The whole-sketch fallback (no element selection) now includes every
  primitive type.

### Internal

- Sketch pattern state grows a `before` snapshot + captured involved
  ids; preview frames restore the snapshot then re-apply the
  transform, so previews never accumulate. Commit diffs against the
  same snapshot to build the `SketchEditOp`.

## [0.3.1] — 2026-05-30

Toolbar polish + Linear / Radial Pattern popups + sketch-mode patterns +
Z-up axis labels.

### Added

- **Toolbar tooltips** on every button (built-in tools and plugin
  contributions). Hover for a one-line description. Toggle via
  Settings → General → Interface → Show toolbar tooltips.
- **Linear Pattern popup** (body): pick an axis (X / Y / Z), count, and
  spacing, with a slider that lives-previews each change.
- **Radial Pattern popup** (body): pick an axis, count, total angle, and
  the world-space origin of the rotation axis — interactively by clicking
  a *Pick axis origin in viewport* button which drops a yellow snap-to-
  grid dot on the picker plane (perpendicular to the chosen rotation
  axis). The picker plane is overlaid with a faint grid so the snap
  points are visible.
- **Sketch-mode Linear / Radial patterns.** Same popup style as body
  patterns, without the axis radio (the sketch is on a fixed plane).
  Linear copies along sketch +X by a user-set spacing; Radial rotates
  around a user-supplied sketch-coords (X, Y) origin. Undoable via the
  normal sketch-edit op.
- **Z-up axis convention** in user-facing axis radios (Pattern, Split):
  X = left / right, Y = forward / back, Z = up. The world stays Y-up
  internally; only labels swap.

### Fixed

- **Items-panel visibility, scroll jitter, popup focus** (already in
  0.3.0; called out again here since users reported related cases).
- **Split X / Y / Z axes** now match the Z-up label convention. *(Note:
  the 0.3.0 ad-hoc X↔Z swap is superseded by the proper user-axis →
  world-axis mapping in 0.3.1.)*
- **Plugin button visibility under face selection.** Split, Duplicate,
  Linear Pattern, Radial Pattern no longer appear when a face is
  selected — those are whole-body operations and would just confuse.
  Move / Rotate / Scale / Mirror still appear (they're useful with a
  face picked: they move the parent body).

### Changed

- **Plugin contribution struct** grows an optional `tooltip` string.
  Existing plugins compile unchanged; new plugin buttons opt in by
  passing a 7th brace-init field.
- **PluginContext** gains `requestInteractiveOp(name)` /
  `takeRequestedInteractiveOp()` — a small request channel a plugin
  can use to defer to Application's popup machinery. Currently used
  by PatternPlugin to hand off the Linear / Radial button to the
  interactive popup; future plugins that need viewport + UI plumbing
  can reuse the same hook.

### Internal

- New `Application::userAxisToWorldVec()` / `userAxisToWorldIdx()`
  helpers centralise the Z-up label → world-axis remap.
- `PatternOp::setRadialOrigin()` lets the rotation axis pass through
  an arbitrary world-space point instead of the implicit (0, 0, 0).

## [0.3.0] — 2026-05-29

Geometry-correctness pass on the cylindrical-resize op and the push/pull
direction, plus an Items-panel rework that adds folders and multi-select.
Merges friend's Settings tabs + JSON preferences + Command Palette removal.

### Added

- **Folders in the Items panel.** `+ Folder` button creates an empty folder;
  any body's right-click menu has a **Move to folder ▶** submenu listing
  existing folders, a `New folder…` option that prompts for a name and drops
  the body into the new folder, and (if the body is currently in one)
  a `(root — no folder)` option to lift it back out. Folders own a
  visibility checkbox and a colour swatch, both of which **cascade to every
  body inside**: hiding the folder hides all members; setting the folder
  colour overwrites every member's colour (re-customisable per body
  afterwards). Folder names are renamable via double-click or context menu.
  Project save / load round-trips folders; older project files load
  unchanged with all bodies at root.
- **Multi-select in the Items panel.** Plain click single-selects (and
  becomes the shift-click anchor); **Ctrl-click** toggles a body in/out of
  the selection; **Shift-click** range-selects from the anchor to the
  clicked body across folders + root in display order. Right-clicking a
  body that's part of a multi-selection makes the Move-to-folder submenu
  act on **every selected body at once** ( " — all selected" is shown on
  each menu entry to make this explicit).
- **`--verbose` / `-v` CLI flag** (with `--log <path>` to override the
  default `/tmp/materializr.log` destination). Flips a process-global
  `materializr::isVerbose()` and redirects stderr to the log file (line-
  buffered, so a crash mid-op still flushes recent traces). Useful when
  triaging modeling bugs — `[Resize]`, `[Push/Pull]`, etc. diagnostics
  land in a file that survives the session. Off by default; normal
  launches stay quiet.

### Fixed

- **Cylindrical resize on holes that pierce non-perpendicular caps.**
  Previously the fill ring used the cylindrical face's parametric V range
  as a flat-disc height, which is correct only when the cap is exactly
  perpendicular to the cylinder axis. For tilted-flat caps (common on
  STEP-imported parts) the ring stuck out past the cap on the high-V
  angles and fell short on the low-V angles, leaving a visible "washer"
  ring of old-radius material at the cap. For curved caps (NURBS / BSpline
  exits — e.g. a hole through a filleted block edge) the same thing
  happened, larger. New approach is topology-agnostic: locate the cap
  face adjacent to the cylinder's extremal-V wire edge, then clip an
  axially-padded ring with a half-space built from the cap. For planar
  caps the half-space is built from the cap's extracted `gp_Pln` (works
  for any orientation, including tilted); for non-planar caps we extrude
  an untrimmed face from the cap's underlying surface along the cylinder
  axis into a finite prism and clip with that. Either path validates the
  clipped volume strictly decreases and falls back to the legacy height-
  bounded ring if the clip degenerates. Handles plane / cone / sphere /
  torus / NURBS / BSpline caps uniformly without classifying the surface
  type.
- **Push/Pull direction on chamfer / fillet / cylinder side faces.** The
  arrow + extrusion direction used the UV-midpoint surface normal, which
  for a curved face is the surface tangent perpendicular at that one
  point — sloped for a cone, twisted for a torus. Looked like the
  direction "followed the polygon clicked" instead of a stable axis. Now
  detects `Geom_ConicalSurface` / `Geom_ToroidalSurface` /
  `Geom_CylindricalSurface` / `Geom_SurfaceOfRevolution` and uses the
  surface's natural rotation axis as the push/pull direction, sign-
  corrected so positive distance still pushes outward. Flat faces are
  unchanged. Mirrored in both the arrow display
  (`Application::beginPushPull`) and the executed extrusion
  (`PushPullOp`) so they agree.
- **Items-panel visibility checkbox didn't actually hide.** Toggling the
  per-body eye checkbox set the document flag but didn't mark meshes
  dirty, so the renderer kept drawing the now-hidden body. Same bug on
  the per-sketch checkbox. Both now mark dirty and rebuild on the next
  frame.
- **Items-panel scroll jitter under multi-select.** The "auto-scroll to
  newly-selected row" code re-issued `SetScrollHereY(0.5)` for every
  selected row whose id differed from the last frame's primary selection
  → all selected rows took turns scrolling themselves into view, twitching
  in the user's intended scroll direction. Auto-scroll now only fires
  when the selection contains exactly one body (which is the case where
  it actually matters — viewport-driven picks bringing a row into view).
- **Edit Diameter popup wasn't draggable.** The popup re-anchored its
  position every frame and carried `ImGuiWindowFlags_NoMove`, so any drag
  attempt was undone immediately. Now anchors only on first appearance
  (`ImGuiCond_Appearing`), drops `NoMove`, and gets a title bar so the
  drag affordance is obvious. Confirm / Cancel buttons unchanged.
- **New-folder popup got stuck on screen.**
  `ImGui::SetKeyboardFocusHere()` was called every frame, hijacking
  focus back to the input field continuously and starving the
  Create / Cancel buttons. Now fires only on the first frame the popup
  opens.

### Internal / refactor

- Deleted ~240 lines of dead code in `ResizeCylindricalOp.cpp` left over
  from an abandoned chamfer-detection approach (`detectCap`, `CapInfo`,
  `makeRevolvedFill`). The current cap-following implementation
  supersedes it. Cleaned up ~14 OpenCASCADE includes that were only
  needed by the removed code.
- New `core/Verbose.{h,cpp}` exposes a process-global verbose flag set
  by `main` from the CLI. `ResizeCylindricalOp` and friends now use a
  `MZLOG(...)` macro that no-ops unless the flag is set, so normal
  launches don't spam stderr.

## [0.2.3] — 2026-05-29

A polish release: clearer tool semantics, a proper close-project flow, an
auto-update check, a `--safe-mode` recovery flag, and a fix to the Windows
CI that was rebuilding OpenCASCADE from scratch on every push (~1.6 h per
build → minutes).

### Added

- **`--safe-mode` CLI flag** (aliases: `--safe-graphics`, `--low-graphics`).
  Brings the app up with rendering forced to known-safe defaults (MSAA off,
  mesh quality Low, default lights), autosave off, auto-open-last-project
  off, and update-check off. The safe values are written to the settings
  file, so subsequent normal launches stay recovered. Use it if a saved
  high-quality setting crashes the app at startup or a complex
  auto-opened project hangs a lower-core machine. `--help` / `-h` prints
  the available options. Works identically on Linux AppImage and the
  installed Windows build.
- **File → Close Project.** Prompts to save if there are unsaved changes
  (unless autosave is on, in which case it saves quietly first), then
  clears the document, history, selection, and project path to leave you
  at an empty viewport.
- **Settings → Open last project on launch.** When on, Materializr
  reopens whichever project you had open the last time you quit. Using
  File → Close Project before quitting clears the stored path, so the
  next launch starts empty.
- **Settings → Check for updates on launch.** Default on. Hits the GitHub
  releases API at startup and pops a small "newer release available"
  dialog with a one-click link to the release page when applicable.
  Off in `--safe-mode`; can be turned off in Settings for offline /
  portable use (the Help → Check for Updates manual path still works).
- **"Extrude From"** appears in **Face Operations** as well as the
  sketch-region tools. On a sketch it extrudes the region into a new body
  (unchanged); on a body face it extrudes that face's silhouette along
  its normal into a new body. Push/Pull remains the in-place
  modify-the-body tool; Extrude always creates a separate body.
- **Troubleshooting docs** in `docs/usage.md` covering rendering-induced
  crashes, with `--safe-mode` as the easy recovery path and the
  hand-edit / file-delete fallback for everything else.

### Changed

- **Boolean Ops (Union / Subtract / Intersect)** now only appear in the
  toolbar when at least two bodies are selected. With a single body
  picked they can't do anything, so showing them was just noise.
- **Push / Pull vs. Extrude semantics tightened.** Push/Pull modifies the
  body that owns the picked face / region. Extrude always creates a
  separate body. The Extrude plugin's redundant face-extrude button +
  "Extrude Face" palette command are gone — Push/Pull is the polished
  in-place tool, and "Extrude From" handles the new-body case.
- **"Extrude Sketch" button label → "Extrude From"** since the same
  button now also extrudes from a face.
- **Settings → Apply also closes the dialog** (previously it just
  persisted and kept the dialog open).

### Fixed

- **Windows CI was rebuilding OpenCASCADE from scratch on every push**
  (~1.6 h per build) because the `x-gha` vcpkg binary-cache backend was
  silently removed upstream and the workflow's `clear;x-gha,readwrite`
  was a no-op. Switched to a NuGet-on-GitHub-Packages binary cache. The
  first build after this fix still pays the cold OCCT compile but writes
  the binaries to the repo's NuGet feed; subsequent builds restore them
  in a few minutes. Permissions for the workflow now include
  `packages: write` so the cache writes are allowed.

### Removed

- **Offset Face** is gone. Its implementation was the same OCCT call
  (`BRepOffsetAPI_MakeThickSolid::MakeThickSolidByJoin`) as Shell, with
  the picked face in the "face to remove" list — meaning it hollowed the
  body rather than offsetting a single face. Even if it were
  reimplemented to do what its name implied (move one face along its
  normal), that's exactly Push/Pull's job. Op file, plugin file,
  force-link entry, and CMakeLists entries all removed.
- **Toolbar `Extrude` button on `HasFaces` context + "Extrude Face"
  palette command** removed; Push/Pull covers the modify-in-place case
  and "Extrude From" covers the make-a-new-body case.

## [0.2.2] — 2026-05-29

Incremental polish on top of 0.2.1 plus the first batch of contributions
from R4stl1n landing back upstream.

### Added

- **Interactive Shell tool with thickness popup.** Picks up the existing
  `ShellOp` (hollow a body, remove the picked face) and wraps it in the
  same popup-with-live-preview pattern as push/pull and edit-diameter.
  Defaults to 1.0 mm with a slider that scrubs 0.1–20 mm; Enter / Apply
  commits, Esc reverts. Replaces the prior one-shot "always 1 mm" plugin
  button.
- **Configurable rendering settings** (from R4stl1n): the Settings panel
  grew a Rendering section with ambient-light slider, headlight (light
  follows camera) toggle, fill-light toggle, multisample anti-aliasing
  selector (Off / 2x / 4x / 8x), and a mesh-quality / OCCT-deflection
  control. Tames the harsh single-direction shadows and lets you trade
  detail for performance on heavier models.
- **Subtract sketch tool** (from R4stl1n): an extrude-cut with a red
  preview tied to the source body the sketch was drawn on. Lives on the
  sketch region toolbar so a region click can drive it directly without
  having to enter the body's face flow.
- **"Buy us a Coffee" button** in the About dialog. Proceeds are split
  between stevebushwa and R4stl1n — stevebushwa just runs the page.
  Coloured in the BMC brand yellow so it reads as a separate "support"
  action rather than another navigation button.

### Changed

- **Application.cpp slimmed by ~31 %** (2,698 → 1,851 lines). All
  interactive-op state machines (Fillet/Chamfer + Edit, Edit Diameter,
  Shell, Extrude, Push/Pull) moved into a new
  `Application_InteractiveOps.cpp`, mirroring the existing
  `Application_Viewport.cpp` / `Application_Dialogs.cpp` split. Pure
  mechanical relocation — no behaviour changes.
- **Slimmed Fillet and Chamfer plugin files** (from R4stl1n): the
  duplicate interactive paths that were shadowed by the inline app-level
  code got removed. `FilletPlugin.cpp` 165 → 56 lines,
  `ChamferPlugin.cpp` 169 → 56 lines.
- `Toolbar::renderGeneralSection` removed (dead method, defined but
  never called; its body was an intentional no-op).

## [0.2.1] — 2026-05-29

CI hotfix: the v0.2.0 Linux Build workflow couldn't execute
`scripts/build-appimage.sh` because the file lacked its executable bit in
the repo (locally I'd been invoking it through `bash`, which masked it).
Nothing else changes — this is the same code as 0.2.0 with a chmod
correction so the published AppImages actually build.

## [0.2.0] — 2026-05-29

Two new editability features on top of the 0.1.1 polish: previously-applied
fillets and chamfers are re-editable by clicking their faces, and bodies that
are essentially solid cylinders or tubes can be resized by diameter on the
cylindrical face itself. A failed attempt at fixing the sketch rotate popup
ended up making the popup disappear entirely — the underlying 15° drag-snap
still works, the type-in path is gone until the next release.

### Added

- **Edit Fillet / Edit Chamfer by clicking the face.** When the selected face
  was generated by a `FilletOp` or `ChamferOp` in the current session's
  history, a new button appears in the Face Operations toolbar. Clicking it
  re-opens the existing radius/distance editor with the value pre-filled,
  drag handle and live preview reused from the create flow, Apply re-runs
  via `History::editStep` so any downstream ops re-execute, Esc cancels.
- **Edit Diameter on any closed cylindrical face or its circular edge.**
  Pick the inner face of a hole, the outer face of a tube or solid cylinder,
  a cylindrical boss, or just one of the circular edges that bound such a
  face. Typing a new diameter live-rebuilds the body via a ring (frustum)
  boolean: the changed annular volume is fused into the body when adding
  material, cut from it when removing. Face pick edits both end edges
  together → stays a cylinder. Single-edge pick edits just that end → makes
  a cone / funnel from a previously straight feature. The resulting top and
  bottom caps are post-processed with `ShapeUpgrade_UnifySameDomain` so the
  ring's planar caps merge cleanly with the body's existing caps instead of
  leaving hairline seams. Confirm pushes a `ResizeCylindricalOp` to history;
  Esc reverts.

### Fixed

- **Sketch rotate-angle popup hover-flicker** — ported from a nested
  Begin/End floating window to a real `OpenPopup` / `BeginPopup` at top
  scope, which kills the flicker but at the cost of the popup not showing.
  See known issues.

### Known issues

- **Sketch rotate-angle popup doesn't appear after the rotate drag.** The
  15° drag snap commits as soon as you release the mouse — there's no
  type-in refinement. Likely caused by the popup's parent viewport's
  `WindowPadding=0` / `NoTitleBar` style state interacting with ImGui's
  popup focus rules. Queued to be reworked in 0.3.0 as a separate floating
  window outside the viewport scope.
- **Fillets / Chamfers / Diameter edits from reopened projects aren't
  editable.** The current save format throws away each op's parameters
  (radius, edges, etc.) and reloads them as generic snapshot replays, so
  the face-edit hooks return "no owning op." Edits work within the session
  they were created; reopening a project resets the editable parameter
  surface. Queued to be lifted in 0.3.0 by extending the save format to
  round-trip `FilletOp` / `ChamferOp` / `ResizeCylindricalOp` parameters
  alongside their result shapes.
- The carryover from 0.1.1 — push/pull ARM artifacting and ortho-view
  entry rotation — are still deferred.

## [0.1.1] — 2026-05-28

A "polish + missing-feature" release. Most work is in interaction: gizmos for
operations that were previously click-twice or hidden, live measurement readouts
during drags, and a measure tool. A handful of geometry and persistence bugs
were tracked down along the way, including one data-loss case in undo replay.

### Added

- **Measure tool** (Object / Edge / Line modes) reachable from the no-selection
  and sketch toolbars. Object measures the combined bbox of every selected body
  (clicking a face counts as picking its body); Edge sums the lengths of every
  selected edge; Line is a two-click point-to-point distance drawn as a purple
  line in the viewport that lives in 3D — orbiting moves it correctly.
- **Sketch Move/Rotate gizmo** rendered on the selection centroid in Select
  mode. Axis arrows (red X, green Y in the sketch plane) and a centre dot for
  free move snap to the active grid step; the rotate ring snaps to 15° while
  dragging. After a rotate drag, a popup lets you type an exact angle. The
  standalone sketch "Rotate" toolbar button has been retired in favour of the
  gizmo ring.
- **Sketch box-select** — click and drag from empty space in Select mode draws
  a rectangle and selects every sketch element whose screen-space projection
  lands inside. Ctrl preserves existing selection.
- **Double-click a sketch line** to select its entire connected chain (lines
  sharing endpoints, transitively). Double-clicking empty space still selects
  every element.
- **Push/Pull face arrow** — selecting a face and choosing Push/Pull now draws
  an arrow along the face normal, with a popup to type the distance and a live
  mm readout while dragging the arrow.
- **Fillet / Chamfer drag handle** on the picked edge — drag perpendicular to
  the edge to set the radius / distance, with a live measurement, alongside the
  existing value popup + slider.
- **Multi-body rotate panel** — three axis sliders + text entry + Apply/Reset/
  Close for rotating large multi-body selections. Works around the live-rotate
  gizmo being too slow on heavy selections without a renderer refactor.
- **Context-aware Ctrl+A** — nothing selected → select every body; an edge
  selected → every edge on that body; in sketch → the whole sketch.
- **3DS Max-style ViewCube** with a rotation ring around it (replaces the older
  navigation buttons).
- **Trackpad navigation mode** in Settings for laptop use.
- **Click-and-drag multi-object box select** in 3D, with Ctrl to extend.
- **Adaptive view clipping** — near/far planes scale with orbit distance so
  distant geometry and the grid no longer vanish at far zoom.
- **Sketch-face grid** now matches the host face's bounds and centroid, instead
  of always rendering a fixed 100 × 100 mm patch at the origin.
- **Grid emphasis** — every 100 lines gets a heavier weight in addition to the
  10-line emphasis.

### Changed

- **STEP import/export** now applies a Z-up → Y-up rotation around X, matching
  Materializr's world convention; export inverts the rotation so a STEP round
  trip is a no-op.
- **Move gizmo on a multi-body selection** moves every selected body, not just
  the one the gizmo is anchored to. The drag caches its pivot at start so the
  motion doesn't jolt as bodies translate beneath the renderer.
- **Sketch transforms (Copy / Mirror / Rotate)** can be invoked with nothing
  selected and will operate on the whole sketch.
- **Sketch on face** centres the face in the viewport on entry and restores the
  prior camera on exit; the camera save/restore is preserved across sketch
  enter/leave round trips.
- **Camera "up" preservation** when aligning to a sketch face via projection,
  so face-orientated views don't surprise-flip.
- **Documentation** split out of README into `docs/` (architecture, building,
  features, getting-started, usage) and the Help menu picks them up.

### Fixed

- **Critical: undo replay was deleting unselected bodies** when applied as a
  batched in-session op. `ReplayOp` now distinguishes "project reload" (where
  bodies not in the snapshot should be removed) from "in-session batch" (where
  they must not), via a `fromReload` flag.
- **Grid drifting vertically at far zoom** — the depth bias was a constant in
  NDC, so its world-space size grew with depth. Now it's a fraction of the ray
  length to the near point.
- **Sketch element drag** — clicking and holding a selected point or line
  translates the whole selection by the cursor delta each frame (was per-point
  only).
- **Sketch double-click selects everything** instead of being a no-op.
- **Multi-edge select** — Ctrl-clicking edges now extends the selection
  correctly (`SelectionManager::findEntry` matches by `IsSame()` on the shape).
- **No more jolt** when panning or orbiting during an extrude / push-pull /
  sketch placement — input projections are skipped while the camera is being
  dragged.
- **Copy offset** — duplicated sketch geometry now lands exactly on the
  original (was offset).
- **Far-zoom disappearance** of distant parts (same root cause as the grid
  drift; clip planes now adapt to orbit distance).
- **Sketch face grid** is centred on the face centroid rather than the origin.
- **Ortho view entry** retains the user's view orientation more sensibly
  (still has a known case noted below).

### Known issues

- **Sketch rotate-angle popup glitches on hover** — the small "Rotation (deg)"
  panel that appears after a rotate-gizmo drag flickers and becomes unclickable
  when the cursor is over it. The 15° drag snap works fine; only the type-in
  refinement is affected. Esc reverts and exits. To be fixed by porting the
  popup to true ImGui popup semantics (`OpenPopup` / `BeginPopup`) in a
  follow-up.
- **Push/pull multi-target preview artifacting on ARM** — visible on aarch64
  builds, not reproducible on x86_64. Deferred until a reliable repro path is
  found.
- **Ortho view entry rotates counter-clockwise** relative to expectation in
  some camera states. Exit is clean. Deferred for a separate pass.

## [0.1.0] — Initial release

Initial Materializr release. Parametric 3D CAD on OCCT with sketches, extrude,
push/pull, fillet, chamfer, transforms, plugin system, project save/load,
ViewCube, settings, Linux AppImage and Windows packaging.

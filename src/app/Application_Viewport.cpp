#include "gl_common.h"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "app/Application.h"
#include "app/Window.h"
#include "viewport/Viewport.h"
#include "viewport/Grid.h"
#include "viewport/ShapeRenderer.h"
#include "viewport/SketchRenderer.h"
#include "viewport/ViewCube.h"
#include "viewport/Picker.h"
#include "viewport/Gizmo.h"
#include "viewport/SelectionHighlight.h"
#include "viewport/BoxSelect.h"
#include "viewport/EdgeRenderer.h"
#include "viewport/PlaneRenderer.h"
#include "viewport/BackgroundRenderer.h"
#include "core/Document.h"
#include "core/History.h"
#include "core/SelectionManager.h"
#include "ui/Toolbar.h"
#include "ui/HistoryPanel.h"
#include "ui/ItemsPanel.h"
#include "ui/StatusBar.h"
#include "ui/ThemeManager.h"
#include "ui/PropertiesPanel.h"
#include "ui/AboutDialog.h"
#include "ui/ShortcutsPanel.h"
#include "ui/HelpPanel.h"
#include "ui/MeasureTool.h"
#include "ui/UpdateChecker.h"
#include "modeling/Sketch.h"
#include "modeling/SketchSolver.h"
#include "modeling/SketchTool.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/ReplayOp.h"
#include "modeling/PushPullOp.h"
#include "modeling/TransformOp.h"
#include "modeling/MirrorOp.h"
#include "modeling/FilletOp.h"
#include "modeling/ChamferOp.h"
#include "modeling/DeleteOp.h"
#include "modeling/SketchEditOp.h"
#include "io/StepIO.h"
#include "io/StlExport.h"
#include "io/FileDialogs.h"
#include "io/ProjectIO.h"
#include "io/Settings.h"
#include "core/EventBus.h"
#include "plugin/PluginContext.h"
#include "plugin/PluginRegistry.h"

namespace materializr { namespace force_link { void linkAll(); } }

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <gp_Ax3.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS.hxx>
#include <Geom_Plane.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Mat.hxx>
#include <gp_XYZ.hxx>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Implementations split out of Application.cpp — the giant 3D viewport
// renderer plus its drag-projection helper.
namespace materializr {

// Map a screen drag onto a world direction: project the mouse delta onto the
// screen-space image of `normal` at `origin`. Falls back to vertical drag when
// that direction is nearly perpendicular to the screen (face head-on) — otherwise
// normalizing a near-zero vector yields NaN, which propagates into a NaN prism
// and crashes the boolean kernel.
static float projectDragOntoNormal(const glm::vec3& origin, const glm::vec3& normal,
                                   const glm::vec2& mouseDelta, const glm::mat4& vp) {
    glm::vec4 o = vp * glm::vec4(origin, 1.0f);
    glm::vec4 t = vp * glm::vec4(origin + normal, 1.0f);
    if (o.w <= 1e-5f || t.w <= 1e-5f) return -mouseDelta.y * 0.05f;
    glm::vec2 os(o.x / o.w, o.y / o.w), ts(t.x / t.w, t.y / t.w);
    glm::vec2 sd(ts.x - os.x, -(ts.y - os.y)); // screen +y is down
    float len = glm::length(sd);
    if (len < 1e-4f) return -mouseDelta.y * 0.05f; // head-on: use vertical drag
    return glm::dot(mouseDelta, sd / len) * 0.05f;
}

void Application::renderViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");

    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    int w = static_cast<int>(contentSize.x);
    int h = static_cast<int>(contentSize.y);

    if (w > 0 && h > 0) {
        m_viewport->resize(w, h);

        if (m_meshesDirty) {
            rebuildMeshes();
            m_meshesDirty = false;
        }

        m_viewport->bind();

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        m_backgroundRenderer->render();
        glEnable(GL_DEPTH_TEST);

        Camera& cam = m_viewport->getCamera();
        glm::mat4 view = cam.getViewMatrix();
        glm::mat4 proj = cam.getProjectionMatrix();

        // Grid: in any sketch mode (whether on a face or from scratch), lay the
        // infinite world grid on the sketch plane so it shows face-on and any
        // nearby face can be referenced. Outside sketch mode, use the XZ ground.
        {
            Grid::Plane gp; // defaults to the XZ ground
            bool sketching = m_inSketchMode && m_activeSketch;
            if (sketching) {
                const gp_Ax3& ax = m_activeSketch->getPlane().Position();
                auto v3 = [](const gp_Dir& d){ return glm::vec3(d.X(), d.Y(), d.Z()); };
                // Use the world-aligned anchor (computed at sketch entry) as
                // the grid origin so grid lines pass through whole world-grid
                // intersections on the sketch plane instead of being shifted
                // by the face's off-grid centre.
                gp.origin = m_sketchSnappedAnchor;
                gp.u = v3(ax.XDirection());
                gp.v = v3(ax.YDirection());
                gp.normal = v3(ax.Direction());
            }
            // Fade radius sized to the view so the grid fills it without a hard edge.
            float fadeDist = cam.isOrthographic()
                ? cam.getOrthoSize() * 8.0f
                : glm::length(cam.getPosition() - cam.getTarget()) * 8.0f;
            m_grid->render(view, proj, cam.getTarget(), std::max(fadeDist, 10.0f),
                           gp, std::max(m_sketchGridStep, 0.01f));
        }
        m_planeRenderer->render(view, proj);
        m_shapeRenderer->render(view, proj, cam.getPosition());
        m_edgeRenderer->render(view, proj);

        // Render selection highlight (face/edge/body)
        m_selectionHighlight->render(*m_selection, *m_document, view, proj);

        // Update gizmo visibility and position based on selection.
        // Suppressed by navigationOnly so a panel pick highlights the body
        // without dropping a move/rotate/scale widget on top of it; the user
        // gets the gizmo back by either explicitly clicking Move/Rotate/Scale
        // (which clears the flag) or by picking again in the viewport.
        if (m_selection->hasSelectedBodies() && !m_selection->navigationOnly() &&
            !m_inSketchMode && !m_extruding && !m_edgeOpActive) {
            const auto& sel = m_selection->getSelection();
            int bodyId = sel[0].bodyId;
            try {
                const TopoDS_Shape& shape = m_document->getBody(bodyId);
                Bnd_Box bbox;
                BRepBndLib::Add(shape, bbox);
                double xmin, ymin, zmin, xmax, ymax, zmax;
                bbox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
                glm::vec3 center((xmin+xmax)*0.5f, (ymin+ymax)*0.5f, (zmin+zmax)*0.5f);
                m_gizmo->setPosition(center);
                m_gizmo->setVisible(true);
            } catch (...) {
                m_gizmo->setVisible(false);
            }
        } else {
            m_gizmo->setVisible(false);
        }

        if (m_gizmo->isVisible()) {
            m_gizmo->render(view, proj);
        }

        // Render all stored sketches (visible only) plus the active sketch
        for (int sid : m_document->getAllSketchIds()) {
            if (!m_document->isSketchVisible(sid)) continue;
            if (m_inSketchMode && sid == m_activeSketchId) continue; // drawn below with tool
            auto sk = m_document->getSketch(sid);
            if (sk) {
                m_sketchRenderer->render(sk.get(), nullptr, view, proj, nullptr);
            }
        }
        if (m_inSketchMode && m_activeSketch) {
            // Keep the tool's snap step in sync with the user-chosen grid. The
            // grid itself is the infinite world grid above (now aligned to the
            // sketch plane), so face sketches no longer need a separate per-face
            // grid — drawing across to neighbouring faces just works.
            m_sketchTool->setGridStep(m_sketchGridStep);
            m_sketchRenderer->render(m_activeSketch.get(), m_sketchTool.get(), view, proj,
                                     m_sketchSolver.get());
        }

        // Highlight hovered/selected sketch regions
        auto highlightRegion = [&](int sketchId, int regionIdx, const glm::vec3& color, float w) {
            if (sketchId < 0 || regionIdx < 0) return;
            std::shared_ptr<Sketch> sk;
            if (sketchId == m_activeSketchId && m_activeSketch) sk = m_activeSketch;
            else sk = m_document->getSketch(sketchId);
            if (!sk) return;
            m_sketchRenderer->renderRegionBoundary(sk.get(), regionIdx, color, w, view, proj);
        };
        // Selected regions in solid yellow
        for (const auto& e : m_selection->getSelection()) {
            if (e.type == SelectionType::SketchRegion) {
                highlightRegion(e.sketchId, e.subShapeIndex,
                                glm::vec3(1.0f, 0.85f, 0.1f), 4.0f);
            }
        }
        // Hovered region in cyan (drawn last so it's on top)
        highlightRegion(m_hoveredSketchId, m_hoveredRegionIndex,
                        glm::vec3(0.2f, 0.9f, 1.0f), 3.0f);

        // Box-select rectangle (screen-space, drawn last so it's on top).
        if (m_boxSelect && m_boxSelect->isActive()) {
            m_boxSelect->render(contentSize.x, contentSize.y);
        }

        m_viewport->unbind();

        ImGui::Image(
            static_cast<ImTextureID>(m_viewport->getTextureID()),
            contentSize,
            ImVec2(0, 1),
            ImVec2(1, 0)
        );

        // --- Live dimension overlay: a measuring annotation (offset line with
        // arrowheads + value) shown while drawing a sketch line/circle, extruding,
        // or moving a body. Drawn in screen space over the viewport image. ---
        {
            ImVec2 imgMin = ImGui::GetItemRectMin();
            ImVec2 imgSize = ImGui::GetItemRectSize();
            glm::mat4 vpMat = proj * view;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            auto toImg = [&](glm::vec3 w, ImVec2& out) -> bool {
                glm::vec4 c = vpMat * glm::vec4(w, 1.0f);
                if (c.w <= 1e-5f) return false; // behind camera
                out = ImVec2(imgMin.x + (c.x / c.w * 0.5f + 0.5f) * imgSize.x,
                             imgMin.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * imgSize.y);
                return true;
            };
            auto drawDim = [&](glm::vec3 aW, glm::vec3 bW, const char* label) {
                ImVec2 sa, sb;
                if (!toImg(aW, sa) || !toImg(bW, sb)) return;
                ImVec2 dir(sb.x - sa.x, sb.y - sa.y);
                float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (len < 2.0f) return;
                dir.x /= len; dir.y /= len;
                ImVec2 perp(-dir.y, dir.x);
                const float off = 26.0f, ah = 7.0f;
                ImVec2 da(sa.x + perp.x * off, sa.y + perp.y * off);
                ImVec2 db(sb.x + perp.x * off, sb.y + perp.y * off);
                ImU32 col = IM_COL32(235, 235, 240, 255);
                ImU32 ext = IM_COL32(170, 170, 180, 150);
                dl->AddLine(sa, da, ext, 1.0f);                 // extension lines
                dl->AddLine(sb, db, ext, 1.0f);
                dl->AddLine(da, db, col, 1.5f);                 // dimension line
                auto arrow = [&](ImVec2 tip, ImVec2 along) {
                    ImVec2 base(tip.x - along.x * ah, tip.y - along.y * ah);
                    dl->AddTriangleFilled(tip,
                        ImVec2(base.x + perp.x * ah * 0.5f, base.y + perp.y * ah * 0.5f),
                        ImVec2(base.x - perp.x * ah * 0.5f, base.y - perp.y * ah * 0.5f), col);
                };
                arrow(da, ImVec2(-dir.x, -dir.y));
                arrow(db, dir);
                ImVec2 ts = ImGui::CalcTextSize(label);
                ImVec2 mid((da.x + db.x) * 0.5f + perp.x * 12.0f,
                           (da.y + db.y) * 0.5f + perp.y * 12.0f);
                ImVec2 tp(mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f);
                dl->AddRectFilled(ImVec2(tp.x - 3, tp.y - 2), ImVec2(tp.x + ts.x + 3, tp.y + ts.y + 2),
                                  IM_COL32(20, 20, 28, 205), 3.0f);
                dl->AddText(tp, col, label);
            };

            char dbuf[40];
            if (m_extruding) {
                std::snprintf(dbuf, sizeof(dbuf), "%.1f mm", std::abs(m_extrudeDistance));
                drawDim(m_extrudeOrigin,
                        m_extrudeOrigin + m_extrudeNormal * m_extrudeDistance, dbuf);
            } else if (m_pushPullActive && m_pushPullHasArrow) {
                // Arrow out of the face + signed-distance measurement.
                std::snprintf(dbuf, sizeof(dbuf), "%.1f mm", m_pushPullDistance);
                drawDim(m_pushPullOrigin,
                        m_pushPullOrigin + m_pushPullNormal * m_pushPullDistance, dbuf);
            } else if (m_edgeOpActive && m_edgeOpHasHandle) {
                // Arrow straight out of the edge (outward, perpendicular) + measurement.
                std::snprintf(dbuf, sizeof(dbuf), "%.1f mm", m_edgeOpValue);
                drawDim(m_edgeOpMid, m_edgeOpMid + m_edgeOpOutDir * m_edgeOpValue, dbuf);
            } else if (m_gizmoDragging && glm::length(m_gizmoTotalDelta) > 1e-3f) {
                // Translate drag: original body centre -> current centre.
                try {
                    Bnd_Box ob, cb;
                    BRepBndLib::Add(m_gizmoDragOriginalShape, ob);
                    BRepBndLib::Add(m_document->getBody(m_gizmoDragBodyId), cb);
                    if (!ob.IsVoid() && !cb.IsVoid()) {
                        double ox1, oy1, oz1, ox2, oy2, oz2, cx1, cy1, cz1, cx2, cy2, cz2;
                        ob.Get(ox1, oy1, oz1, ox2, oy2, oz2);
                        cb.Get(cx1, cy1, cz1, cx2, cy2, cz2);
                        glm::vec3 oc((ox1 + ox2) * 0.5, (oy1 + oy2) * 0.5, (oz1 + oz2) * 0.5);
                        glm::vec3 cc((cx1 + cx2) * 0.5, (cy1 + cy2) * 0.5, (cz1 + cz2) * 0.5);
                        float dist = glm::length(cc - oc);
                        if (dist > 1e-3f) {
                            std::snprintf(dbuf, sizeof(dbuf), "%.1f mm", dist);
                            drawDim(oc, cc, dbuf);
                        }
                    }
                } catch (...) {}
            }

            // Rotate (°) / Scale (%) readout near the body during a gizmo drag —
            // the analogue of the mm readout for moves.
            if (m_gizmoDragging && (m_gizmo->getMode() == GizmoMode::Rotate ||
                                    m_gizmo->getMode() == GizmoMode::Scale)) {
                try {
                    Bnd_Box gb; BRepBndLib::Add(m_document->getBody(m_gizmoDragBodyId), gb);
                    if (!gb.IsVoid()) {
                        double bx1,by1,bz1,bx2,by2,bz2; gb.Get(bx1,by1,bz1,bx2,by2,bz2);
                        glm::vec3 bc((bx1+bx2)*0.5,(by1+by2)*0.5,(bz1+bz2)*0.5);
                        ImVec2 sp;
                        if (toImg(bc, sp)) {
                            char rb[48];
                            if (m_gizmo->getMode() == GizmoMode::Rotate) {
                                // Show the ACTUAL applied angle (after soft 45° snap),
                                // not the raw mouse angle, so the readout matches the body.
                                float n = std::round(m_gizmoTotalAngle / 45.0f) * 45.0f;
                                float shown = (std::abs(m_gizmoTotalAngle - n) < 7.0f) ? n : m_gizmoTotalAngle;
                                std::snprintf(rb, sizeof(rb), "%.0f deg", shown);
                            } else
                                std::snprintf(rb, sizeof(rb), "X %.0f%%  Y %.0f%%  Z %.0f%%",
                                              m_gizmoTotalScale.x*100, m_gizmoTotalScale.y*100,
                                              m_gizmoTotalScale.z*100);
                            ImVec2 ts = ImGui::CalcTextSize(rb);
                            ImVec2 tp(sp.x - ts.x*0.5f, sp.y - ts.y - 14.0f);
                            dl->AddRectFilled(ImVec2(tp.x-4, tp.y-2), ImVec2(tp.x+ts.x+4, tp.y+ts.y+2),
                                              IM_COL32(20,20,28,205), 3.0f);
                            dl->AddText(tp, IM_COL32(235,235,240,255), rb);
                        }
                    }
                } catch (...) {}
            }

            // Sketch preview dimensions: line length, circle diameter (across the
            // full width — makers dimension by diameter), rectangle both sides.
            if (m_inSketchMode && m_activeSketch && m_sketchTool && m_sketchTool->hasPreview()) {
                const gp_Ax3& ax = m_activeSketch->getPlane().Position();
                glm::vec3 O(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
                glm::vec3 X(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
                glm::vec3 Y(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());
                auto sketch2world = [&](glm::vec2 p) { return O + p.x * X + p.y * Y; };

                SketchToolMode pm = m_sketchTool->getPreviewType();
                glm::vec2 ps = m_sketchTool->getPreviewStart();
                glm::vec2 pe = m_sketchTool->getPreviewEnd();

                if (pm == SketchToolMode::Line) {
                    float length = glm::length(pe - ps);
                    if (length > 1e-3f) {
                        std::snprintf(dbuf, sizeof(dbuf), "%.1f mm", length);
                        drawDim(sketch2world(ps), sketch2world(pe), dbuf);
                    }
                } else if (pm == SketchToolMode::Circle) {
                    // ps = centre, pe = a point on the rim. Span the full diameter.
                    glm::vec2 rvec = pe - ps;
                    float dia = 2.0f * glm::length(rvec);
                    if (dia > 1e-3f) {
                        std::snprintf(dbuf, sizeof(dbuf), "%.1f mm dia", dia);
                        drawDim(sketch2world(ps - rvec), sketch2world(pe), dbuf);
                    }
                } else if (pm == SketchToolMode::Rectangle) {
                    // ps, pe = opposite corners. Dimension the bottom and right sides.
                    glm::vec2 bl(ps.x, ps.y), br(pe.x, ps.y), tr(pe.x, pe.y);
                    float w = std::abs(pe.x - ps.x), h = std::abs(pe.y - ps.y);
                    if (w > 1e-3f) {
                        std::snprintf(dbuf, sizeof(dbuf), "%.1f mm", w);
                        drawDim(sketch2world(bl), sketch2world(br), dbuf);
                    }
                    if (h > 1e-3f) {
                        std::snprintf(dbuf, sizeof(dbuf), "%.1f mm", h);
                        drawDim(sketch2world(br), sketch2world(tr), dbuf);
                    }
                }
            }

            // Measure tool — Line mode: render the two captured points and the
            // segment between them in screen space, in a colour deliberately
            // unlike anything else (body edges = white/cyan, sketches = blue,
            // selection highlight = yellow). Magenta-purple here. The first
            // captured point also shows on its own (before the second click)
            // so the user gets feedback that the click landed.
            if (m_measureTool && m_measureTool->getMode() == MeasureMode::Line) {
                const ImU32 measureCol  = IM_COL32(210, 90, 240, 255);
                const ImU32 measureGlow = IM_COL32(210, 90, 240, 90);

                int captured = m_measureTool->getCapturedPointCount();
                ImVec2 sp1, sp2;
                bool ok1 = (captured >= 1) &&
                           toImg(m_measureTool->getCapturedPoint(0), sp1);
                bool ok2 = (captured >= 2) &&
                           toImg(m_measureTool->getCapturedPoint(1), sp2);

                if (ok1) {
                    dl->AddCircleFilled(sp1, 8.0f, measureGlow);
                    dl->AddCircleFilled(sp1, 4.5f, measureCol);
                }
                if (ok2) {
                    dl->AddCircleFilled(sp2, 8.0f, measureGlow);
                    dl->AddCircleFilled(sp2, 4.5f, measureCol);
                }
                if (ok1 && ok2) {
                    dl->AddLine(sp1, sp2, measureCol, 2.5f);
                    // Centred distance label on the midpoint of the segment.
                    const auto& results = m_measureTool->getResults();
                    if (!results.empty()) {
                        char lbl[40];
                        std::snprintf(lbl, sizeof(lbl), "%.2f mm", results[0].value);
                        ImVec2 ts = ImGui::CalcTextSize(lbl);
                        ImVec2 mid((sp1.x + sp2.x) * 0.5f - ts.x * 0.5f,
                                   (sp1.y + sp2.y) * 0.5f - ts.y - 6.0f);
                        dl->AddRectFilled(ImVec2(mid.x - 4, mid.y - 2),
                                          ImVec2(mid.x + ts.x + 4, mid.y + ts.y + 2),
                                          IM_COL32(30, 18, 38, 220), 3.0f);
                        dl->AddText(mid, measureCol, lbl);
                    }
                }
            }
        }

        if (ImGui::IsItemHovered()) {
            ImGuiIO& io = ImGui::GetIO();
            if (io.MouseWheel != 0.0f) cam.zoom(io.MouseWheel);
            // Camera drag uses the configurable bindings (File > Settings). The
            // orbit button pans instead when Shift is held; a distinct pan button
            // always pans.
            //
            // In TRACKPAD mode both orbit and pan are bound to the LEFT button —
            // the same button the gizmo and picker use. Without suppression, a
            // gizmo-handle drag would also yank the camera, so the gizmo and
            // picker block the camera-drag while they own the interaction.
            bool gizmoOwnsDrag = m_gizmoDragging;
            if (!gizmoOwnsDrag && ImGui::IsMouseDragging(m_orbitButton)) {
                ImVec2 delta = io.MouseDelta;
                if (io.KeyShift) cam.pan(delta.x, delta.y);
                else cam.orbit(delta.x, delta.y);
            }
            if (!gizmoOwnsDrag && m_panButton != m_orbitButton &&
                ImGui::IsMouseDragging(m_panButton)) {
                cam.pan(io.MouseDelta.x, io.MouseDelta.y);
            }

            // Pause interactive operations while a camera button is also being
            // dragged — otherwise the changing view matrix re-projects the same
            // mouse motion onto a moving target each frame and the value jolts.
            bool camDragging = !gizmoOwnsDrag &&
                (ImGui::IsMouseDragging(m_orbitButton) ||
                 (m_panButton != m_orbitButton && ImGui::IsMouseDragging(m_panButton)));

            // Pattern axis-origin picker. While the user is picking the
            // axis origin in the radial-pattern popup, we project the mouse
            // ray onto the world plane PERPENDICULAR to the chosen rotation
            // axis (so e.g. Z-axis rotation picks on the floor XY plane).
            // Coordinates snap to the current sketch grid step; the chosen
            // axis-coordinate is forced to 0 so the axis line passes through
            // the world origin in that dimension. A yellow dot follows the
            // cursor; left-click commits the origin and exits pick mode.
            if (m_patternActive && m_patternPickingOrigin && !camDragging) {
                ImVec2 mp = ImGui::GetMousePos();
                ImVec2 wp = ImGui::GetItemRectMin();
                glm::mat4 invVP = glm::inverse(proj * view);
                float nx = ((mp.x - wp.x) / contentSize.x) * 2.0f - 1.0f;
                float ny = 1.0f - ((mp.y - wp.y) / contentSize.y) * 2.0f;
                glm::vec4 np = invVP * glm::vec4(nx, ny, -1.0f, 1.0f);
                glm::vec4 fp = invVP * glm::vec4(nx, ny,  1.0f, 1.0f);
                glm::vec3 ro(np / np.w);
                glm::vec3 rd = glm::normalize(glm::vec3(fp / fp.w) - ro);
                // Picker plane = perpendicular to the rotation axis. Use the
                // user-axis → world mapping so "Z" picks on the floor (XZ in
                // Y-up world) which is what the user expects.
                glm::vec3 planeN = userAxisToWorldVec(m_patternAxisIdx);
                int worldIdx = userAxisToWorldIdx(m_patternAxisIdx);
                float denom = glm::dot(rd, planeN);
                if (std::abs(denom) > 1e-6f) {
                    float t = -glm::dot(ro, planeN) / denom;
                    if (t > 0.0f) {
                        glm::vec3 hit = ro + rd * t;
                        float step = std::max(m_sketchGridStep, 0.01f);
                        hit.x = std::round(hit.x / step) * step;
                        hit.y = std::round(hit.y / step) * step;
                        hit.z = std::round(hit.z / step) * step;
                        hit[worldIdx] = 0.0f;
                        // Project the snapped 3D point back to screen for the dot.
                        glm::vec4 clip = (proj * view) * glm::vec4(hit, 1.0f);
                        auto* dl = ImGui::GetForegroundDrawList();

                        // Picker plane grid: faint lines at the snap step,
                        // drawn on the same plane the dot snaps to. Lets the
                        // user see where the snap points are. Capped at
                        // 50 lines per direction so a fine snap step doesn't
                        // produce a wall of pixels.
                        auto worldToScreen = [&](const glm::vec3& p, ImVec2& out) -> bool {
                            glm::vec4 c = (proj * view) * glm::vec4(p, 1.0f);
                            if (c.w <= 0.0f) return false;
                            glm::vec3 ndc(c / c.w);
                            out = ImVec2(wp.x + (ndc.x * 0.5f + 0.5f) * contentSize.x,
                                         wp.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * contentSize.y);
                            return true;
                        };
                        // Two in-plane unit vectors. Choose by which world
                        // axis is the plane normal so they're axis-aligned.
                        glm::vec3 axU, axV;
                        if (worldIdx == 0)      { axU = glm::vec3(0,1,0); axV = glm::vec3(0,0,1); }
                        else if (worldIdx == 1) { axU = glm::vec3(1,0,0); axV = glm::vec3(0,0,1); }
                        else                    { axU = glm::vec3(1,0,0); axV = glm::vec3(0,1,0); }
                        int half = std::min(50, static_cast<int>(50.0f / step));
                        float halfRange = half * step;
                        for (int i = -half; i <= half; ++i) {
                            float t = i * step;
                            glm::vec3 p0 = t * axU - halfRange * axV;
                            glm::vec3 p1 = t * axU + halfRange * axV;
                            glm::vec3 q0 = -halfRange * axU + t * axV;
                            glm::vec3 q1 =  halfRange * axU + t * axV;
                            ImVec2 s0, s1, t0, t1;
                            // Axes through origin pop in yellow; the surrounding
                            // grid is medium-grey at clearly-visible alpha.
                            ImU32 col = (i == 0) ? IM_COL32(255, 220, 50, 220)
                                                 : IM_COL32(180, 180, 180, 160);
                            float thick = (i == 0) ? 1.5f : 1.0f;
                            if (worldToScreen(p0, s0) && worldToScreen(p1, s1))
                                dl->AddLine(s0, s1, col, thick);
                            if (worldToScreen(q0, t0) && worldToScreen(q1, t1))
                                dl->AddLine(t0, t1, col, thick);
                        }

                        if (clip.w > 0.0f) {
                            glm::vec3 ndc(clip / clip.w);
                            ImVec2 sp(wp.x + (ndc.x * 0.5f + 0.5f) * contentSize.x,
                                      wp.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * contentSize.y);
                            dl->AddCircleFilled(sp, 7.0f, IM_COL32(255, 220, 50, 255));
                            dl->AddCircle      (sp, 7.0f, IM_COL32(0, 0, 0, 255), 0, 1.5f);
                            // Coord label next to the dot.
                            char buf[64];
                            std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)",
                                          hit.x, hit.y, hit.z);
                            dl->AddText(ImVec2(sp.x + 10.0f, sp.y - 8.0f),
                                        IM_COL32(255, 220, 50, 255), buf);
                        }
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            m_patternOriginX = hit.x;
                            m_patternOriginY = hit.y;
                            m_patternOriginZ = hit.z;
                            m_patternPickingOrigin = false;
                            updatePattern();
                        }
                    }
                }
                // While picking, suppress the rest of the viewport input so
                // the click doesn't also fire a body pick / gizmo drag.
                // We do that by returning from this section after handling.
                // (Caller still does the camera + UI rendering paths.)
            }

            // Interactive extrude drag: left-drag moves distance along normal
            if (m_extruding && !camDragging &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                glm::vec2 md(io.MouseDelta.x, io.MouseDelta.y);
                m_extrudeDistance += projectDragOntoNormal(m_extrudeOrigin, m_extrudeNormal,
                                                           md, proj * view);
                std::snprintf(m_extrudeInputBuf, sizeof(m_extrudeInputBuf), "%.1f", m_extrudeDistance);
                updateInteractiveExtrude();
            }

            // Push/Pull face arrow: left-drag moves the distance along the face normal.
            if (m_pushPullActive && m_pushPullHasArrow && !camDragging &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                glm::vec2 md(io.MouseDelta.x, io.MouseDelta.y);
                m_pushPullDistance += projectDragOntoNormal(m_pushPullOrigin, m_pushPullNormal,
                                                            md, proj * view);
                std::snprintf(m_pushPullInputBuf, sizeof(m_pushPullInputBuf), "%.1f", m_pushPullDistance);
                updatePushPull();
            }

            // Fillet/Chamfer drag handle: left-drag sets the radius/distance to the
            // perpendicular distance from the edge to the cursor (on a plane through
            // the edge midpoint facing the camera).
            if (m_edgeOpActive && m_edgeOpHasHandle && !camDragging &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                ImVec2 mp = ImGui::GetMousePos();
                ImVec2 wp = ImGui::GetItemRectMin();
                glm::mat4 invVP = glm::inverse(proj * view);
                float nx = ((mp.x - wp.x) / contentSize.x) * 2.0f - 1.0f;
                float ny = 1.0f - ((mp.y - wp.y) / contentSize.y) * 2.0f;
                glm::vec4 np = invVP * glm::vec4(nx, ny, -1.0f, 1.0f);
                glm::vec4 fp = invVP * glm::vec4(nx, ny, 1.0f, 1.0f);
                glm::vec3 ro(np / np.w), rd = glm::normalize(glm::vec3(fp / fp.w) - ro);
                glm::vec3 camFwd = glm::normalize(cam.getTarget() - cam.getPosition());
                float denom = glm::dot(rd, camFwd);
                if (std::abs(denom) > 1e-6f) {
                    float t = glm::dot(m_edgeOpMid - ro, camFwd) / denom;
                    glm::vec3 hit = ro + rd * t;
                    // Signed distance along the outward arrow: dragging away from the
                    // edge grows the value (≥0.1 mm); dragging back toward/through the
                    // edge returns to 0 (no change).
                    float proj = glm::dot(hit - m_edgeOpMid, m_edgeOpOutDir);
                    m_edgeOpValue = (proj <= 0.0f) ? 0.0f : std::max(0.1f, proj);
                    std::snprintf(m_edgeOpInputBuf, sizeof(m_edgeOpInputBuf), "%.1f", m_edgeOpValue);
                    updateInteractiveEdgeOp();
                }
            }

            // Gizmo input + Face hover highlighting + picking (suppressed while an
            // interactive op owns the left-drag: extrude, push/pull, fillet/chamfer,
            // or the pattern axis-origin picker).
            if (!m_inSketchMode && !m_extruding && !m_pushPullActive && !m_edgeOpActive &&
                !(m_patternActive && m_patternPickingOrigin)) {
                ImVec2 mousePos = ImGui::GetMousePos();
                ImVec2 winPos = ImGui::GetItemRectMin();
                float localX = mousePos.x - winPos.x;
                float localY = mousePos.y - winPos.y;

                // Gizmo interaction takes priority
                bool gizmoConsumedInput = false;
                if (m_gizmo->isVisible()) {
                    bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
                    bool mouseJustPressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

                    GizmoResult gResult = m_gizmo->handleInput(
                        localX, localY, contentSize.x, contentSize.y,
                        mouseDown, mouseJustPressed, cam);

                    // Helpers shared by drag-apply and commit.
                    auto axisDirOf = [](GizmoAxis a) -> glm::vec3 {
                        if (a == GizmoAxis::X) return glm::vec3(1, 0, 0);
                        if (a == GizmoAxis::Y) return glm::vec3(0, 1, 0);
                        return glm::vec3(0, 0, 1);
                    };
                    auto softSnap45 = [](float deg) {
                        float n = std::round(deg / 45.0f) * 45.0f;
                        return (std::abs(deg - n) < 7.0f) ? n : deg; // free, snaps near 45°
                    };

                    // Start drag: save originals for every selected body (so Move
                    // can apply to all of them) and reset accumulators.
                    if (gResult.activeAxis != GizmoAxis::None && !m_gizmoDragging) {
                        m_gizmoDragOriginals.clear();
                        for (const auto& sel : m_selection->getSelection()) {
                            if (sel.type != SelectionType::Body) continue;
                            try {
                                m_gizmoDragOriginals.push_back(
                                    {sel.bodyId, m_document->getBody(sel.bodyId)});
                            } catch (...) {}
                        }
                        if (!m_gizmoDragOriginals.empty()) {
                            m_gizmoDragBodyId = m_gizmoDragOriginals.front().first;
                            m_gizmoDragOriginalShape = m_gizmoDragOriginals.front().second;
                            m_gizmoDragging = true;
                            m_gizmoTotalDelta = glm::vec3(0.0f);
                            m_gizmoTotalAngle = 0.0f;
                            m_gizmoScaleAccum = glm::vec3(0.0f);
                            m_gizmoTotalScale = glm::vec3(1.0f);
                            // Pre-compute the shared pivot once — the originals
                            // don't change during the drag, so this is constant.
                            m_gizmoSharedPivot = glm::vec3(0.0f);
                            int np = 0;
                            for (auto& [id, orig] : m_gizmoDragOriginals) {
                                try {
                                    Bnd_Box bb; BRepBndLib::Add(orig, bb);
                                    if (bb.IsVoid()) continue;
                                    double x0,y0,z0,x1,y1,z1; bb.Get(x0,y0,z0,x1,y1,z1);
                                    m_gizmoSharedPivot += glm::vec3((x0+x1)*0.5f, (y0+y1)*0.5f, (z0+z1)*0.5f);
                                    ++np;
                                } catch (...) {}
                            }
                            if (np > 0) m_gizmoSharedPivot /= static_cast<float>(np);
                        }
                    }

                    // During drag: accumulate totals and (re)apply to the ORIGINAL
                    // shape each frame, so snapping and per-axis scale stay stable.
                    if (gResult.changed && m_gizmoDragging) {
                        try {
                            Bnd_Box ob; BRepBndLib::Add(m_gizmoDragOriginalShape, ob);
                            double ox1,oy1,oz1,ox2,oy2,oz2; ob.Get(ox1,oy1,oz1,ox2,oy2,oz2);
                            gp_Pnt center((ox1+ox2)/2,(oy1+oy2)/2,(oz1+oz2)/2);

                            TopoDS_Shape result;
                            bool applied = false;

                            if (gResult.mode == GizmoMode::Translate) {
                                m_gizmoTotalDelta += gResult.delta;
                                glm::vec3 d = m_gizmoTotalDelta;
                                if (m_snapToGrid && m_sketchGridStep > 0.0f) {
                                    float step = m_sketchGridStep, thr = step * 0.4f;
                                    auto s1 = [&](float v){ float n=std::round(v/step)*step; return std::abs(v-n)<thr?n:v; };
                                    d.x = s1(d.x); d.y = s1(d.y); d.z = s1(d.z);
                                }
                                gp_Trsf trsf; trsf.SetTranslation(gp_Vec(d.x, d.y, d.z));
                                // Apply the same translation to every selected body,
                                // each from its own original shape. copy=false is
                                // a location-only transform, so the underlying
                                // topology (and its cached triangulation) is shared
                                // — orders of magnitude faster per frame than the
                                // full topology copy.
                                for (auto& [id, orig] : m_gizmoDragOriginals) {
                                    BRepBuilderAPI_Transform xf(orig, trsf, /*copy=*/false);
                                    if (xf.IsDone()) m_document->updateBody(id, xf.Shape());
                                }
                                m_meshesDirty = true;
                                applied = false; // already handled per-body above
                            } else if (gResult.mode == GizmoMode::Rotate) {
                                glm::vec3 ad = axisDirOf(gResult.activeAxis);
                                m_gizmoRotAxis = ad;
                                m_gizmoTotalAngle += glm::dot(gResult.delta, ad);
                                float ang = softSnap45(m_gizmoTotalAngle);
                                // Pivot was captured once at drag start —
                                // reusing it avoids 65 bbox computations per
                                // frame for a large multi-selection.
                                const glm::vec3& pivot = m_gizmoSharedPivot;
                                gp_Trsf trsf;
                                trsf.SetRotation(gp_Ax1(gp_Pnt(pivot.x, pivot.y, pivot.z),
                                                        gp_Dir(ad.x, ad.y, ad.z)),
                                                 ang * M_PI / 180.0);
                                // copy=false: location-only, shared topology, fast.
                                for (auto& [id, orig] : m_gizmoDragOriginals) {
                                    BRepBuilderAPI_Transform xf(orig, trsf, /*copy=*/false);
                                    if (xf.IsDone()) m_document->updateBody(id, xf.Shape());
                                }
                                m_meshesDirty = true;
                                applied = false; // per-body above
                            } else { // Scale — per-axis, non-uniform about the centre
                                float os = static_cast<float>(glm::length(
                                    glm::vec3(ox2-ox1, oy2-oy1, oz2-oz1)));
                                if (os < 0.001f) os = 1.0f;
                                int ai = gResult.activeAxis == GizmoAxis::X ? 0
                                       : gResult.activeAxis == GizmoAxis::Y ? 1 : 2;
                                m_gizmoScaleAccum[ai] += (ai==0?gResult.delta.x:ai==1?gResult.delta.y:gResult.delta.z);
                                if (m_scaleUniform) {
                                    float f = glm::clamp(1.0f + m_gizmoScaleAccum[ai]/os, 0.05f, 20.0f);
                                    f = std::round(f * 100.0f) / 100.0f;
                                    m_gizmoTotalScale = glm::vec3(f);
                                } else {
                                    for (int k = 0; k < 3; ++k) {
                                        float f = glm::clamp(1.0f + m_gizmoScaleAccum[k]/os, 0.05f, 20.0f);
                                        m_gizmoTotalScale[k] = std::round(f * 100.0f) / 100.0f;
                                    }
                                }
                                // Cached pivot — see Rotate branch above.
                                const glm::vec3& pivot = m_gizmoSharedPivot;
                                gp_GTrsf gt;
                                gt.SetVectorialPart(gp_Mat(m_gizmoTotalScale.x,0,0,
                                                           0,m_gizmoTotalScale.y,0,
                                                           0,0,m_gizmoTotalScale.z));
                                gt.SetTranslationPart(gp_XYZ(pivot.x - m_gizmoTotalScale.x * pivot.x,
                                                             pivot.y - m_gizmoTotalScale.y * pivot.y,
                                                             pivot.z - m_gizmoTotalScale.z * pivot.z));
                                for (auto& [id, orig] : m_gizmoDragOriginals) {
                                    BRepBuilderAPI_GTransform xf(orig, gt, true);
                                    if (xf.IsDone()) m_document->updateBody(id, xf.Shape());
                                }
                                m_meshesDirty = true;
                                applied = false; // per-body above
                            }

                            if (applied) {
                                m_document->updateBody(m_gizmoDragBodyId, result);
                                m_meshesDirty = true;
                            }
                        } catch (...) {}
                        gizmoConsumedInput = true;
                    }

                    // End drag: commit the right operation for the gizmo's mode.
                    // Single body -> a TransformOp (parameters stay editable in
                    // the Properties panel). Multi-body -> a single batched
                    // ReplayOp snapshot so the history shows one entry, not one
                    // per body.
                    if (m_gizmoDragging && gResult.activeAxis == GizmoAxis::None && !mouseDown) {
                        try {
                            GizmoMode gm = m_gizmo->getMode();
                            const size_t nBodies = m_gizmoDragOriginals.size();
                            const bool isMulti = nBodies > 1;

                            // Snapshot the post-drag state BEFORE restoring originals,
                            // since the batched ReplayOp needs it as the "after".
                            ReplayOp::BodyState afterState;
                            if (isMulti) {
                                for (auto& [id, orig] : m_gizmoDragOriginals) {
                                    afterState.push_back({id, m_document->getBody(id)});
                                }
                            }
                            // Restore originals so any TransformOp captures the right
                            // previousShape on execute(), and so the ReplayOp's
                            // "before" snapshot is the pre-drag state.
                            for (auto& [id, orig] : m_gizmoDragOriginals) {
                                m_document->updateBody(id, orig);
                            }

                            // Shared pivot captured once at drag start.
                            const glm::vec3& pivot = m_gizmoSharedPivot;

                            glm::vec3 d = m_gizmoTotalDelta;
                            if (gm == GizmoMode::Translate &&
                                m_snapToGrid && m_sketchGridStep > 0.0f) {
                                float step = m_sketchGridStep, thr = step * 0.4f;
                                auto s1 = [&](float v){ float n=std::round(v/step)*step; return std::abs(v-n)<thr?n:v; };
                                d.x = s1(d.x); d.y = s1(d.y); d.z = s1(d.z);
                            }
                            float ang = (gm == GizmoMode::Rotate)
                                            ? softSnap45(m_gizmoTotalAngle) : 0.0f;

                            bool validMove   = glm::length(d) > 1e-4f;
                            bool validRotate = std::abs(ang) > 1e-3f;
                            bool validScale  = glm::length(m_gizmoTotalScale - glm::vec3(1.0f)) > 1e-3f;
                            bool anyValid    = (gm == GizmoMode::Translate && validMove) ||
                                               (gm == GizmoMode::Rotate    && validRotate) ||
                                               (gm == GizmoMode::Scale     && validScale);

                            if (!anyValid) {
                                /* below-threshold drag: leave history alone */
                            } else if (isMulti) {
                                // Batched commit: one ReplayOp covering all bodies.
                                ReplayOp::BodyState beforeState;
                                for (auto& [id, orig] : m_gizmoDragOriginals) {
                                    beforeState.push_back({id, orig});
                                }
                                std::string label;
                                std::string desc;
                                if (gm == GizmoMode::Translate) {
                                    label = "Move (" + std::to_string(nBodies) + " bodies)";
                                    char buf[96];
                                    std::snprintf(buf, sizeof(buf), "Move %d bodies by (%.2f, %.2f, %.2f) mm",
                                                  (int)nBodies, d.x, d.y, d.z);
                                    desc = buf;
                                } else if (gm == GizmoMode::Rotate) {
                                    label = "Rotate (" + std::to_string(nBodies) + " bodies)";
                                    char buf[96];
                                    std::snprintf(buf, sizeof(buf), "Rotate %d bodies by %.1f°",
                                                  (int)nBodies, ang);
                                    desc = buf;
                                } else {
                                    label = "Scale (" + std::to_string(nBodies) + " bodies)";
                                    char buf[128];
                                    std::snprintf(buf, sizeof(buf),
                                                  "Scale %d bodies (%.0f%%, %.0f%%, %.0f%%)",
                                                  (int)nBodies,
                                                  m_gizmoTotalScale.x * 100.0f,
                                                  m_gizmoTotalScale.y * 100.0f,
                                                  m_gizmoTotalScale.z * 100.0f);
                                    desc = buf;
                                }
                                auto op = std::make_unique<ReplayOp>(
                                    "batchtransform", label, desc,
                                    std::move(beforeState), std::move(afterState),
                                    /*fromReload=*/false);
                                // Apply the after-state to the doc and stamp it as
                                // executed — the live drag already left the bodies
                                // in that state visually.
                                op->execute(*m_document);
                                m_history->pushExecuted(std::move(op));
                            } else {
                                // Single body: keep the TransformOp path so the
                                // Properties panel still lets the user edit the
                                // translation/angle/scale after the fact.
                                auto op = std::make_unique<TransformOp>();
                                op->setBodyId(m_gizmoDragBodyId);
                                op->setCenter(pivot.x, pivot.y, pivot.z);
                                if (gm == GizmoMode::Translate) {
                                    op->setType(TransformType::Translate);
                                    op->setTranslation(d.x, d.y, d.z);
                                } else if (gm == GizmoMode::Rotate) {
                                    op->setType(TransformType::Rotate);
                                    op->setRotation(m_gizmoRotAxis.x,
                                                    m_gizmoRotAxis.y,
                                                    m_gizmoRotAxis.z, ang);
                                } else {
                                    op->setType(TransformType::Scale);
                                    op->setScaleXYZ(m_gizmoTotalScale.x,
                                                    m_gizmoTotalScale.y,
                                                    m_gizmoTotalScale.z);
                                }
                                m_history->pushOperation(std::move(op), *m_document);
                            }
                            m_meshesDirty = true;
                        } catch (...) {}

                        m_gizmoDragging = false;
                        m_gizmoDragOriginalShape.Nullify();
                        m_gizmoDragBodyId = -1;
                        m_gizmoDragOriginals.clear();
                    }

                    if (gResult.activeAxis != GizmoAxis::None) {
                        gizmoConsumedInput = true;
                    }
                }

                if (!gizmoConsumedInput && !m_viewCube->wasHovered()) {
                    auto result = m_picker->pick(localX, localY,
                        contentSize.x, contentSize.y, cam, *m_document);

                    m_hoveredBodyId = result.hit ? result.bodyId : -1;

                    // Sketch-region hover (takes priority over body picking when present)
                    SketchRegionHit regionHit = pickSketchRegion(localX, localY,
                        contentSize.x, contentSize.y);
                    // Reject a sketch region that sits behind a body under the cursor —
                    // only what's visible should be selectable. Compare hit distances
                    // from the camera (origin-independent) and drop the region if the
                    // body face is nearer.
                    if (regionHit.regionIndex >= 0 && result.hit) {
                        glm::vec3 camPos = cam.getPosition();
                        float bodyD = glm::length(result.hitPoint - camPos);
                        float sketchD = glm::length(regionHit.worldPoint - camPos);
                        if (bodyD < sketchD - 1e-3f) {
                            regionHit.sketchId = -1;
                            regionHit.regionIndex = -1;
                        }
                    }
                    m_hoveredSketchId = regionHit.sketchId;
                    m_hoveredRegionIndex = regionHit.regionIndex;

                    bool regionConsumedClick = false;
                    if (regionHit.regionIndex >= 0 &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        SelectionEntry entry;
                        entry.type = SelectionType::SketchRegion;
                        entry.sketchId = regionHit.sketchId;
                        entry.subShapeIndex = regionHit.regionIndex;
                        if (io.KeyCtrl) {
                            m_selection->addToSelection(entry);
                        } else {
                            m_selection->select(entry);
                        }
                        regionConsumedClick = true;
                    }

                    // Mirror "across a face" mode: the next planar face click
                    // defines the mirror plane (Esc cancels via handleShortcuts).
                    if (m_mirrorPickFace && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        if (result.hit && !result.pickedShape.IsNull() &&
                            result.pickedShape.ShapeType() == TopAbs_FACE && m_mirrorBodyId >= 0) {
                            try {
                                TopoDS_Face face = TopoDS::Face(result.pickedShape);
                                Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
                                if (!surf.IsNull() && surf->IsKind(STANDARD_TYPE(Geom_Plane))) {
                                    gp_Pln pln = Handle(Geom_Plane)::DownCast(surf)->Pln();
                                    const gp_Ax3& ax = pln.Position();
                                    auto op = std::make_unique<MirrorOp>();
                                    op->setBody(m_mirrorBodyId);
                                    op->setPlane(MirrorPlane::Custom);
                                    op->setCustomPlane(gp_Ax2(ax.Location(), ax.Direction()));
                                    op->setKeepOriginal(true);
                                    if (m_history->pushOperation(std::move(op), *m_document))
                                        m_meshesDirty = true;
                                }
                            } catch (...) {}
                        }
                        m_mirrorPickFace = false;
                        regionConsumedClick = true; // don't also change selection
                    }

                    // Measure: point-to-point click capture. When the tool is
                    // in PointToPoint mode, a left-click on something the
                    // picker can hit is captured as a measurement point and
                    // we skip the normal selection logic for that click.
                    bool measureConsumedClick = false;
                    if (m_measureTool &&
                        m_measureTool->getMode() == MeasureMode::Line &&
                        result.hit &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        m_measureTool->capturePoint(result.hitPoint);
                        measureConsumedClick = true;
                        regionConsumedClick = true; // suppress selection paths
                    }
                    (void)measureConsumedClick;

                    // Double-click to select body, single-click to select face
                    if (!regionConsumedClick && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (result.hit) {
                            SelectionEntry entry;
                            entry.type = SelectionType::Body;
                            entry.bodyId = result.bodyId;
                            try { entry.shape = m_document->getBody(result.bodyId); } catch (...) {}
                            if (io.KeyCtrl) {
                                m_selection->addToSelection(entry);
                            } else {
                                m_selection->select(entry);
                            }
                        }
                    } else if (!regionConsumedClick && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        int ownerStep = -1; // fillet/chamfer step to open in the editor
                        if (result.hit) {
                            SelectionEntry entry;
                            // If click is near an edge, select edge; otherwise face. The
                            // 8 px threshold is clamped to ¼ of the face's on-screen size
                            // so a small face (say 20 px wide) doesn't lose every interior
                            // click to one of its own boundary edges (every interior pixel
                            // is within 8 px of an edge when the face is itself only 16 px
                            // across).
                            float edgeThresh = std::min(8.0f, result.faceScreenSize * 0.25f);
                            if (result.edgeScreenDist < edgeThresh && !result.nearestEdge.IsNull()) {
                                entry.type = SelectionType::Edge;
                                entry.bodyId = result.bodyId;
                                entry.shape = result.nearestEdge;
                            } else {
                                entry.type = SelectionType::Face;
                                entry.bodyId = result.bodyId;
                                entry.subShapeIndex = result.faceIndex;
                                entry.shape = result.pickedShape;
                                // Trace a clicked face back to the fillet/chamfer that
                                // produced it, so the user can re-edit it after the fact.
                                if (!entry.shape.IsNull()) {
                                    int upTo = m_history->currentStep();
                                    for (int s = 0; s <= upTo; ++s) {
                                        const Operation* op = m_history->getStep(s);
                                        if (op && op->isEnabled() && op->ownsFace(entry.shape)) {
                                            ownerStep = s;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (io.KeyCtrl) {
                                m_selection->addToSelection(entry);
                            } else {
                                m_selection->select(entry);
                            }
                        } else {
                            // Empty-space click: begin a box-select drag instead of
                            // clearing immediately. The release handler below decides
                            // whether to multi-select bodies (drag had area) or treat
                            // it as a plain click and clear.
                            bool boxEligible = !m_inSketchMode && !m_extruding &&
                                !m_pushPullActive && !m_edgeOpActive && !m_gizmoDragging &&
                                m_orbitButton != ImGuiMouseButton_Left &&
                                m_panButton  != ImGuiMouseButton_Left;
                            if (boxEligible && m_boxSelect) {
                                ImVec2 mp = ImGui::GetMousePos();
                                ImVec2 wp = ImGui::GetItemRectMin();
                                m_boxSelect->begin(glm::vec2(mp.x - wp.x, mp.y - wp.y));
                            } else if (!io.KeyCtrl) {
                                m_selection->clear();
                            }
                        }
                        // Open the owning fillet/chamfer in the History editor, or
                        // close that editor when clicking anything else.
                        m_historyPanel->setEditingStep(ownerStep);
                    }

                    // Box-select drag + release. Update while LEFT is held; on
                    // release, intersect bodies' screen-space bboxes with the
                    // rectangle and add them to selection (Ctrl preserves the
                    // existing selection).
                    if (m_boxSelect && m_boxSelect->isActive()) {
                        ImVec2 mp = ImGui::GetMousePos();
                        ImVec2 wp = ImGui::GetItemRectMin();
                        glm::vec2 curScreen(mp.x - wp.x, mp.y - wp.y);
                        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                            m_boxSelect->update(curScreen);
                        }
                        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                            glm::vec2 mn = m_boxSelect->getMin();
                            glm::vec2 mx = m_boxSelect->getMax();
                            m_boxSelect->end();

                            // Tiny rectangle = treat as a plain click → clear.
                            if (glm::distance(mn, mx) < 4.0f) {
                                if (!io.KeyCtrl) m_selection->clear();
                            } else {
                                if (!io.KeyCtrl) m_selection->clear();
                                glm::mat4 vp = proj * view;
                                for (int id : m_document->getAllBodyIds()) {
                                    if (!m_document->isBodyVisible(id)) continue;
                                    try {
                                        const TopoDS_Shape& shape = m_document->getBody(id);
                                        Bnd_Box bb; BRepBndLib::Add(shape, bb);
                                        if (bb.IsVoid()) continue;
                                        double x0,y0,z0,x1,y1,z1;
                                        bb.Get(x0,y0,z0,x1,y1,z1);
                                        // Project the 8 bbox corners into screen pixels,
                                        // skipping any behind the camera (w<=0).
                                        glm::vec2 bMin( FLT_MAX,  FLT_MAX);
                                        glm::vec2 bMax(-FLT_MAX, -FLT_MAX);
                                        bool any = false;
                                        for (int c = 0; c < 8; ++c) {
                                            glm::vec4 p((c & 1) ? x1 : x0,
                                                        (c & 2) ? y1 : y0,
                                                        (c & 4) ? z1 : z0, 1.0f);
                                            glm::vec4 cp = vp * p;
                                            if (cp.w <= 0.0f) continue;
                                            glm::vec2 ndc(cp.x / cp.w, cp.y / cp.w);
                                            glm::vec2 sp(
                                                (ndc.x * 0.5f + 0.5f) * contentSize.x,
                                                (1.0f - (ndc.y * 0.5f + 0.5f)) * contentSize.y);
                                            bMin = glm::min(bMin, sp);
                                            bMax = glm::max(bMax, sp);
                                            any = true;
                                        }
                                        if (!any) continue;
                                        // AABB overlap test against the box rect.
                                        if (bMax.x >= mn.x && bMin.x <= mx.x &&
                                            bMax.y >= mn.y && bMin.y <= mx.y) {
                                            SelectionEntry e;
                                            e.type = SelectionType::Body;
                                            e.bodyId = id;
                                            e.shape = shape;
                                            m_selection->addToSelection(e);
                                        }
                                    } catch (...) {}
                                }
                            }
                        }
                    }

                    // Right click on a face: context menu (only if not a pan drag)
                    ImVec2 dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
                    bool wasDragging = (std::abs(dragDelta.x) > 1.0f || std::abs(dragDelta.y) > 1.0f);
                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !wasDragging) {
                        if (result.hit && !result.pickedShape.IsNull()) {
                            m_contextMenuBodyId = result.bodyId;
                            m_contextMenuFace = result.pickedShape;
                            m_contextMenuPending = true;
                        }
                    }
                }
            }

            // Sketch mode mouse input — ray-plane intersection. Skipped while
            // the camera is being dragged so the in-progress preview (e.g. the
            // line endpoint following the cursor) doesn't jolt as the view moves.
            // Suppress while the ViewCube widget is being hovered/dragged so its
            // click doesn't pass through and start a line draw underneath.
            if (m_inSketchMode && m_activeSketch && !camDragging &&
                !m_viewCube->wasHovered()) {
                ImVec2 mousePos = ImGui::GetMousePos();
                ImVec2 winPos = ImGui::GetItemRectMin();
                float localX = mousePos.x - winPos.x;
                float localY = mousePos.y - winPos.y;
                glm::vec2 sketchCoord = screenToSketch(localX, localY, contentSize.x, contentSize.y);

                // Sketch pattern axis-origin picker. While the radial sketch
                // pattern popup is asking for an origin, the next click in
                // the sketch viewport sets it (snapped to the sketch grid)
                // instead of going through SketchTool's normal input.
                bool patternPickingNow = m_sketchPatternActive && m_sketchPatternPickingOrigin;
                if (patternPickingNow) {
                    float step = std::max(m_sketchGridStep, 0.01f);
                    glm::vec2 snapped(std::round(sketchCoord.x / step) * step,
                                      std::round(sketchCoord.y / step) * step);
                    ImVec2 sp(mousePos.x, mousePos.y);
                    auto* dl = ImGui::GetForegroundDrawList();
                    dl->AddCircleFilled(sp, 7.0f, IM_COL32(255, 220, 50, 255));
                    dl->AddCircle      (sp, 7.0f, IM_COL32(0, 0, 0, 255), 0, 1.5f);
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f)", snapped.x, snapped.y);
                    dl->AddText(ImVec2(sp.x + 10.0f, sp.y - 8.0f),
                                IM_COL32(255, 220, 50, 255), buf);
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        m_sketchPatternOriginX = snapped.x;
                        m_sketchPatternOriginY = snapped.y;
                        std::snprintf(m_sketchPatternOXBuf, sizeof(m_sketchPatternOXBuf),
                                      "%.2f", m_sketchPatternOriginX);
                        std::snprintf(m_sketchPatternOYBuf, sizeof(m_sketchPatternOYBuf),
                                      "%.2f", m_sketchPatternOriginY);
                        m_sketchPatternPickingOrigin = false;
                        updateSketchPattern();
                    }
                }

                if (!patternPickingNow) {
                // === Sketch Move/Rotate gizmo ====================================
                // Drawn on the selection centroid in Select mode. Axis arrows for
                // constrained X/Y move, centre dot for free move, ring for rotate.
                // Held-drag: click a handle → drag → release commits. While the
                // gizmo owns the drag, the normal sketch tool input is skipped so
                // both don't try to mutate the same points.
                ImDrawList* gdl = ImGui::GetWindowDrawList();
                glm::mat4 gvp = proj * view;
                auto gToImg = [&](glm::vec3 w, ImVec2& out) -> bool {
                    glm::vec4 c = gvp * glm::vec4(w, 1.0f);
                    if (c.w <= 1e-5f) return false; // behind camera
                    out = ImVec2(winPos.x + (c.x / c.w * 0.5f + 0.5f) * contentSize.x,
                                 winPos.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * contentSize.y);
                    return true;
                };
                bool gizmoOwnsInput = (m_sketchGizmoHandle != SketchGizmoHandle::None);
                if (m_sketchTool->getMode() == SketchToolMode::Select &&
                    m_sketchTool->hasElementSelection()) {

                    // Resolve involved point ids (selected points + endpoints of
                    // selected lines) and compute centroid.
                    std::set<int> involved(m_sketchTool->getSelectedPoints().begin(),
                                           m_sketchTool->getSelectedPoints().end());
                    for (int lid : m_sketchTool->getSelectedLines()) {
                        for (const auto& l : m_activeSketch->getLines()) {
                            if (l.id == lid) {
                                involved.insert(l.startPointId);
                                involved.insert(l.endPointId);
                                break;
                            }
                        }
                    }
                    glm::vec2 c{0.0f};
                    int nInv = 0;
                    for (int id : involved)
                        if (auto* p = m_activeSketch->getPoint(id)) { c += p->pos; ++nInv; }
                    if (nInv > 0) {
                        c /= static_cast<float>(nInv);
                        // During a drag, anchor the gizmo visual at the drag-start
                        // centroid so it doesn't slide around with the selection.
                        glm::vec2 gizmoC = gizmoOwnsInput ? m_sketchGizmoCenter : c;

                        const gp_Ax3& ax = m_activeSketch->getPlane().Position();
                        glm::vec3 O(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
                        glm::vec3 Xw(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
                        glm::vec3 Yw(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());
                        auto sk2w = [&](glm::vec2 p) { return O + p.x * Xw + p.y * Yw; };

                        ImVec2 sc, sx, sy;
                        bool projOk = gToImg(sk2w(gizmoC), sc) &&
                                      gToImg(sk2w(gizmoC + glm::vec2(1.0f, 0.0f)), sx) &&
                                      gToImg(sk2w(gizmoC + glm::vec2(0.0f, 1.0f)), sy);
                        glm::vec2 vx(sx.x - sc.x, sx.y - sc.y);
                        glm::vec2 vy(sy.x - sc.x, sy.y - sc.y);
                        if (projOk && glm::length(vx) > 1e-3f && glm::length(vy) > 1e-3f) {
                            vx = glm::normalize(vx);
                            vy = glm::normalize(vy);
                            const float armLen  = 70.0f;
                            const float ringR   = 100.0f;
                            const float centerR = 6.5f;
                            ImVec2 ex(sc.x + vx.x * armLen, sc.y + vx.y * armLen);
                            ImVec2 ey(sc.x + vy.x * armLen, sc.y + vy.y * armLen);

                            // Hit-test against handles in screen space.
                            auto distSegSq = [](glm::vec2 q, glm::vec2 a, glm::vec2 b) {
                                glm::vec2 ab = b - a;
                                float len2 = glm::dot(ab, ab);
                                if (len2 < 1e-6f) return glm::dot(q - a, q - a);
                                float t = glm::clamp(glm::dot(q - a, ab) / len2, 0.0f, 1.0f);
                                glm::vec2 proj = a + t * ab;
                                return glm::dot(q - proj, q - proj);
                            };
                            glm::vec2 mv(mousePos.x, mousePos.y);
                            glm::vec2 scV(sc.x, sc.y), exV(ex.x, ex.y), eyV(ey.x, ey.y);
                            float distC    = glm::length(mv - scV);
                            float dxSq     = distSegSq(mv, scV, exV);
                            float dySq     = distSegSq(mv, scV, eyV);
                            float distRing = std::abs(distC - ringR);

                            const float pickPx = 8.0f;
                            const float pickPxSq = pickPx * pickPx;
                            SketchGizmoHandle hover = SketchGizmoHandle::None;
                            if      (distC < centerR + 4.0f)                            hover = SketchGizmoHandle::MoveFree;
                            else if (dxSq < pickPxSq && distC < armLen + 12.0f)         hover = SketchGizmoHandle::MoveX;
                            else if (dySq < pickPxSq && distC < armLen + 12.0f)         hover = SketchGizmoHandle::MoveY;
                            else if (distRing < pickPx)                                 hover = SketchGizmoHandle::Rotate;

                            // Yellow highlight for the active or hovered handle.
                            auto col = [&](ImU32 base, SketchGizmoHandle h) -> ImU32 {
                                bool hot = (hover == h) || (m_sketchGizmoHandle == h);
                                return hot ? IM_COL32(255, 240, 80, 255) : base;
                            };
                            ImU32 colX      = col(IM_COL32(230,  70,  70, 230), SketchGizmoHandle::MoveX);
                            ImU32 colY      = col(IM_COL32( 90, 200,  90, 230), SketchGizmoHandle::MoveY);
                            ImU32 colRing   = col(IM_COL32(220, 180,  60, 220), SketchGizmoHandle::Rotate);
                            ImU32 colCenter = col(IM_COL32(240, 240, 230, 230), SketchGizmoHandle::MoveFree);

                            // Rotate ring (behind the arrows so arrowheads aren't clipped).
                            gdl->AddCircle(sc, ringR, colRing, 64, 2.5f);
                            // Axis arrows.
                            gdl->AddLine(sc, ex, colX, 3.0f);
                            gdl->AddLine(sc, ey, colY, 3.0f);
                            auto arrowhead = [&](ImVec2 tip, glm::vec2 d, ImU32 c) {
                                glm::vec2 perp(-d.y, d.x);
                                ImVec2 a(tip.x - d.x * 14.0f + perp.x * 6.0f,
                                         tip.y - d.y * 14.0f + perp.y * 6.0f);
                                ImVec2 b(tip.x - d.x * 14.0f - perp.x * 6.0f,
                                         tip.y - d.y * 14.0f - perp.y * 6.0f);
                                gdl->AddTriangleFilled(tip, a, b, c);
                            };
                            arrowhead(ex, vx, colX);
                            arrowhead(ey, vy, colY);
                            // Centre free-move dot, on top.
                            gdl->AddCircleFilled(sc, centerR, colCenter);
                            gdl->AddCircle(sc, centerR, IM_COL32(30, 30, 40, 230), 0, 1.2f);

                            // Live angle label while dragging the rotate ring, plus
                            // a small type-in popup once the drag is released so the
                            // user can refine the angle exactly.
                            if (m_sketchGizmoHandle == SketchGizmoHandle::Rotate) {
                                char angBuf[24];
                                std::snprintf(angBuf, sizeof(angBuf), "%.1f\xC2\xB0",
                                              m_sketchGizmoRotateDegrees);
                                ImVec2 ts = ImGui::CalcTextSize(angBuf);
                                ImVec2 lp(sc.x + 14.0f, sc.y - 14.0f - ts.y);
                                gdl->AddRectFilled(ImVec2(lp.x - 4, lp.y - 2),
                                                   ImVec2(lp.x + ts.x + 4, lp.y + ts.y + 2),
                                                   IM_COL32(20, 20, 28, 220), 3.0f);
                                gdl->AddText(lp, IM_COL32(255, 240, 80, 255), angBuf);
                            }

                            // Track the popup's screen-space anchor each frame so
                            // the camera can pan/orbit and the popup follows the
                            // centroid. Actual popup rendering happens at top scope
                            // below — keeping it out of these nested ifs is what
                            // killed the hover flicker.
                            m_sketchGizmoAdjustAnchor = glm::vec2(sc.x + 20.0f, sc.y + 16.0f);

                            // Start drag: clicking a handle arms the gizmo, snapshots
                            // the involved points, and stops the click from reaching
                            // the SketchTool below. Ctrl+click is the multi-select
                            // modifier for sketch elements — never intercept it for
                            // the gizmo, even if the click happens to land on a
                            // handle (the ring is large and crosses sketch lines).
                            if (!gizmoOwnsInput && hover != SketchGizmoHandle::None &&
                                !io.KeyCtrl &&
                                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                m_sketchGizmoHandle = hover;
                                m_sketchGizmoCenter = c;
                                m_sketchGizmoAnchor = sketchCoord;
                                m_sketchGizmoBefore = std::make_shared<Sketch>(*m_activeSketch);
                                m_sketchGizmoOriginals.clear();
                                for (int id : involved)
                                    if (auto* p = m_activeSketch->getPoint(id))
                                        m_sketchGizmoOriginals.push_back({id, p->pos});
                                gizmoOwnsInput = true;
                            }
                        }
                    }
                }

                // Apply the gizmo drag (skipped once Rotate has transitioned to
                // popup-adjust mode — there's no live drag anymore at that point).
                // MoveX/MoveY clamp the cursor delta to the chosen sketch axis and
                // snap the resulting centroid to grid; MoveFree snaps both axes;
                // Rotate spins around the centroid with a 15° soft snap.
                if (gizmoOwnsInput && !m_sketchGizmoRotateAdjusting) {
                    glm::vec2 cur = sketchCoord;
                    if (m_sketchGizmoHandle == SketchGizmoHandle::Rotate) {
                        glm::vec2 v0 = m_sketchGizmoAnchor - m_sketchGizmoCenter;
                        glm::vec2 v1 = cur                  - m_sketchGizmoCenter;
                        if (glm::length(v0) > 1e-4f && glm::length(v1) > 1e-4f) {
                            float angRad = std::atan2(v1.y, v1.x) - std::atan2(v0.y, v0.x);
                            float angDeg = angRad * 180.0f / static_cast<float>(M_PI);
                            // Soft 15° snap: lock when within 3° of an increment so
                            // the user can ride the snap without it feeling sticky.
                            float snapped = std::round(angDeg / 15.0f) * 15.0f;
                            if (std::abs(angDeg - snapped) < 3.0f) angDeg = snapped;
                            m_sketchGizmoRotateDegrees = angDeg;
                            float rad = angDeg * static_cast<float>(M_PI) / 180.0f;
                            float ca = std::cos(rad), sa = std::sin(rad);
                            for (auto& [id, orig] : m_sketchGizmoOriginals) {
                                glm::vec2 d = orig - m_sketchGizmoCenter;
                                glm::vec2 r(d.x * ca - d.y * sa, d.x * sa + d.y * ca);
                                m_activeSketch->movePoint(id, m_sketchGizmoCenter + r);
                            }
                        }
                    } else {
                        glm::vec2 delta = cur - m_sketchGizmoAnchor;
                        if (m_sketchGizmoHandle == SketchGizmoHandle::MoveX) delta.y = 0.0f;
                        else if (m_sketchGizmoHandle == SketchGizmoHandle::MoveY) delta.x = 0.0f;
                        // Snap the new centroid (on the unconstrained axes) so the
                        // numbers land on grid lines.
                        float step = m_sketchTool->getGridStep();
                        if (step > 0.0f) {
                            glm::vec2 nc = m_sketchGizmoCenter + delta;
                            if (m_sketchGizmoHandle != SketchGizmoHandle::MoveY)
                                nc.x = std::round(nc.x / step) * step;
                            if (m_sketchGizmoHandle != SketchGizmoHandle::MoveX)
                                nc.y = std::round(nc.y / step) * step;
                            delta = nc - m_sketchGizmoCenter;
                        }
                        for (auto& [id, orig] : m_sketchGizmoOriginals)
                            m_activeSketch->movePoint(id, orig + delta);
                    }

                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                        if (m_sketchGizmoHandle == SketchGizmoHandle::Rotate) {
                            // Transition to popup-adjust mode instead of committing
                            // straight away — the user can refine the angle by
                            // typing, then Apply / Enter commits. Use a real ImGui
                            // popup so focus semantics don't fight the viewport
                            // window (a Begin/End float here flickers on hover).
                            std::snprintf(m_sketchGizmoRotateBuf,
                                          sizeof(m_sketchGizmoRotateBuf),
                                          "%.1f", m_sketchGizmoRotateDegrees);
                            m_sketchGizmoRotateAdjusting = true;
                            ImGui::OpenPopup("##SketchRotateAdjust");
                        } else {
                            bool changed = false;
                            for (auto& [id, orig] : m_sketchGizmoOriginals) {
                                if (auto* p = m_activeSketch->getPoint(id))
                                    if (glm::distance(p->pos, orig) > 1e-5f) {
                                        changed = true; break;
                                    }
                            }
                            if (changed) {
                                auto after = std::make_shared<Sketch>(*m_activeSketch);
                                auto op = std::make_unique<SketchEditOp>(
                                    m_activeSketch, m_sketchGizmoBefore, after);
                                m_history->pushExecuted(std::move(op));
                            }
                            m_sketchGizmoHandle = SketchGizmoHandle::None;
                            m_sketchGizmoBefore.reset();
                            m_sketchGizmoOriginals.clear();
                        }
                    }
                }

                // === Rotate-angle adjust popup ====================================
                // Real ImGui popup (OpenPopup/BeginPopup) instead of a Begin/End
                // floating window: focus, hover hit-test, and z-order all behave
                // correctly when rendered at top scope. The earlier nested-Begin
                // approach flickered when the cursor hovered the popup.
                if (m_sketchGizmoRotateAdjusting) {
                    ImGui::SetNextWindowPos(ImVec2(m_sketchGizmoAdjustAnchor.x,
                                                   m_sketchGizmoAdjustAnchor.y),
                                            ImGuiCond_Appearing);
                    if (ImGui::BeginPopup("##SketchRotateAdjust",
                                          ImGuiWindowFlags_AlwaysAutoResize)) {
                        ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.30f, 1.0f),
                                           "Rotation (deg)");
                        ImGui::SetNextItemWidth(150.0f);
                        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                        bool typedEnter = ImGui::InputText(
                            "##sketchRotAng", m_sketchGizmoRotateBuf,
                            sizeof(m_sketchGizmoRotateBuf),
                            ImGuiInputTextFlags_EnterReturnsTrue |
                            ImGuiInputTextFlags_CharsDecimal);
                        if (ImGui::IsItemDeactivatedAfterEdit()) {
                            // Re-apply the typed value live as the user clicks off
                            // the field, so the sketch reflects what they typed.
                            float deg = static_cast<float>(std::atof(m_sketchGizmoRotateBuf));
                            m_sketchGizmoRotateDegrees = deg;
                            float rad = deg * static_cast<float>(M_PI) / 180.0f;
                            float ca = std::cos(rad), sa = std::sin(rad);
                            for (auto& [id, orig] : m_sketchGizmoOriginals) {
                                glm::vec2 d = orig - m_sketchGizmoCenter;
                                glm::vec2 r(d.x * ca - d.y * sa, d.x * sa + d.y * ca);
                                m_activeSketch->movePoint(id, m_sketchGizmoCenter + r);
                            }
                        }
                        ImGui::Separator();
                        bool apply  = ImGui::Button("Apply", ImVec2(70, 0)) || typedEnter;
                        ImGui::SameLine();
                        bool cancel = ImGui::Button("Cancel", ImVec2(70, 0));

                        if (apply) {
                            float deg = static_cast<float>(std::atof(m_sketchGizmoRotateBuf));
                            float rad = deg * static_cast<float>(M_PI) / 180.0f;
                            float ca = std::cos(rad), sa = std::sin(rad);
                            for (auto& [id, orig] : m_sketchGizmoOriginals) {
                                glm::vec2 d = orig - m_sketchGizmoCenter;
                                glm::vec2 r(d.x * ca - d.y * sa, d.x * sa + d.y * ca);
                                m_activeSketch->movePoint(id, m_sketchGizmoCenter + r);
                            }
                            bool changed = false;
                            for (auto& [id, orig] : m_sketchGizmoOriginals) {
                                if (auto* p = m_activeSketch->getPoint(id))
                                    if (glm::distance(p->pos, orig) > 1e-5f) {
                                        changed = true; break;
                                    }
                            }
                            if (changed) {
                                auto after = std::make_shared<Sketch>(*m_activeSketch);
                                auto op = std::make_unique<SketchEditOp>(
                                    m_activeSketch, m_sketchGizmoBefore, after);
                                m_history->pushExecuted(std::move(op));
                            }
                            m_sketchGizmoHandle = SketchGizmoHandle::None;
                            m_sketchGizmoBefore.reset();
                            m_sketchGizmoOriginals.clear();
                            m_sketchGizmoRotateAdjusting = false;
                            ImGui::CloseCurrentPopup();
                        } else if (cancel) {
                            for (auto& [id, orig] : m_sketchGizmoOriginals)
                                m_activeSketch->movePoint(id, orig);
                            m_sketchGizmoHandle = SketchGizmoHandle::None;
                            m_sketchGizmoBefore.reset();
                            m_sketchGizmoOriginals.clear();
                            m_sketchGizmoRotateAdjusting = false;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    } else {
                        // Click-outside auto-closed the popup → treat as commit
                        // (whatever angle is currently applied stays). This is the
                        // friendlier default for a CAD adjust popup; Esc reverts
                        // via the shortcut handler.
                        bool changed = false;
                        for (auto& [id, orig] : m_sketchGizmoOriginals) {
                            if (auto* p = m_activeSketch->getPoint(id))
                                if (glm::distance(p->pos, orig) > 1e-5f) {
                                    changed = true; break;
                                }
                        }
                        if (changed) {
                            auto after = std::make_shared<Sketch>(*m_activeSketch);
                            auto op = std::make_unique<SketchEditOp>(
                                m_activeSketch, m_sketchGizmoBefore, after);
                            m_history->pushExecuted(std::move(op));
                        }
                        m_sketchGizmoHandle = SketchGizmoHandle::None;
                        m_sketchGizmoBefore.reset();
                        m_sketchGizmoOriginals.clear();
                        m_sketchGizmoRotateAdjusting = false;
                    }
                }

                // Hit-test helper shared by box-select start and chain-select
                // (double-click). Mirrors handleSelectTool: nearest point first,
                // then a nearby line, within the same tolerance.
                auto pickSketchAt = [&](glm::vec2 pos, int& outPointId, int& outLineId) {
                    outPointId = -1; outLineId = -1;
                    const float pointTol = 0.3f; // matches SketchTool::findCoincidentPoint
                    for (const auto& pt : m_activeSketch->getPoints()) {
                        if (glm::length(pos - pt.pos) < pointTol) { outPointId = pt.id; return; }
                    }
                    float bestD = 0.0f;
                    const float lineTol = std::max(m_sketchTool->getGridStep() * 0.5f, 0.5f);
                    for (const auto& l : m_activeSketch->getLines()) {
                        const SketchPoint* a = m_activeSketch->getPoint(l.startPointId);
                        const SketchPoint* b = m_activeSketch->getPoint(l.endPointId);
                        if (!a || !b) continue;
                        glm::vec2 ab = b->pos - a->pos;
                        float len2 = glm::dot(ab, ab);
                        if (len2 < 1e-12f) continue;
                        float t = glm::clamp(glm::dot(pos - a->pos, ab) / len2, 0.0f, 1.0f);
                        glm::vec2 proj = a->pos + ab * t;
                        float d = glm::distance(proj, pos);
                        if (d < lineTol && (outLineId < 0 || d < bestD)) {
                            outLineId = l.id; bestD = d;
                        }
                    }
                };

                if (gizmoOwnsInput) {
                    // Suppress normal sketch input while the gizmo owns the drag /
                    // popup adjust.
                } else if (m_sketchTool->getMode() == SketchToolMode::Select &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    // Double-click behaviour:
                    //   on a line → select every line in its connected chain
                    //               (lines sharing endpoints, transitively),
                    //   empty space → select every element in the sketch.
                    int hitPt = -1, hitLn = -1;
                    pickSketchAt(sketchCoord, hitPt, hitLn);
                    if (hitLn >= 0) {
                        // BFS over lines via shared endpoint ids.
                        std::set<int> selPts, selLns;
                        std::vector<int> stack{hitLn};
                        while (!stack.empty()) {
                            int lid = stack.back(); stack.pop_back();
                            if (!selLns.insert(lid).second) continue;
                            for (const auto& l : m_activeSketch->getLines()) {
                                if (l.id != lid) continue;
                                selPts.insert(l.startPointId);
                                selPts.insert(l.endPointId);
                                // walk neighbours via the two endpoints
                                for (const auto& n : m_activeSketch->getLines()) {
                                    if (n.id == lid || selLns.count(n.id)) continue;
                                    if (n.startPointId == l.startPointId ||
                                        n.startPointId == l.endPointId   ||
                                        n.endPointId   == l.startPointId ||
                                        n.endPointId   == l.endPointId) {
                                        stack.push_back(n.id);
                                    }
                                }
                                break;
                            }
                        }
                        m_sketchTool->setSelection(selPts, selLns);
                    } else {
                        m_sketchTool->selectAll();
                    }
                    m_sketchDragBefore.reset(); // not a drag
                } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    // Pick first to decide between element-drag and box-select.
                    bool tryBoxSelect = (m_sketchTool->getMode() == SketchToolMode::Select);
                    int hitPt = -1, hitLn = -1;
                    if (tryBoxSelect) pickSketchAt(sketchCoord, hitPt, hitLn);
                    bool hitElement = (hitPt >= 0 || hitLn >= 0);

                    if (tryBoxSelect && !hitElement && m_boxSelect) {
                        // Empty space in Select mode → start a box-select drag.
                        // Don't clear the existing selection yet; the release
                        // handler does that based on whether the rect is tiny.
                        m_boxSelect->begin(glm::vec2(localX, localY));
                        m_sketchBoxSelectActive = true;
                        m_sketchDragBefore.reset();
                    } else {
                        // Select/drag mutates point positions only — no structural
                        // change — so recordSketchMutation's signature wouldn't see
                        // it. Snapshot manually for the drag-commit on mouse-up.
                        if (m_sketchTool->getMode() == SketchToolMode::Select) {
                            m_sketchDragBefore = std::make_shared<Sketch>(*m_activeSketch);
                        }
                        recordSketchMutation([&]{ m_sketchTool->onMouseDown(sketchCoord, io.KeyCtrl); });
                    }
                }

                // Box-select drag/release in sketch mode.
                if (m_sketchBoxSelectActive && m_boxSelect && m_boxSelect->isActive()) {
                    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                        m_boxSelect->update(glm::vec2(localX, localY));
                    }
                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                        glm::vec2 mn = m_boxSelect->getMin();
                        glm::vec2 mx = m_boxSelect->getMax();
                        m_boxSelect->end();
                        m_sketchBoxSelectActive = false;

                        if (glm::distance(mn, mx) < 4.0f) {
                            // Tiny rect — plain click on empty space. Clear unless Ctrl.
                            if (!io.KeyCtrl) m_sketchTool->clearElementSelection();
                        } else {
                            // Project sketch points to viewport-local screen pixels,
                            // pick anything that lands in the rectangle, plus any
                            // line whose endpoints' AABB overlaps the rectangle.
                            const gp_Ax3& ax = m_activeSketch->getPlane().Position();
                            glm::vec3 O(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
                            glm::vec3 Xw(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
                            glm::vec3 Yw(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());
                            glm::mat4 vp = proj * view;
                            auto projectPt = [&](glm::vec2 sp, glm::vec2& out) -> bool {
                                glm::vec3 w = O + sp.x * Xw + sp.y * Yw;
                                glm::vec4 cp = vp * glm::vec4(w, 1.0f);
                                if (cp.w <= 1e-5f) return false;
                                out = glm::vec2((cp.x / cp.w * 0.5f + 0.5f) * contentSize.x,
                                                (1.0f - (cp.y / cp.w * 0.5f + 0.5f)) * contentSize.y);
                                return true;
                            };

                            std::set<int> selPts = io.KeyCtrl ? m_sketchTool->getSelectedPoints()
                                                              : std::set<int>{};
                            std::set<int> selLns = io.KeyCtrl ? m_sketchTool->getSelectedLines()
                                                              : std::set<int>{};
                            // Cache projected positions so the line test reuses them.
                            std::map<int, glm::vec2> ptScreen;
                            for (const auto& p : m_activeSketch->getPoints()) {
                                glm::vec2 sp;
                                if (!projectPt(p.pos, sp)) continue;
                                ptScreen[p.id] = sp;
                                if (sp.x >= mn.x && sp.x <= mx.x &&
                                    sp.y >= mn.y && sp.y <= mx.y) {
                                    selPts.insert(p.id);
                                }
                            }
                            for (const auto& l : m_activeSketch->getLines()) {
                                auto a = ptScreen.find(l.startPointId);
                                auto b = ptScreen.find(l.endPointId);
                                if (a == ptScreen.end() || b == ptScreen.end()) continue;
                                glm::vec2 lmin = glm::min(a->second, b->second);
                                glm::vec2 lmax = glm::max(a->second, b->second);
                                if (lmax.x >= mn.x && lmin.x <= mx.x &&
                                    lmax.y >= mn.y && lmin.y <= mx.y) {
                                    selLns.insert(l.id);
                                }
                            }
                            m_sketchTool->setSelection(selPts, selLns);
                        }
                    }
                }

                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !m_sketchBoxSelectActive) {
                    m_sketchTool->onMouseUp(sketchCoord);
                    if (m_sketchDragBefore) {
                        // Compare point positions; commit a SketchEditOp if any moved.
                        const auto& before = m_sketchDragBefore->getPoints();
                        const auto& after  = m_activeSketch->getPoints();
                        bool changed = (before.size() != after.size());
                        for (size_t i = 0; !changed && i < before.size(); ++i) {
                            if (glm::distance(before[i].pos, after[i].pos) > 1e-5f)
                                changed = true;
                        }
                        if (changed) {
                            auto after_ptr = std::make_shared<Sketch>(*m_activeSketch);
                            auto op = std::make_unique<SketchEditOp>(
                                m_activeSketch, m_sketchDragBefore, after_ptr);
                            m_history->pushExecuted(std::move(op));
                        }
                        m_sketchDragBefore.reset();
                    }
                }
                if (!gizmoOwnsInput) m_sketchTool->onMouseMove(sketchCoord);
                } // if (!patternPickingNow)
            }
        }
    }

    // ViewCube overlay
    ViewCubeAction vcAction = m_viewCube->render(m_viewport->getCamera(), m_invertCubeDrag);
    if (vcAction != ViewCubeAction::None) {
        handleViewCubeAction(static_cast<int>(vcAction));
    }

    // Right-click face context menu
    if (m_contextMenuPending) {
        ImGui::OpenPopup("FaceContextMenu");
        m_contextMenuPending = false;
    }
    if (ImGui::BeginPopup("FaceContextMenu")) {
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Face Options");
        ImGui::Separator();

        if (ImGui::MenuItem("Sketch on this Face")) {
            // Select the face, then enter sketch mode (enterSketchMode reads the selection)
            SelectionEntry entry;
            entry.type = SelectionType::Face;
            entry.bodyId = m_contextMenuBodyId;
            entry.shape = m_contextMenuFace;
            m_selection->select(entry);
            enterSketchMode();
            m_contextMenuFace.Nullify();
        }
        if (ImGui::MenuItem("Extrude Face")) {
            beginInteractiveExtrude(m_contextMenuFace, ExtrudeMode::NewBody, -1);
            m_contextMenuFace.Nullify();
        }
        if (ImGui::MenuItem("Select Body")) {
            SelectionEntry entry;
            entry.type = SelectionType::Body;
            entry.bodyId = m_contextMenuBodyId;
            try { entry.shape = m_document->getBody(m_contextMenuBodyId); } catch (...) {}
            m_selection->select(entry);
            m_contextMenuFace.Nullify();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Cancel")) {
            m_contextMenuFace.Nullify();
        }
        ImGui::EndPopup();
    }

    // Gizmo hint
    if (m_gizmo->isVisible()) {
        ImGui::SetCursorPos(ImVec2(10, 30));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGui::Text("Arrows: Move | Rings: Rotate | Cubes: Scale");
        ImGui::PopStyleColor();
    }

    // Interactive extrude UI
    if (m_extruding) {
        ImGui::SetCursorPos(ImVec2(10, 30));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
        ImGui::Text("EXTRUDE - Drag in viewport or type distance. Enter to confirm, Escape to cancel.");
        ImGui::PopStyleColor();

        // Floating distance input panel
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 260,
                                        ImGui::GetWindowPos().y + 50));
        ImGui::SetNextWindowSize(ImVec2(240, 0));
        ImGui::Begin("##ExtrudeInput", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("Extrude Distance (mm)");
        ImGui::Separator();

        if (m_extrudeInputFocus) {
            ImGui::SetKeyboardFocusHere();
            m_extrudeInputFocus = false;
        }

        bool valueChanged = false;
        if (ImGui::InputText("##dist", m_extrudeInputBuf, sizeof(m_extrudeInputBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            // Enter pressed — commit
            m_extrudeDistance = static_cast<float>(std::atof(m_extrudeInputBuf));
            updateInteractiveExtrude();
            commitInteractiveExtrude();
        } else {
            // Update distance from text as user types
            float parsed = static_cast<float>(std::atof(m_extrudeInputBuf));
            if (std::abs(parsed - m_extrudeDistance) > 0.01f && std::abs(parsed) > 0.01f) {
                m_extrudeDistance = parsed;
                updateInteractiveExtrude();
            }
        }

        ImGui::SameLine();
        ImGui::Text("mm");

        // Slider for quick adjustment
        if (ImGui::SliderFloat("##slider", &m_extrudeDistance, -50.0f, 50.0f, "%.1f mm")) {
            std::snprintf(m_extrudeInputBuf, sizeof(m_extrudeInputBuf), "%.1f", m_extrudeDistance);
            updateInteractiveExtrude();
        }

        ImGui::Spacing();
        if (ImGui::Button("Confirm (Enter)", ImVec2(110, 0))) {
            commitInteractiveExtrude();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel (Esc)", ImVec2(110, 0))) {
            cancelInteractiveExtrude();
        }

        ImGui::End();
    }

    // Interactive Push/Pull UI
    if (m_pushPullActive) {
        ImGui::SetCursorPos(ImVec2(10, 30));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.85f, 1.0f, 1.0f));
        ImGui::Text("PUSH/PULL - Positive = extrude, Negative = cut. Enter to confirm, Escape to cancel.");
        ImGui::PopStyleColor();

        ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 260,
                                        ImGui::GetWindowPos().y + 50));
        ImGui::SetNextWindowSize(ImVec2(240, 0));
        ImGui::Begin("##PushPullInput", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("Distance (mm) - signed");
        ImGui::Separator();

        if (m_pushPullInputFocus) {
            ImGui::SetKeyboardFocusHere();
            m_pushPullInputFocus = false;
        }

        if (ImGui::InputText("##ppdist", m_pushPullInputBuf, sizeof(m_pushPullInputBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            m_pushPullDistance = static_cast<float>(std::atof(m_pushPullInputBuf));
            updatePushPull();
            commitPushPull();
        } else {
            float parsed = static_cast<float>(std::atof(m_pushPullInputBuf));
            if (std::abs(parsed - m_pushPullDistance) > 0.01f) {
                m_pushPullDistance = parsed;
                updatePushPull();
            }
        }

        ImGui::SameLine();
        ImGui::Text("mm");

        if (ImGui::SliderFloat("##ppslider", &m_pushPullDistance, -50.0f, 50.0f, "%.1f mm")) {
            std::snprintf(m_pushPullInputBuf, sizeof(m_pushPullInputBuf), "%.1f", m_pushPullDistance);
            updatePushPull();
        }

        ImGui::Spacing();
        if (ImGui::Button("Confirm (Enter)", ImVec2(110, 0))) {
            commitPushPull();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel (Esc)", ImVec2(110, 0))) {
            cancelPushPull();
        }

        ImGui::End();
    }

    // Interactive fillet/chamfer UI
    if (m_edgeOpActive) {
        const char* opName = m_edgeOpType == EdgeOpType::Fillet ? "FILLET" : "CHAMFER";
        const char* label = m_edgeOpType == EdgeOpType::Fillet ? "Radius (mm)" : "Distance (mm)";

        ImGui::SetCursorPos(ImVec2(10, 30));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.5f, 1.0f));
        ImGui::Text("%s - Type value or use slider. Enter to confirm, Escape to cancel.", opName);
        ImGui::PopStyleColor();

        ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 260,
                                        ImGui::GetWindowPos().y + 50));
        ImGui::SetNextWindowSize(ImVec2(240, 0));
        ImGui::Begin("##EdgeOpInput", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("%s", label);
        ImGui::Separator();

        if (m_edgeOpInputFocus) {
            ImGui::SetKeyboardFocusHere();
            m_edgeOpInputFocus = false;
        }

        if (ImGui::InputText("##val", m_edgeOpInputBuf, sizeof(m_edgeOpInputBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            m_edgeOpValue = static_cast<float>(std::atof(m_edgeOpInputBuf));
            updateInteractiveEdgeOp();
            commitInteractiveEdgeOp();
        } else {
            float parsed = static_cast<float>(std::atof(m_edgeOpInputBuf));
            if (std::abs(parsed - m_edgeOpValue) > 0.01f && parsed > 0.01f) {
                m_edgeOpValue = parsed;
                updateInteractiveEdgeOp();
            }
        }

        ImGui::SameLine();
        ImGui::Text("mm");

        if (ImGui::SliderFloat("##eslider", &m_edgeOpValue, 0.1f, 20.0f, "%.1f mm")) {
            std::snprintf(m_edgeOpInputBuf, sizeof(m_edgeOpInputBuf), "%.1f", m_edgeOpValue);
            updateInteractiveEdgeOp();
        }

        ImGui::Spacing();
        if (ImGui::Button("Confirm (Enter)", ImVec2(110, 0))) {
            commitInteractiveEdgeOp();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel (Esc)", ImVec2(110, 0))) {
            cancelInteractiveEdgeOp();
        }

        ImGui::End();
    }

    // Scale gizmo side panel (X/Y/Z % + uniform + Apply), shown in Scale mode.
    renderScalePanel();

    // Sketch mode indicator
    if (m_inSketchMode) {
        ImGui::SetCursorPos(ImVec2(10, 30));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.4f, 1.0f));
        ImGui::Text("SKETCH MODE - Press Escape to finish");
        ImGui::PopStyleColor();
    }

    // Inline dimension input while placing a sketch shape
    if (m_inSketchMode && m_sketchTool && m_sketchTool->hasPreview()) {
        SketchToolMode mode = m_sketchTool->getPreviewType();
        const char* dimLabel = nullptr;
        const char* dimHint  =
            "Type a value and press Enter. The shape extends from your first click toward the cursor.";
        switch (mode) {
            case SketchToolMode::Line:      dimLabel = "Length (mm)"; break;
            case SketchToolMode::Circle:    dimLabel = "Radius (mm)"; break;
            case SketchToolMode::Polygon:
                dimLabel = "Sides";
                dimHint  = "Type sides (≥3) and Enter to preview — radius and "
                           "rotation come from cursor. Click to commit; the "
                           "first vertex lands on the (snapped) cursor.";
                break;
            case SketchToolMode::Rectangle: dimLabel = "Side (mm)";   break;
            default: dimLabel = nullptr;
        }
        if (dimLabel) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 230,
                                            ImGui::GetWindowPos().y + 50));
            ImGui::SetNextWindowSize(ImVec2(220, 0));
            ImGui::Begin("##SketchDimInput", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_AlwaysAutoResize);

            ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", dimLabel);
            ImGui::Separator();
            ImGui::TextWrapped("%s", dimHint);

            // Grab keyboard focus the first frame placement begins
            if (!m_sketchDimWasShown) {
                ImGui::SetKeyboardFocusHere();
                m_sketchDimWasShown = true;
            }

            if (ImGui::InputText("##sketchDim", m_sketchDimBuf, sizeof(m_sketchDimBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_CharsDecimal |
                                 ImGuiInputTextFlags_AutoSelectAll)) {
                float v = static_cast<float>(std::atof(m_sketchDimBuf));
                if (v > 0.0f) {
                    recordSketchMutation([&]{ m_sketchTool->applyDimension(v); });
                }
                m_sketchDimBuf[0] = '\0';
                m_sketchDimWasShown = false; // re-focus on the next placement
            }

            ImGui::End();
        }
    } else {
        // Reset when not placing
        m_sketchDimBuf[0] = '\0';
        m_sketchDimWasShown = false;
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace materializr

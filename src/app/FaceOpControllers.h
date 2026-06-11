#pragma once
#include "InteractiveOpController.h"
#include <TopoDS_Face.hxx>
#include <glm/glm.hpp>
#include <vector>

namespace materializr {

// ─── Shell ───────────────────────────────────────────────────────────────────
// Hollow a body, removing the picked face; thickness in the popup.
class ShellController : public InteractiveOpController {
protected:
    const char* title() const override { return "Shell"; }
    int onBegin(const IopContext& ctx) override;
    std::unique_ptr<Operation> buildOp(const IopContext& ctx) override;
    void panelBody(const IopContext& ctx, bool& changed) override;
    void onCleanup() override;
    float panelWidth() const override { return 240.0f; }

private:
    TopoDS_Face m_face;
    float m_thickness = 1.0f;
    char m_inputBuf[32] = "1.0";
    bool m_inputFocus = true;
};

// ─── Taper ───────────────────────────────────────────────────────────────────
// Draft the picked face(s) by an angle about the body's base.
class TaperController : public InteractiveOpController {
protected:
    const char* title() const override { return "Taper"; }
    int onBegin(const IopContext& ctx) override;
    std::unique_ptr<Operation> buildOp(const IopContext& ctx) override;
    void panelBody(const IopContext& ctx, bool& changed) override;
    void onCleanup() override;

private:
    bool resolveFrame(const IopContext& ctx, glm::vec3& dirOut,
                      glm::vec3& neutralOut) const;

    std::vector<TopoDS_Face> m_faces;
    float m_angle = 10.0f;     // degrees
    int   m_axisIdx = 0;       // 0=Auto, 1=X, 2=Y, 3=Z (user convention)
    bool  m_flipBase = false;
};

// ─── Project Sketch ──────────────────────────────────────────────────────────
// Project a sketch onto the picked face along the sketch normal, then
// engrave or emboss the projected regions — text wrapped onto a cylinder.
class ProjectSketchController : public InteractiveOpController {
protected:
    const char* title() const override { return "Projection"; }
    int onBegin(const IopContext& ctx) override;
    std::unique_ptr<Operation> buildOp(const IopContext& ctx) override;
    void panelBody(const IopContext& ctx, bool& changed) override;
    void onCleanup() override;
    float panelWidth() const override { return 300.0f; }
    bool wantsLivePreview(const IopContext& ctx) const override;

    // Past this many projected regions the live preview is dropped (the
    // per-change boolean would freeze the UI); Confirm still applies it. Set
    // low deliberately — it got slow around ~30 regions on high-end hardware,
    // so weaker machines need the cutoff earlier.
    static constexpr int kPreviewRegionCap = 20;

private:
    int effectiveRegionCount(const IopContext& ctx) const;
    TopoDS_Face m_face;
    std::vector<int> m_sketchIds;   // combo choices, built at begin
    int  m_sketchPick = 0;          // index into m_sketchIds
    std::vector<int> m_regionFilter; // region subset from selection; empty = all
    float m_depth = 1.0f;
    int   m_mode = 0;               // 0=Engrave, 1=Emboss
    int   m_cycleMode = 0;          // 0=all, 1=loops only, 2=islands only
};

// ─── Scale Face ──────────────────────────────────────────────────────────────
// Pinch/flare the body toward a scaled copy of a planar END face. Carries
// the 2D gizmo frame the viewport draws and drags (red U / blue V arrows).
class ScaleFaceController : public InteractiveOpController {
public:
    // Gizmo access for the viewport overlay + drag handling.
    const glm::vec3& center() const { return m_center; }
    const glm::vec3& axisU() const { return m_axisU; }
    const glm::vec3& axisV() const { return m_axisV; }
    float halfU() const { return m_halfU; }
    float halfV() const { return m_halfV; }
    float pctU() const { return m_pctU; }
    float pctV() const { return m_pctV; }
    int dragAxis() const { return m_dragAxis; }
    void setDragAxis(int a) { m_dragAxis = a; }
    // Drag delta (percent) onto one axis; respects the Uniform link.
    void applyHandleDrag(int axis, float dPct, const IopContext& ctx);

protected:
    const char* title() const override { return "Scale Face"; }
    int onBegin(const IopContext& ctx) override;
    std::unique_ptr<Operation> buildOp(const IopContext& ctx) override;
    void panelBody(const IopContext& ctx, bool& changed) override;
    void onCleanup() override;

private:
    TopoDS_Face m_face;
    float m_pctU = 30.0f;
    float m_pctV = 30.0f;
    bool  m_uniform = true;
    float m_len = 10.0f;
    float m_lenMax = 100.0f;
    int   m_mode = 1; // 0=Extend, 1=Pinch

    glm::vec3 m_center{0.0f};
    glm::vec3 m_axisU{1.0f, 0.0f, 0.0f};
    glm::vec3 m_axisV{0.0f, 0.0f, 1.0f};
    float m_halfU = 10.0f;
    float m_halfV = 10.0f;
    int   m_dragAxis = -1;
};

} // namespace materializr

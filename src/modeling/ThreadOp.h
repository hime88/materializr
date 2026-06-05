#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <string>
#include <memory>

// Cuts a helical V-groove screw thread into a cylindrical face — external
// (boss/bolt: groove cut inward from the surface) or internal (hole/nut:
// groove cut outward into the wall), chosen from which side the material is
// on. The thread is pure derived geometry (axis + radius + extent + pitch +
// depth), no sub-shape references, so reloaded steps rehydrate fully
// editable: pitch / depth / handedness recompute via editStep.
class ThreadOp : public Operation {
public:
    ThreadOp();
    ~ThreadOp() override = default;

    // Parameters. The axis is anchored at the threaded span's V_min end with
    // its direction pointing along the cylinder (same convention as the
    // cylindrical-face detector); `length` extends from there.
    void setBody(int id) { m_bodyId = id; }
    void setAxis(const gp_Ax2& axis);
    void setRadius(double r) { m_radius = r; }
    void setLength(double l) { m_length = l; }
    void setPitch(double p) { m_pitch = p; }
    void setDepth(double d) { m_depth = d; }
    void setIsHole(bool h) { m_isHole = h; }
    void setRightHanded(bool rh) { m_rightHanded = rh; }

    // The heavy geometry (helix sweep + boolean cut), as a pure function of
    // the input body — no Document access, so the popup can run it on a
    // worker thread while the UI keeps pumping events. Returns a null shape
    // on failure. execute() uses it directly for the synchronous paths
    // (editStep recompute, redo).
    TopoDS_Shape buildResult(const TopoDS_Shape& body) const;
    // Hand execute() a result that buildResult already produced on a worker
    // thread; consumed on the next execute() so redo/edit recompute normally.
    void setPrecomputedResult(const TopoDS_Shape& s) { m_precomputed = s; }

    // Reflow propagation: a thread retargeted at a body the reflowed op
    // created (e.g. the other half of a split). Pure parameters copy over;
    // execution state resets so the clone recomputes fresh.
    std::unique_ptr<Operation> cloneForBody(int bodyId) const override {
        auto c = std::make_unique<ThreadOp>(*this);
        c->m_bodyId = bodyId;
        c->m_previousShape.Nullify();
        c->m_precomputed.Nullify();
        return c;
    }

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Thread"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "thread"; }
    OperationDiff captureDiff() const override;
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    int m_bodyId = -1;
    gp_Ax2 m_axis;
    double m_radius = 5.0;
    double m_length = 10.0;
    double m_pitch = 1.0;
    double m_depth = 0.6;
    bool m_isHole = false;     // false: external (boss), true: internal (hole)
    bool m_rightHanded = true;

    TopoDS_Shape m_previousShape; // for undo
    TopoDS_Shape m_precomputed;   // see setPrecomputedResult()

    // Axis components for (de)serialisation; m_axis is rebuilt from these in
    // execute() so a reloaded op recomputes identically.
    double m_axOX = 0, m_axOY = 0, m_axOZ = 0;
    double m_axDX = 0, m_axDY = 0, m_axDZ = 1;
    double m_axXX = 1, m_axXY = 0, m_axXZ = 0;
};

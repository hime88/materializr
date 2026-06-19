#include "BooleanOp.h"
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <cstdio>
#include <cstdlib>
#include <imgui.h>

BooleanOp::BooleanOp() = default;

void BooleanOp::setTargetBodyId(int id) {
    m_targetBodyId = id;
}

void BooleanOp::setToolBodyId(int id) {
    m_toolBodyId = id;
}

void BooleanOp::setMode(BooleanMode mode) {
    m_mode = mode;
}

bool BooleanOp::execute(Document& doc) {
    if (m_targetBodyId < 0 || m_toolBodyId < 0) {
        return false;
    }

    try {
        // Store previous shapes for undo
        m_previousTargetShape = doc.getBody(m_targetBodyId);
        m_previousToolShape = doc.getBody(m_toolBodyId);

        TopoDS_Shape resultShape;

        switch (m_mode) {
            case BooleanMode::Union: {
                BRepAlgoAPI_Fuse fuse(m_previousTargetShape, m_previousToolShape);
                fuse.Build();
                if (!fuse.IsDone()) {
                    return false;
                }
                resultShape = fuse.Shape();
                // Merge coplanar/tangent neighbouring faces so the union doesn't leave
                // a spurious seam edge between the two original bodies.
                try {
                    ShapeUpgrade_UnifySameDomain unifier(resultShape,
                                                        /*UnifyEdges*/ true,
                                                        /*UnifyFaces*/ true,
                                                        /*ConcatBSplines*/ true);
                    unifier.Build();
                    TopoDS_Shape unified = unifier.Shape();
                    if (!unified.IsNull()) resultShape = unified;
                } catch (...) { /* fall back to un-unified result */ }
                break;
            }
            case BooleanMode::Subtract: {
                BRepAlgoAPI_Cut cut(m_previousTargetShape, m_previousToolShape);
                cut.Build();
                if (!cut.IsDone()) {
                    return false;
                }
                resultShape = cut.Shape();
                break;
            }
            case BooleanMode::Intersect: {
                BRepAlgoAPI_Common common(m_previousTargetShape, m_previousToolShape);
                common.Build();
                if (!common.IsDone()) {
                    return false;
                }
                resultShape = common.Shape();
                break;
            }
        }

        // Update target body with the result
        doc.updateBody(m_targetBodyId, resultShape);

        // Remove the tool body
        doc.removeBody(m_toolBodyId);
        m_removedToolId = m_toolBodyId;

        return true;
    } catch (...) {
        return false;
    }
}

bool BooleanOp::undo(Document& doc) {
    try {
        // Restore target body to previous shape
        if (m_targetBodyId >= 0 && !m_previousTargetShape.IsNull()) {
            doc.updateBody(m_targetBodyId, m_previousTargetShape);
        }

        // Re-add the tool body that was removed — restore it under its ORIGINAL
        // id (not a fresh addBody id). editStep rolls a boolean back then
        // re-executes the steps above it; an upstream op that targets the tool
        // body (e.g. a fillet on it) must still find it by its old id, and the
        // boolean's own re-execute looks the tool up by m_toolBodyId. putBody
        // also pulls folder/colour/visibility back from the tombstone.
        if (m_removedToolId >= 0 && !m_previousToolShape.IsNull()) {
            doc.putBody(m_toolBodyId, m_previousToolShape, "Boolean Tool (restored)");
            m_removedToolId = -1;
        }

        return true;
    } catch (...) {
        return false;
    }
}

std::string BooleanOp::description() const {
    std::string modeStr;
    switch (m_mode) {
        case BooleanMode::Union:     modeStr = "Union"; break;
        case BooleanMode::Subtract:  modeStr = "Subtract"; break;
        case BooleanMode::Intersect: modeStr = "Intersect"; break;
    }
    return "Boolean " + modeStr + " (body " + std::to_string(m_targetBodyId) +
           " with body " + std::to_string(m_toolBodyId) + ")";
}

void BooleanOp::renderProperties() {
    ImGui::Text("Boolean Operation");
    ImGui::Separator();

    const char* modeItems[] = { "Union", "Subtract", "Intersect" };
    int modeIndex = static_cast<int>(m_mode);
    if (ImGui::Combo("Mode", &modeIndex, modeItems, 3)) {
        m_mode = static_cast<BooleanMode>(modeIndex);
    }

    ImGui::InputInt("Target Body ID", &m_targetBodyId);
    ImGui::InputInt("Tool Body ID", &m_toolBodyId);
}

OperationDiff BooleanOp::captureDiff() const {
    OperationDiff d;
    // The target mutates in place; the tool body is consumed by the boolean.
    if (m_targetBodyId >= 0 && !m_previousTargetShape.IsNull())
        d.modifiedBefore.push_back({m_targetBodyId, m_previousTargetShape});
    if (m_toolBodyId >= 0 && !m_previousToolShape.IsNull())
        d.deletedBefore.push_back({m_toolBodyId, m_previousToolShape});
    return d;
}

std::string BooleanOp::serializeParams() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "target=%d;tool=%d;mode=%d",
                  m_targetBodyId, m_toolBodyId, static_cast<int>(m_mode));
    return buf;
}

bool BooleanOp::deserializeParams(const std::string& blob) {
    // Tolerant key=value parser (same scheme as FilletOp/ChamferOp).
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "target") { m_targetBodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "tool")   { m_toolBodyId   = std::atoi(val.c_str()); any = true; }
        else if (key == "mode")   { int m = std::atoi(val.c_str());
                                    if (m >= 0 && m <= 2) m_mode = static_cast<BooleanMode>(m);
                                    any = true; }
        pos = end + 1;
    }
    return any;
}

bool BooleanOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_targetBodyId < 0 || m_toolBodyId < 0) return false;

    // Restore the pre-boolean shapes from the saved step diff: the target was
    // modified in place, the tool was consumed (deleted). Both are needed so
    // undo()/redo() and an editStep replay can roll the boolean back and re-run
    // it against the (possibly edited) upstream geometry.
    m_previousTargetShape.Nullify();
    m_previousToolShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_targetBodyId) { m_previousTargetShape = shp; break; }
    for (const auto& [id, shp] : state.deletedBefore)
        if (id == m_toolBodyId) { m_previousToolShape = shp; break; }
    if (m_previousTargetShape.IsNull() || m_previousToolShape.IsNull()) return false;

    // Post-execution bookkeeping: this step already consumed the tool body, so
    // undo() knows to restore it.
    m_removedToolId = m_toolBodyId;
    return true;
}

#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Shape.hxx>
#include <string>

enum class BooleanMode { Union, Subtract, Intersect };

class BooleanOp : public Operation {
public:
    BooleanOp();
    ~BooleanOp() override = default;

    // Parameters
    void setTargetBodyId(int id);
    void setToolBodyId(int id);
    void setMode(BooleanMode mode);

    // Getters
    int getTargetBodyId() const { return m_targetBodyId; }
    int getToolBodyId() const { return m_toolBodyId; }
    BooleanMode getMode() const { return m_mode; }

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Boolean"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "boolean"; }
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override {
        return {m_targetBodyId, m_toolBodyId};
    }
    // A boolean references its target/tool purely by body id (+ a mode enum),
    // so it can reload as a fully editable real op — recomputing from upstream
    // geometry on edit instead of baking a stale result over it (the bug where
    // editing a fillet upstream of a reloaded union silently did nothing).
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    int m_targetBodyId = -1;
    int m_toolBodyId = -1;
    BooleanMode m_mode = BooleanMode::Union;

    // For undo
    TopoDS_Shape m_previousTargetShape;
    TopoDS_Shape m_previousToolShape;
    int m_removedToolId = -1;
};

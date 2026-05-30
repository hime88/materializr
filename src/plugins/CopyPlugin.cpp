#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../core/SelectionManager.h"
#include "../modeling/CopyOp.h"

REGISTER_PLUGIN(Copy, [](materializr::PluginContext& ctx) {
    ctx.registerToolbarButton({"Duplicate", "Edit",
        materializr::SelectionContext::HasBodies, 800,
        [](materializr::PluginContext& ctx) {
            const auto& sel = ctx.selection().getSelection();
            if (!sel.empty() && sel[0].bodyId >= 0) {
                auto op = std::make_unique<CopyOp>();
                op->setSourceBodyId(sel[0].bodyId);
                // No offset — the duplicate lands exactly on the original.
                // Use the Move gizmo (now multi-body-aware) to reposition it.
                op->setOffset(0.0, 0.0, 0.0);
                if (ctx.history().pushOperation(std::move(op), ctx.document())) {
                    ctx.markMeshesDirty();
                }
            }
        }, nullptr,
        "Add an exact copy of the selected body on top of itself. Use Move to reposition."});
})

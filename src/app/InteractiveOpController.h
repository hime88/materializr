#pragma once
#include <functional>
#include <memory>
#include <TopoDS_Shape.hxx>

class Document;
class History;
class SelectionManager;
class Operation;

namespace materializr {

// Everything an interactive-op controller needs from the app, without
// seeing the Application god-class. Built on demand by Application.
struct IopContext {
    Document& doc;
    History& history;
    SelectionManager& selection;
    std::function<void()> markMeshesDirty;
};

// Base for "popup with live preview" modeling operations (Shell, Taper,
// Scale Face, …). Before this existed, every such feature deposited ~400
// lines across five Application files: a state block, four lifecycle
// methods, a panel function, a ToolAction case, an Esc-chain entry, and
// membership in TWO hand-maintained gizmo-suppression lists (forgetting a
// list entry was a recurring bug class). A controller subclass implements
// four small hooks; the lifecycle, the panel scaffold, and the
// suppression/Esc registration come from here.
//
// Lifecycle contract (identical to the pattern the hand-written ops used):
//   begin   — snapshot the target body, capture params from the selection
//             (onBegin), run the first preview.
//   update  — restore the snapshot, build a fresh Operation from current
//             params, execute it as the live preview; previewOk() records
//             whether it landed.
//   commit  — restore the snapshot, build the op once more, push it onto
//             History (which re-executes it cleanly), clear selection.
//   cancel  — restore the snapshot, drop all state.
class InteractiveOpController {
public:
    virtual ~InteractiveOpController() = default;

    bool begin(const IopContext& ctx);
    void update(const IopContext& ctx);
    void commit(const IopContext& ctx);
    void cancel(const IopContext& ctx);

    // Draws nothing when inactive. The scaffold handles window placement,
    // title, Confirm/Cancel buttons, and Enter/Esc keys.
    void renderPanel(const IopContext& ctx);

    bool active() const { return m_active; }
    bool previewOk() const { return m_previewOk; }

protected:
    virtual const char* title() const = 0;
    // Capture the selection into params. Return the target body id, or -1
    // to refuse to start (nothing usable selected).
    virtual int onBegin(const IopContext& ctx) = 0;
    // Build a fresh Operation from the current parameters (used for both
    // the live preview and the final commit).
    virtual std::unique_ptr<Operation> buildOp(const IopContext& ctx) = 0;
    // Parameter widgets (sliders, radios, status lines). Set `changed`
    // when a value moved so the scaffold re-runs the preview. Call
    // requestCommit() to commit from inside the body (e.g. Enter in a
    // text field).
    virtual void panelBody(const IopContext& ctx, bool& changed) = 0;
    virtual void onCleanup() {}
    virtual float panelWidth() const { return 260.0f; }
    // Override to suppress the per-change live preview when recomputing it would
    // freeze the UI (e.g. projecting a sketch with hundreds of regions). Commit
    // still builds + runs the op once. Default: always preview.
    virtual bool wantsLivePreview(const IopContext&) const { return true; }

    void requestCommit() { m_commitRequested = true; }

    int bodyId() const { return m_bodyId; }
    const TopoDS_Shape& snapshot() const { return m_snapshot; }

private:
    void cleanup();

    bool m_active = false;
    bool m_previewOk = false;
    bool m_commitRequested = false;
    int m_bodyId = -1;
    TopoDS_Shape m_snapshot;
};

} // namespace materializr

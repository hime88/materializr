#pragma once
#include <chrono>
#include <string>
#include <memory>
#include <vector>
#include <utility>
#include <TopoDS_Shape.hxx>

class Document; // forward declare

// A non-destructive description of the body changes an operation made, read
// straight from the op's stored undo data. Used to serialize history without
// calling undo()/execute() (which mutate op state and recompute geometry).
struct OperationDiff {
    std::vector<std::pair<int, TopoDS_Shape>> modifiedBefore; // body id -> shape BEFORE this op
    std::vector<int> created;                                 // ids this op created
    std::vector<std::pair<int, TopoDS_Shape>> deletedBefore;  // id -> shape this op deleted
};

class Operation {
public:
    virtual ~Operation() = default;

    virtual bool execute(Document& doc) = 0;
    virtual bool undo(Document& doc) = 0;
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;

    // For the properties panel — each operation renders its own ImGui editor
    virtual void renderProperties() = 0;

    // Unique type identifier for serialization
    virtual std::string typeId() const = 0;

    // True if this operation produced the given face (e.g. a fillet/chamfer
    // blend surface). Lets the UI map a clicked face back to the op that made
    // it so the user can re-edit it. Default: operations own no faces.
    virtual bool ownsFace(const TopoDS_Shape& /*face*/) const { return false; }

    // Report the body changes this op made, read from its stored undo data.
    // Non-destructive (unlike undo()). Default: no body changes (e.g. a sketch
    // edit). Used to persist the operation history in the project file.
    virtual OperationDiff captureDiff() const { return {}; }

    // True for a step reconstructed from a saved project: it replays stored
    // geometry for undo/redo but its parameters can't be re-edited. Lets the UI
    // flag such steps after a project is reopened.
    virtual bool isReloaded() const { return false; }

    // Serialise this op's input parameters (radii, distances, axis, etc.) as
    // a single-line opaque text blob. Empty default = nothing to save (sketch
    // edits, replay ops, simple ops without parameters). Read back by
    // deserializeParams; returns true on a clean parse. The format is up to
    // each op — keep it stable per typeId or version it inside the blob.
    virtual std::string serializeParams() const { return ""; }
    virtual bool deserializeParams(const std::string& /*blob*/) { return true; }

    // The body changes a step made, reconstructed from the saved project on
    // load. Handed to rehydrateFromReload() so a freshly-deserialized op can
    // restore the bookkeeping it would normally have built during execute()
    // (which ids it created / modified / deleted), without re-running the
    // geometry. `created` ids are exactly the bodies this step introduced;
    // `modifiedBefore` / `deletedBefore` carry the prior shape so undo() can
    // restore them.
    struct ReloadState {
        std::vector<int> created;
        std::vector<std::pair<int, TopoDS_Shape>> modifiedBefore;
        // The same modified bodies AFTER this step — sub-shape-referencing ops
        // resolve their generated-geometry indices (e.g. fillet blend faces
        // for click-to-edit) against the result shape.
        std::vector<std::pair<int, TopoDS_Shape>> modifiedAfter;
        std::vector<std::pair<int, TopoDS_Shape>> deletedBefore;
    };

    // Restore an op (already populated via deserializeParams) to its
    // post-execution state from a saved project, so undo()/redo() and
    // parameter re-editing work across sessions. `doc` is the live document
    // (sketch-sourced ops re-derive their profile from a persistent sketch id
    // through it). Return true if this op is now a fully editable reloaded op;
    // the default returns false, which tells the loader to fall back to a baked
    // ReplayOp (preserving prior behaviour for every op that hasn't opted in).
    // Ops that reference whole bodies by id (patterns) or a persistent sketch
    // (extrude/revolve) can implement this; ops that reference specific raw
    // sub-shapes (fillet edges, face push/pull) need persistent topological
    // naming first and should leave it unimplemented.
    virtual bool rehydrateFromReload(const ReloadState& /*state*/, Document& /*doc*/) { return false; }

    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool enabled) { m_enabled = enabled; }

    // Clone this operation retargeted at another body. Used by the
    // thread-last reflow to propagate a finishing pass onto bodies the
    // inserted op created (the second half of a split gets its own thread
    // cut). Default: not cloneable.
    virtual std::unique_ptr<Operation> cloneForBody(int /*bodyId*/) const {
        return nullptr;
    }

    // Body ids this op will read/modify when executed — known BEFORE
    // execution for ops that carry explicit target ids. Used by the
    // thread-last reflow: an op that touches a threaded body is inserted
    // BEFORE the trailing Thread steps so its boolean runs against clean
    // geometry (OCCT can't classify cuts along helical groove fields), and
    // the thread re-cuts parametrically afterwards. Empty (the default)
    // means "unknown / not boolean-sensitive" — no reflow.
    virtual std::vector<int> plannedBodyIds() const { return {}; }

    // Maintained by History: the serialised parameter set from this op's last
    // SUCCESSFUL execute. Used to roll a rejected edit back — the UI mutates
    // params in place before editStep runs, so "the values that worked" must
    // be captured at execute time, not edit time. Empty for ops without
    // parameter serialisation (no rescue possible — they fail-and-suspend).
    const std::string& lastGoodParams() const { return m_lastGoodParams; }
    void rememberGoodParams() { m_lastGoodParams = serializeParams(); }

    // Wall-clock time this op was constructed (or restored to a stored value
    // on project load). Used by the HistoryPanel to bucket steps into
    // "Today / Yesterday / <date>" collapsible groups so a 145-step project
    // is browsable without endless scrolling.
    std::chrono::system_clock::time_point timestamp() const { return m_timestamp; }
    void setTimestamp(std::chrono::system_clock::time_point t) { m_timestamp = t; }

protected:
    bool m_enabled = true;
    std::string m_lastGoodParams; // see lastGoodParams()
    std::chrono::system_clock::time_point m_timestamp = std::chrono::system_clock::now();
};

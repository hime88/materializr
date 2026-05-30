#pragma once
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

    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool enabled) { m_enabled = enabled; }

protected:
    bool m_enabled = true;
};

#pragma once
#include <functional>
#include <set>
#include <string>
#include <vector>

class Document;
class SelectionManager;
class History;

namespace materializr {

class ItemsPanel {
public:
    ItemsPanel();

    void setDocument(Document* doc);
    void setSelectionManager(SelectionManager* sel);
    void setHistory(History* hist);

    // True if the panel was hovered last frame — the touch input layer uses this
    // to arm long-press (right-click) over the panel's rows for their context
    // menus, the same way it does over the viewport.
    bool isHovered() const { return m_hovered; }
    // Called whenever a rename / non-history mutation happens so the
    // Application can mark the project dirty (otherwise closing without a
    // manual Save silently drops the change).
    void setDirtyCallback(std::function<void()> cb) { m_markDirty = std::move(cb); }
    // Called when the user picks "Export STL…" from a body's context menu.
    // ItemsPanel doesn't own STL I/O, so the Application wires this up to
    // route the click into its own per-body export flow.
    void setExportStlCallback(std::function<void(int)> cb) { m_exportStl = std::move(cb); }
    // Called when the user picks "Edit Sketch" from a sketch's right-click
    // menu. Routes to Application::editSketch which enters sketch mode on
    // that sketch — the only way to re-enter a sketch that was created in
    // a previous session.
    void setEditSketchCallback(std::function<void(int)> cb) { m_editSketch = std::move(cb); }
    // Called when the user picks "Export as SVG…" from a sketch's right-click
    // menu. Routes to Application::exportSketchAsSvg (1:1-mm polyline SVG for
    // laser / 2.5D CNC). Sketch-only by design — a File-menu export would also
    // catch non-planar geometry, which SVG can't represent.
    void setExportSketchSvgCallback(std::function<void(int)> cb) { m_exportSketchSvg = std::move(cb); }
    // Called when the user picks "Duplicate Sketch" — makes an independent copy.
    // Routes to Application::duplicateSketch.
    void setDuplicateSketchCallback(std::function<void(int)> cb) { m_duplicateSketch = std::move(cb); }
    // Called when the user picks "Combine sketches" — merges the selected
    // coplanar sketches into the first. Routes to Application::combineSketches.
    void setCombineSketchesCallback(std::function<void(const std::vector<int>&)> cb) {
        m_combineSketches = std::move(cb);
    }
    // Called when the user picks "Rotate About Axis…" from a construction
    // plane's right-click menu. Routes to Application, which opens the
    // rotate-plane-about-axis popup targeting the given plane id.
    void setRotatePlaneCallback(std::function<void(int)> cb) { m_rotatePlane = std::move(cb); }

    // Returns true if a body was deleted (caller must rebuild meshes)
    bool render();

private:
    Document* m_document = nullptr;
    SelectionManager* m_selection = nullptr;
    History* m_history = nullptr;
    std::function<void()> m_markDirty;
    std::function<void(int)> m_exportStl;
    std::function<void(int)> m_editSketch;
    std::function<void(int)> m_exportSketchSvg;
    std::function<void(int)> m_duplicateSketch;
    std::function<void(const std::vector<int>&)> m_combineSketches;
    std::function<void(int)> m_rotatePlane;
    int m_renamingId = -1;
    char m_renameBuffer[128] = {};
    // Selected body ids, rebuilt once at the top of render() — renderBodyRow
    // reads it per row instead of rescanning the whole selection.
    std::set<int> m_selectedBodyIdsFrame;
    bool m_showBodies = true;
    bool m_showSketches = true;
    bool m_showPlanes = true;
    bool m_bodyDeleted = false;
    bool m_hovered = false;   // panel hovered last frame (for touch long-press arming)
    // Auto-scroll: when the selected body / sketch changes (e.g. a viewport
    // pick), scroll its row into view. -1 = no pending scroll.
    int m_lastSelectedBodyId = -1;
    int m_lastSelectedSketchId = -1;
    // Anchor body for shift-click range selection in the Items panel. Set
    // whenever a plain click (no Ctrl, no Shift) selects a body.
    int m_anchorBodyId = -1;
    // "New folder…" submenu prompts for a name — kept across frames until the
    // user confirms / cancels via Enter / Esc. The body being moved is
    // remembered so we can assign it once the folder exists.
    bool m_newFolderPopupOpen = false;
    bool m_newFolderFocusInput = false; // first-frame focus only — else the
                                        // input steals focus from Create/Cancel
                                        // every frame and the popup locks up.
    char m_newFolderName[128] = {};
    // Bodies to move into the newly-created folder once its name is confirmed.
    // Empty = create the folder empty (e.g. "+ Folder" header button).
    std::vector<int> m_newFolderForBodyIds;

    // Renders one body row (visibility + name + colour + context menu).
    // Pulled out of render() so it can be called both at the root level and
    // inside each folder's expanded content.
    bool renderBodyRow(int id, bool& colorChanged);
};

} // namespace materializr

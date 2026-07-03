#pragma once
#include <set>

class History;
class Document;

namespace materializr {
class EventBus;
}

namespace materializr {

class HistoryPanel {
public:
    HistoryPanel();

    void setHistory(History* history);
    void setDocument(Document* doc);
    void setEventBus(EventBus* bus) { m_eventBus = bus; }

    // Lock history mutation (undo/redo buttons) while a live tool preview
    // owns the top of the history — an outside undo during a preview pops
    // the preview op, and the preview's next frame then pops the user's
    // last COMMITTED step (which the following push erases for good).
    void setHistoryLocked(bool locked) { m_historyLocked = locked; }

    // Render the panel. Returns true if history was modified (undo/redo/edit).
    bool render();

    // Open a given step in the inline editor (e.g. when the user clicks the face
    // a fillet/chamfer produced). -1 closes the editor.
    void setEditingStep(int step) { m_editingStep = step; m_showProperties = (step >= 0); }
    int getEditingStep() const { return m_editingStep; }

    // Soft-highlight a step (distinct from editing) so the list shows which step
    // owns the sketch element currently selected in the viewport. -1 = none.
    void setHighlightStep(int step) { m_highlightStep = step; }
    int getHighlightStep() const { return m_highlightStep; }

private:
    History* m_history = nullptr;
    bool m_historyLocked = false;
    Document* m_document = nullptr;
    materializr::EventBus* m_eventBus = nullptr;
    int m_editingStep = -1;
    int m_highlightStep = -1; // step owning the viewport-selected sketch element
    bool m_showProperties = false;
    bool m_deleteConflict = false; // last delete was blocked by a dependent step
    // Steps with same typeId in a row collapse into a single expandable group
    // header. The set holds the START step index of each group the user has
    // currently collapsed. Groups default to expanded; the user clicks ▼ / ▶
    // to toggle. Keyed by start index — adding / deleting steps shifts the
    // run and the saved state effectively resets, which is acceptable.
    std::set<int> m_collapsedGroupStarts;
    // Set of group start indices we've already auto-classified once. Lets us
    // give historical (non-today) date buckets a collapsed default the first
    // frame they appear, while letting the user override afterwards.
    std::set<int> m_seenGroupStarts;
};

} // namespace materializr

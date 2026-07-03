#include "UiTheme.h"
#include "ItemsPanel.h"
#include "../touch_mode.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../core/SelectionManager.h"
#include "../modeling/DeleteOp.h"
#include <imgui.h>
#include <glm/glm.hpp>
#include <cstring>
#include <cstdio>
#include <memory>
#include <string>
#include <algorithm>

namespace materializr {

ItemsPanel::ItemsPanel() = default;

void ItemsPanel::setDocument(Document* doc) {
    m_document = doc;
}

void ItemsPanel::setSelectionManager(SelectionManager* sel) {
    m_selection = sel;
}

void ItemsPanel::setHistory(History* hist) {
    m_history = hist;
}

bool ItemsPanel::render() {
    m_bodyDeleted = false;
    ImGui::Begin("Items", nullptr, ImGuiWindowFlags_NoCollapse);
    // AllowWhenBlockedByActiveItem: a held body row is the "active item", which
    // would otherwise make IsWindowHovered() report false — exactly during the
    // long-press we need to detect to arm its context menu.
    m_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                       ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    if (!m_document) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No document loaded.");
        ImGui::End();
        return false;
    }

    bool colorChanged = false; // a body colour edit also needs a mesh rebuild

    // Selected-body ids collected ONCE per frame — renderBodyRow used to
    // rescan the whole selection per row (O(bodies × selection) per frame).
    m_selectedBodyIdsFrame.clear();
    if (m_selection)
        for (const auto& e : m_selection->getSelection())
            if (e.type == SelectionType::Body && e.bodyId >= 0)
                m_selectedBodyIdsFrame.insert(e.bodyId);

    // Filter toggles at top
    ImGui::TextColored(materializr::accentText(), "Filter");
    ImGui::Separator();

    // Touch mode wraps the third toggle to a second row so the filter strip needs
    // only the width of two buttons (the 2x font otherwise pushes the whole right
    // panel wide). Desktop keeps all three on one line.
    const bool filterWrap = materializr::touchMode();
    if (ImGui::Button(m_showBodies ? "[Bodies]" : " Bodies ")) {
        m_showBodies = !m_showBodies;
    }
    ImGui::SameLine();
    if (ImGui::Button(m_showSketches ? "[Sketches]" : " Sketches ")) {
        m_showSketches = !m_showSketches;
    }
    if (!filterWrap) ImGui::SameLine();
    if (ImGui::Button(m_showPlanes ? "[Construction]" : " Construction ")) {
        m_showPlanes = !m_showPlanes;
    }

    ImGui::Separator();

    // Bodies section
    if (m_showBodies) {
        ImGui::TextColored(materializr::accentText(), "Bodies");
        // "+ New folder" button at the section header creates an empty folder
        // (bodies join via right-click → Move to folder…).
        ImGui::SameLine();
        if (ImGui::SmallButton("+ Folder")) {
            m_newFolderForBodyIds.clear();
            m_newFolderName[0] = '\0';
            m_newFolderPopupOpen = true;
        }

        // 1) Folders, each with their member bodies indented underneath.
        for (int folderId : m_document->getAllFolderIds()) {
            ImGui::PushID(2000000 + folderId); // namespace away from body ids

            // Visibility checkbox (cascades to members in Document).
            bool fvis = m_document->isFolderVisible(folderId);
            if (ImGui::Checkbox("##fvis", &fvis)) {
                m_document->setFolderVisible(folderId, fvis);
                colorChanged = true; // forces mesh rebuild
            }
            ImGui::SameLine();

            // Tree-node arrow + name. Use TreeNodeEx so we can pre-set the
            // expanded state from the Document (persisted across frames).
            // Deliberately NO SpanAvailWidth — it makes the whole row a tree-
            // node hit target, which silently swallowed clicks on the colour
            // swatch (popup never appeared).
            ImGuiTreeNodeFlags fflags = ImGuiTreeNodeFlags_OpenOnArrow;
            bool wantExpanded = m_document->isFolderExpanded(folderId);
            ImGui::SetNextItemOpen(wantExpanded, ImGuiCond_Always);

            std::string fname = m_document->getFolderName(folderId);
            // Reserve room for the colour swatch on the right.
            float swatchW = ImGui::GetFrameHeight();
            float nameW = ImGui::GetContentRegionAvail().x - swatchW - 6.0f;
            bool open = ImGui::TreeNodeEx(("##fnode" + std::to_string(folderId)).c_str(),
                                          fflags | (wantExpanded ? ImGuiTreeNodeFlags_DefaultOpen : 0));
            if (open != wantExpanded) {
                m_document->setFolderExpanded(folderId, open);
            }
            // Folder label, overlaid on the tree-node line.
            ImGui::SameLine();
            const int renameKey = 2000000 + folderId;
            if (m_renamingId == renameKey) {
                ImGui::SetNextItemWidth(nameW);
                ImGui::SetKeyboardFocusHere();
                bool committed = ImGui::InputText("##frename", m_renameBuffer,
                                                  sizeof(m_renameBuffer),
                                                  ImGuiInputTextFlags_EnterReturnsTrue |
                                                  ImGuiInputTextFlags_AutoSelectAll);
                bool clickedOff = !ImGui::IsItemActive() && ImGui::IsMouseClicked(0);
                if (committed || clickedOff) {
                    m_document->setFolderName(folderId, m_renameBuffer);
                    if (m_markDirty) m_markDirty();
                    m_renamingId = -1;
                } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    m_renamingId = -1;
                }
            } else {
                ImGui::TextUnformatted(fname.c_str());
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    m_renamingId = renameKey;
                    std::strncpy(m_renameBuffer, fname.c_str(), sizeof(m_renameBuffer) - 1);
                    m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
                }
                if (ImGui::BeginPopupContextItem("FolderContextMenu")) {
                    if (ImGui::MenuItem("Rename")) {
                        m_renamingId = renameKey;
                        std::strncpy(m_renameBuffer, fname.c_str(), sizeof(m_renameBuffer) - 1);
                        m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
                    }
                    if (ImGui::MenuItem("Delete folder (keeps bodies)")) {
                        m_document->removeFolder(folderId);
                        ImGui::EndPopup();
                        ImGui::PopID();
                        colorChanged = true;
                        continue;
                    }
                    ImGui::EndPopup();
                }
            }

            // Colour swatch.
            ImGui::SameLine();
            glm::vec3 fcol = m_document->getFolderColor(folderId);
            if (ImGui::ColorEdit3("##fcolor", &fcol.x,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
                    ImGuiColorEditFlags_PickerHueWheel)) {
                m_document->setFolderColor(folderId, fcol);
                colorChanged = true;
            }

            // Member bodies, only when expanded.
            if (open) {
                ImGui::Indent();
                for (int bid : m_document->getBodiesInFolder(folderId)) {
                    if (!renderBodyRow(bid, colorChanged)) {
                        // Body was deleted; member list is stale. Bail to
                        // outer loop, will re-fetch next frame.
                        ImGui::Unindent();
                        ImGui::TreePop();
                        ImGui::PopID();
                        goto end_bodies;
                    }
                }
                ImGui::Unindent();
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        // 2) Root-level bodies (folderId == -1).
        for (int id : m_document->getBodiesInFolder(-1)) {
            if (!renderBodyRow(id, colorChanged)) goto end_bodies;
        }
        end_bodies:;

        // "New folder…" name prompt (kept as a modal popup so it survives the
        // frame the user clicked the menu item — ImGui menus auto-close).
        if (m_newFolderPopupOpen) {
            ImGui::OpenPopup("New Folder##itemspanel");
            m_newFolderPopupOpen = false;
            m_newFolderFocusInput = true;
        }
        if (ImGui::BeginPopupModal("New Folder##itemspanel", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Folder name:");
            if (m_newFolderFocusInput) {
                ImGui::SetKeyboardFocusHere();
                m_newFolderFocusInput = false;
            }
            bool committed = ImGui::InputText("##newfoldername",
                                              m_newFolderName,
                                              sizeof(m_newFolderName),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
            bool createClicked = ImGui::Button("Create");
            ImGui::SameLine();
            bool cancelClicked = ImGui::Button("Cancel");
            bool escPressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            if (committed || createClicked) {
                if (m_newFolderName[0] != '\0') {
                    int newId = m_document->addFolder(m_newFolderName);
                    for (int bid : m_newFolderForBodyIds) {
                        m_document->setBodyFolder(bid, newId);
                    }
                    if (m_markDirty) m_markDirty();
                }
                m_newFolderForBodyIds.clear();
                ImGui::CloseCurrentPopup();
            } else if (cancelClicked || escPressed) {
                m_newFolderForBodyIds.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // Sketches section
    if (m_showSketches) {
        ImGui::Separator();
        ImGui::TextColored(materializr::accentText(), "Sketches");

        std::vector<int> sketchIds = m_document->getAllSketchIds();
        if (sketchIds.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(none)");
        }
        for (int id : sketchIds) {
            ImGui::PushID(1000000 + id); // namespace away from body ids

            bool visible = m_document->isSketchVisible(id);
            if (ImGui::Checkbox("##svis", &visible)) {
                m_document->setSketchVisible(id, visible);
                // NOT colorChanged: sketch visibility is read live by the
                // viewport's sketch loop every frame — setting the flag here
                // forced a FULL re-tessellation of every visible body (a
                // multi-second stall on a heavy project) for a toggle that
                // doesn't touch body meshes at all.
            }
            ImGui::SameLine();

            bool isSelected = false;
            if (m_selection) {
                const auto& sel = m_selection->getSelection();
                for (const auto& e : sel) {
                    if (e.type == SelectionType::Sketch && e.sketchId == id) {
                        isSelected = true; break;
                    }
                }
            }

            // Rename ids are namespaced (1000000 + id) so they don't collide with
            // body rename ids in the shared m_renamingId.
            const int renameKey = 1000000 + id;
            auto beginRename = [&]() {
                m_renamingId = renameKey;
                std::string n = m_document->getSketchName(id);
                std::strncpy(m_renameBuffer, n.c_str(), sizeof(m_renameBuffer) - 1);
                m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
            };

            bool deleted = false;
            if (m_renamingId == renameKey) {
                ImGui::SetKeyboardFocusHere();
                bool committed = ImGui::InputText("##srename", m_renameBuffer,
                                                  sizeof(m_renameBuffer),
                                                  ImGuiInputTextFlags_EnterReturnsTrue |
                                                  ImGuiInputTextFlags_AutoSelectAll);
                bool clickedOff = !ImGui::IsItemActive() && ImGui::IsMouseClicked(0);
                if (committed || clickedOff) {
                    m_document->setSketchName(id, m_renameBuffer);
                    if (m_markDirty) m_markDirty();
                    m_renamingId = -1;
                } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    m_renamingId = -1;
                }
            } else {
                std::string name = m_document->getSketchName(id);
                if (isSelected && id != m_lastSelectedSketchId) {
                    ImGui::SetScrollHereY(0.5f);
                }
                if (ImGui::Selectable(name.c_str(), isSelected)) {
                    if (m_selection) {
                        SelectionEntry entry;
                        entry.type = SelectionType::Sketch;
                        entry.sketchId = id;
                        m_selection->select(entry);
                    }
                }

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    beginRename();
                }

                if (ImGui::BeginPopupContextItem("SketchContextMenu")) {
                    if (ImGui::MenuItem("Edit Sketch")) {
                        if (m_editSketch) m_editSketch(id);
                    }
                    if (ImGui::MenuItem("Export as SVG…")) {
                        if (m_exportSketchSvg) m_exportSketchSvg(id);
                    }
                    // Make an independent copy — edit it freely (e.g. resize
                    // holes) to derive a same-layout variant without touching
                    // this sketch or any body built from it.
                    if (ImGui::MenuItem("Duplicate Sketch")) {
                        if (m_duplicateSketch) m_duplicateSketch(id);
                    }
                    // Fold every OTHER coplanar sketch into this one (the app
                    // filters to the ones sharing this sketch's plane). Only
                    // offered when there's more than one sketch to fold.
                    if (sketchIds.size() > 1 &&
                        ImGui::MenuItem("Combine coplanar into this")) {
                        if (m_combineSketches) {
                            std::vector<int> ids{ id };
                            for (int other : sketchIds)
                                if (other != id) ids.push_back(other);
                            m_combineSketches(ids);
                        }
                    }
                    if (ImGui::MenuItem("Rename")) {
                        beginRename();
                    }
                    if (ImGui::MenuItem("Delete")) {
                        m_document->removeSketch(id);
                        if (m_selection) m_selection->clear();
                        m_renamingId = -1;
                        deleted = true;
                    }
                    ImGui::EndPopup();
                }
            }

            ImGui::PopID();
            if (deleted) break; // sketchIds is now stale
        }
    }

    // Construction (planes + axes) section
    if (m_showPlanes) {
        ImGui::Separator();
        ImGui::TextColored(materializr::accentText(), "Construction Planes");

        std::vector<int> planeIds = m_document->getAllPlaneIds();
        if (planeIds.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(none)");
        }
        for (int id : planeIds) {
            ImGui::PushID(2000000 + id); // namespace away from body/sketch ids

            bool visible = m_document->isPlaneVisible(id);
            if (ImGui::Checkbox("##pvis", &visible)) {
                m_document->setPlaneVisible(id, visible);
                if (m_markDirty) m_markDirty();
            }
            ImGui::SameLine();

            bool isSelected = false;
            if (m_selection) {
                for (const auto& e : m_selection->getSelection()) {
                    if (e.type == SelectionType::Plane && e.planeId == id) {
                        isSelected = true; break;
                    }
                }
            }

            // Rename ids namespaced (4000000 + id) so they don't collide with
            // body / sketch / axis rename ids in the shared m_renamingId.
            const int renameKey = 4000000 + id;
            auto beginRename = [&]() {
                m_renamingId = renameKey;
                std::string n = m_document->getPlaneName(id);
                std::strncpy(m_renameBuffer, n.c_str(), sizeof(m_renameBuffer) - 1);
                m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
            };

            if (m_renamingId == renameKey) {
                ImGui::SetKeyboardFocusHere();
                bool committed = ImGui::InputText("##prename", m_renameBuffer,
                                                  sizeof(m_renameBuffer),
                                                  ImGuiInputTextFlags_EnterReturnsTrue |
                                                  ImGuiInputTextFlags_AutoSelectAll);
                bool clickedOff = !ImGui::IsItemActive() && ImGui::IsMouseClicked(0);
                if (committed || clickedOff) {
                    m_document->setPlaneName(id, m_renameBuffer);
                    if (m_markDirty) m_markDirty();
                    m_renamingId = -1;
                } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    m_renamingId = -1;
                }
            } else {
                const auto* p = m_document->getPlane(id);
                std::string label = p ? p->name : std::string("Plane ") + std::to_string(id);
                if (ImGui::Selectable(label.c_str(), isSelected)) {
                    if (m_selection) {
                        SelectionEntry entry;
                        entry.type = SelectionType::Plane;
                        entry.planeId = id;
                        m_selection->select(entry);
                    }
                }

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    beginRename();
                }

                if (ImGui::BeginPopupContextItem("PlaneCtx")) {
                    if (ImGui::MenuItem("Rename")) {
                        beginRename();
                    }
                    if (ImGui::MenuItem("Flip Normal")) {
                        m_document->flipPlaneNormal(id);
                        if (m_markDirty) m_markDirty();
                    }
                    if (ImGui::MenuItem("Rotate About Axis...")) {
                        if (m_rotatePlane) m_rotatePlane(id);
                    }
                    if (ImGui::MenuItem("Delete")) {
                        m_document->removePlane(id);
                        if (m_selection) m_selection->clear();
                        m_renamingId = -1;
                        ImGui::EndPopup();
                        ImGui::PopID();
                        break; // planeIds is stale
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::TextColored(materializr::accentText(), "Construction Axes");

        std::vector<int> axisIds = m_document->getAllAxisIds();
        if (axisIds.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(none)");
        }
        for (int id : axisIds) {
            ImGui::PushID(3000000 + id);

            bool visible = m_document->isAxisVisible(id);
            if (ImGui::Checkbox("##avis", &visible)) {
                m_document->setAxisVisible(id, visible);
                if (m_markDirty) m_markDirty();
            }
            ImGui::SameLine();

            bool isSelected = false;
            if (m_selection) {
                for (const auto& e : m_selection->getSelection()) {
                    if (e.type == SelectionType::Axis && e.axisId == id) {
                        isSelected = true; break;
                    }
                }
            }

            // Rename ids namespaced (5000000 + id) so they don't collide with
            // body / sketch / plane rename ids in the shared m_renamingId.
            const int renameKey = 5000000 + id;
            auto beginRename = [&]() {
                m_renamingId = renameKey;
                std::string n = m_document->getAxisName(id);
                std::strncpy(m_renameBuffer, n.c_str(), sizeof(m_renameBuffer) - 1);
                m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
            };

            if (m_renamingId == renameKey) {
                ImGui::SetKeyboardFocusHere();
                bool committed = ImGui::InputText("##arename", m_renameBuffer,
                                                  sizeof(m_renameBuffer),
                                                  ImGuiInputTextFlags_EnterReturnsTrue |
                                                  ImGuiInputTextFlags_AutoSelectAll);
                bool clickedOff = !ImGui::IsItemActive() && ImGui::IsMouseClicked(0);
                if (committed || clickedOff) {
                    m_document->setAxisName(id, m_renameBuffer);
                    if (m_markDirty) m_markDirty();
                    m_renamingId = -1;
                } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    m_renamingId = -1;
                }
            } else {
                const auto* a = m_document->getAxis(id);
                std::string label = a ? a->name : std::string("Axis ") + std::to_string(id);
                if (ImGui::Selectable(label.c_str(), isSelected)) {
                    if (m_selection) {
                        SelectionEntry entry;
                        entry.type = SelectionType::Axis;
                        entry.axisId = id;
                        m_selection->select(entry);
                    }
                }

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    beginRename();
                }

                if (ImGui::BeginPopupContextItem("AxisCtx")) {
                    if (ImGui::MenuItem("Rename")) {
                        beginRename();
                    }
                    if (ImGui::MenuItem("Flip Direction")) {
                        m_document->flipAxisDirection(id);
                        if (m_markDirty) m_markDirty();
                    }
                    if (ImGui::MenuItem("Delete")) {
                        m_document->removeAxis(id);
                        if (m_selection) m_selection->clear();
                        m_renamingId = -1;
                        ImGui::EndPopup();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::PopID();
        }
    }

    // Bottom: counts
    ImGui::Separator();
    char countText[128];
    std::snprintf(countText, sizeof(countText), "Bodies: %d", m_document->bodyCount());
    ImGui::Text("%s", countText);

    // Remember which body / sketch is the primary selection so the next frame
    // knows whether selection changed (= time to auto-scroll). Use -1 when
    // nothing is selected so the next pick of any id triggers a scroll.
    m_lastSelectedBodyId   = -1;
    m_lastSelectedSketchId = -1;
    if (m_selection) {
        for (const auto& entry : m_selection->getSelection()) {
            if (entry.type == SelectionType::Body && entry.bodyId >= 0) {
                m_lastSelectedBodyId = entry.bodyId; break;
            }
        }
        for (const auto& entry : m_selection->getSelection()) {
            if (entry.type == SelectionType::Sketch && entry.sketchId >= 0) {
                m_lastSelectedSketchId = entry.sketchId; break;
            }
        }
    }

    ImGui::End();
    return m_bodyDeleted || colorChanged;
}

// One body row. Pulled out of render() so it can run at the root level OR
// inside a folder's expanded contents (indented by the caller). Returns false
// if this body was deleted via its context menu — caller must stop iterating
// because the body list is now stale.
bool ItemsPanel::renderBodyRow(int id, bool& colorChanged) {
    ImGui::PushID(id);

    bool visible = m_document->isBodyVisible(id);
    if (ImGui::Checkbox("##vis", &visible)) {
        m_document->setBodyVisible(id, visible);
        colorChanged = true;
    }
    ImGui::SameLine();

    const bool isSelected = m_selectedBodyIdsFrame.count(id) > 0;

    if (m_renamingId == id) {
        ImGui::SetKeyboardFocusHere();
        bool committed = ImGui::InputText("##rename", m_renameBuffer,
                                          sizeof(m_renameBuffer),
                                          ImGuiInputTextFlags_EnterReturnsTrue |
                                          ImGuiInputTextFlags_AutoSelectAll);
        bool clickedOff = !ImGui::IsItemActive() && ImGui::IsMouseClicked(0);
        if (committed || clickedOff) {
            m_document->setBodyName(id, m_renameBuffer);
            if (m_markDirty) m_markDirty();
            m_renamingId = -1;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_renamingId = -1;
        }
        ImGui::PopID();
        return true;
    }

    float swatchW = ImGui::GetFrameHeight();
    float nameW = ImGui::GetContentRegionAvail().x - swatchW - 6.0f;
    std::string name = m_document->getBodyName(id);
    // Auto-scroll into view ONLY when this is the lone selected body and it's
    // newly selected (typically a viewport pick changing selection). With
    // multi-select every selected row would otherwise re-issue SetScrollHereY,
    // causing the panel to ping-pong between the first and last selected rows.
    if (isSelected && id != m_lastSelectedBodyId &&
        m_selection && m_selection->selectedBodyCount() == 1) {
        ImGui::SetScrollHereY(0.5f);
    }
    if (ImGui::Selectable(name.c_str(), isSelected, 0,
                          ImVec2(nameW > 1.0f ? nameW : 0.0f, 0.0f))) {
        if (m_selection) {
            ImGuiIO& io = ImGui::GetIO();
            auto makeEntry = [this](int bid) {
                SelectionEntry e;
                e.type = SelectionType::Body;
                e.bodyId = bid;
                try { e.shape = m_document->getBody(bid); } catch (...) {}
                return e;
            };
            if (io.KeyShift && m_anchorBodyId >= 0) {
                // Range select from the anchor to here using display order:
                // folder members first (in folder iteration order), then root.
                std::vector<int> displayOrder;
                for (int fid : m_document->getAllFolderIds()) {
                    for (int b : m_document->getBodiesInFolder(fid))
                        displayOrder.push_back(b);
                }
                for (int b : m_document->getBodiesInFolder(-1))
                    displayOrder.push_back(b);
                int a = -1, b = -1;
                for (int i = 0; i < static_cast<int>(displayOrder.size()); ++i) {
                    if (displayOrder[i] == m_anchorBodyId) a = i;
                    if (displayOrder[i] == id)             b = i;
                }
                if (a >= 0 && b >= 0) {
                    if (a > b) std::swap(a, b);
                    m_selection->clear();
                    for (int i = a; i <= b; ++i)
                        m_selection->addToSelection(makeEntry(displayOrder[i]));
                    m_selection->setNavigationOnly(true);
                }
            } else if (io.KeyCtrl) {
                // Toggle this body in/out of the selection without disturbing
                // the others. Keeps the anchor where it was.
                m_selection->toggleSelection(makeEntry(id));
                m_selection->setNavigationOnly(true);
            } else {
                // Plain click → single-select, this body becomes the anchor.
                m_selection->select(makeEntry(id));
                m_selection->setNavigationOnly(true);
                m_anchorBodyId = id;
            }
        }
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        m_renamingId = id;
        std::string bodyName = m_document->getBodyName(id);
        std::strncpy(m_renameBuffer, bodyName.c_str(), sizeof(m_renameBuffer) - 1);
        m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
    }

    bool deleted = false;
    if (ImGui::BeginPopupContextItem("BodyContextMenu")) {
        if (ImGui::MenuItem("Rename")) {
            m_renamingId = id;
            std::string bodyName = m_document->getBodyName(id);
            std::strncpy(m_renameBuffer, bodyName.c_str(), sizeof(m_renameBuffer) - 1);
            m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
        }
        if (ImGui::MenuItem("Delete")) {
            if (m_history) {
                auto op = std::make_unique<DeleteOp>();
                op->setBodyId(id);
                m_history->pushOperation(std::move(op), *m_document);
            } else {
                m_document->removeBody(id);
            }
            if (m_selection) m_selection->clear();
            m_renamingId = -1;
            m_bodyDeleted = true;
            deleted = true;
        }
        if (!deleted && ImGui::MenuItem("Isolate")) {
            for (int otherId : m_document->getAllBodyIds()) {
                m_document->setBodyVisible(otherId, otherId == id);
            }
            colorChanged = true;
        }
        // The way back from Isolate in one click (mirrors the viewport
        // context menu) — beats re-ticking every checkbox above.
        if (!deleted && ImGui::MenuItem("Show All Bodies")) {
            for (int otherId : m_document->getAllBodyIds()) {
                m_document->setBodyVisible(otherId, true);
            }
            colorChanged = true;
        }
        // Per-body STL export: dumps only this body's mesh to a file the
        // user picks. Default filename = the body's current name (see
        // Application::exportBodyAsStl). Wired via callback so ItemsPanel
        // doesn't depend on the STL I/O module.
        if (!deleted && m_exportStl && ImGui::MenuItem("Export STL…")) {
            m_exportStl(id);
        }
        // Move-to-folder submenu. If the right-clicked body is part of a
        // multi-selection, the action moves EVERY selected body at once;
        // otherwise it just moves this one. Lists existing folders + a "(root)"
        // entry (when at least one target is currently in a folder) + a
        // "New folder…" prompt that drops all the targets into the new folder.
        if (!deleted && ImGui::BeginMenu("Move to folder")) {
            std::vector<int> targets;
            bool multi = false;
            if (m_selection) {
                for (const auto& e : m_selection->getSelection()) {
                    if (e.type == SelectionType::Body && e.bodyId >= 0)
                        targets.push_back(e.bodyId);
                }
                multi = targets.size() > 1 &&
                        std::find(targets.begin(), targets.end(), id) != targets.end();
            }
            if (!multi) { targets.clear(); targets.push_back(id); }
            // Are any of the targets currently in a folder?
            bool anyInFolder = false;
            for (int t : targets) {
                if (m_document->getBodyFolder(t) >= 0) { anyInFolder = true; break; }
            }
            const char* moveLabel = multi ? "(root — no folder) — all selected"
                                          : "(root — no folder)";
            if (anyInFolder && ImGui::MenuItem(moveLabel)) {
                for (int t : targets) m_document->setBodyFolder(t, -1);
                if (m_markDirty) m_markDirty();
            }
            for (int fid : m_document->getAllFolderIds()) {
                std::string label = m_document->getFolderName(fid);
                if (multi) label += " — all selected";
                if (ImGui::MenuItem(label.c_str())) {
                    for (int t : targets) m_document->setBodyFolder(t, fid);
                    if (m_markDirty) m_markDirty();
                }
            }
            ImGui::Separator();
            const char* newLabel = multi ? "New folder… (all selected)"
                                         : "New folder…";
            if (ImGui::MenuItem(newLabel)) {
                m_newFolderForBodyIds = targets;
                m_newFolderName[0] = '\0';
                m_newFolderPopupOpen = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    glm::vec3 col = m_document->getBodyColor(id);
    if (ImGui::ColorEdit3("##bodycolor", &col.x,
            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
            ImGuiColorEditFlags_PickerHueWheel)) {
        m_document->setBodyColor(id, col);
        colorChanged = true;
    }

    ImGui::PopID();
    return !deleted;
}

} // namespace materializr

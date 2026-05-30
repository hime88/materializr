#pragma once
#include <string>
#include <vector>
#include <utility>
#include <TopoDS_Shape.hxx>

class Document;
class History;

namespace materializr {

struct ProjectSaveResult {
    bool success = false;
    std::string errorMessage;
};

struct ProjectLoadResult {
    bool success = false;
    std::string errorMessage;
    int bodiesLoaded = 0;
};

// One persisted operation: identity/labels for the History panel plus a body
// diff (changed bodies' resulting shapes + deleted ids) relative to the prior
// step. Replaying these diffs from `initialState` reproduces every step.
struct ProjectHistoryStep {
    std::string typeId, name, description;
    bool enabled = true;
    std::vector<std::pair<int, TopoDS_Shape>> changed; // id -> shape after this step
    std::vector<int> deleted;                          // ids removed at this step
    // Opaque per-op parameter blob (radii, distances, etc.) produced by
    // Operation::serializeParams() and consumed by deserializeParams() on
    // load. Empty for ops that don't override serialisation or for project
    // files that predate the params extension.
    std::string params;
};

struct ProjectHistory {
    bool present = false;
    std::vector<std::pair<int, TopoDS_Shape>> initialState; // bodies before step 0
    std::vector<ProjectHistoryStep> steps;
};

class ProjectIO {
public:
    // `history` is optional; when provided it is written as a HISTORY section.
    static ProjectSaveResult save(const std::string& filePath, const Document& doc,
                                  const ProjectHistory* history = nullptr);
    // `historyOut` is optional; when provided it receives the parsed HISTORY
    // section (left empty/.present=false if the file has none).
    static ProjectLoadResult load(const std::string& filePath, Document& doc,
                                  ProjectHistory* historyOut = nullptr);
};

} // namespace materializr

#pragma once
#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <glm/glm.hpp>

namespace materializr {

class PluginContext;
class InteractiveTool;

enum class SelectionContext {
    Always,
    NoSelection,
    HasBodies,
    HasFaces,
    HasEdges,
    HasSketches,
    HasSketchRegions,
    InSketchMode,
    MultipleBodies
};

struct ToolbarContribution {
    std::string name;
    std::string section;
    SelectionContext context = SelectionContext::Always;
    int priority = 100;
    std::function<void(PluginContext&)> action;
    std::function<std::unique_ptr<InteractiveTool>()> toolFactory;
    // Optional hover-description shown when "Show toolbar tooltips" is on.
    // Empty = no tooltip for this button.
    std::string tooltip;
};

struct CommandContribution {
    std::string name;
    std::string shortcut;
    std::function<void(PluginContext&)> action;
    int priority = 100;
};

struct MenuContribution {
    std::string path;
    std::string shortcut;
    int priority = 100;
    std::function<void(PluginContext&)> action;
    std::function<bool(const PluginContext&)> enabled;
};

struct IOFormatContribution {
    std::string name;
    std::vector<std::string> extensions;
    bool canImport = false;
    bool canExport = false;
    std::function<bool(PluginContext&, const std::string& path)> importFn;
    std::function<bool(PluginContext&, const std::string& path)> exportFn;
};

struct RenderPassContribution {
    std::string name;
    int priority = 500;
    std::function<void(PluginContext&, const glm::mat4& view, const glm::mat4& proj)> render;
    std::function<bool()> initialize;
};

struct PropertyContribution {
    std::string name;
    SelectionContext context = SelectionContext::Always;
    int priority = 100;
    std::function<bool(PluginContext&)> render;
};

} // namespace materializr

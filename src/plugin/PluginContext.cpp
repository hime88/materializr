#include "PluginContext.h"
#include "PluginRegistry.h"
#include "../core/EventBus.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../core/SelectionManager.h"
#include "../viewport/Camera.h"

namespace materializr {

Document& PluginContext::document() { return *m_document; }
History& PluginContext::history() { return *m_history; }
SelectionManager& PluginContext::selection() { return *m_selection; }
EventBus& PluginContext::events() { return *m_eventBus; }
const Camera& PluginContext::camera() const { return *m_camera; }

void PluginContext::markMeshesDirty() {
    if (m_meshesDirtyFlag) *m_meshesDirtyFlag = true;
}

void PluginContext::requestInteractiveOp(const std::string& name) {
    m_pendingInteractiveOp = name;
}

std::string PluginContext::takeRequestedInteractiveOp() {
    std::string taken;
    taken.swap(m_pendingInteractiveOp);
    return taken;
}

void PluginContext::registerToolbarButton(ToolbarContribution contrib) {
    PluginRegistry::instance().toolbarContributions().push_back(std::move(contrib));
}

void PluginContext::registerCommand(CommandContribution contrib) {
    PluginRegistry::instance().commandContributions().push_back(std::move(contrib));
}

void PluginContext::registerMenuItem(MenuContribution contrib) {
    PluginRegistry::instance().menuContributions().push_back(std::move(contrib));
}

void PluginContext::registerIOFormat(IOFormatContribution contrib) {
    PluginRegistry::instance().ioFormats().push_back(std::move(contrib));
}

void PluginContext::registerRenderPass(RenderPassContribution contrib) {
    PluginRegistry::instance().renderPasses().push_back(std::move(contrib));
}

void PluginContext::registerPropertySection(PropertyContribution contrib) {
    PluginRegistry::instance().propertyContributions().push_back(std::move(contrib));
}

void PluginContext::_bind(Document* doc, History* hist, SelectionManager* sel,
                          EventBus* bus, Camera* cam, bool* meshesDirtyFlag) {
    m_document = doc;
    m_history = hist;
    m_selection = sel;
    m_eventBus = bus;
    m_camera = cam;
    m_meshesDirtyFlag = meshesDirtyFlag;
}

} // namespace materializr

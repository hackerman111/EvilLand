#include "ScreenShare.hpp"

#include "../desktop/view/LayerSurface.hpp"
#include "../desktop/view/Popup.hpp"
#include "../desktop/view/window/Window.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../protocols/core/Subcompositor.hpp"

bool Render::surfaceHiddenFromScreenShare(SP<CWLSurfaceResource> surface) {
    if (!surface)
        return false;

    while (surface && surface->m_role && surface->m_role->role() == SURFACE_ROLE_SUBSURFACE) {
        const auto ROLE = dynamicPointerCast<CSubsurfaceRole>(surface->m_role);
        surface         = ROLE && ROLE->m_subsurface ? ROLE->m_subsurface->m_parent.lock() : nullptr;
    }

    const auto SURFACE = Desktop::View::CWLSurface::fromResource(surface);
    const auto VIEW    = SURFACE ? SURFACE->view() : nullptr;
    if (!VIEW)
        return false;

    if (const auto WINDOW = Desktop::View::CWindow::fromView(VIEW); WINDOW)
        return WINDOW->m_ruleApplicator->hideFromScreenShare().valueOrDefault();

    if (const auto LAYER = Desktop::View::CLayerSurface::fromView(VIEW); LAYER)
        return LAYER->m_ruleApplicator->hideFromScreenShare().valueOrDefault();

    const auto POPUP = Desktop::View::CPopup::fromView(VIEW);
    if (!POPUP)
        return false;

    if (const auto WINDOW = POPUP->windowOwner(); WINDOW)
        return WINDOW->m_ruleApplicator->hideFromScreenShare().valueOrDefault();

    const auto LAYER = POPUP->layerOwner();
    return LAYER && LAYER->m_ruleApplicator->hideFromScreenShare().valueOrDefault();
}

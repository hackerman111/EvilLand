#include "Fadeout.hpp"
#include "../view/window/Window.hpp"
#include "../view/LayerSurface.hpp"

using namespace Desktop;
using namespace Desktop::View;

SP<Render::IFramebuffer> IFadeout::framebuffer() const {
    return m_framebuffer;
}

PHLWORKSPACEREF IFadeout::workspace() const {
    return m_workspace;
}

SFadeoutRenderEffects IFadeout::effects() const {
    return m_effects;
}

bool IFadeout::hiddenFromScreenShare() const {
    return m_hiddenFromScreenShare || (m_screenShareWindow && m_screenShareWindow->m_ruleApplicator->hideFromScreenShare().valueOrDefault()) ||
        (m_screenShareLayer && m_screenShareLayer->m_ruleApplicator->hideFromScreenShare().valueOrDefault());
}

SFadeoutSource IFadeout::source() const {
    switch (plane()) {
        case FADEOUT_PLANE_WINDOW_TILED:
        case FADEOUT_PLANE_WINDOW_FLOATING:
        case FADEOUT_PLANE_WINDOW_OVER_FULLSCREEN: return {.type = eFadeoutSource::WINDOW, .workspace = m_workspace};
        case FADEOUT_PLANE_LAYER_BACKGROUND:
        case FADEOUT_PLANE_LAYER_BOTTOM:
        case FADEOUT_PLANE_LAYER_TOP:
        case FADEOUT_PLANE_LAYER_OVERLAY: return {.type = eFadeoutSource::LAYER};
        default: return {};
    }
}

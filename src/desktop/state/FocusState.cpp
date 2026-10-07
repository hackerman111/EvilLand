#include "FocusState.hpp"
#include "../view/window/WindowFullscreenPolicy.hpp"
#include "../view/window/WindowGroupMembership.hpp"
#include "../view/window/Window.hpp"
#include "../view/window/WindowPresentation.hpp"
#include "../../Compositor.hpp"
#include "../../render/Renderer.hpp"
#include "../../ipc/s2/S2.hpp"
#include "../../managers/input/InputManager.hpp"
#include "../../managers/SeatManager.hpp"
#include "../../protocols/PointerConstraints.hpp"
#include "animation/WorkspaceAnimationController.hpp"
#include "../../managers/fullscreen/FullscreenController.hpp"
#include "../../layout/LayoutManager.hpp"
#include "../../layout/target/WindowTarget.hpp"
#include "../../event/EventBus.hpp"
#include "../../workspace/query/Query.hpp"

using namespace Desktop;

#define COMMA ,

SP<CFocusState> Desktop::focusState() {
    static SP<CFocusState> state = makeShared<CFocusState>();
    return state;
}

Desktop::CFocusState::CFocusState() {
    m_windowClose = Event::bus()->m_events.window.close.listen([this](const auto& window) {
        if (window == m_guardWindow) {
            m_guardWindow.reset();
            if (window == m_preservedFocusWindow) {
                releasePreservedFocus();
                g_pSeatManager->setKeyboardFocus(g_pSeatManager->m_state.keyboardFocus.lock());
            }
            return;
        }

        if (const auto GUARD = guardedWindow(); GUARD && window == m_focusWindow)
            fullWindowFocus(GUARD, FOCUS_REASON_OTHER);
    });
}

static bool canPreserveFocus(PHLWINDOW target, PHLWINDOW previous, SP<CWLSurfaceResource> previousSurface) {
    if (!validMapped(target) || !validMapped(previous) || target == previous || !previousSurface || previous->isHidden() || !previous->m_workspace)
        return false;

    if (target->backend().isX11() || previous->backend().isX11() || g_pSessionLockManager->isSessionLocked() || !g_pInputManager->m_exclusiveKeyboardLSes.empty() ||
        (g_pSeatManager->m_seatGrab && g_pSeatManager->m_seatGrab->m_keyboard))
        return false;

    return target->m_ruleApplicator->hideFromScreenShare().valueOrDefault() && target->m_ruleApplicator->preservePreviousFocus().valueOrDefault() &&
        previousSurface->client() == previous->wlSurface()->resource()->client() && previousSurface->client() != target->wlSurface()->resource()->client();
}

void CFocusState::updatePreservedFocus(PHLWINDOW nextWindow) {
    const auto PREVIOUS = m_preservedFocusWindow ? m_preservedFocusWindow.lock() : m_focusWindow.lock();
    const auto SURFACE  = m_preservedFocusSurface ? m_preservedFocusSurface.lock() : g_pSeatManager->m_state.keyboardFocus.lock();

    if (!canPreserveFocus(nextWindow, PREVIOUS, SURFACE)) {
        releasePreservedFocus(nextWindow);
        return;
    }

    m_preservedFocusWindow  = PREVIOUS;
    m_preservedFocusSurface = SURFACE;
    PREVIOUS->setSuspended(false);
}

SP<CWLSurfaceResource> CFocusState::preservedSurface() {
    return m_preservedFocusSurface.lock();
}

PHLWINDOW CFocusState::preservedWindow() {
    return m_preservedFocusWindow.lock();
}

void CFocusState::releasePreservedFocus(PHLWINDOW nextWindow) {
    const auto PREVIOUS = m_preservedFocusWindow.lock();
    m_preservedFocusWindow.reset();
    m_preservedFocusSurface.reset();

    if (!validMapped(PREVIOUS))
        return;

    PREVIOUS->setSuspended(PREVIOUS->isHidden() || !PREVIOUS->m_workspace || !PREVIOUS->m_workspace->visible());
    if (PREVIOUS != nextWindow && PREVIOUS != m_focusWindow)
        g_pXWaylandManager->activateWindow(PREVIOUS, false);
}

void CFocusState::refreshPreservedFocus() {
    refreshFocusGuard();

    if (const auto GUARD = guardedWindow(); GUARD && !allowsGuardedFocus(m_focusWindow.lock())) {
        fullWindowFocus(GUARD, FOCUS_REASON_OTHER);
        return;
    }

    if (!m_preservedFocusWindow || !m_focusWindow || canPreserveFocus(m_focusWindow.lock(), m_preservedFocusWindow.lock(), m_preservedFocusSurface.lock()))
        return;

    releasePreservedFocus();
    g_pSeatManager->setKeyboardFocus(g_pSeatManager->m_state.keyboardFocus.lock());
}

PHLWINDOW CFocusState::guardedWindow() const {
    const auto GUARD = m_guardWindow.lock();
    if (!validMapped(GUARD) || !GUARD->backend().isMapped() || GUARD->backend().isX11() || GUARD->isHidden() || !GUARD->m_ruleApplicator->focusGuard().valueOrDefault() ||
        !GUARD->m_workspace || !GUARD->m_workspace->visible() || !GUARD->m_monitor || !GUARD->m_monitor->enabled() || g_pSessionLockManager->isSessionLocked())
        return nullptr;

    return GUARD;
}

PHLWINDOW CFocusState::inputWindow() {
    return m_focusWindow.lock();
}

void CFocusState::refreshFocusGuard() {
    const auto PREVIOUS = m_guardWindow.lock();
    if (PREVIOUS && !guardedWindow()) {
        m_guardWindow.reset();
        if (validMapped(PREVIOUS) && PREVIOUS->backend().isMapped() && PREVIOUS == m_preservedFocusWindow && !g_pSessionLockManager->isSessionLocked()) {
            fullWindowFocus(PREVIOUS, FOCUS_REASON_OTHER);
            return;
        }
    }

    const auto CURRENT = m_focusWindow.lock();
    if (m_guardWindow || !validMapped(CURRENT) || CURRENT->backend().isX11() || !CURRENT->m_ruleApplicator->focusGuard().valueOrDefault())
        return;

    const auto KEYBOARD = g_pSeatManager->m_state.keyboardFocus.lock();
    if (KEYBOARD && KEYBOARD->client() == CURRENT->wlSurface()->resource()->client())
        m_guardWindow = CURRENT;
}

bool CFocusState::allowsGuardedFocus(PHLWINDOW nextWindow) {
    const auto GUARD = guardedWindow();
    if (!GUARD || nextWindow == GUARD)
        return true;

    if (!validMapped(nextWindow) || nextWindow->isHidden() || !nextWindow->isFloating() || nextWindow->m_workspace != GUARD->m_workspace ||
        nextWindow->m_monitor != GUARD->m_monitor || nextWindow->m_ruleApplicator->noFocus().valueOrDefault())
        return false;

    return canPreserveFocus(nextWindow, GUARD, m_preservedFocusSurface ? m_preservedFocusSurface.lock() : GUARD->wlSurface()->resource());
}

struct SFullscreenWorkspaceFocusResult {
    PHLWINDOW overrideFocusWindow = nullptr;
};

static SFullscreenWorkspaceFocusResult onFullscreenWorkspaceFocusWindow(PHLWINDOW pWindow, bool forceFSCycle) {
    const auto FSWINDOW        = Fullscreen::controller()->getFullscreenWindow(pWindow->m_workspace);
    const auto FSMODE_INTERNAL = Fullscreen::controller()->getFullscreenModes(pWindow->m_workspace).internal;
    const auto LAYOUT_HANDLED  = Fullscreen::controller()->layoutManagedFS(FSWINDOW);

    if (pWindow == FSWINDOW)
        return {}; // no conflict

    if (pWindow->isFloating()) {
        // if the window is floating, just bring it to the top
        pWindow->fullscreenPolicy().setAllowedOverFullscreen(true);
        pWindow->updateFullscreenInputState();
        Animation::Workspace::setFullscreenFloatingFade(pWindow, 1.F);
        g_pHyprRenderer->damageWindow(pWindow);
        return {};
    }

    static auto PONFOCUSUNDERFS = CConfigValue<Config::INTEGER>("misc:on_focus_under_fullscreen");

    switch (*PONFOCUSUNDERFS) {
        case 0:
            // focus the fullscreen window instead
            return {.overrideFocusWindow = FSWINDOW};
        case 2:
            // undo fs, unless we force a cycle
            if (!forceFSCycle) {
                Fullscreen::controller()->setFullscreenMode(FSWINDOW, Fullscreen::FSMODE_NONE);
                break;
            }
            [[fallthrough]];
        case 1:
            // replace fullscreen using the layoutHandled mode from prev FS window
            Fullscreen::controller()->setFullscreenMode(FSWINDOW, Fullscreen::FSMODE_NONE);
            Fullscreen::controller()->setFullscreenMode(pWindow, FSMODE_INTERNAL, std::nullopt, LAYOUT_HANDLED);
            break;

        default: LOG(Log::ERR, "Invalid misc:on_focus_under_fullscreen mode: {}", *PONFOCUSUNDERFS); break;
    }

    return {};
}

void CFocusState::fullWindowFocus(PHLWINDOW pWindow, eFocusReason reason, SP<CWLSurfaceResource> surface, bool forceFSCycle) {
    if (!allowsGuardedFocus(pWindow))
        return;

    if (pWindow) {
        if (!pWindow->m_workspace)
            return;

        const auto FSWINDOW = Fullscreen::controller()->getFullscreenWindow(pWindow->m_workspace);
        if (FSWINDOW && !Fullscreen::controller()->layoutManagedFS(FSWINDOW)) {
            const auto RESULT = onFullscreenWorkspaceFocusWindow(pWindow, forceFSCycle);
            if (RESULT.overrideFocusWindow)
                pWindow = RESULT.overrideFocusWindow;
        }
    }

    static auto PMODALPARENTBLOCKING = CConfigValue<Config::INTEGER>("general:modal_parent_blocking");

    if (*PMODALPARENTBLOCKING && pWindow && !pWindow->backend().isX11() && pWindow->backend().traits().hasModalChild) {
        LOG(Log::DEBUG, "Refusing focus to window shadowed by modal dialog");
        return;
    }

    rawWindowFocus(pWindow, reason, surface);
}

void CFocusState::rawWindowFocus(PHLWINDOW pWindow, eFocusReason reason, SP<CWLSurfaceResource> surface) {
    static auto PFOLLOWMOUSE        = CConfigValue<Config::INTEGER>("input:follow_mouse");
    static auto PSPECIALFALLTHROUGH = CConfigValue<Config::INTEGER>("input:special_fallthrough");

    if (!allowsGuardedFocus(pWindow))
        return;

    if (pWindow == m_focusWindow && surface == m_focusSurface && m_focusSurface)
        return;

    if (!pWindow || !pWindow->priorityFocus()) {
        if (g_pSessionLockManager->isSessionLocked()) {
            LOG(Log::DEBUG, "Refusing a keyboard focus to a window because of a sessionlock");
            return;
        }

        if (!g_pInputManager->m_exclusiveKeyboardLSes.empty()) {
            LOG(Log::DEBUG, "Refusing a keyboard focus to a window because of an exclusive ls");
            return;
        }
    }

    if (pWindow && pWindow->backend().isX11()) {
        const auto TRAITS = pWindow->backend().traits();
        if (TRAITS.overrideRedirect && !TRAITS.wantsFocus)
            return;
    }

    // m_target on purpose, this avoids the group
    if (pWindow)
        g_layoutManager->bringTargetToTop(pWindow->windowTarget());

    g_pInputManager->unconstrainMouse();

    if (!pWindow || !validMapped(pWindow)) {

        if (m_focusWindow.expired() && !pWindow && !m_preservedFocusSurface)
            return;

        const auto PLASTWINDOW = m_focusWindow.lock();
        releasePreservedFocus();
        m_focusWindow.reset();

        if (PLASTWINDOW && PLASTWINDOW->mapped()) {
            PLASTWINDOW->m_ruleApplicator->propertiesChanged(Rule::RULE_PROP_FOCUS);
            PLASTWINDOW->presentation().refreshValues();

            g_pXWaylandManager->activateWindow(PLASTWINDOW, false);
        }

        g_pSeatManager->setKeyboardFocus(nullptr);

        IPC::Socket2::sock()->postEvent({"activewindow", ","});
        IPC::Socket2::sock()->postEvent({"activewindowv2", ""});

        Event::bus()->m_events.window.active.emit(nullptr, reason);

        m_focusSurface.reset();

        g_pInputManager->recheckIdleInhibitorStatus();
        return;
    }

    if (pWindow->m_ruleApplicator->noFocus().valueOrDefault()) {
        LOG(Log::DEBUG, "Ignoring focus to nofocus window!");
        return;
    }

    if (m_focusWindow.lock() == pWindow && g_pSeatManager->m_state.keyboardFocus == surface && g_pSeatManager->m_state.keyboardFocus)
        return;

    if (pWindow->m_state & Desktop::View::WINDOW_STATE_PINNED)
        pWindow->m_workspace = m_focusMonitor->m_activeWorkspace;

    const auto PMONITOR = pWindow->m_monitor.lock();

    if (!pWindow->m_workspace || !pWindow->m_workspace->visible()) {
        const auto PWORKSPACE = pWindow->m_workspace;
        // This is to fix incorrect feedback on the focus history.
        PWORKSPACE->rememberFocusedWindow(pWindow);
        if (PWORKSPACE->type() == Workspace::eWorkspaceType::SPECIAL)
            m_focusMonitor->changeWorkspace(PWORKSPACE, false, true); // if special ws, open on current monitor
        else if (PMONITOR)
            PMONITOR->changeWorkspace(PWORKSPACE, false, true);
        // changeworkspace already calls focusWindow
        return;
    }

    if (PMONITOR && !(pWindow->m_state & Desktop::View::WINDOW_STATE_PINNED))
        rawMonitorFocus(PMONITOR);

    const auto PREVIOUS_PUBLIC_WINDOW = window();
    updatePreservedFocus(pWindow);

    const auto PLASTWINDOW = m_focusWindow.lock();
    m_focusWindow          = pWindow;
    if (!guardedWindow())
        pWindow->m_workspace->rememberFocusedWindow(pWindow);

    /* If special fallthrough is enabled, this behavior will be disabled, as I have no better idea of nicely tracking which
       window focuses are "via keybinds" and which ones aren't. */
    if (PMONITOR && PMONITOR->m_activeSpecialWorkspace && PMONITOR->m_activeSpecialWorkspace != pWindow->m_workspace && !(pWindow->m_state & Desktop::View::WINDOW_STATE_PINNED) &&
        !*PSPECIALFALLTHROUGH)
        PMONITOR->setSpecialWorkspace(nullptr);

    // we need to make the PLASTWINDOW not equal to m_pLastWindow so that RENDERDATA is correct for an unfocused window
    if (PLASTWINDOW && PLASTWINDOW->mapped()) {
        PLASTWINDOW->m_ruleApplicator->propertiesChanged(Rule::RULE_PROP_FOCUS);
        PLASTWINDOW->presentation().refreshValues();

        if (PLASTWINDOW != m_preservedFocusWindow && (!pWindow->backend().isX11() || !pWindow->backend().traits().overrideRedirect))
            g_pXWaylandManager->activateWindow(PLASTWINDOW, false);
    }

    const auto PWINDOWSURFACE = surface ? surface : pWindow->wlSurface()->resource();
    rawSurfaceFocus(PWINDOWSURFACE, pWindow);

    g_pXWaylandManager->activateWindow(pWindow, true); // sets the m_pLastWindow

    pWindow->m_ruleApplicator->propertiesChanged(Rule::RULE_PROP_FOCUS);
    pWindow->presentation().onFocusAnimUpdate();
    pWindow->presentation().refreshValues();

    if (pWindow->m_hints & Desktop::View::WINDOW_HINT_URGENT)
        pWindow->m_hints &= ~Desktop::View::WINDOW_HINT_URGENT;

    refreshFocusGuard();
    const auto PUBLIC_WINDOW = window();
    if (!guardedWindow() || PUBLIC_WINDOW != PREVIOUS_PUBLIC_WINDOW) {
        IPC::Socket2::sock()->postEvent({.event = "activewindow", .data = std::format("{},{}", PUBLIC_WINDOW->metadata().appID(), PUBLIC_WINDOW->metadata().title())});
        IPC::Socket2::sock()->postEvent({.event = "activewindowv2", .data = std::format("{:x}", rc<uintptr_t>(PUBLIC_WINDOW.get()))});
        Event::bus()->m_events.window.active.emit(PUBLIC_WINDOW, reason);
    }

    g_pInputManager->recheckIdleInhibitorStatus();

    if (*PFOLLOWMOUSE == 0)
        g_pInputManager->sendMotionEventsToFocused();

    if (pWindow->grouping().group())
        pWindow->deactivateGroupMembers();
}

void CFocusState::rawSurfaceFocus(SP<CWLSurfaceResource> pSurface, PHLWINDOW pWindowOwner) {
    if (g_pSessionLockManager->isSessionLocked())
        m_guardWindow.reset();

    if (const auto GUARD = guardedWindow()) {
        if (!pSurface || (pWindowOwner && !allowsGuardedFocus(pWindowOwner)))
            return;

        if (pWindowOwner && pWindowOwner != m_focusWindow) {
            rawWindowFocus(pWindowOwner, FOCUS_REASON_OTHER, pSurface);
            return;
        }

        if (!pWindowOwner) {
            const auto INPUT = inputWindow();
            if (pSurface->client() == GUARD->wlSurface()->resource()->client()) {
                if (INPUT != GUARD) {
                    rawWindowFocus(GUARD, FOCUS_REASON_OTHER, pSurface);
                    return;
                }
                pWindowOwner = GUARD;
            } else if (validMapped(INPUT) && allowsGuardedFocus(INPUT) && pSurface->client() == INPUT->wlSurface()->resource()->client())
                pWindowOwner = INPUT;
            else
                return;
        }
    }

    if (g_pSeatManager->m_state.keyboardFocus == pSurface || (pWindowOwner && g_pSeatManager->m_state.keyboardFocus == pWindowOwner->wlSurface()->resource()))
        return; // Don't focus when already focused on this.

    if (g_pSessionLockManager->isSessionLocked() && pSurface && !g_pSessionLockManager->isSurfaceSessionLock(pSurface))
        return;

    if (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(pSurface)) {
        LOG(Log::DEBUG, "surface {:x} won't receive kb focus because grab rejected it", rc<uintptr_t>(pSurface.get()));
        return;
    }

    if (!pWindowOwner)
        releasePreservedFocus();

    // Unfocus last surface if should
    if (m_focusSurface && !pWindowOwner)
        g_pXWaylandManager->activateSurface(m_focusSurface.lock(), false);

    if (!pSurface) {
        g_pSeatManager->setKeyboardFocus(nullptr);
        IPC::Socket2::sock()->postEvent({.event = "activewindow", .data = ","});
        IPC::Socket2::sock()->postEvent({.event = "activewindowv2", .data = ""});
        Event::bus()->m_events.input.keyboard.focus.emit(nullptr);
        m_focusSurface.reset();
        return;
    }

    if (g_pSeatManager->m_keyboard)
        g_pSeatManager->setKeyboardFocus(pSurface);

    if (pWindowOwner)
        LOG(Log::DEBUG, "Set keyboard focus to surface {:x}, with {}", rc<uintptr_t>(pSurface.get()), pWindowOwner);
    else
        LOG(Log::DEBUG, "Set keyboard focus to surface {:x}", rc<uintptr_t>(pSurface.get()));

    g_pXWaylandManager->activateSurface(pSurface, true);
    m_focusSurface = pSurface;

    Event::bus()->m_events.input.keyboard.focus.emit(pSurface);
}

void CFocusState::rawMonitorFocus(PHLMONITOR pMonitor) {
    if (const auto GUARD = guardedWindow(); GUARD && GUARD->m_monitor != pMonitor)
        return;

    if (m_focusMonitor == pMonitor)
        return;

    if (!pMonitor) {
        m_focusMonitor.reset();
        return;
    }

    const auto PWORKSPACE = pMonitor->m_activeWorkspace;

    const auto WORKSPACE_ADDRESS = PWORKSPACE ? PWORKSPACE->addressableName() : "";
    const auto WORKSPACE_NAME    = PWORKSPACE ? PWORKSPACE->addressableName() : "?";

    IPC::Socket2::sock()->postEvent({.event = "focusedmon", .data = std::format("{},{}", pMonitor->m_name, WORKSPACE_NAME)});
    IPC::Socket2::sock()->postEvent({.event = "focusedmonv2", .data = std::format("{},{}", pMonitor->m_name, WORKSPACE_ADDRESS)});

    Event::bus()->m_events.monitor.focused.emit(pMonitor);
    m_focusMonitor = pMonitor;
}

SP<CWLSurfaceResource> CFocusState::surface() {
    if (const auto GUARD = guardedWindow(); GUARD && m_focusWindow != GUARD)
        return m_preservedFocusSurface.lock();

    return m_focusSurface.lock();
}

PHLWINDOW CFocusState::window() {
    if (const auto GUARD = guardedWindow())
        return GUARD;

    return m_focusWindow.lock();
}

PHLMONITOR CFocusState::monitor() {
    return m_focusMonitor.lock();
}

void CFocusState::resetWindowFocus() {
    if (guardedWindow())
        return;

    m_focusWindow.reset();
    m_focusSurface.reset();
}

bool CFocusState::isWindowActive(PHLWINDOW pWindow) const {
    if (const auto GUARD = guardedWindow())
        return pWindow == GUARD;

    const auto FOCUSWINDOW  = m_focusWindow.lock();
    const auto FOCUSSURFACE = m_focusSurface.lock();

    if (!FOCUSWINDOW && !FOCUSSURFACE)
        return false;

    if (!pWindow || !pWindow->mapped())
        return false;

    const auto PSURFACE = pWindow->wlSurface()->resource();

    return PSURFACE == FOCUSSURFACE || pWindow == FOCUSWINDOW;
}

bool Desktop::isHardInputFocusReason(eFocusReason r) {
    return r == FOCUS_REASON_NEW_WINDOW || r == FOCUS_REASON_KEYBIND || r == FOCUS_REASON_GHOSTS || r == FOCUS_REASON_CLICK_UP || r == FOCUS_REASON_DESKTOP_STATE_CHANGE ||
        r == FOCUS_REASON_UNMAP_WINDOW_TILING || r == FOCUS_REASON_SWITCH_TO_WINDOW_HARD;
}

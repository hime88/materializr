#include "app/Window.h"

#include "gl_common.h"   // GLEW (Windows) must be included before other GL users
#include "touch_mode.h"
#include "mobile_files.h" // mobileShow/HideTextInput (no-ops on desktop and iOS)
#include <SDL.h>
#include <imgui_impl_sdl2.h>
#include <imgui_internal.h> // g.MovingWindow — let tab-drag (re-dock) beat drag-to-scroll
#include <stdexcept>
#include <iostream>
#include <string>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdint>

namespace materializr {

// Declared in gl_common.h; overwritten on iOS in the constructor below.
unsigned int g_windowFramebuffer = 0;

Window::Window(int width, int height, const std::string& title)
    : m_width(width), m_height(height) {

#if defined(MZ_MOBILE)
    // Stop SDL from synthesizing mouse events from touch. On Android that
    // synthesis leaves ImGui's mouse button stuck "down" after a tap (so every
    // gesture reads as click-and-hold). We feed ImGui clean finger events
    // ourselves in pollEvents() instead.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif

#if defined(_WIN32)
    // Per-monitor-v2 DPI awareness (SDL 2.24+) so Windows renders us at NATIVE
    // resolution instead of bitmap-upscaling a virtualised low-res desktop —
    // the upscale is what made the whole UI blurry on a scaled (125–200%)
    // laptop display. We deliberately do NOT set SDL_HINT_WINDOWS_DPI_SCALING:
    // that makes SDL report the window in points and hand back a >1
    // DisplayFramebufferScale, which would double-scale against our own
    // uiScale(). Instead window + drawable stay in physical pixels (so the 3D
    // viewport is crisp at native res) and uiScale() sizes the UI up by the
    // display DPI so fonts/panels stay legible. Must precede SDL_Init.
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif

    // NOTE: the port uses SDL2 on every platform, so upstream's GLFW-only X11/
    // Wayland drag-and-drop workaround doesn't apply here (kept the SDL init).
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        throw std::runtime_error(std::string("Failed to initialize SDL: ") + SDL_GetError());
    }

    // Request the right GL context per platform. Desktop: GL 3.3 Core. Android:
    // GL ES 3.0 (same shader/feature subset Materializr uses).
#if defined(MZ_GLES)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#if defined(__APPLE__)
    // macOS only grants a 3.2+ context to a forward-compatible CORE profile;
    // without this flag the request silently falls back to legacy GL 2.1, which
    // can't compile the GLSL 330 shaders. (Forward-compatible drops removed-in-
    // core legacy entry points — none of which this renderer uses.) This is the
    // only writer of SDL_GL_CONTEXT_FLAGS; if a debug-context flag is ever added,
    // OR it in rather than overwrite.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN
                 | SDL_WINDOW_RESIZABLE;
    // Deliberately NOT SDL_WINDOW_FULLSCREEN on Android: SDL turns that into the
    // window-level FLAG_FULLSCREEN, which Lenovo/Samsung "desktop / PC mode" reads
    // as "maximize me and hide the taskbar" (normal apps like Chrome never set
    // it). The bare-tablet edge-to-edge look comes from MaterializrActivity's
    // immersive system-UI flags instead — those hide the bars without that flag,
    // so in a desktop dock the app stays a normal window with the taskbar intact.

#if defined(_WIN32)
    // DPI-aware ⇒ the window is created in PHYSICAL pixels, so scale the default
    // logical 1600×900 up by the display DPI — otherwise it would render smaller
    // on a high-DPI panel than the pre-awareness (virtualised) window did (a
    // 4K/150% screen used to get a 2400×1350 physical window; without this it'd
    // drop to 1600×900). Matches uiScale() so the window holds the same amount of
    // logical UI room at any scaling. The clamp below then caps it to the work
    // area on genuinely small panels.
    {
        float ddpi = 96.0f, hh = 0.0f, vv = 0.0f;
        if (SDL_GetDisplayDPI(0, &ddpi, &hh, &vv) == 0 && ddpi > 96.0f) {
            float sc = ddpi / 96.0f;
            if (sc > 3.0f) sc = 3.0f;
            m_width  = static_cast<int>(m_width  * sc);
            m_height = static_cast<int>(m_height * sc);
        }
    }
#endif

    // Clamp the fixed initial size to the display's usable area (the screen minus
    // the taskbar) BEFORE creating the window. Now that the process is per-monitor
    // DPI-aware (see the SDL_HINT_WINDOWS_DPI_AWARENESS above), both the create
    // size and SDL_GetDisplayUsableBounds are in PHYSICAL pixels, so the two are
    // in the same coordinate space and the clamp is apples-to-apples. On a small
    // or low-res laptop panel the hardcoded 1600×900 can still exceed the work
    // area (e.g. a 1366×768 screen), so we clamp + start maximized rather than
    // spill past the taskbar / title bar / dock panels; roomier screens are
    // untouched, and Android overrides the size below regardless. (Pre-DPI-aware
    // this also fixed the *virtualised* small-desktop overflow at 125–150%
    // scaling; the crisp-rendering fix removed the virtualisation, the clamp still
    // guards genuinely small panels.) Leave a margin for the window's own borders.
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
        const int marginW = 16;  // left+right borders
        const int marginH = 64;  // title bar + bottom border
        const int maxW = usable.w - marginW;
        const int maxH = usable.h - marginH;
        bool clamped = false;
        if (maxW > 0 && m_width  > maxW) { m_width  = maxW; clamped = true; }
        if (maxH > 0 && m_height > maxH) { m_height = maxH; clamped = true; }
#if !defined(MZ_MOBILE)
        // On a screen too small for the default size, also start maximized so the
        // app fills the work area immediately. The clamped values above become the
        // window's *restore* size, so un-maximizing — or a minimize→restore — drops
        // back to a size that still fits the screen instead of overrunning it again.
        if (clamped) flags |= SDL_WINDOW_MAXIMIZED;
#endif
    }

    m_window = SDL_CreateWindow(title.c_str(),
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                m_width, m_height, flags);
    if (!m_window) {
        SDL_Quit();
        throw std::runtime_error(std::string("Failed to create SDL window: ") + SDL_GetError());
    }

    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext) {
        SDL_DestroyWindow(m_window);
        SDL_Quit();
        throw std::runtime_error(std::string("Failed to create GL context: ") + SDL_GetError());
    }
    SDL_GL_MakeCurrent(m_window, static_cast<SDL_GLContext>(m_glContext));

#ifdef _WIN32
    // Load GL 3.3 core entry points (no-op on Linux/Android, which export them).
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        throw std::runtime_error("Failed to initialize GLEW (OpenGL loader)");
    }
#endif

#if defined(MZ_IOS)
    // On iOS the screen is NOT framebuffer 0 — SDL backs the window with a
    // renderbuffer FBO and binding 0 draws into the void. Capture the real
    // one (bound current by SDL_GL_CreateContext) so g_windowFramebuffer
    // binds the screen everywhere the code would otherwise bind 0. The color
    // renderbuffer matters too: SDL's swap presents whatever GL_RENDERBUFFER
    // is bound at that moment, so swapBuffers() re-binds this before swapping
    // (Viewport's own depth/MSAA renderbuffer setup leaves others bound).
    {
        GLint fbo = 0, rbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &rbo);
        g_windowFramebuffer = static_cast<unsigned int>(fbo);
        m_windowRenderbuffer = static_cast<unsigned int>(rbo);
        std::cout << "iOS window framebuffer=" << fbo
                  << " renderbuffer=" << rbo << std::endl;
    }
#endif

    // Log the context we actually got. The 3.3-core request can be silently
    // downgraded (notably on macOS without the forward-compatible flag → GL 2.1,
    // where the GLSL 330 shaders won't compile); surfacing the version here turns
    // that from a mystery black screen into a one-line diagnostic.
    {
        const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* glsl = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        const char* rend = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        std::cout << "GL " << (ver ? ver : "?") << " | GLSL " << (glsl ? glsl : "?")
                  << " | " << (rend ? rend : "?") << std::endl;
    }

    SDL_GL_SetSwapInterval(1); // vsync

    // Reflect the actual created size (Android fullscreen overrides the request).
    SDL_GetWindowSize(m_window, &m_width, &m_height);
}

Window::~Window() {
    if (m_glContext) SDL_GL_DeleteContext(static_cast<SDL_GLContext>(m_glContext));
    if (m_window) SDL_DestroyWindow(m_window);
    SDL_Quit();
}

void Window::swapBuffers() {
#if defined(MZ_IOS)
    // presentRenderbuffer presents the *currently bound* GL_RENDERBUFFER —
    // restore SDL's color renderbuffer in case frame code bound another.
    glBindRenderbuffer(GL_RENDERBUFFER, m_windowRenderbuffer);
#endif
    SDL_GL_SwapWindow(m_window);
}

bool Window::isForeground() const {
    if (!m_window) return true;
    Uint32 f = SDL_GetWindowFlags(m_window);
    if (f & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) return false;
    return (f & SDL_WINDOW_INPUT_FOCUS) != 0;
}

int Window::pollEvents(int waitMs) {
    if (waitMs > 0) SDL_WaitEventTimeout(nullptr, waitMs);
    // 0 = nothing, 1 = trivial (motion / expose), 2 = significant (click / key / scroll …)
    int result = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        // Classify the event before handing it to ImGui.
        if (result < 2) {
            switch (e.type) {
                case SDL_KEYDOWN: case SDL_KEYUP:
                case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
                case SDL_MOUSEWHEEL:
                case SDL_TEXTINPUT: case SDL_TEXTEDITING:
                case SDL_DROPFILE:
                case SDL_QUIT:
                case SDL_FINGERDOWN: case SDL_FINGERUP:
                    result = 2;
                    break;
                case SDL_WINDOWEVENT:
                    switch (e.window.event) {
                        case SDL_WINDOWEVENT_RESIZED:
                        case SDL_WINDOWEVENT_SIZE_CHANGED:
                        case SDL_WINDOWEVENT_FOCUS_GAINED:
                        case SDL_WINDOWEVENT_FOCUS_LOST:
                        case SDL_WINDOWEVENT_SHOWN:
                        case SDL_WINDOWEVENT_RESTORED:
                        case SDL_WINDOWEVENT_MAXIMIZED:
                        case SDL_WINDOWEVENT_MINIMIZED:
                            result = 2; break;
                        default: // EXPOSED and others — need 1 repaint, not 5
                            if (result < 1) result = 1; break;
                    }
                    break;
                case SDL_MOUSEMOTION:
                case SDL_FINGERMOTION:
                    if (result < 1) result = 1;
                    break;
                default:
                    if (result < 1) result = 1;
                    break;
            }
        }
#if defined(MZ_MOBILE)
        // Touch gestures, handled directly (SDL's own touch->mouse synthesis is
        // off). One finger drives the left mouse (tap = select, drag = orbit in
        // trackpad mode); two fingers pan/pinch-zoom the camera.
        if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION || e.type == SDL_FINGERUP) {
            handleFingerEvent(e.type, (std::int64_t)e.tfinger.fingerId, e.tfinger.x, e.tfinger.y);
            continue;   // don't also route finger events through the backend
        }
#endif
        // Feed every event to ImGui (handles mouse, keyboard, text).
        ImGui_ImplSDL2_ProcessEvent(&e);
        switch (e.type) {
            case SDL_QUIT:
                m_shouldClose = true;
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_CLOSE &&
                    e.window.windowID == SDL_GetWindowID(m_window)) {
                    m_shouldClose = true;
                }
                break;
            default:
                break;
        }
    }
#if defined(MZ_MOBILE)
    updateHoldSelect();          // arm the long-press (box-select on drag / menu on lift)
    pumpSyntheticRightClick();   // play back a queued long-press context-menu click
#endif
    SDL_GetWindowSize(m_window, &m_width, &m_height);
    return result;
}

#if defined(MZ_MOBILE)
void Window::handleFingerEvent(unsigned type, std::int64_t id, float nx, float ny) {
    ImGuiIO& io = ImGui::GetIO();
    const float x = nx * io.DisplaySize.x;   // normalised [0,1] -> pixels
    const float y = ny * io.DisplaySize.y;

    auto it = std::find_if(m_fingers.begin(), m_fingers.end(),
                           [&](const Finger& f) { return f.id == id; });
    if (type == SDL_FINGERDOWN) {
        if (it == m_fingers.end()) m_fingers.push_back({id, x, y});
        else { it->x = x; it->y = y; }
    } else if (type == SDL_FINGERMOTION) {
        if (it == m_fingers.end()) return;
        it->x = x; it->y = y;
    } else { // SDL_FINGERUP
        if (it != m_fingers.end()) m_fingers.erase(it);
    }

    const int count = static_cast<int>(m_fingers.size());

    if (count >= 2) {
        const float cx = (m_fingers[0].x + m_fingers[1].x) * 0.5f;
        const float cy = (m_fingers[0].y + m_fingers[1].y) * 0.5f;
        const float sx = m_fingers[0].x - m_fingers[1].x;
        const float sy = m_fingers[0].y - m_fingers[1].y;
        const float dist = std::sqrt(sx * sx + sy * sy);
        if (!m_twoFinger) {
            // Two-finger gesture begins: cancel any in-progress orbit, set refs.
            if (m_leftDown) {
                io.AddMouseButtonEvent(0, false); m_leftDown = false;
                m_leftReleaseWasGesture = true; // spurious release from the 2nd finger
            }
            m_twoFinger = true;
            m_suppressLeft = true;
            m_holdSelect = false;          // a two-finger gesture cancels hold-select
            m_movedBeyondHold = false;
            m_lastCentroidX = cx; m_lastCentroidY = cy;
            m_lastPinchDist = dist;
            m_startCentroidX = cx; m_startCentroidY = cy; // net-intent references
            m_startPinchDist = dist;
            m_twoFingerMode = 0;          // undecided until one gesture dominates
        } else {
            const float dCx = cx - m_lastCentroidX;
            const float dCy = cy - m_lastCentroidY;
            const float dZ  = dist - m_lastPinchDist;
            if (m_twoFingerMode == 0) {
                // Decide pan vs zoom from NET change since the gesture began, not
                // a running sum of per-frame deltas. Summing |Δspacing| each frame
                // integrates the spacing wobble of two never-quite-parallel
                // fingers, so a slow, deliberate pan accumulated enough phantom
                // "zoom" to mis-lock — the slower you panned, the worse it got
                // (issue #1). Net change cancels that wobble: only a sustained
                // pinch grows zoomNet, while a real pan grows panNet.
                const float panNet  = std::sqrt((cx - m_startCentroidX) * (cx - m_startCentroidX) +
                                                (cy - m_startCentroidY) * (cy - m_startCentroidY));
                const float zoomNet = std::fabs(dist - m_startPinchDist);
                // Strong pan bias: pan is the gesture users struggle to land, so
                // it commits on modest travel and only needs to edge out zoom,
                // whereas zoom must clearly dominate AND clear a real-pinch floor
                // (incidental splay during a pan never reaches it).
                const float panLock   = 10.0f; // net centroid px to commit to pan
                const float zoomFloor = 20.0f; // net spacing px before zoom is even considered
                const float zoomDead  =  6.0f; // discount incidental spacing drift
                if (panNet > panLock && panNet > zoomNet * 1.2f)
                    m_twoFingerMode = 1; // pan
                else if (zoomNet > zoomFloor && (zoomNet - zoomDead) > panNet * 2.0f)
                    m_twoFingerMode = 2; // zoom
            }
            if (m_twoFingerMode == 1) { m_panAccX += dCx; m_panAccY += dCy; }
            else if (m_twoFingerMode == 2) { m_zoomAcc += dZ; }
            m_lastCentroidX = cx; m_lastCentroidY = cy;
            m_lastPinchDist = dist;
        }
        return;
    }

    if (count == 1) {
        // A finger left over from a two-finger gesture is ignored (no jump-orbit)
        // until the user fully lifts off.
        if (m_suppressLeft) { m_twoFinger = false; return; }
        // Note: the left button is always fed (even in Move mode) so on-screen
        // buttons stay clickable; Move mode is enforced at the viewport level
        // (it gates drawing/selection there, not the raw input here).
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        if (type == SDL_FINGERDOWN && !m_leftDown) {
            io.AddMousePosEvent(m_fingers[0].x, m_fingers[0].y);
            io.AddMouseButtonEvent(0, true);
            m_leftDown = true;
            m_leftReleaseWasGesture = false; // a genuine new press
            m_downTicks = SDL_GetTicks();   // begin press-and-hold tracking
            m_downX = m_fingers[0].x; m_downY = m_fingers[0].y;
            m_movedBeyondHold = false;
            m_holdSelect = false;
            m_panelScroll = false;
            m_scrollArmed = false;
            m_lastScrollY = m_fingers[0].y;
        } else if (type == SDL_FINGERMOTION) {
            // Track movement even after the hold arms: a hold that then drags is
            // a box-select; a hold that never moves is a long-press (menu).
            const float dx = m_fingers[0].x - m_downX, dy = m_fingers[0].y - m_downY;
            if (dx * dx + dy * dy > 25.0f * 25.0f) m_movedBeyondHold = true; // a drag
            // Touch drag-to-scroll: over a panel (anything but the 3D canvas), a
            // vertical-dominant drag scrolls the window the finger is over, like
            // a mobile list. Horizontal drags fall through untouched so sliders
            // still work. The canvas keeps its one-finger orbit.
            // Don't let drag-to-scroll hijack a dock-splitter RESIZE: while a
            // splitter is grabbed ImGui shows a resize cursor, so a vertical drag
            // there is a panel resize, not a list scroll. Without this, dragging a
            // panel border past ~25px got reclassified as a scroll and the button
            // was released, dropping the resize (worse on small screens).
            const ImGuiMouseCursor curCursor = ImGui::GetMouseCursor();
            const bool onSplitter =
                curCursor == ImGuiMouseCursor_ResizeNS ||
                curCursor == ImGuiMouseCursor_ResizeEW ||
                curCursor == ImGuiMouseCursor_ResizeNESW ||
                curCursor == ImGuiMouseCursor_ResizeNWSE;
            // A tab/title drag to re-dock a panel sets g.MovingWindow (no resize
            // cursor, so onSplitter misses it) — also a real drag, not a scroll.
            ImGuiContext* g = ImGui::GetCurrentContext();
            const bool movingWindow = g && g->MovingWindow != nullptr;
            // A scrollbar drag (including a CHILD window's — e.g. the Settings
            // body lives in a BeginChild) is a real interaction; don't release it
            // for a scroll latch or the bar just twitches and snaps back to top.
            bool onScrollbar = false;
            if (g && g->ActiveId != 0 && g->ActiveIdWindow) {
                onScrollbar =
                    g->ActiveId == ImGui::GetWindowScrollbarID(g->ActiveIdWindow, ImGuiAxis_Y) ||
                    g->ActiveId == ImGui::GetWindowScrollbarID(g->ActiveIdWindow, ImGuiAxis_X);
            }
            const bool wantScroll =
                materializr::touchMode() && !m_touchOnCanvas && !m_panelScroll &&
                !onSplitter && !movingWindow && !onScrollbar &&
                (dx * dx + dy * dy) > 25.0f * 25.0f && std::fabs(dy) > std::fabs(dx);
            bool justLatched = false;
            // Arm on the first frame past the threshold, commit on the next — that
            // one frame lets ImGui set MovingWindow for a straight-down tab/title
            // drag (input is read a frame ahead of ImGui), so the move wins over
            // the scroll instead of being stolen.
            if (wantScroll && !m_scrollArmed) {
                m_scrollArmed = true;
            } else if (wantScroll && m_scrollArmed) {
                // Switch press -> scroll: release the left button so the row the
                // finger started on isn't selected/activated by the flick. Park
                // the cursor off-screen BEFORE releasing — a release while still
                // over the button/row reads as a click (ImGui buttons fire on
                // mouse-up over the active item), which is exactly the "scrolling
                // also selects tools" bug. The justLatched block below moves the
                // cursor back onto the panel for the wheel target.
                if (m_leftDown) {
                    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                    io.AddMouseButtonEvent(0, false);
                    m_leftDown = false;
                    m_leftReleaseWasGesture = true;
                }
                m_panelScroll = true;
                // NB: do NOT reset m_lastScrollY here. It carries from the press,
                // so the latch frame's delta is the (non-zero) threshold distance
                // already travelled — that fires a wheel event WHILE the mouse is
                // still over the panel, which is what locks ImGui onto it
                // (g.WheelingWindow). Zeroing it made inc==0 on the one frame the
                // mouse was over the panel, so the lock never took and parking the
                // mouse off-screen afterwards left nothing to scroll.
                justLatched = true;
            }
            if (m_panelScroll) {
                // Report the finger position ONLY on the frame the scroll latches,
                // so ImGui picks the window under it as the wheel target and locks
                // onto it (g.WheelingWindow). After that, park the mouse off-screen:
                // the wheel keeps scrolling the latched window, but the finger's
                // path no longer lights up every row's hover highlight / tooltip.
                if (justLatched) io.AddMousePosEvent(m_fingers[0].x, m_fingers[0].y);
                else             io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                // ImGui scrolls ~5*FontSize px per wheel unit, so dividing the
                // pixel delta by that tracks the finger roughly 1:1. Finger down
                // (inc>0) -> positive wheel -> content follows the finger.
                float step = 5.0f * ImGui::GetFontSize();
                if (step < 1.0f) step = 60.0f;
                const float inc = m_fingers[0].y - m_lastScrollY;
                m_lastScrollY = m_fingers[0].y;
                if (inc != 0.0f) io.AddMouseWheelEvent(0.0f, inc / step);
            } else {
                io.AddMousePosEvent(m_fingers[0].x, m_fingers[0].y);
            }
        } else {
            // Any other single-finger event (e.g. a 2->1 finger transition):
            // keep ImGui's mouse position current.
            io.AddMousePosEvent(m_fingers[0].x, m_fingers[0].y);
        }
        return;
    }

    // count == 0: everything lifted — release and reset.
    if (m_leftDown) { io.AddMouseButtonEvent(0, false); m_leftDown = false; }
    // Genuine double-tap detection: this lift completes a quick tap (not a hold,
    // not a drag, not a 2-finger leftover). Two such taps at the same spot within
    // the double-click time → a touch "double-click" (escalates a face pick to its
    // body, viewport-side). Honors the user's double-click-time setting.
    {
        const std::uint32_t nowT = SDL_GetTicks();
        const bool quickTap = !m_holdSelect && !m_movedBeyondHold && !m_suppressLeft &&
                              (nowT - m_downTicks) < 300u;
        if (quickTap) {
            const std::uint32_t dblMs =
                static_cast<std::uint32_t>(io.MouseDoubleClickTime * 1000.0f);
            const float ddx = m_downX - m_lastTapX, ddy = m_downY - m_lastTapY;
            if (m_lastTapTick != 0 && (nowT - m_lastTapTick) <= dblMs &&
                (ddx * ddx + ddy * ddy) < 40.0f * 40.0f) {
                m_doubleTapPending = true;
                m_lastTapTick = 0; // consumed; a 3rd tap starts a fresh pair
            } else {
                m_lastTapTick = nowT; m_lastTapX = m_downX; m_lastTapY = m_downY;
            }
        }
    }
    // A one-finger press that armed the hold but never dragged is a long-press:
    // queue a synthetic right-click at the held point so the context menu opens,
    // and mark the left-up as a gesture so it doesn't also place a sketch point.
    if (m_holdSelect && !m_movedBeyondHold && !m_suppressLeft) {
        m_rightClickX = m_downX; m_rightClickY = m_downY;
        m_rightClickPhase = 1;
        m_leftReleaseWasGesture = true;
    }
    m_twoFinger = false;
    m_suppressLeft = false;
    m_holdSelect = false;
    m_movedBeyondHold = false;
    m_panelScroll = false;
}

void Window::updateHoldSelect() {
    if (m_holdSelect) return;
    // Only arm over the 3D canvas — a press on a slider/panel must never become a
    // long-press (slow slider drags were popping the context-menu ring).
    if (!m_touchOverViewport) return;
    if (m_fingers.size() != 1 || m_movedBeyondHold || m_suppressLeft || m_twoFinger) return;
    if (SDL_GetTicks() - m_downTicks > 450u) m_holdSelect = true;  // long-press armed
}

void Window::pumpSyntheticRightClick() {
    if (m_rightClickPhase == 0) return;
    ImGuiIO& io = ImGui::GetIO();
    // Present it as a real mouse so popups open without touch hover-delay; the
    // finger has already lifted, so we keep re-asserting the held position.
    io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    io.AddMousePosEvent(m_rightClickX, m_rightClickY);
    if (m_rightClickPhase == 1) {
        io.AddMouseButtonEvent(1, true);   // right button down
        m_rightClickPhase = 2;
    } else {
        io.AddMouseButtonEvent(1, false);  // ...and up next frame → a right-click
        m_rightClickPhase = 0;
    }
}

#else
void Window::handleFingerEvent(unsigned, std::int64_t, float, float) {}
#endif

// Defined on every platform: the runtime touch-mode hold ring (Application::
// endFrame) calls it even on desktop (a desktop touchscreen can enable touch
// mode). With no finger events fed (m_fingers stays empty off Android), it just
// returns 0 there.
float Window::holdProgress(float& x, float& y) const {
    if (m_fingers.size() != 1 || m_movedBeyondHold || m_suppressLeft || m_twoFinger ||
        !m_touchOverViewport)
        return 0.0f;
    x = m_downX; y = m_downY;
    if (m_holdSelect) return 1.0f;                 // armed: ring full while held
    std::uint32_t held = SDL_GetTicks() - m_downTicks;
    if (held < 120u) return 0.0f;                  // ignore brief taps
    float t = static_cast<float>(held) / 450.0f;
    return t > 1.0f ? 1.0f : t;
}

bool Window::consumeTouchPan(float& dx, float& dy) {
    if (m_panAccX == 0.0f && m_panAccY == 0.0f) return false;
    dx = m_panAccX; dy = m_panAccY;
    m_panAccX = m_panAccY = 0.0f;
    return true;
}

bool Window::consumeTouchZoom(float& dz) {
    if (m_zoomAcc == 0.0f) return false;
    dz = m_zoomAcc;
    m_zoomAcc = 0.0f;
    return true;
}

bool Window::consumeDoubleTap() {
    if (!m_doubleTapPending) return false;
    m_doubleTapPending = false;
    return true;
}

void Window::updateTextInput(bool wantTextInput) {
#if defined(MZ_MOBILE)
    if (wantTextInput && !m_textInputActive) {
        SDL_StartTextInput();              // enables SDL_TEXTINPUT events
        // SDL's own keyboard-raise is gated on SDL_GetFocusWindow() != NULL,
        // which is NULL in our immersive surface, so it no-ops. Raise the IME
        // ourselves via SDLActivity (text still routes through SDL → ImGui).
        mobileShowTextInput();
        m_textInputActive = true;
    } else if (!wantTextInput && m_textInputActive) {
        SDL_StopTextInput();
        mobileHideTextInput();
        m_textInputActive = false;
    }
#else
    (void)wantTextInput;
#endif
}

void Window::framebufferSize(int& w, int& h) const {
    SDL_GL_GetDrawableSize(m_window, &w, &h);
}

float Window::uiScale() const {
    if (materializr::touchMode()) {
#if defined(MZ_IOS)
        // iOS window coords are POINTS — the OS already normalizes density
        // (the drawable is the 2-3x pixel surface underneath). SDL's reported
        // display DPI is a synthetic 160·scale, not physical, so no formula:
        // desktop density is the right size in point space.
        return 1.0f;
#else
        // Scale the desktop-density UI up for a touch screen. Use the physical
        // DPI against a 96-dpi baseline (so a 240-dpi tablet -> 2.5x), clamped.
        float ddpi = 240.0f, hdpi = 0.0f, vdpi = 0.0f;
        if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) != 0 || ddpi <= 0.0f) ddpi = 240.0f;
        float s = ddpi / 120.0f;    // 240-dpi tablet -> 2.0x (was 2.5x, a bit too big)
        if (s < 1.4f) s = 1.4f;     // never smaller than 1.4x on a touch device
        if (s > 2.5f) s = 2.5f;
        return s;
#endif
    }
#if defined(_WIN32)
    // Desktop Windows HiDPI: now that the process is per-monitor DPI-aware (see
    // the SDL_HINT_WINDOWS_DPI_AWARENESS above) the framebuffer is NATIVE-res
    // and crisp, but window coordinates are physical pixels — so a 15 px font
    // would render tiny on a 150% display. Scale the UI up by the display's DPI
    // (96 dpi = 100% = 1.0x, 144 = 150% = 1.5x, …) so it stays the same physical
    // size the user set in Windows, now sharp instead of bitmap-upscaled. Fonts
    // are rasterised at 15·scale (crisp) and ImGui sizes scale to match.
    float ddpi = 96.0f, hdpi = 0.0f, vdpi = 0.0f;
    if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) != 0 || ddpi <= 0.0f) ddpi = 96.0f;
    float s = ddpi / 96.0f;
    if (s < 1.0f) s = 1.0f;     // never shrink below 100%
    if (s > 3.0f) s = 3.0f;     // 300% cap (Windows tops out ~250% on laptops)
    return s;
#else
    // Linux / macOS handle HiDPI through the drawable-size / DisplayFramebufferScale
    // path (Retina, Wayland fractional scaling), so the UI is already right at 1.0.
    return 1.0f;
#endif
}

bool Window::isCtrlDown() {
    // Poll the real keyboard on every platform. With no physical keyboard the
    // state is simply all-zero, so this is false on a bare touch tablet (where
    // multi-select uses the on-screen toggle instead); when an Android tablet has
    // a keyboard attached, hardware Ctrl (undo/redo, additive select) just works.
    const Uint8* state = SDL_GetKeyboardState(nullptr);
    return state[SDL_SCANCODE_LCTRL] || state[SDL_SCANCODE_RCTRL];
}

} // namespace materializr

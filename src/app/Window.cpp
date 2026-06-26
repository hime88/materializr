#include "app/Window.h"

#include "gl_common.h"   // GLEW (Windows) must be included before other GL users
#include "touch_mode.h"
#include "android_files.h" // androidShow/HideTextInput (no-ops on desktop)
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

Window::Window(int width, int height, const std::string& title)
    : m_width(width), m_height(height) {

#if defined(__ANDROID__)
    // Stop SDL from synthesizing mouse events from touch. On Android that
    // synthesis leaves ImGui's mouse button stuck "down" after a tap (so every
    // gesture reads as click-and-hold). We feed ImGui clean finger events
    // ourselves in pollEvents() instead.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif

    // NOTE: the port uses SDL2 on every platform, so upstream's GLFW-only X11/
    // Wayland drag-and-drop workaround doesn't apply here (kept the SDL init).
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        throw std::runtime_error(std::string("Failed to initialize SDL: ") + SDL_GetError());
    }

    // Request the right GL context per platform. Desktop: GL 3.3 Core. Android:
    // GL ES 3.0 (same shader/feature subset Materializr uses).
#if defined(__ANDROID__)
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
#if defined(__ANDROID__)
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
#if defined(__ANDROID__)
    updateHoldSelect();          // arm the long-press (box-select on drag / menu on lift)
    pumpSyntheticRightClick();   // play back a queued long-press context-menu click
#endif
    SDL_GetWindowSize(m_window, &m_width, &m_height);
    return result;
}

#if defined(__ANDROID__)
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
            m_twoFingerMode = 0;          // undecided until one gesture dominates
            m_twoFingerPanMag = 0.0f;
            m_twoFingerZoomMag = 0.0f;
        } else {
            const float dCx = cx - m_lastCentroidX;
            const float dCy = cy - m_lastCentroidY;
            const float dZ  = dist - m_lastPinchDist;
            // Confidence check: accumulate how much the centroid has travelled
            // (pan intent) vs how much the finger spacing has changed (zoom
            // intent), both in pixels, and lock to whichever clearly wins. Until
            // then apply NOTHING, so a two-finger gesture doesn't pan AND zoom at
            // once (the jitter). Re-decided each fresh two-finger gesture.
            m_twoFingerPanMag  += std::sqrt(dCx * dCx + dCy * dCy);
            m_twoFingerZoomMag += std::fabs(dZ);
            if (m_twoFingerMode == 0) {
                const float lock = 12.0f; // px of clear intent before committing
                if (m_twoFingerPanMag > lock && m_twoFingerPanMag > m_twoFingerZoomMag * 1.4f)
                    m_twoFingerMode = 1; // pan
                else if (m_twoFingerZoomMag > lock && m_twoFingerZoomMag > m_twoFingerPanMag * 1.4f)
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
                // finger started on isn't selected/activated by the flick.
                if (m_leftDown) {
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
#if defined(__ANDROID__)
    if (wantTextInput && !m_textInputActive) {
        SDL_StartTextInput();              // enables SDL_TEXTINPUT events
        // SDL's own keyboard-raise is gated on SDL_GetFocusWindow() != NULL,
        // which is NULL in our immersive surface, so it no-ops. Raise the IME
        // ourselves via SDLActivity (text still routes through SDL → ImGui).
        androidShowTextInput();
        m_textInputActive = true;
    } else if (!wantTextInput && m_textInputActive) {
        SDL_StopTextInput();
        androidHideTextInput();
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
    // Only the touch UI scales up; in desktop mode the UI is already sized right
    // (this is what lets a tablet with a mouse/keyboard run the desktop layout).
    if (!materializr::touchMode()) return 1.0f;
    // Scale the desktop-density UI up for a touch screen. Use the physical DPI
    // against a 96-dpi desktop baseline (so a 240-dpi tablet -> 2.5x), clamped.
    float ddpi = 240.0f, hdpi = 0.0f, vdpi = 0.0f;
    if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) != 0 || ddpi <= 0.0f) ddpi = 240.0f;
    float s = ddpi / 120.0f;    // 240-dpi tablet -> 2.0x (was 2.5x, a bit too big)
    if (s < 1.4f) s = 1.4f;     // never smaller than 1.4x on a touch device
    if (s > 2.5f) s = 2.5f;
    return s;
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

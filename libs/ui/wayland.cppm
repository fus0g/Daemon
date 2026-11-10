// daemon.wayland — the windowing layer: a native Wayland client (xdg-shell, xkbcommon,
// wl_data_device clipboard, themed cursors, integer HiDPI scale).
module;
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "ext-background-effect-v1-client-protocol.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
export module daemon.ui.wayland;
import std;
import daemon.ui.input;
import daemon.base.util;

export namespace ide {

class Window {
public:
    // ---- callbacks (all coordinates are logical pixels; sizes exclude the buffer scale)
    std::function<void(int w, int h, int scale)> onResize;
    std::function<void(const KeyEvent&)> onKey;
    std::function<void(float x, float y)> onPointerMove;
    std::function<void(Button, bool pressed, float x, float y)> onPointerButton;
    // dyPx/dxPx: smooth (touchpad) distance; stepsY/stepsX: wheel notches. Positive = down/right.
    std::function<void(float dxPx, float dyPx, int stepsX, int stepsY)> onScroll;
    std::function<void(bool focused)> onFocus;
    std::function<void()> onClose;

    Window() = default;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window() { destroy(); }

    std::expected<void, std::string> create(std::string_view title, int width, int height);

    // Dispatches events, waiting at most `timeoutMs` (-1 = forever). False once the window should close.
    bool pump(int timeoutMs);

    void* display() const { return display_; }
    void* surface() const { return surface_; }
    // Asks the compositor to blur whatever is behind the whole window (the app paints translucent chrome
    // and opaque islands on top). No-op when ext-background-effect is unavailable.
    bool blurAvailable() const { return bgfx_mgr_ != nullptr; }
    void updateBlur();
    int width() const { return width_; }
    int height() const { return height_; }
    int scale() const { return scale_; }
    bool configured() const { return configured_; }
    // Frame pacing: ask the compositor for a callback with the next commit. While one is outstanding the
    // window may be hidden/occluded, so the app should not render (see frameWait()).
    void requestFrame();
    void cancelFrame() { if (frameCb_) { wl_callback_destroy(frameCb_); frameCb_ = nullptr; } }
    bool framePending() const { return frameCb_ != nullptr; }
    // Milliseconds since the outstanding frame callback was requested (0 if none).
    long long frameWaitMs() const;
    bool serverDecorations() const { return serverDeco_; }
    bool maximized() const { return maximized_; }
    bool fullscreen() const { return fullscreen_; }
    bool focused() const { return focused_; }
    bool shiftDown() const { return modActive(XKB_MOD_NAME_SHIFT); }
    bool ctrlDown() const { return modActive(XKB_MOD_NAME_CTRL); }

    // Calls `cb` from pump() whenever `fd` is readable (or hung up). Replaces an existing watch on `fd`.
    void watch(int fd, std::function<void()> cb) {
        unwatch(fd);
        if (fd >= 0) watches_.push_back({fd, std::move(cb)});
    }
    void unwatch(int fd) { std::erase_if(watches_, [fd](const Watch& w) { return w.fd == fd; }); }

    void setTitle(std::string_view t) {
        std::string s(t);
        if (toplevel_) xdg_toplevel_set_title(toplevel_, s.c_str());
    }
    void startMove() { if (toplevel_ && seat_) xdg_toplevel_move(toplevel_, seat_, inputSerial_); }
    void startResize(Edge e) {
        if (toplevel_ && seat_ && e != Edge::None) xdg_toplevel_resize(toplevel_, seat_, inputSerial_, static_cast<std::uint32_t>(e));
    }
    void setMaximized() { if (toplevel_) xdg_toplevel_set_maximized(toplevel_); }
    void toggleMaximize() {
        if (!toplevel_) return;
        if (maximized_) xdg_toplevel_unset_maximized(toplevel_); else xdg_toplevel_set_maximized(toplevel_);
    }
    void toggleFullscreen() {
        if (!toplevel_) return;
        if (fullscreen_) xdg_toplevel_unset_fullscreen(toplevel_); else xdg_toplevel_set_fullscreen(toplevel_, nullptr);
    }
    void minimize() { if (toplevel_) xdg_toplevel_set_minimized(toplevel_); }

    void setCursor(Cursor c) {
        if (c == cursor_) return;
        cursor_ = c;
        applyCursor();
    }

    // ---- clipboard
    void setClipboard(std::string text);

    std::string clipboard();

private:
    bool modActive(const char* name) const {
        return xkbState_ && xkb_state_mod_name_is_active(xkbState_, name, XKB_STATE_MODS_EFFECTIVE) > 0;
    }

    struct Watch {
        int fd;
        std::function<void()> cb;
    };
    std::vector<Watch> watches_;

    struct Output {
        wl_output* output;
        std::uint32_t name;
        int scale = 1;
    };

    // ------------------------------------------------------------ listeners
    // Each listener is built once; unused events get explicit no-ops.

    static const wl_registry_listener& registryListener();

    void onGlobal(wl_registry* reg, std::uint32_t name, const char* iface, std::uint32_t ver);

    static const wl_output_listener& outputListener();

    static const wl_surface_listener& surfaceListener();

    void updateScaleFromOutputs();

    void setScale(int s);

    static const xdg_surface_listener& xdgSurfaceListener();

    static const xdg_toplevel_listener& toplevelListener();

    static const zxdg_toplevel_decoration_v1_listener& decorationListener();

    // ------------------------------------------------------------ seat
    static const wl_seat_listener& seatListener();

    static const wl_pointer_listener& pointerListener();

    static const wl_keyboard_listener& keyboardListener();

    void handleKey(std::uint32_t code, bool pressed);

    void stopRepeat() {
        repeatCode_ = 0;
        itimerspec its{};
        if (repeatFd_ >= 0) timerfd_settime(repeatFd_, 0, &its, nullptr);
    }

    // ------------------------------------------------------------ cursor
    void loadCursorTheme();

    void applyCursor();

    // ------------------------------------------------------------ clipboard
    static const wl_data_offer_listener& offerListener();

    static const wl_data_device_listener& dataDeviceListener();

    static const wl_data_source_listener& dataSourceListener();

    // ------------------------------------------------------------ teardown
    void destroy();

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_shm* shm_ = nullptr;
    xdg_wm_base* wm_base_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    wl_keyboard* keyboard_ = nullptr;
    wl_data_device_manager* data_mgr_ = nullptr;
    wl_data_device* data_device_ = nullptr;
    wl_data_source* source_ = nullptr;
    wl_data_offer* offer_ = nullptr;
    zxdg_decoration_manager_v1* decoration_mgr_ = nullptr;
    ext_background_effect_manager_v1* bgfx_mgr_ = nullptr;
    ext_background_effect_surface_v1* bgfx_ = nullptr;
    int blurW_ = 0, blurH_ = 0;
    zxdg_toplevel_decoration_v1* decoration_ = nullptr;
    wl_surface* surface_ = nullptr;
    xdg_surface* xdg_surface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;
    std::uint32_t compositorVersion_ = 1;

    std::vector<Output> outputs_;
    std::vector<wl_output*> entered_;
    bool usePreferredScale_ = false;
    int scale_ = 1;
    int width_ = 1280, height_ = 800;
    int pendingW_ = 0, pendingH_ = 0;
    bool pendingMaximized_ = false, pendingFullscreen_ = false, pendingActivated_ = false;
    wl_callback* frameCb_ = nullptr;
    std::chrono::steady_clock::time_point frameAsked_{};
    bool maximized_ = false, fullscreen_ = false, focused_ = false, configured_ = false, closed_ = false,
         serverDeco_ = false;

    xkb_context* xkb_ = nullptr;
    xkb_keymap* keymap_ = nullptr;
    xkb_state* xkbState_ = nullptr;
    int repeatFd_ = -1;
    int repeatRate_ = 25, repeatDelay_ = 400;
    std::uint32_t repeatCode_ = 0;
    KeyEvent repeatEvent_;
    std::uint32_t inputSerial_ = 0, pointerSerial_ = 0;

    float px_ = 0, py_ = 0, axisX_ = 0, axisY_ = 0;
    int stepsX_ = 0, stepsY_ = 0;

    wl_cursor_theme* theme_ = nullptr;
    wl_surface* cursorSurface_ = nullptr;
    Cursor cursor_ = Cursor::Default;

    std::string clipboard_;
    std::vector<std::string> pendingMimes_, offerMimes_;
};

}  // namespace ide

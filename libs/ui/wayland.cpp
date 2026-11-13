// SPDX-License-Identifier: GPL-3.0-or-later
// wayland - implementation of module daemon.ui.wayland.
module;
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "ext-background-effect-v1-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"
#include <fcntl.h>
#include <cerrno>
#include <poll.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
module daemon.ui.wayland;
import std;
import daemon.ui.input;
import daemon.base.util;

namespace ide {

std::expected<void, std::string> Window::create(std::string_view title, int width, int height) {
    width_ = width;
    height_ = height;
    display_ = wl_display_connect(nullptr);
    if (!display_)
        return std::unexpected("cannot connect to a Wayland compositor (is WAYLAND_DISPLAY / XDG_RUNTIME_DIR set?)");

    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registryListener(), this);
    wl_display_roundtrip(display_);
    wl_display_roundtrip(display_);  // seat capabilities, output scales
    if (!compositor_) return std::unexpected("compositor does not provide wl_compositor");
    if (!shm_) return std::unexpected("compositor does not provide wl_shm");
    if (!wm_base_) return std::unexpected("compositor does not support xdg-shell");

    xkb_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    repeatFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);

    surface_ = wl_compositor_create_surface(compositor_);
    wl_surface_add_listener(surface_, &surfaceListener(), this);
    xdg_surface_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
    xdg_surface_add_listener(xdg_surface_, &xdgSurfaceListener(), this);
    toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    xdg_toplevel_add_listener(toplevel_, &toplevelListener(), this);
    setTitle(title);
    xdg_toplevel_set_app_id(toplevel_, "daemon");
    xdg_toplevel_set_min_size(toplevel_, 480, 320);
    updateBlur();
    if (decoration_mgr_) {
        decoration_ = zxdg_decoration_manager_v1_get_toplevel_decoration(decoration_mgr_, toplevel_);
        zxdg_toplevel_decoration_v1_add_listener(decoration_, &decorationListener(), this);
        zxdg_toplevel_decoration_v1_set_mode(decoration_, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    if (data_mgr_ && seat_) {
        data_device_ = wl_data_device_manager_get_data_device(data_mgr_, seat_);
        wl_data_device_add_listener(data_device_, &dataDeviceListener(), this);
    }
    loadCursorTheme();
    wl_surface_commit(surface_);

    for (int i = 0; i < 50 && !configured_ && !closed_; ++i)
        if (wl_display_roundtrip(display_) < 0) break;
    if (!configured_) return std::unexpected("compositor never configured the window");
    return {};
}

void Window::setIcon(const std::vector<std::uint32_t>& argb, int size) {
    if (!icon_mgr_ || !shm_ || !toplevel_ || size <= 0 || argb.size() < static_cast<std::size_t>(size) * size) return;
    const std::size_t bytes = static_cast<std::size_t>(size) * size * 4;
    int fd = ::memfd_create("daemon-icon", MFD_CLOEXEC);
    if (fd < 0) return;
    if (::ftruncate(fd, static_cast<off_t>(bytes)) < 0) { ::close(fd); return; }
    void* map = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { ::close(fd); return; }
    std::memcpy(map, argb.data(), bytes);
    ::munmap(map, bytes);
    wl_shm_pool* pool = wl_shm_create_pool(shm_, fd, static_cast<int>(bytes));
    wl_buffer* buf = wl_shm_pool_create_buffer(pool, 0, size, size, size * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    ::close(fd);
    xdg_toplevel_icon_v1* icon = xdg_toplevel_icon_manager_v1_create_icon(icon_mgr_);
    xdg_toplevel_icon_v1_set_name(icon, "daemon");   // a themed icon wins when installed, the pixels are the fallback
    xdg_toplevel_icon_v1_add_buffer(icon, buf, 1);
    xdg_toplevel_icon_manager_v1_set_icon(icon_mgr_, toplevel_, icon);
    xdg_toplevel_icon_v1_destroy(icon);
    wl_buffer_destroy(buf);
    wl_display_flush(display_);
}

void Window::requestFrame() {
    if (frameCb_) return;
    static const wl_callback_listener listener = {[](void* d, wl_callback* cb, std::uint32_t) {
        auto* w = static_cast<Window*>(d);
        wl_callback_destroy(cb);
        w->frameCb_ = nullptr;
    }};
    frameCb_ = wl_surface_frame(surface_);
    wl_callback_add_listener(frameCb_, &listener, this);
    frameAsked_ = std::chrono::steady_clock::now();
}

long long Window::frameWaitMs() const {
    if (!frameCb_) return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - frameAsked_).count();
}

bool Window::pump(int timeoutMs) {
    while (wl_display_prepare_read(display_) != 0) wl_display_dispatch_pending(display_);
    // A full socket (EAGAIN) leaves requests unsent: also wait for POLLOUT so they go out as soon as possible.
    const bool blocked = wl_display_flush(display_) < 0 && errno == EAGAIN;
    std::vector<pollfd> fds = {{wl_display_get_fd(display_), static_cast<short>(blocked ? POLLIN | POLLOUT : POLLIN), 0},
                               {repeatFd_, POLLIN, 0}};
    for (const auto& w : watches_) fds.push_back({w.fd, POLLIN, 0});
    int r = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);
    if (r > 0 && (fds[0].revents & (POLLHUP | POLLERR))) {
        logLine("wayland connection hung up");
        closed_ = true;
    }
    if (r > 0 && (fds[0].revents & POLLIN)) {
        if (wl_display_read_events(display_) < 0) closed_ = true;
    } else {
        wl_display_cancel_read(display_);
    }
    if (wl_display_dispatch_pending(display_) < 0) closed_ = true;
    if (r > 0 && (fds[1].revents & POLLIN)) {
        std::uint64_t expirations = 0;
        if (::read(repeatFd_, &expirations, sizeof expirations) > 0 && repeatCode_ && onKey) {
            KeyEvent ev = repeatEvent_;
            ev.repeat = true;
            onKey(ev);
        }
    }
    if (r > 0) {  // watched descriptors (child processes, language server)
        std::vector<int> ready;
        for (std::size_t i = 2; i < fds.size(); ++i)
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) ready.push_back(fds[i].fd);
        for (int fd : ready) {
            auto it = std::ranges::find(watches_, fd, &Watch::fd);  // a callback may have unwatched it
            if (it != watches_.end()) { auto cb = it->cb; cb(); }
        }
    }
    return !closed_ && wl_display_get_error(display_) == 0;
}

void Window::updateBlur() {
    if (!bgfx_mgr_ || (blurW_ == width_ && blurH_ == height_ && bgfx_)) return;
    if (!bgfx_) bgfx_ = ext_background_effect_manager_v1_get_background_effect(bgfx_mgr_, surface_);
    wl_region* r = wl_compositor_create_region(compositor_);
    wl_region_add(r, 0, 0, width_, height_);
    ext_background_effect_surface_v1_set_blur_region(bgfx_, r);
    wl_region_destroy(r);
    blurW_ = width_; blurH_ = height_;
}

void Window::setClipboard(std::string text) {
    if (!data_mgr_ || !data_device_) return;
    clipboard_ = std::move(text);
    if (source_) wl_data_source_destroy(source_);
    source_ = wl_data_device_manager_create_data_source(data_mgr_);
    wl_data_source_add_listener(source_, &dataSourceListener(), this);
    for (const char* mime : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING"})
        wl_data_source_offer(source_, mime);
    wl_data_device_set_selection(data_device_, source_, inputSerial_);
    wl_display_flush(display_);
}

std::string Window::clipboard() {
    if (source_) return clipboard_;  // we own the selection
    if (!offer_) return {};
    const char* mime = nullptr;
    for (const char* want : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"})
        if (std::ranges::find(offerMimes_, std::string_view(want)) != offerMimes_.end()) { mime = want; break; }
    if (!mime) return {};
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) < 0) return {};
    wl_data_offer_receive(offer_, mime, fds[1]);
    ::close(fds[1]);
    wl_display_flush(display_);
    std::string out;
    char buf[4096];
    while (true) {
        pollfd p{fds[0], POLLIN, 0};
        if (::poll(&p, 1, 500) <= 0) break;  // never hang on a stuck source
        ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
        if (out.size() > (64u << 20)) break;
    }
    ::close(fds[0]);
    return out;
}

const wl_registry_listener& Window::registryListener() {
    static const wl_registry_listener l = [] {
        wl_registry_listener l{};
        l.global = [](void* d, wl_registry* reg, std::uint32_t name, const char* iface, std::uint32_t ver) {
            static_cast<Window*>(d)->onGlobal(reg, name, iface, ver);
        };
        l.global_remove = [](void* d, wl_registry*, std::uint32_t name) {
            auto* w = static_cast<Window*>(d);
            std::erase_if(w->outputs_, [&](const Output& o) { return o.name == name; });
        };
        return l;
    }();
    return l;
}

void Window::onGlobal(wl_registry* reg, std::uint32_t name, const char* iface, std::uint32_t ver) {
    std::string_view i = iface;
    if (i == wl_compositor_interface.name) {
        compositor_ = static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, std::min(ver, 6u)));
        compositorVersion_ = std::min(ver, 6u);
    } else if (i == wl_shm_interface.name) {
        shm_ = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (i == ext_background_effect_manager_v1_interface.name) {
        bgfx_mgr_ = static_cast<ext_background_effect_manager_v1*>(wl_registry_bind(reg, name, &ext_background_effect_manager_v1_interface, 1));
    } else if (i == xdg_toplevel_icon_manager_v1_interface.name) {
        icon_mgr_ = static_cast<xdg_toplevel_icon_manager_v1*>(wl_registry_bind(reg, name, &xdg_toplevel_icon_manager_v1_interface, 1));
    } else if (i == xdg_wm_base_interface.name) {
        wm_base_ = static_cast<xdg_wm_base*>(wl_registry_bind(reg, name, &xdg_wm_base_interface, std::min(ver, 5u)));
        static const xdg_wm_base_listener l = [] {
            xdg_wm_base_listener l{};
            l.ping = [](void*, xdg_wm_base* b, std::uint32_t serial) { xdg_wm_base_pong(b, serial); };
            return l;
        }();
        xdg_wm_base_add_listener(wm_base_, &l, this);
    } else if (i == wl_seat_interface.name && !seat_) {
        seat_ = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, std::min(ver, 5u)));
        wl_seat_add_listener(seat_, &seatListener(), this);
    } else if (i == wl_output_interface.name) {
        auto* out = static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, std::min(ver, 3u)));
        outputs_.push_back({out, name, 1});
        wl_output_add_listener(out, &outputListener(), this);
    } else if (i == wl_data_device_manager_interface.name) {
        data_mgr_ = static_cast<wl_data_device_manager*>(wl_registry_bind(reg, name, &wl_data_device_manager_interface, std::min(ver, 3u)));
    } else if (i == zxdg_decoration_manager_v1_interface.name) {
        decoration_mgr_ = static_cast<zxdg_decoration_manager_v1*>(wl_registry_bind(reg, name, &zxdg_decoration_manager_v1_interface, 1));
    }
}

const wl_output_listener& Window::outputListener() {
    static const wl_output_listener l = [] {
        wl_output_listener l{};
        l.geometry = [](void*, wl_output*, int, int, int, int, int, const char*, const char*, int) {};
        l.mode = [](void*, wl_output*, std::uint32_t, int, int, int) {};
        l.done = [](void*, wl_output*) {};
        l.scale = [](void* d, wl_output* o, int s) {
            auto* w = static_cast<Window*>(d);
            for (auto& out : w->outputs_) if (out.output == o) out.scale = s;
            w->updateScaleFromOutputs();
        };
        l.name = [](void*, wl_output*, const char*) {};
        l.description = [](void*, wl_output*, const char*) {};
        return l;
    }();
    return l;
}

const wl_surface_listener& Window::surfaceListener() {
    static const wl_surface_listener l = [] {
        wl_surface_listener l{};
        l.enter = [](void* d, wl_surface*, wl_output* o) {
            auto* w = static_cast<Window*>(d);
            w->entered_.push_back(o);
            w->updateScaleFromOutputs();
        };
        l.leave = [](void* d, wl_surface*, wl_output* o) {
            auto* w = static_cast<Window*>(d);
            std::erase(w->entered_, o);
            w->updateScaleFromOutputs();
        };
        l.preferred_buffer_scale = [](void* d, wl_surface*, std::int32_t s) {
            auto* w = static_cast<Window*>(d);
            w->usePreferredScale_ = true;
            w->setScale(std::max(1, s));
        };
        l.preferred_buffer_transform = [](void*, wl_surface*, std::uint32_t) {};
        return l;
    }();
    return l;
}

void Window::updateScaleFromOutputs() {
    if (usePreferredScale_) return;
    int best = 1;
    for (wl_output* e : entered_)
        for (auto& o : outputs_) if (o.output == e) best = std::max(best, o.scale);
    setScale(best);
}

void Window::setScale(int s) {
    if (s == scale_) return;
    scale_ = s;
    wl_surface_set_buffer_scale(surface_, s);
    loadCursorTheme();
    updateBlur();
    if (configured_ && onResize) onResize(width_, height_, scale_);
}

const xdg_surface_listener& Window::xdgSurfaceListener() {
    static const xdg_surface_listener l = [] {
        xdg_surface_listener l{};
        l.configure = [](void* d, xdg_surface* s, std::uint32_t serial) {
            auto* w = static_cast<Window*>(d);
            xdg_surface_ack_configure(s, serial);
            if (w->pendingW_ > 0 && w->pendingH_ > 0) { w->width_ = w->pendingW_; w->height_ = w->pendingH_; }
            w->maximized_ = w->pendingMaximized_;
            w->fullscreen_ = w->pendingFullscreen_;
            bool activated = w->pendingActivated_;
            w->configured_ = true;
            w->updateBlur();
            if (w->onResize) w->onResize(w->width_, w->height_, w->scale_);
            if (activated != w->focused_) { w->focused_ = activated; if (w->onFocus) w->onFocus(activated); }
        };
        return l;
    }();
    return l;
}

const xdg_toplevel_listener& Window::toplevelListener() {
    static const xdg_toplevel_listener l = [] {
        xdg_toplevel_listener l{};
        l.configure = [](void* d, xdg_toplevel*, std::int32_t w, std::int32_t h, wl_array* states) {
            auto* self = static_cast<Window*>(d);
            self->pendingW_ = w;
            self->pendingH_ = h;
            self->pendingMaximized_ = self->pendingFullscreen_ = self->pendingActivated_ = false;
            auto* p = static_cast<std::uint32_t*>(states->data);
            for (std::size_t i = 0; i < states->size / sizeof(std::uint32_t); ++i) {
                switch (p[i]) {
                    case XDG_TOPLEVEL_STATE_MAXIMIZED: self->pendingMaximized_ = true; break;
                    case XDG_TOPLEVEL_STATE_FULLSCREEN: self->pendingFullscreen_ = true; break;
                    case XDG_TOPLEVEL_STATE_ACTIVATED: self->pendingActivated_ = true; break;
                    default: break;
                }
            }
        };
        l.close = [](void* d, xdg_toplevel*) {
            auto* w = static_cast<Window*>(d);
            if (w->onClose) w->onClose(); else w->closed_ = true;
        };
        l.configure_bounds = [](void*, xdg_toplevel*, std::int32_t, std::int32_t) {};
        l.wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {};
        return l;
    }();
    return l;
}

const zxdg_toplevel_decoration_v1_listener& Window::decorationListener() {
    static const zxdg_toplevel_decoration_v1_listener l = [] {
        zxdg_toplevel_decoration_v1_listener l{};
        l.configure = [](void* d, zxdg_toplevel_decoration_v1*, std::uint32_t mode) {
            static_cast<Window*>(d)->serverDeco_ = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
        };
        return l;
    }();
    return l;
}

const wl_seat_listener& Window::seatListener() {
    static const wl_seat_listener l = [] {
        wl_seat_listener l{};
        l.capabilities = [](void* d, wl_seat* seat, std::uint32_t caps) {
            auto* w = static_cast<Window*>(d);
            logLine(std::format("seat capabilities: pointer={} keyboard={}", (caps & WL_SEAT_CAPABILITY_POINTER) != 0,
                                (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0));
            // The compositor may withdraw a capability (suspend/resume, device re-plug) and announce it again
            // later: the old wl_pointer / wl_keyboard is dead then and a fresh one must be requested.
            if (!(caps & WL_SEAT_CAPABILITY_POINTER) && w->pointer_) {
                if (wl_pointer_get_version(w->pointer_) >= WL_POINTER_RELEASE_SINCE_VERSION) wl_pointer_release(w->pointer_);
                else wl_pointer_destroy(w->pointer_);
                w->pointer_ = nullptr;
                w->pointerSerial_ = 0;
            }
            if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && w->keyboard_) {
                w->stopRepeat();
                if (wl_keyboard_get_version(w->keyboard_) >= WL_KEYBOARD_RELEASE_SINCE_VERSION) wl_keyboard_release(w->keyboard_);
                else wl_keyboard_destroy(w->keyboard_);
                w->keyboard_ = nullptr;
            }
            if ((caps & WL_SEAT_CAPABILITY_POINTER) && !w->pointer_) {
                w->pointer_ = wl_seat_get_pointer(seat);
                wl_pointer_add_listener(w->pointer_, &pointerListener(), w);
            }
            if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !w->keyboard_) {
                w->keyboard_ = wl_seat_get_keyboard(seat);
                wl_keyboard_add_listener(w->keyboard_, &keyboardListener(), w);
            }
        };
        l.name = [](void*, wl_seat*, const char*) {};
        return l;
    }();
    return l;
}

const wl_pointer_listener& Window::pointerListener() {
    static const wl_pointer_listener l = [] {
        wl_pointer_listener l{};
        l.enter = [](void* d, wl_pointer*, std::uint32_t serial, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
            auto* w = static_cast<Window*>(d);
            w->pointerSerial_ = serial;
            w->px_ = static_cast<float>(wl_fixed_to_double(x));
            w->py_ = static_cast<float>(wl_fixed_to_double(y));
            w->applyCursor();
            if (w->onPointerMove) w->onPointerMove(w->px_, w->py_);
        };
        l.leave = [](void*, wl_pointer*, std::uint32_t, wl_surface*) {};
        l.motion = [](void* d, wl_pointer*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
            auto* w = static_cast<Window*>(d);
            w->px_ = static_cast<float>(wl_fixed_to_double(x));
            w->py_ = static_cast<float>(wl_fixed_to_double(y));
            if (w->onPointerMove) w->onPointerMove(w->px_, w->py_);
        };
        l.button = [](void* d, wl_pointer*, std::uint32_t serial, std::uint32_t, std::uint32_t button, std::uint32_t state) {
            auto* w = static_cast<Window*>(d);
            w->inputSerial_ = serial;
            Button b;
            switch (button) {  // linux/input-event-codes.h: BTN_LEFT, BTN_RIGHT, BTN_MIDDLE
                case 0x110: b = Button::Left; break;
                case 0x111: b = Button::Right; break;
                case 0x112: b = Button::Middle; break;
                default: return;
            }
            if (w->onPointerButton) w->onPointerButton(b, state == WL_POINTER_BUTTON_STATE_PRESSED, w->px_, w->py_);
        };
        l.axis = [](void* d, wl_pointer*, std::uint32_t, std::uint32_t axis, wl_fixed_t value) {
            auto* w = static_cast<Window*>(d);
            float v = static_cast<float>(wl_fixed_to_double(value));
            (axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? w->axisY_ : w->axisX_) += v;
        };
        l.frame = [](void* d, wl_pointer*) {
            auto* w = static_cast<Window*>(d);
            if (w->axisX_ == 0 && w->axisY_ == 0 && w->stepsX_ == 0 && w->stepsY_ == 0) return;
            float ax = w->axisX_, ay = w->axisY_;
            int sx = w->stepsX_, sy = w->stepsY_;
            w->axisX_ = w->axisY_ = 0;
            w->stepsX_ = w->stepsY_ = 0;
            if (w->onScroll) w->onScroll(ax, ay, sx, sy);
        };
        l.axis_source = [](void*, wl_pointer*, std::uint32_t) {};
        l.axis_stop = [](void*, wl_pointer*, std::uint32_t, std::uint32_t) {};
        l.axis_discrete = [](void* d, wl_pointer*, std::uint32_t axis, std::int32_t steps) {
            auto* w = static_cast<Window*>(d);
            (axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? w->stepsY_ : w->stepsX_) += steps;
        };
        l.axis_value120 = [](void*, wl_pointer*, std::uint32_t, std::int32_t) {};
        l.axis_relative_direction = [](void*, wl_pointer*, std::uint32_t, std::uint32_t) {};
        return l;
    }();
    return l;
}

const wl_keyboard_listener& Window::keyboardListener() {
    static const wl_keyboard_listener l = [] {
        wl_keyboard_listener l{};
        l.keymap = [](void* d, wl_keyboard*, std::uint32_t format, std::int32_t fd, std::uint32_t size) {
            auto* w = static_cast<Window*>(d);
            if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
                void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                if (map != MAP_FAILED) {
                    xkb_keymap* km = xkb_keymap_new_from_string(w->xkb_, static_cast<const char*>(map),
                                                                 XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
                    ::munmap(map, size);
                    if (km) {
                        if (w->xkbState_) xkb_state_unref(w->xkbState_);
                        if (w->keymap_) xkb_keymap_unref(w->keymap_);
                        w->keymap_ = km;
                        w->xkbState_ = xkb_state_new(km);
                    }
                }
            }
            ::close(fd);
        };
        l.enter = [](void* d, wl_keyboard*, std::uint32_t serial, wl_surface*, wl_array*) {
            static_cast<Window*>(d)->inputSerial_ = serial;
        };
        l.leave = [](void* d, wl_keyboard*, std::uint32_t, wl_surface*) { static_cast<Window*>(d)->stopRepeat(); };
        l.key = [](void* d, wl_keyboard*, std::uint32_t serial, std::uint32_t, std::uint32_t key, std::uint32_t state) {
            auto* w = static_cast<Window*>(d);
            w->inputSerial_ = serial;
            w->handleKey(key + 8, state == WL_KEYBOARD_KEY_STATE_PRESSED);
        };
        l.modifiers = [](void* d, wl_keyboard*, std::uint32_t, std::uint32_t depressed, std::uint32_t latched,
                         std::uint32_t locked, std::uint32_t group) {
            auto* w = static_cast<Window*>(d);
            if (w->xkbState_) xkb_state_update_mask(w->xkbState_, depressed, latched, locked, 0, 0, group);
        };
        l.repeat_info = [](void* d, wl_keyboard*, std::int32_t rate, std::int32_t delay) {
            auto* w = static_cast<Window*>(d);
            w->repeatRate_ = rate;
            w->repeatDelay_ = delay;
        };
        return l;
    }();
    return l;
}

void Window::handleKey(std::uint32_t code, bool pressed) {
    if (!xkbState_) return;
    if (!pressed) {
        if (code == repeatCode_) stopRepeat();
        return;
    }
    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkbState_, code);
    auto active = [&](const char* mod) {
        return xkb_state_mod_name_is_active(xkbState_, mod, XKB_STATE_MODS_EFFECTIVE) > 0;
    };
    KeyEvent ev;
    ev.ctrl = active(XKB_MOD_NAME_CTRL);
    ev.shift = active(XKB_MOD_NAME_SHIFT);
    ev.alt = active(XKB_MOD_NAME_ALT);
    ev.super = active(XKB_MOD_NAME_LOGO);

    switch (sym) {
        case XKB_KEY_Left: case XKB_KEY_KP_Left: ev.key = Key::Left; break;
        case XKB_KEY_Right: case XKB_KEY_KP_Right: ev.key = Key::Right; break;
        case XKB_KEY_Up: case XKB_KEY_KP_Up: ev.key = Key::Up; break;
        case XKB_KEY_Down: case XKB_KEY_KP_Down: ev.key = Key::Down; break;
        case XKB_KEY_Home: case XKB_KEY_KP_Home: ev.key = Key::Home; break;
        case XKB_KEY_End: case XKB_KEY_KP_End: ev.key = Key::End; break;
        case XKB_KEY_Page_Up: case XKB_KEY_KP_Page_Up: ev.key = Key::PageUp; break;
        case XKB_KEY_Page_Down: case XKB_KEY_KP_Page_Down: ev.key = Key::PageDown; break;
        case XKB_KEY_BackSpace: ev.key = Key::Backspace; break;
        case XKB_KEY_Delete: case XKB_KEY_KP_Delete: ev.key = Key::Delete; break;
        case XKB_KEY_Return: case XKB_KEY_KP_Enter: ev.key = Key::Enter; break;
        case XKB_KEY_Tab: ev.key = Key::Tab; break;
        case XKB_KEY_ISO_Left_Tab: ev.key = Key::Tab; ev.shift = true; break;
        case XKB_KEY_Escape: ev.key = Key::Escape; break;
        case XKB_KEY_Insert: ev.key = Key::Insert; break;
        case XKB_KEY_F1: ev.key = Key::F1; break;
        case XKB_KEY_F2: ev.key = Key::F2; break;
        case XKB_KEY_F3: ev.key = Key::F3; break;
        case XKB_KEY_F4: ev.key = Key::F4; break;
        case XKB_KEY_F5: ev.key = Key::F5; break;
        case XKB_KEY_F6: ev.key = Key::F6; break;
        case XKB_KEY_F7: ev.key = Key::F7; break;
        case XKB_KEY_F8: ev.key = Key::F8; break;
        case XKB_KEY_F9: ev.key = Key::F9; break;
        case XKB_KEY_F10: ev.key = Key::F10; break;
        case XKB_KEY_F11: ev.key = Key::F11; break;
        case XKB_KEY_F12: ev.key = Key::F12; break;
        default: {
            char32_t cp = xkb_keysym_to_utf32(xkb_keysym_to_lower(sym));
            if (cp == 0) return;  // modifier keys etc.
            ev.key = Key::Char;
            ev.ch = cp;
        }
    }
    if (!ev.ctrl && !ev.alt && !ev.super) {
        char buf[16];
        int n = xkb_state_key_get_utf8(xkbState_, code, buf, sizeof buf);
        if (n > 0 && n < static_cast<int>(sizeof buf)) {
            std::string_view t(buf, static_cast<std::size_t>(n));
            bool control = static_cast<unsigned char>(t[0]) < 0x20 || t[0] == 0x7f;
            if (!control) ev.text = std::string(t);
        }
    }

    if (xkb_keymap_key_repeats(keymap_, code) && repeatRate_ > 0) {
        repeatCode_ = code;
        repeatEvent_ = ev;
        long interval = 1'000'000'000L / repeatRate_;
        itimerspec its{};
        its.it_value.tv_sec = repeatDelay_ / 1000;
        its.it_value.tv_nsec = (repeatDelay_ % 1000) * 1'000'000L;
        its.it_interval.tv_sec = interval / 1'000'000'000L;
        its.it_interval.tv_nsec = interval % 1'000'000'000L;
        timerfd_settime(repeatFd_, 0, &its, nullptr);
    }
    if (onKey) onKey(ev);
}

void Window::loadCursorTheme() {
    if (!shm_ || !compositor_) return;
    if (theme_) wl_cursor_theme_destroy(theme_);
    const char* env = std::getenv("XCURSOR_SIZE");
    int size = env ? std::max(8, std::atoi(env)) : 24;
    theme_ = wl_cursor_theme_load(std::getenv("XCURSOR_THEME"), size * scale_, shm_);
    if (!cursorSurface_) cursorSurface_ = wl_compositor_create_surface(compositor_);
    if (pointer_) applyCursor();
}

void Window::applyCursor() {
    if (!theme_ || !pointer_ || !cursorSurface_ || pointerSerial_ == 0) return;
    std::array<const char*, 3> names{};
    switch (cursor_) {
        case Cursor::Text: names = {"text", "xterm", "ibeam"}; break;
        case Cursor::Hand: names = {"pointer", "hand2", "hand1"}; break;
        case Cursor::ResizeN: names = {"n-resize", "top_side", "ns-resize"}; break;
        case Cursor::ResizeS: names = {"s-resize", "bottom_side", "ns-resize"}; break;
        case Cursor::ResizeW: names = {"w-resize", "left_side", "ew-resize"}; break;
        case Cursor::ResizeE: names = {"e-resize", "right_side", "ew-resize"}; break;
        case Cursor::ResizeNW: names = {"nw-resize", "top_left_corner", "nwse-resize"}; break;
        case Cursor::ResizeNE: names = {"ne-resize", "top_right_corner", "nesw-resize"}; break;
        case Cursor::ResizeSW: names = {"sw-resize", "bottom_left_corner", "nesw-resize"}; break;
        case Cursor::ResizeSE: names = {"se-resize", "bottom_right_corner", "nwse-resize"}; break;
        default: names = {"default", "left_ptr", "arrow"}; break;
    }
    wl_cursor* c = nullptr;
    for (const char* n : names)
        if ((c = wl_cursor_theme_get_cursor(theme_, n))) break;
    if (!c && (c = wl_cursor_theme_get_cursor(theme_, "left_ptr")) == nullptr) return;
    wl_cursor_image* img = c->images[0];
    wl_surface_set_buffer_scale(cursorSurface_, scale_);
    wl_surface_attach(cursorSurface_, wl_cursor_image_get_buffer(img), 0, 0);
    wl_surface_damage_buffer(cursorSurface_, 0, 0, static_cast<int>(img->width), static_cast<int>(img->height));
    wl_surface_commit(cursorSurface_);
    wl_pointer_set_cursor(pointer_, pointerSerial_, cursorSurface_, static_cast<int>(img->hotspot_x) / scale_,
                          static_cast<int>(img->hotspot_y) / scale_);
}

const wl_data_offer_listener& Window::offerListener() {
    static const wl_data_offer_listener l = [] {
        wl_data_offer_listener l{};
        l.offer = [](void* d, wl_data_offer*, const char* mime) {
            static_cast<Window*>(d)->pendingMimes_.emplace_back(mime);
        };
        l.source_actions = [](void*, wl_data_offer*, std::uint32_t) {};
        l.action = [](void*, wl_data_offer*, std::uint32_t) {};
        return l;
    }();
    return l;
}

const wl_data_device_listener& Window::dataDeviceListener() {
    static const wl_data_device_listener l = [] {
        wl_data_device_listener l{};
        l.data_offer = [](void* d, wl_data_device*, wl_data_offer* offer) {
            auto* w = static_cast<Window*>(d);
            w->pendingMimes_.clear();
            wl_data_offer_add_listener(offer, &offerListener(), w);
        };
        l.enter = [](void*, wl_data_device*, std::uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer* o) {
            if (o) wl_data_offer_destroy(o);  // drag & drop is not supported
        };
        l.leave = [](void*, wl_data_device*) {};
        l.motion = [](void*, wl_data_device*, std::uint32_t, wl_fixed_t, wl_fixed_t) {};
        l.drop = [](void*, wl_data_device*) {};
        l.selection = [](void* d, wl_data_device*, wl_data_offer* offer) {
            auto* w = static_cast<Window*>(d);
            if (w->offer_) wl_data_offer_destroy(w->offer_);
            w->offer_ = offer;
            w->offerMimes_ = std::move(w->pendingMimes_);
            w->pendingMimes_.clear();
        };
        return l;
    }();
    return l;
}

const wl_data_source_listener& Window::dataSourceListener() {
    static const wl_data_source_listener l = [] {
        wl_data_source_listener l{};
        l.target = [](void*, wl_data_source*, const char*) {};
        l.send = [](void* d, wl_data_source*, const char*, std::int32_t fd) {
            auto* w = static_cast<Window*>(d);
            const char* p = w->clipboard_.data();
            std::size_t left = w->clipboard_.size();
            while (left > 0) {
                ssize_t n = ::write(fd, p, left);
                if (n <= 0) break;
                p += n;
                left -= static_cast<std::size_t>(n);
            }
            ::close(fd);
        };
        l.cancelled = [](void* d, wl_data_source* s) {
            auto* w = static_cast<Window*>(d);
            wl_data_source_destroy(s);
            if (w->source_ == s) w->source_ = nullptr;
        };
        l.dnd_drop_performed = [](void*, wl_data_source*) {};
        l.dnd_finished = [](void*, wl_data_source*) {};
        l.action = [](void*, wl_data_source*, std::uint32_t) {};
        return l;
    }();
    return l;
}

void Window::destroy() {
    if (repeatFd_ >= 0) ::close(repeatFd_);
    if (source_) wl_data_source_destroy(source_);
    if (offer_) wl_data_offer_destroy(offer_);
    if (data_device_) wl_data_device_destroy(data_device_);
    if (xkbState_) xkb_state_unref(xkbState_);
    if (keymap_) xkb_keymap_unref(keymap_);
    if (xkb_) xkb_context_unref(xkb_);
    if (theme_) wl_cursor_theme_destroy(theme_);
    if (cursorSurface_) wl_surface_destroy(cursorSurface_);
    if (decoration_) zxdg_toplevel_decoration_v1_destroy(decoration_);
    if (toplevel_) xdg_toplevel_destroy(toplevel_);
    if (xdg_surface_) xdg_surface_destroy(xdg_surface_);
    if (surface_) wl_surface_destroy(surface_);
    if (display_) {
        wl_display_flush(display_);
        wl_display_disconnect(display_);
    }
    display_ = nullptr;
    repeatFd_ = -1;
}

}  // namespace ide

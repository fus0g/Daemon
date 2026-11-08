// app_ui_helpers - implementation of module daemon.app.
module daemon.app;
import std;
import daemon.base.util;
import daemon.ui.input;
import daemon.text.buffer;
import daemon.text.syntax;
import daemon.ui.font;
import daemon.ui.draw;
import daemon.ui.geometry;
import daemon.app.options;
import daemon.app.theme;
import daemon.ui.vulkan;
import daemon.ui.wayland;
import daemon.text.document;
import daemon.services.explorer;
import daemon.ui.icons;
import daemon.base.json;
import daemon.base.process;
import daemon.services.cmake;
import daemon.services.project;
import daemon.services.git;
import daemon.services.search;
import daemon.services.gdb;
import daemon.services.terminal;
import daemon.services.lsp;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace ide {

float App::uiLabelR(float xRight, float rowY, float rowH, std::string_view t, Color c) {
    const float w = uiW(t);
    uiLabel(xRight - w, rowY, rowH, t, c);
    return xRight - w;
}

float App::uiLabelC(float cx, float rowY, float rowH, std::string_view t, Color c) {
    const float w = uiW(t);
    uiLabel(cx - w / 2, rowY, rowH, t, c);
    return w;
}

}  // namespace ide

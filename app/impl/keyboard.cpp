// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / keyboard - part of the App implementation (see app.cppm for the class).
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

bool App::runKey(const KeyEvent& k) {
    if (k.alt || k.super) return false;
    if (k.key == Key::Escape) { runFocus_ = false; return true; }
    if (k.ctrl) {
        if (k.key != Key::Char) return false;
        if (k.ch == 'c' && !k.shift) { runProc_->write("\x03"); return true; }   // interrupt
        if (k.ch == 'd' && !k.shift) { runProc_->write("\x04"); return true; }   // end of input
        if (k.ch == 'v' && !k.shift) { std::string c = win_.clipboard(); std::erase(c, '\r'); runProc_->write(c); return true; }
        return false;
    }
    switch (k.key) {
        case Key::Enter: runProc_->write("\n"); return true;
        case Key::Backspace: runProc_->write("\x7f"); return true;
        case Key::Tab: runProc_->write("\t"); return true;
        case Key::Left: case Key::Right: case Key::Up: case Key::Down: case Key::Home: case Key::End:
        case Key::PageUp: case Key::PageDown: case Key::Delete: case Key::Insert: return true;
        case Key::Char: if (!k.text.empty()) runProc_->write(k.text); return true;
        default: return false;   // function keys (build / run / stop ...) keep working
    }
}

void App::onKey(const KeyEvent& k) {
    resetCaret();
    if (!(k.ctrl && k.key == Key::Char && k.ch == 'q')) quitArmed_ = false;
    if (!(k.ctrl && k.key == Key::Char && k.ch == 'w')) closeArmed_ = -1;
    if (dlg_.kind != DlgKind::None) { dlgKey(k); dirty_ = true; return; }
    if (k.ctrl && !k.shift && !k.alt && k.key == Key::Char && k.ch == 'c' && copyOverlay()) { dirty_ = true; return; }
    if (gitMsgFocus_ && gitView() && gitMsgKey(k)) { dirty_ = true; return; }
    if (sFocus_ >= 0 && searchView() && searchKey(k)) { dirty_ = true; return; }
    if (termFocus_ && panelOpen_ && panelTab_ == 3 && termKey(k)) { dirty_ = true; return; }
    if (dbgConFocus_ && panelOpen_ && panelTab_ == 4 && dbgConKey(k)) { dirty_ = true; return; }
    if (runFocus_ && runProc_ && panelOpen_ && panelTab_ == 1 && runKey(k)) { dirty_ = true; return; }
    if (tabDrag_.active) {  // a tab is being dragged: Esc cancels, other keys are ignored
        if (k.key == Key::Escape) { tabDrag_ = {}; dirty_ = true; }
        return;
    }
    if (menu_.open) { menuKey(k); dirty_ = true; return; }
    if (prompt_.kind != PromptKind::None) { promptKey(k); return; }
    if (sidebar_ && sideFocus_ && !sideGit_ && !sideSearch_ && !sideDebug_ && explorerKey(k)) { dirty_ = true; return; }
    editorKey(k);
    dirty_ = true;
}

}  // namespace ide

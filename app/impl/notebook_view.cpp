// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / notebook_view - the notebook tab: an .ipynb as a list of editable cells, executed by the C++ kernel.
module;
#include <unistd.h>
module daemon.app;
import std;
import daemon.base.util;
import daemon.ui.input;
import daemon.ui.font;
import daemon.ui.draw;
import daemon.ui.geometry;
import daemon.ui.icons;
import daemon.app.theme;
import daemon.app.mdview;
import daemon.text.buffer;
import daemon.text.markdown;
import daemon.text.syntax;
import daemon.services.notebook;
import daemon.services.lsp;
import daemon.base.json;
import daemon.base.process;

namespace ide {

namespace {

enum Btn { kRun, kRunAll, kStop, kRestart, kAddCode, kAddText, kDelete, kSave };

constexpr Color kCodeBox = Color::rgb(0x0f1114);
constexpr Color kErr = Color::rgb(0xf2777a);
constexpr Color kEditGreen = Color::rgb(0x6aab73);
constexpr int kMaxOutLines = 400;
constexpr int kMaxErrLines = 40;
int maxLinesFor(const nb::Output& o) { return o.kind == nb::Output::Kind::Error ? kMaxErrLines : kMaxOutLines; }
constexpr std::size_t kMaxOutBytes = 256 * 1024;

int lineCount(const std::string& t) {
    if (t.empty()) return 0;
    int n = static_cast<int>(std::ranges::count(t, '\n')) + (t.back() == '\n' ? 0 : 1);
    return n;
}

std::string_view lineOf(const std::string& t, int n) {   // the n-th line of `t` (no newline)
    std::size_t pos = 0;
    for (int i = 0; i < n; ++i) {
        const auto nl = t.find('\n', pos);
        if (nl == std::string::npos) return {};
        pos = nl + 1;
    }
    const auto nl = t.find('\n', pos);
    return std::string_view(t).substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
}

bool blank(const std::string& s) { return std::ranges::all_of(s, [](unsigned char c) { return std::isspace(c); }); }

}  // namespace

// ---------------------------------------------------------------------------- model

auto App::makeNbCell(NbView& v, nb::Cell data) -> NbCell {
    NbCell c;
    c.rid = v.nextRid++;
    c.doc = std::make_unique<Document>();
    while (!data.source.empty() && (data.source.back() == '\n' || data.source.back() == '\r')) data.source.pop_back();   // Jupyter stores no final newline
    c.doc->buf.setText(data.source);
    c.doc->buf.markSaved();
    data.source.clear();
    c.data = std::move(data);
    c.doc->lang = langForPath(c.data.type == nb::Cell::Type::Markdown ? "cell.md" : "cell.cpp");
    c.doc->hl.setLang(c.doc->lang);
    return c;
}

std::unique_ptr<Document> App::createNotebookDoc(const std::string& absPath, std::string& error) {
    std::string text;
    if (auto t = readFile(absPath)) text = std::move(*t);   // a path that does not exist yet becomes a new notebook
    auto parsed = nb::parseNotebook(text);
    if (!parsed) { error = parsed.error(); return nullptr; }
    auto d = std::make_unique<Document>();
    d->path = absPath;
    d->name = fs::path(absPath).filename().string();
    d->buf.readOnly = true;   // the text of a notebook lives in its cells
    d->stampDisk();
    auto view = std::make_unique<NbView>();
    view->owner = d.get();
    for (auto& c : parsed->cells) view->cells.push_back(makeNbCell(*view, std::move(c)));
    parsed->cells.clear();
    view->runnable = parsed->isCpp();
    view->meta = std::move(*parsed);
    view->md.src = d.get();
    notebooks_[d.get()] = std::move(view);
    return d;
}

void App::openNotebook(const std::string& abs) {
    for (std::size_t i = 0; i < docs_.size(); ++i)
        if (docs_[i]->path == abs) { switchTab(static_cast<int>(i)); return; }
    std::string err;
    auto d = createNotebookDoc(abs, err);
    if (!d) { setMessage(err, true, 5); return; }
    if (docs_.size() == 1 && docs_[0]->path.empty() && !docs_[0]->modified() && docs_[0]->buf.lineCount() == 1 && docs_[0]->buf.line(0).empty()) docs_.clear();
    docs_.push_back(std::move(d));
    cur_ = static_cast<int>(docs_.size()) - 1;
    revealCurrent();
    dirty_ = true;
}

std::string App::notebookText(NbView& v) {
    nb::Notebook out = v.meta;
    out.cells.clear();
    for (const auto& c : v.cells) {
        nb::Cell cell = c.data;
        cell.source = c.doc->buf.text();
        out.cells.push_back(std::move(cell));
    }
    return nb::serializeNotebook(out);
}

void App::saveNotebook(Document& d) {
    auto it = notebooks_.find(&d);
    if (it == notebooks_.end()) return;
    if (!writeFile(d.path, notebookText(*it->second))) { setMessage("cannot write " + d.path, true, 4); return; }
    d.extraModified = false;
    d.stampDisk();
    setMessage("Saved " + d.name);
}

bool App::notebookBusy() const {
    for (const auto& [d, v] : notebooks_) if (v->session && v->session->pending()) return true;
    return false;
}

void App::nbSelect(NbView& v, int i, bool edit) {
    if (v.cells.empty()) return;
    closeCompletion();
    v.sel = std::clamp(i, 0, static_cast<int>(v.cells.size()) - 1);
    v.editing = edit;
    if (edit && v.cells[static_cast<std::size_t>(v.sel)].data.type == nb::Cell::Type::Code) nbEnsureLsp(v);
    v.pendingD = false;
    v.follow = true;
    resetCaret();
}

void App::nbInsert(NbView& v, int at, nb::Cell::Type t) {
    nb::Cell c;
    c.type = t;
    c.id = nb::newCellId();
    at = std::clamp(at, 0, static_cast<int>(v.cells.size()));
    v.cells.insert(v.cells.begin() + at, makeNbCell(v, std::move(c)));
    nbSelect(v, at, false);
    nbMarkModified(v);
}

void App::nbDelete(NbView& v) {
    if (v.cells.empty()) return;
    if (v.cells.size() == 1) {   // the last cell is emptied instead
        NbCell& c = v.cells[0];
        c.doc->buf.setText("");
        c.data.outputs.clear();
        c.data.execCount = 0;
    } else {
        v.cells.erase(v.cells.begin() + v.sel);
    }
    nbSelect(v, std::min(v.sel, static_cast<int>(v.cells.size()) - 1), false);
    nbMarkModified(v);
}

void App::nbSetType(NbView& v, nb::Cell::Type t) {
    NbCell& c = v.cells[static_cast<std::size_t>(v.sel)];
    if (c.data.type == t) return;
    c.data.type = t;
    if (t != nb::Cell::Type::Code) c.data.outputs.clear();
    c.doc->lang = langForPath(t == nb::Cell::Type::Markdown ? "cell.md" : "cell.cpp");
    c.doc->hl.setLang(c.doc->lang);
    c.mdParsed = false;
    nbMarkModified(v);
}

bool App::nbNeedsSession(NbView& v) {
    if (!v.runnable) { setMessage("This notebook is not a C++ notebook (kernel: " + v.meta.metadata["kernelspec"]["name"].str("unknown") + ")", true, 5); return false; }
    if (!v.session) v.session = std::make_unique<nb::Session>(v.owner->path);
    return true;
}

void App::nbRun(NbView& v, int idx) {
    NbCell& c = v.cells[static_cast<std::size_t>(idx)];
    if (c.data.type == nb::Cell::Type::Markdown) { if (idx == v.sel) v.editing = false; c.mdParsed = false; dirty_ = true; return; }
    if (c.data.type != nb::Cell::Type::Code) return;
    if (!nbNeedsSession(v)) return;
    c.data.outputs.clear();
    c.queued = true;
    c.running = false;
    v.session->request(c.rid, c.doc->buf.text());
    nbMarkModified(v);
}

void App::nbRunAll(NbView& v) {
    for (std::size_t i = 0; i < v.cells.size(); ++i)
        if (v.cells[i].data.type == nb::Cell::Type::Code && !blank(v.cells[i].doc->buf.text())) nbRun(v, static_cast<int>(i));
}

// Applies what the kernel reported since the last frame.
void App::notebookPoll() {
    for (auto& [doc, vp] : notebooks_) {
        NbView& v = *vp;
        if (!v.session) continue;
        for (auto& e : v.session->poll()) {
            dirty_ = true;
            if (e.type == nb::Event::Type::Status) { v.note = e.text; continue; }
            auto it = std::ranges::find(v.cells, e.cell, &NbCell::rid);
            if (it == v.cells.end()) continue;   // the cell was deleted meanwhile
            NbCell& c = *it;
            auto& outs = c.data.outputs;
            switch (e.type) {
                case nb::Event::Type::Started: c.queued = false; c.running = true; c.data.execCount = e.execCount; break;
                case nb::Event::Type::Stream: {
                    std::size_t total = 0;
                    for (const auto& o : outs) total += o.text.size();
                    if (total > kMaxOutBytes) { if (outs.empty() || outs.back().text != "[output truncated]") outs.push_back({nb::Output::Kind::Stream, "stderr", "[output truncated]"}); break; }
                    if (!outs.empty() && outs.back().kind == nb::Output::Kind::Stream && outs.back().name == e.name) outs.back().text += e.text;
                    else outs.push_back({nb::Output::Kind::Stream, e.name, e.text});
                    break;
                }
                case nb::Event::Type::Result: outs.push_back({nb::Output::Kind::Result, "", e.text}); break;
                case nb::Event::Type::Error: outs.push_back({nb::Output::Kind::Error, e.name, e.text}); break;
                case nb::Event::Type::Finished: c.running = false; c.queued = false; v.note.clear(); nbMarkModified(v); break;
                default: break;
            }
        }
    }
}

// ---------------------------------------------------------------------------- layout and drawing

Pos App::nbCellPosAt(const NbView& v, const NbCell& c, float mx, float cy) const {
    (void)v;
    const float bpad = std::round(8 * S());
    const int lines = c.doc->buf.lineCount();
    const int line = std::clamp(static_cast<int>(std::floor((cy - (c.boxY + bpad)) / lh())), 0, lines - 1);
    const int dcol = std::max(0, static_cast<int>(std::floor((mx - (c.boxX + bpad)) / cw() + 0.5f)));
    return {line, byteColAt(c.doc->buf.line(line), dcol, c.doc->tabWidth)};
}

void App::layoutNotebook(Document& d) {
    auto it = notebooks_.find(&d);
    if (it == notebooks_.end()) return;
    NbView& v = *it->second;
    const Rect& e = L_.editor;
    const float s = S(), line = lh();
    const float tbH = std::round(uiH() + 14 * s);
    v.viewTop = e.y + tbH;
    v.viewH = std::max(0.0f, e.h - tbH);
    const float pad = std::round(14 * s), promptW = std::round(74 * s), gap = std::round(12 * s), bpad = std::round(8 * s);
    const float boxX = e.x + pad + promptW;
    const float boxW = std::max(160 * s, e.w - pad * 2 - promptW - std::round(12 * s));
    float y = std::round(10 * s);
    for (std::size_t i = 0; i < v.cells.size(); ++i) {
        NbCell& c = v.cells[i];
        const bool rendered = c.data.type == nb::Cell::Type::Markdown && !(v.editing && static_cast<int>(i) == v.sel);
        c.y = y;
        c.boxX = boxX;
        c.boxW = boxW;
        c.boxY = y;
        if (rendered) {
            const std::uint64_t ver = c.doc->buf.version();
            if (!c.mdParsed || c.mdVer != ver || c.mdWidth != boxW) {
                if (!c.mdParsed || c.mdVer != ver) c.blocks = md::parse(c.doc->buf.text());
                c.mdVer = ver;
                c.mdWidth = boxW;
                c.mdParsed = true;
                c.lay = mdLayout(c.blocks, font_, boxW + 2 * std::round(28 * s) + std::round(10 * s), s,
                                 [this, &v](const std::string& src) { return pictureSize(v.md, src); }, false);
            }
            const bool empty = c.blocks.empty();
            c.boxH = empty ? std::round(uiH() + 14 * s) : std::max(std::round(26 * s), c.lay.height - std::round(22 * s) - std::round(36 * s) + std::round(8 * s));
            c.outY = y + c.boxH;
            c.h = c.boxH;
        } else {
            c.boxH = static_cast<float>(c.doc->buf.lineCount()) * line + 2 * bpad;
            float oh = 0;
            if (c.data.type == nb::Cell::Type::Code)
                for (const auto& o : c.data.outputs) oh += static_cast<float>(std::min(maxLinesFor(o) + 1, lineCount(o.text))) * line + std::round(4 * s);
            if (oh > 0) oh += std::round(4 * s);
            c.outY = y + c.boxH + std::round(4 * s);
            c.h = c.boxH + oh;
        }
        y += c.h + gap;
    }
    v.contentH = y + std::round(40 * s);
    if (v.follow && !v.cells.empty()) {   // keep the selection (or the caret of the cell being edited) in view
        const NbCell& c = v.cells[static_cast<std::size_t>(std::clamp(v.sel, 0, static_cast<int>(v.cells.size()) - 1))];
        float lo = c.y - std::round(10 * s), hi = c.y + std::min(c.h, v.viewH * 0.8f);
        if (v.editing) {
            const float cy = c.boxY + bpad + static_cast<float>(c.doc->cursor.line) * line;
            lo = std::min(lo, cy - line);
            hi = std::max(std::min(hi, c.y + v.viewH), cy + 2 * line);
        }
        if (lo < d.targetY) d.targetY = std::max(0.0f, lo);
        else if (hi > d.targetY + v.viewH) d.targetY = hi - v.viewH;
        d.targetY = std::clamp(d.targetY, 0.0f, std::max(0.0f, v.contentH - v.viewH));
        v.follow = false;
    }
}

void App::drawCellText(Document& cd, float x, float y, float w, bool active, const std::vector<LspDiag>* diags) {
    const float line = lh(), cell = cw(), s = S();
    const int total = cd.buf.lineCount();
    const bool hasSel = active && cd.hasSelection();
    const Pos a = cd.selBegin(), b = cd.selEnd();
    const float right = x + w;
    for (int ln = 0; ln < total; ++ln) {
        const float ly = y + static_cast<float>(ln) * line;
        if (ly > L_.editor.bottom() || ly + line < L_.editor.y) continue;
        const std::string& str = cd.buf.line(ln);
        if (hasSel && ln >= a.line && ln <= b.line) {
            const int c0 = ln == a.line ? a.col : 0, c1 = ln == b.line ? b.col : static_cast<int>(str.size());
            float x0 = x + static_cast<float>(displayCol(str, c0, cd.tabWidth)) * cell, x1 = x + static_cast<float>(displayCol(str, c1, cd.tabWidth)) * cell;
            if (ln < b.line) x1 += cell * 0.6f;
            fill(x0, ly, std::min(x1, right) - x0, line, theme::selection);
        }
        cd.hl.highlight(cd.buf, ln, toks_);
        const float baseline = ly + font_.baseline();
        float cx = x;
        int dcol = 0;
        for (std::size_t i = 0; i < str.size() && cx < right;) {
            const std::size_t start = i;
            char32_t cp = decodeUtf8(str, i);
            if (cp == '\t') { const int adv = cd.tabWidth - dcol % cd.tabWidth; dcol += adv; cx += static_cast<float>(adv) * cell; continue; }
            if (cp != ' ') {
                if (cp < 0x20 || cp == 0x7f) cp = 0xFFFD;
                dl_.glyph(font_.glyph(cp), cx, baseline, theme::syntax(start < toks_.size() ? toks_[start] : Tok::Normal));
            }
            cx += cell;
            ++dcol;
        }
    }
    if (diags) {   // squiggles under what clangd complains about
        for (const LspDiag& dg : *diags) {
            const Color c = diagColor(dg.severity);
            for (int ln = std::max(dg.line, 0); ln <= std::min(dg.endLine, total - 1); ++ln) {
                const std::string& str = cd.buf.line(ln);
                int sc = ln == dg.line ? byteColFromUtf16(str, dg.col) : static_cast<int>(std::min(str.find_first_not_of(" \t"), str.size()));
                int ec = ln == dg.endLine ? byteColFromUtf16(str, dg.endCol) : static_cast<int>(str.size());
                if (ec <= sc) ec = static_cast<int>(utf8Next(str, static_cast<std::size_t>(sc)));
                const float x0 = x + static_cast<float>(displayCol(str, sc, cd.tabWidth)) * cell;
                const float x1 = std::max(x0 + cell, x + static_cast<float>(displayCol(str, ec, cd.tabWidth)) * cell);
                const float sy = y + static_cast<float>(ln + 1) * line - std::round(3.5f * s);
                const float step = std::max(2.0f, std::round(2.0f * s)), th = std::max(1.0f, std::round(1.2f * s));
                int k = 0;
                for (float sx = x0; sx < std::min(x1, right); sx += step, ++k) fill(sx, sy + ((k & 1) ? std::round(1.5f * s) : 0.0f), std::min(step, x1 - sx), th, c);
            }
        }
    }
    if (active && caretOn_ && (focused_ || !opt_.screenshot.empty())) {
        const std::string& cl = cd.buf.line(std::clamp(cd.cursor.line, 0, total - 1));
        const float cx = x + static_cast<float>(displayCol(cl, cd.cursor.col, cd.tabWidth)) * cell;
        fill(cx, y + static_cast<float>(cd.cursor.line) * line + 1, std::max(2.0f, std::round(1.6f * s)), line - 2, theme::caret);
    }
}

void App::drawNotebook(Document& d) {
    auto nit = notebooks_.find(&d);
    if (nit == notebooks_.end()) return;
    layoutNotebook(d);
    NbView& v = *nit->second;
    const Rect& e = L_.editor;
    const float s = S(), line = lh(), sy = std::round(d.scrollY);
    const float pad = std::round(14 * s), bpad = std::round(8 * s);
    const float top = v.viewTop;
    const bool activeNb = drawActive_;
    for (std::size_t i = 0; i < v.cells.size(); ++i) {
        NbCell& c = v.cells[i];
        const float y0 = top + c.y - sy;
        if (y0 > e.bottom() || y0 + c.h < top - 4) continue;
        const bool selected = static_cast<int>(i) == v.sel;
        const bool editingThis = selected && v.editing;
        const bool code = c.data.type != nb::Cell::Type::Markdown;
        const float by = top + c.boxY - sy;
        // the bar at the left edge marks the selected cell (red in command mode, green while typing)
        if (selected && activeNb) fill(e.x + std::round(4 * s), by, std::round(3 * s), c.boxH, editingThis ? kEditGreen : theme::accent, std::round(1.5f * s));
        if (code || editingThis) {
            fill(c.boxX, by, c.boxW, c.boxH, kCodeBox, std::round(6 * s));
            const Color ring = editingThis ? kEditGreen.alpha(170) : selected ? theme::accent.alpha(160) : Color::rgb(0xffffff, 26);
            dl_.ring(font_, c.boxX, by, c.boxW, c.boxH, ring, std::round(6 * s), std::max(1.0f, std::round(s * (selected ? 1.2f : 0.8f))));
            const auto dit = v.lsp.diags.find(c.doc.get());
            drawCellText(*c.doc, c.boxX + bpad, by + bpad, c.boxW - 2 * bpad, editingThis, dit != v.lsp.diags.end() ? &dit->second : nullptr);
            if (c.doc->buf.lineCount() == 1 && c.doc->buf.line(0).empty() && !editingThis)
                label(c.boxX + bpad, by + bpad, code ? "// code" : "markdown", theme::lineNum);
            if (c.data.type == nb::Cell::Type::Code) {   // the execution counter in the margin
                const std::string p = c.running ? "In [*]:" : c.queued ? "In [ ]:" : c.data.execCount > 0 ? std::format("In [{}]:", c.data.execCount) : "In [ ]:";
                uiLabelR(c.boxX - std::round(10 * s), by, std::round(2 * bpad + line), p, c.running ? theme::accent : theme::dim);
            }
        } else {
            if (c.blocks.empty())
                uiLabel(c.boxX, by, c.boxH, "Empty markdown cell - press Enter to edit", theme::lineNum);
            else {
                MdView& ctx = v.md;
                drawMdItems(c.lay, {}, c.boxX - std::round(28 * s), by - std::round(22 * s), std::max(top, by), e.bottom(), ctx);
            }
            if (selected) dl_.ring(font_, c.boxX - std::round(6 * s), by - std::round(2 * s), c.boxW + std::round(12 * s), c.boxH + std::round(4 * s), theme::accent.alpha(90), std::round(6 * s), std::max(1.0f, std::round(s * 0.8f)));
        }
        // outputs
        if (c.data.type == nb::Cell::Type::Code) {
            float oy = top + c.outY - sy;
            for (const auto& o : c.data.outputs) {
                const int n = lineCount(o.text);
                const int shown = std::min(n, maxLinesFor(o));
                const Color col = o.kind == nb::Output::Kind::Error ? kErr : o.kind == nb::Output::Kind::Result ? theme::text : Color::rgb(0xc2c6cf);
                if (o.kind == nb::Output::Kind::Result)
                    uiLabelR(c.boxX - std::round(10 * s), oy, line, c.data.execCount > 0 ? std::format("Out [{}]:", c.data.execCount) : "Out:", theme::accent.mix(theme::text, 0.3f));
                if (o.kind == nb::Output::Kind::Error)
                    fill(c.boxX - std::round(5 * s), oy, std::round(2 * s), static_cast<float>(shown) * line, kErr.alpha(160), s);
                for (int ln = 0; ln < shown; ++ln) {
                    const float ly = oy + static_cast<float>(ln) * line;
                    if (ly > e.bottom()) break;
                    if (ly + line < top) continue;
                    std::string row(lineOf(o.text, ln));
                    if (ln + 1 == shown && n > shown) row = std::format("... {} more lines", n - shown);
                    label(c.boxX + bpad, ly, row, col, c.boxX + c.boxW);
                }
                oy += static_cast<float>(std::min(maxLinesFor(o) + 1, n)) * line + std::round(4 * s);
            }
        }
    }
    // scrollbar
    const float maxS = maxScroll(d);
    if (maxS > 0) {
        const float contentH = maxS + v.viewH;
        const float thumbH = std::max(24 * s, v.viewH * v.viewH / contentH);
        const float thumbY = top + (v.viewH - thumbH) * std::clamp(sy / maxS, 0.0f, 1.0f);
        const float w = (scrollbarDrag_ ? 10.0f : 7.0f) * s;
        fill(e.right() - w - 4 * s, thumbY, w, thumbH, theme::scrollbar, w / 2);
    }
    // toolbar: opaque, drawn over whatever scrolled beneath it
    const float tbH = top - e.y;
    fill(e.x, e.y, e.w, tbH, theme::stripBg);
    fill(e.x, e.y + tbH - std::max(1.0f, std::round(s * 0.7f)), e.w, std::max(1.0f, std::round(s * 0.7f)), Color::rgb(0xffffff, 18));
    v.btns.clear();
    static const std::array<std::pair<Btn, const char*>, 8> items = {{{kRun, "Run"}, {kRunAll, "Run all"}, {kStop, "Stop"}, {kRestart, "Restart"},
                                                                       {kAddCode, "+ Code"}, {kAddText, "+ Text"}, {kDelete, "Delete"}, {kSave, "Save"}}};
    float bx = e.x + pad - std::round(6 * s);
    const bool busy = v.session && v.session->pending();
    for (const auto& [id, text] : items) {
        const float w = uiW(text) + std::round(22 * s);
        Rect r{bx, e.y + std::round(4 * s), w, tbH - std::round(8 * s)};
        v.btns.push_back({r, id});
        const bool hot = r.contains(mx_, my_) && drawActive_;
        const bool dim = (id == kStop && !busy) || ((id == kRun || id == kRunAll) && !v.runnable);
        if (hot && !dim) fill(r, theme::tabHover, std::round(6 * s));
        uiLabel(r.x + std::round(11 * s), e.y, tbH, text, dim ? theme::lineNum : (hot ? theme::text : theme::dim));
        bx += w + std::round(2 * s);
    }
    std::string status = !v.runnable ? "no C++ kernel for this notebook" : v.session ? v.session->statusLine() : "C++ kernel (GCC modules) - starts with the first run";
    if (!v.note.empty() && busy) status = v.note;
    status = std::string(v.editing ? "EDIT mode" : "COMMAND mode (Enter edits)") + "   |   " + status;
    uiLabelR(e.right() - pad, e.y, tbH, status, busy ? theme::text : theme::dim);
}

// ---------------------------------------------------------------------------- input

bool App::notebookKey(const KeyEvent& k) {
    Document& d = doc();
    auto nit = notebooks_.find(&d);
    if (nit == notebooks_.end()) return false;
    NbView& v = *nit->second;
    if (v.cells.empty()) nbInsert(v, 0, nb::Cell::Type::Code);
    NbCell& cur = v.cells[static_cast<std::size_t>(v.sel)];
    const bool plain = !k.ctrl && !k.alt;

    if (k.ctrl && !k.alt && !k.shift && k.key == Key::Char && k.ch == 's') { saveNotebook(d); return true; }
    if (k.ctrl && k.shift && k.key == Key::Enter) { nbRunAll(v); return true; }
    if (k.key == Key::Enter && (k.shift || k.ctrl || k.alt)) {
        nbRun(v, v.sel);
        if (k.shift) {                     // run and move on (a fresh cell after the last one)
            if (v.sel + 1 >= static_cast<int>(v.cells.size())) nbInsert(v, v.sel + 1, nb::Cell::Type::Code);
            else nbSelect(v, v.sel + 1, false);
        } else if (k.alt) nbInsert(v, v.sel + 1, nb::Cell::Type::Code), nbSelect(v, v.sel, true);
        return true;
    }

    if (v.editing) {
        if (k.key == Key::Escape && !comp_.open) { v.editing = false; v.pendingD = false; return true; }
        if (plain && !k.shift && !comp_.open) {   // arrow keys cross into the neighbouring cell at its first / last line
            if (k.key == Key::Up && cur.doc->cursor.line == 0 && v.sel > 0) {
                nbSelect(v, v.sel - 1, true);
                NbCell& p = v.cells[static_cast<std::size_t>(v.sel)];
                p.doc->setCursor({p.doc->buf.lineCount() - 1, std::min(cur.doc->cursor.col, static_cast<int>(p.doc->buf.line(p.doc->buf.lineCount() - 1).size()))});
                return true;
            }
            if (k.key == Key::Down && cur.doc->cursor.line == cur.doc->buf.lineCount() - 1 && v.sel + 1 < static_cast<int>(v.cells.size())) {
                nbSelect(v, v.sel + 1, true);
                v.cells[static_cast<std::size_t>(v.sel)].doc->setCursor({0, 0});
                return true;
            }
        }
        // everything else is ordinary editing of the cell's text
        docOverride_ = cur.doc.get();
        const std::uint64_t before = cur.doc->buf.version();
        editorKey(k);
        docOverride_ = nullptr;
        if (cur.doc->buf.version() != before) { cur.mdParsed = cur.mdParsed && false; nbMarkModified(v); }
        v.follow = true;
        return true;
    }

    // command mode
    if (!plain) return false;   // Ctrl / Alt shortcuts (close tab, switch tab, ...) work as everywhere else
    if (k.key == Key::Char && k.ch == 'd') {
        if (v.pendingD) { v.pendingD = false; nbDelete(v); }
        else v.pendingD = true;
        return true;
    }
    v.pendingD = false;
    switch (k.key) {
        case Key::Up: nbSelect(v, v.sel - 1, false); return true;
        case Key::Down: nbSelect(v, v.sel + 1, false); return true;
        case Key::Home: nbSelect(v, 0, false); return true;
        case Key::End: nbSelect(v, static_cast<int>(v.cells.size()) - 1, false); return true;
        case Key::Enter: nbSelect(v, v.sel, true); return true;
        case Key::Delete: nbDelete(v); return true;
        case Key::Left: case Key::Right: setMessage("Command mode: press Enter or click inside the cell to edit its text", false, 3.5f); return true;
        case Key::Char:
            switch (k.ch) {
                case U'k': nbSelect(v, v.sel - 1, false); return true;
                case U'j': nbSelect(v, v.sel + 1, false); return true;
                case U'a': nbInsert(v, v.sel, nb::Cell::Type::Code); return true;
                case U'b': nbInsert(v, v.sel + 1, nb::Cell::Type::Code); return true;
                case U'm': nbSetType(v, nb::Cell::Type::Markdown); return true;
                case U'y': nbSetType(v, nb::Cell::Type::Code); return true;
                case U'r': nbSetType(v, nb::Cell::Type::Raw); return true;
                case U'i': if (v.session) { v.session->interrupt(); setMessage("Kernel stopped - its variables are gone", false, 3); } return true;
                default: setMessage("Command mode: press Enter or click inside the cell to type (Esc comes back here)", false, 3.5f); return true;
            }
        default: return false;
    }
}

void App::notebookClick(Button b) {
    Document& d = doc();
    auto nit = notebooks_.find(&d);
    if (nit == notebooks_.end() || b != Button::Left) return;
    NbView& v = *nit->second;
    for (const auto& btn : v.btns) {
        if (!btn.r.contains(mx_, my_)) continue;
        switch (btn.id) {
            case kRun: if (!v.cells.empty()) { nbRun(v, v.sel); if (v.sel + 1 < static_cast<int>(v.cells.size())) nbSelect(v, v.sel + 1, false); } break;
            case kRunAll: nbRunAll(v); break;
            case kStop: if (v.session && v.session->pending()) { v.session->interrupt(); setMessage("Stopped - the kernel was restarted, its variables are gone", false, 4); } break;
            case kRestart: if (v.session) { v.session->restart(); for (auto& c : v.cells) c.running = c.queued = false; setMessage("Kernel restarted - its variables are gone", false, 3); } break;
            case kAddCode: nbInsert(v, v.sel + 1, nb::Cell::Type::Code); nbSelect(v, v.sel, true); break;
            case kAddText: nbInsert(v, v.sel + 1, nb::Cell::Type::Markdown); nbSelect(v, v.sel, true); break;
            case kDelete: nbDelete(v); break;
            case kSave: saveNotebook(d); break;
            default: break;
        }
        dirty_ = true;
        return;
    }
    if (my_ < v.viewTop) return;
    const float cy = my_ - v.viewTop + std::round(d.scrollY);
    for (std::size_t i = 0; i < v.cells.size(); ++i) {
        NbCell& c = v.cells[i];
        if (cy < c.y || cy >= c.y + c.h + std::round(6 * S())) continue;
        const bool inBox = mx_ >= c.boxX && mx_ < c.boxX + c.boxW && cy >= c.boxY && cy < c.boxY + c.boxH;
        const bool rendered = c.data.type == nb::Cell::Type::Markdown && !(v.editing && static_cast<int>(i) == v.sel);
        const auto now = Clock::now();
        const bool again = static_cast<int>(i) == v.sel && now - lastClick_ < std::chrono::milliseconds(450) && std::abs(mx_ - lastClickX_) < 6 * S() && std::abs(my_ - lastClickY_) < 6 * S();
        if (inBox && rendered) {   // a rendered markdown cell: double click edits it
            nbSelect(v, static_cast<int>(i), again);
            lastClick_ = now; lastClickX_ = mx_; lastClickY_ = my_;
            return;
        }
        if (inBox) {
            nbSelect(v, static_cast<int>(i), true);
            const Pos p = nbCellPosAt(v, c, mx_, cy);
            clickCount_ = again ? clickCount_ % 3 + 1 : 1;
            lastClick_ = now; lastClickX_ = mx_; lastClickY_ = my_;
            switch (clickCount_) {
                case 1: c.doc->setCursor(p, win_.shiftDown()); nbDragging_ = true; break;
                case 2: { auto [a, e] = c.doc->wordRange(p); c.doc->select(a, e); break; }
                default: c.doc->selectLine(p.line); break;
            }
            v.follow = false;
        } else {
            nbSelect(v, static_cast<int>(i), false);
        }
        return;
    }
}

void App::notebookDragStep() {
    if (docs_.empty()) return;
    auto nit = notebooks_.find(&doc());
    if (nit == notebooks_.end()) { nbDragging_ = false; return; }
    NbView& v = *nit->second;
    if (v.sel < 0 || v.sel >= static_cast<int>(v.cells.size())) return;
    NbCell& c = v.cells[static_cast<std::size_t>(v.sel)];
    const float cy = my_ - v.viewTop + std::round(doc().scrollY);
    c.doc->setCursor(nbCellPosAt(v, c, mx_, cy), true);
    dirty_ = true;
}

// ---------------------------------------------------------------------------- IntelliSense

auto App::nbOfCell(const Document& d) const -> NbView* {
    for (const auto& [doc, v] : notebooks_)
        for (const auto& c : v->cells) if (c.doc.get() == &d) return v.get();
    return nullptr;
}

int App::nbCellIndex(const NbView& v, const Document& d) const {
    for (std::size_t i = 0; i < v.cells.size(); ++i) if (v.cells[i].doc.get() == &d) return static_cast<int>(i);
    return -1;
}

void App::nbStopLsp(NbView& v) {
    if (v.lsp.fd >= 0) win_.unwatch(v.lsp.fd);
    v.lsp.fd = -1;
    v.lsp.client.reset();
    v.lsp.opened = false;
    v.lsp.diags.clear();
    if (!v.lsp.dir.empty()) { std::error_code ec; fs::remove_all(v.lsp.dir, ec); }
}

void App::eraseNotebook(const Document* d) {
    auto it = notebooks_.find(d);
    if (it == notebooks_.end()) return;
    nbStopLsp(*it->second);
    notebooks_.erase(it);
}

// clangd for the notebook, started on first use: it sees the virtual C++ file described in services/notebook.
bool App::nbEnsureLsp(NbView& v) {
    if (v.lsp.client) return true;
    if (v.lsp.failed || !v.runnable) return false;
    const char* env = std::getenv("IDE_CLANGD");
    auto exe = findExecutable(env && *env ? env : "clangd");
    if (!exe) {
        v.lsp.failed = true;
        if (!lspNoticeShown_) { lspNoticeShown_ = true; setMessage("clangd not found - install it (or set IDE_CLANGD) for completion in notebooks", true, 7); }
        return false;
    }
    fs::path base;
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) base = x;
    else if (const char* h = std::getenv("HOME"); h && *h) base = fs::path(h) / ".cache";
    else base = fs::temp_directory_path();
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : v.owner->path) { hash ^= c; hash *= 1099511628211ull; }
    hash ^= static_cast<std::uint64_t>(::getpid()) * 0x9E3779B97F4A7C15ull;
    v.lsp.dir = (base / "daemon" / "notebook" / std::format("lsp{:016x}", hash)).string();
    v.lsp.path = (fs::path(v.lsp.dir) / "notebook.cpp").string();
    std::error_code ec;
    fs::create_directories(v.lsp.dir, ec);
    writeFile(v.lsp.path, "");
    // compile command: clang with libstdc++ (the GCC the kernel uses), plus -I / -D / -std from the cells' %flags magics
    auto args = json::array();
    for (const char* a : {"clang++", "-std=c++23", "-stdlib=libstdc++", "-x", "c++"}) args.push(a);
    args.push("-I" + fs::path(v.owner->path).parent_path().string());
    for (const auto& c : v.cells)
        for (const auto& f : nb::planCell(c.doc->buf.text()).flags)
            if (f.starts_with("-I") || f.starts_with("-D") || f.starts_with("-U") || f.starts_with("-std=")) args.push(f);
    args.push(v.lsp.path);
    auto entry = json::object();
    entry.set("directory", v.lsp.dir).set("file", v.lsp.path).set("arguments", std::move(args));
    writeFile(fs::path(v.lsp.dir) / "compile_commands.json", json::dump(json::array().push(std::move(entry))));

    std::vector<std::string> argv = {"nice", "-n", "10", *exe, "--log=error", "--header-insertion=never", "--completion-parse=always", "-j=2",
                                     "--background-index=false", "--compile-commands-dir=" + v.lsp.dir};
    auto client = std::make_unique<LspClient>();
    NbView* vp = &v;
    client->onDiagnostics = [this, vp](const std::string& path, std::vector<LspDiag> diags) {
        if (path != vp->lsp.path) return;
        vp->lsp.diags.clear();
        for (auto& d : diags) {
            if (d.severity > 2 || d.code == "unused-includes") continue;
            std::string m = d.message;
            for (auto& ch : m) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (m.find("unused") != std::string::npos || m.find("result of") != std::string::npos) continue;   // noise: values shown by the notebook, globals nobody reads yet
            const auto a = nb::fromVirtual(vp->lsp.vdoc, d.line, d.col);
            if (!a || a->cell < 0 || a->cell >= static_cast<int>(vp->cells.size())) continue;
            LspDiag out = d;
            out.line = a->line;
            out.col = a->col;
            const auto b = nb::fromVirtual(vp->lsp.vdoc, d.endLine, d.endCol);
            if (b && b->cell == a->cell) { out.endLine = b->line; out.endCol = b->col; }
            else { out.endLine = a->line; out.endCol = a->col + 1; }
            vp->lsp.diags[vp->cells[static_cast<std::size_t>(a->cell)].doc.get()].push_back(std::move(out));
        }
        dirty_ = true;
    };
    client->onState = [vp](LspClient::State st) { if (st == LspClient::State::Failed) vp->lsp.failed = true; };
    if (auto r = client->start(argv, v.lsp.dir); !r) { v.lsp.failed = true; return false; }
    v.lsp.fd = client->fd();
    v.lsp.client = std::move(client);
    win_.watch(v.lsp.fd, [this, vp] {
        if (!vp->lsp.client) return;
        vp->lsp.client->pump();
        if (vp->lsp.client->fd() < 0) { win_.unwatch(vp->lsp.fd); vp->lsp.fd = -1; }
        dirty_ = true;
    });
    v.lsp.opened = false;
    nbLspSync(v, true);
    return true;
}

// Sends the current text of all code cells (as one virtual file) when it changed; edits are batched for 150 ms.
void App::nbLspSync(NbView& v, bool force) {
    if (!v.lsp.client) return;
    std::uint64_t sig = 1469598103934665603ull;
    auto mix = [&](std::uint64_t x) { sig ^= x + 0x9E3779B97F4A7C15ull + (sig << 6) + (sig >> 2); };
    for (std::size_t i = 0; i < v.cells.size(); ++i) {
        if (v.cells[i].data.type != nb::Cell::Type::Code) continue;
        mix(i); mix(v.cells[i].doc->buf.version()); mix(v.cells[i].doc->buf.lineCount());
    }
    if (v.lsp.opened && sig == v.lsp.sig) { v.lsp.flushAt.reset(); return; }
    const auto now = Clock::now();
    if (!force && v.lsp.opened) {
        if (!v.lsp.flushAt) v.lsp.flushAt = now + std::chrono::milliseconds(150);
        if (now < *v.lsp.flushAt) return;
    }
    v.lsp.flushAt.reset();
    std::vector<std::pair<int, std::string>> cells;
    for (std::size_t i = 0; i < v.cells.size(); ++i)
        if (v.cells[i].data.type == nb::Cell::Type::Code) cells.emplace_back(static_cast<int>(i), v.cells[i].doc->buf.text());
    v.lsp.vdoc = nb::buildVirtualDoc(cells);
    if (!v.lsp.opened) { v.lsp.client->didOpen(v.lsp.path, "cpp", ++v.lsp.version, v.lsp.vdoc.text); v.lsp.opened = true; }
    else v.lsp.client->didChange(v.lsp.path, ++v.lsp.version, v.lsp.vdoc.text);
    v.lsp.sig = sig;
}

std::optional<App::LspTarget> App::lspTarget(Document& d, Pos p) {
    if (NbView* v = nbOfCell(d)) {
        const int idx = nbCellIndex(*v, d);
        if (idx < 0 || v->cells[static_cast<std::size_t>(idx)].data.type != nb::Cell::Type::Code) return std::nullopt;
        if (!v->lsp.client || !v->lsp.client->ready()) return std::nullopt;
        nbLspSync(*v, true);   // the server must see what is on screen
        const auto m = nb::toVirtual(v->lsp.vdoc, idx, p.line, p.col);
        if (!m) return std::nullopt;
        return LspTarget{v->lsp.client.get(), v->lsp.path, m->first, utf16Col(d.buf.line(p.line), p.col)};
    }
    if (lsp_ && lsp_->ready() && isCppDoc(d) && d.lspOpen) {
        flushDoc(d);
        auto [l, c] = lspPos(d, p);
        return LspTarget{lsp_.get(), d.path, l, c};
    }
    return std::nullopt;
}

bool App::lspAvailable(Document& d) {
    if (NbView* v = nbOfCell(d)) {
        const int idx = nbCellIndex(*v, d);
        if (idx < 0 || v->cells[static_cast<std::size_t>(idx)].data.type != nb::Cell::Type::Code) return false;
        return nbEnsureLsp(*v) && v->lsp.client && v->lsp.client->ready();
    }
    return lsp_ && lsp_->ready() && isCppDoc(d);
}

void App::nbHoverMoved() {
    if (docs_.empty()) return;
    auto nit = notebooks_.find(&doc());
    if (nit == notebooks_.end()) return;
    NbView& v = *nit->second;
    const float cy = my_ - v.viewTop + std::round(doc().scrollY);
    for (auto& c : v.cells) {
        if (c.data.type != nb::Cell::Type::Code || mx_ < c.boxX || mx_ >= c.boxX + c.boxW || cy < c.boxY || cy >= c.boxY + c.boxH) continue;
        const Pos p = nbCellPosAt(v, c, mx_, cy);
        if ((hover_.pending || hover_.show) && hoverCell_ == c.doc.get() && p == hover_.pos) return;
        nbEnsureLsp(v);
        hoverCell_ = c.doc.get();
        hover_.show = false;
        hover_.pos = p;
        hover_.px = mx_;
        hover_.py = my_;
        hover_.pending = true;
        hover_.due = Clock::now() + std::chrono::milliseconds(450);
        ++hoverSeq_;
        dirty_ = true;
        return;
    }
    hoverCell_ = nullptr;
    hideHover();
}

void App::nbHoverTick(Clock::time_point now) {
    if (!hover_.pending || now < hover_.due || !hoverCell_) return;
    hover_.pending = false;
    settleUntil_ = now + std::chrono::milliseconds(250);
    if (menu_.open || prompt_.kind != PromptKind::None || comp_.open) return;
    Document& d = *hoverCell_;
    NbView* v = nbOfCell(d);
    if (!v) return;
    hover_.lines.clear();
    if (auto it = v->lsp.diags.find(&d); it != v->lsp.diags.end()) {
        for (const auto& dg : it->second) {
            if (dg.line >= d.buf.lineCount() || dg.endLine >= d.buf.lineCount()) continue;
            Pos a{dg.line, byteColFromUtf16(d.buf.line(dg.line), dg.col)};
            Pos b{dg.endLine, byteColFromUtf16(d.buf.line(dg.endLine), dg.endCol)};
            if (b <= a) b.col = a.col + 1;
            if (hover_.pos >= a && hover_.pos <= b) addHoverText(dg.message, dg.severity);
        }
    }
    const std::uint64_t seq = hoverSeq_;
    const Pos at = hover_.pos;
    if (auto t = lspTarget(d, at)) {
        t->c->hover(t->path, t->line, t->col, [this, seq, at](std::string text) {
            if (seq != hoverSeq_ || hover_.pos != at || text.empty()) return;
            if (!hover_.lines.empty()) hover_.lines.emplace_back("", 0);
            addHoverText(cleanMarkdown(text), 0);
            hover_.show = !hover_.lines.empty();
            dirty_ = true;
        });
    }
    hover_.show = !hover_.lines.empty();
    dirty_ = true;
}

}  // namespace ide

// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app.mdview - implementation (see mdview.cppm).
module daemon.app.mdview;
import std;
import daemon.base.util;
import daemon.app.theme;
import daemon.text.syntax;

namespace ide {

namespace {

constexpr Color kHeading = Color::rgb(0xe9ebef);
constexpr Color kStrong = Color::rgb(0xf2f3f6);
constexpr Color kItalic = Color::rgb(0xc2a9e8);
constexpr Color kLink = Color::rgb(0x6aa7f5);
constexpr Color kCode = Color::rgb(0xe7aeb1);
constexpr Color kCodeBg = Color::rgb(0xffffff, 20);
constexpr Color kBlockBg = Color::rgb(0x111317);
constexpr Color kQuote = Color::rgb(0xa2a6b0);

struct Atom {
    std::string text;
    std::uint8_t style = 0;
    std::string url;
    float w = 0;
    bool space = false, image = false, brk = false;
    bool padL = false, padR = false;   // first / last word of an inline code span
};

const Lang* langByName(std::string_view name) {
    std::string n(name);
    for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.empty()) return nullptr;
    if (n == "c++" || n == "cxx" || n == "hpp") n = "cpp";
    if (n == "sh" || n == "shell" || n == "zsh" || n == "console") n = "bash";
    if (n == "js") n = "javascript";
    if (n == "ts") n = "typescript";
    if (n == "py") n = "python";
    if (n == "yml") n = "yaml";
    for (const auto& l : languages()) {
        std::string ln = l.name;
        for (auto& c : ln) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ln == n || std::ranges::contains(l.exts, n)) return &l;
    }
    return nullptr;
}

class Builder {
public:
    Builder(FontAtlas& f, float width, float s) : f_(f), s_(s), width_(width) {
        bh_ = std::round(f_.uiLineHeight() * 1.5f);
        boldOff_ = std::max(1.0f, std::round(0.5f * s_));
        items_ = &out_.items;
        links_ = &out_.links;
    }

    MdLayout run(const std::vector<md::Block>& blocks) {
        const float pad = std::round(28 * s_);
        const float usable = std::max(120.0f * s_, width_ - 2 * pad - std::round(10 * s_));
        const float colW = std::min(usable, std::round(880 * s_));
        const float x0 = pad + std::max(0.0f, std::floor((usable - colW) / 2));
        float y = std::round(22 * s_);
        y = sequence(blocks, x0, colW, y, 0, false, theme::text);
        out_.height = y + std::round(36 * s_);
        return std::move(out_);
    }

private:
    FontAtlas& f_;
    float s_, width_, bh_ = 20, boldOff_ = 1;
    MdLayout out_;
    std::vector<MdItem>* items_;
    std::vector<MdLink>* links_;
    std::unordered_map<std::string, int> slugs_;
    int lastPill_ = -1;   // index of the inline-code background that the next code word may extend
    float lastPillEnd_ = -1, lastPillY_ = -1;

    float gap(bool tight) const { return std::round(bh_ * (tight ? 0.16f : 0.6f)); }

    // ---------------------------------------------------------------- emit helpers

    void rect(float x, float y, float w, float h, Color c, float r = 0) {
        MdItem it; it.t = MdItem::T::Rect; it.x = x; it.y = y; it.w = w; it.h = h; it.c = c; it.radius = r;
        items_->push_back(std::move(it));
    }
    void ring(float x, float y, float w, float h, Color c, float r) {
        MdItem it; it.t = MdItem::T::Ring; it.x = x; it.y = y; it.w = w; it.h = h; it.c = c; it.radius = r;
        items_->push_back(std::move(it));
    }
    void text(MdItem::T t, float x, float y, float h, std::string s, Color c, float clip = 0, bool bold = false, int link = -1) {
        MdItem it; it.t = t; it.x = x; it.y = y; it.h = h; it.w = clip; it.s = std::move(s); it.c = c; it.bold = bold; it.link = link;
        items_->push_back(std::move(it));
    }

    // ---------------------------------------------------------------- inline flow

    float wordWidth(std::string_view s, int mode, bool bold) {
        const float w = mode == 1 ? f_.titleWidth(s) : f_.uiWidth(s);
        return w + (bold ? boldOff_ : 0);
    }

    std::vector<Atom> atoms(const std::vector<md::Span>& spans, int mode, bool forceBold) {
        std::vector<Atom> out;
        const float spaceW = mode == 1 ? f_.titleWidth(" ") : f_.uiWidth(" ");
        for (const auto& sp : spans) {
            if (sp.brk) { Atom a; a.brk = true; out.push_back(std::move(a)); continue; }
            const bool bold = forceBold || (sp.style & md::Bold) != 0;
            if (!sp.src.empty()) {
                Atom a;
                a.image = true;
                a.text = sp.text.empty() ? "image" : sp.text;
                a.url = sp.url;
                a.style = sp.style;
                a.w = f_.uiWidth(a.text) + std::round(16 * s_);
                out.push_back(std::move(a));
                continue;
            }
            const bool code = (sp.style & md::Code) != 0;
            std::size_t i = 0;
            const std::size_t first = out.size();
            while (i < sp.text.size()) {
                const bool sp0 = sp.text[i] == ' ' || sp.text[i] == '\t';
                std::size_t j = i;
                while (j < sp.text.size() && ((sp.text[j] == ' ' || sp.text[j] == '\t') == sp0)) ++j;
                Atom a;
                a.style = sp.style;
                a.url = sp.url;
                if (sp0) { a.space = true; a.text = " "; a.w = spaceW; }
                else { a.text = sp.text.substr(i, j - i); a.w = wordWidth(a.text, mode, bold); }
                out.push_back(std::move(a));
                i = j;
            }
            if (code && out.size() > first) {
                std::size_t lo = first, hi = out.size() - 1;
                while (lo <= hi && out[lo].space) ++lo;
                while (hi > lo && out[hi].space) --hi;
                if (lo <= hi && !out[lo].space) {
                    out[lo].padL = true; out[lo].w += std::round(4 * s_);
                    out[hi].padR = true; out[hi].w += std::round(4 * s_);
                }
            }
        }
        return out;
    }

    float naturalWidth(const std::vector<md::Span>& spans, bool bold) {
        float w = 0;
        for (const auto& a : atoms(spans, 0, bold)) if (!a.brk) w += a.w;
        return w;
    }

    // mode: 0 body text, 1 large heading (title font), 2 small heading (bold UI font). Returns the y below the last line.
    float flow(const std::vector<md::Span>& spans, float left, float right, float y, Color base, int mode, float firstIndent = 0,
               bool forceBold = false) {
        const auto list = atoms(spans, mode, forceBold || mode == 2);
        if (list.empty()) return y;
        const float lineH = mode == 1 ? std::round(f_.titleLineHeight() * 1.12f) : mode == 2 ? std::round(bh_ * 1.12f) : bh_;
        const float fontH = mode == 1 ? f_.titleLineHeight() : f_.uiLineHeight();
        const float baseline = mode == 1 ? f_.titleBaseline() : f_.uiBaseline();
        float x = left + firstIndent;
        float pendingSpace = 0;
        bool lineHasText = false;
        lastPill_ = -1;
        const Atom* prev = nullptr;
        auto newline = [&] { y += lineH; x = left; pendingSpace = 0; lineHasText = false; lastPill_ = -1; };
        for (const Atom& a : list) {
            if (a.brk) { newline(); prev = &a; continue; }
            if (a.space) { if (lineHasText) pendingSpace = a.w; prev = &a; continue; }
            const bool code = (a.style & md::Code) != 0;
            const bool bold = forceBold || mode == 2 || (a.style & md::Bold) != 0;
            if (lineHasText && x + pendingSpace + a.w > right + 0.5f) newline();
            const bool joinPill = code && lastPill_ >= 0 && pendingSpace > 0 && prev && prev->space && !a.padL;
            if (lineHasText) x += pendingSpace;
            const float gapBefore = pendingSpace;
            pendingSpace = 0;
            const float ty = y + std::round((lineH - fontH) / 2);

            Color c = base;
            if (mode != 0) c = kHeading;
            if (a.style & md::Italic) c = kItalic;
            if (bold && mode == 0) c = kStrong;
            const bool linked = !a.url.empty();
            int linkIdx = -1;
            if (linked) {
                c = kLink;
                links_->push_back({{x, y, a.w, lineH}, a.url});
                linkIdx = static_cast<int>(links_->size()) - 1;
            }
            if (a.image) {
                ring(x, ty + std::round(2 * s_), a.w, fontH - std::round(4 * s_), Color::rgb(0xffffff, 56), std::round(5 * s_));
                text(MdItem::T::Text, x + std::round(8 * s_), ty, fontH, a.text, linked ? kLink : theme::dim, 0, false, linkIdx);
                x += a.w;
                lineHasText = true;
                prev = &a;
                continue;
            }
            float tx = x;
            if (code) {
                c = linked ? kLink : kCode;
                const float pillH = std::round(fontH + 2 * s_), py = y + std::round((lineH - pillH) / 2);
                if (joinPill && lastPillY_ == y && std::abs(lastPillEnd_ - (x - gapBefore)) < 1.5f) {   // one pill across the words
                    MdItem& p = (*items_)[static_cast<std::size_t>(lastPill_)];
                    p.w = (x + a.w) - p.x;
                } else {
                    rect(x, py, a.w, pillH, kCodeBg, std::round(4 * s_));
                    lastPill_ = static_cast<int>(items_->size()) - 1;
                }
                lastPillY_ = y;
                lastPillEnd_ = x + a.w;
                if (a.padL) tx += std::round(4 * s_);
            } else lastPill_ = -1;
            text(mode == 1 ? MdItem::T::Title : MdItem::T::Text, tx, ty, fontH, a.text, c, 0, bold, linkIdx);
            const float textW = a.w - (a.padL ? std::round(4 * s_) : 0) - (a.padR ? std::round(4 * s_) : 0);
            if (linked) {
                MdItem u; u.t = MdItem::T::Rect; u.x = tx; u.w = textW; u.h = std::max(1.0f, std::round(s_ * 0.8f));
                u.y = ty + baseline + std::round(2 * s_); u.c = kLink.alpha(150); u.link = linkIdx;
                items_->push_back(std::move(u));
            }
            if (a.style & md::Strike) rect(tx, ty + std::round(baseline * 0.62f), textW, std::max(1.0f, std::round(s_ * 0.8f)), c.alpha(200));
            x += a.w;
            lineHasText = true;
            prev = &a;
        }
        return y + lineH;
    }

    // ---------------------------------------------------------------- blocks

    float sequence(const std::vector<md::Block>& blocks, float x, float w, float y, int depth, bool tight, Color base) {
        bool first = true, afterHeading = false;
        for (const auto& b : blocks) {
            if (!first) y += gap(tight || afterHeading);
            first = false;
            y = block(b, x, w, y, depth, tight, base);
            afterHeading = b.kind == md::Kind::Heading;
        }
        return y;
    }

    float block(const md::Block& b, float x, float w, float y, int depth, bool tight, Color base) {
        switch (b.kind) {
            case md::Kind::Paragraph: return flow(b.spans, x, x + w, y, base, 0);
            case md::Kind::Heading: return heading(b, x, w, y);
            case md::Kind::Code: return codeBlock(b, x, w, y);
            case md::Kind::Quote: {
                const std::size_t barAt = items_->size();
                rect(x, y, std::round(3 * s_), 0, Color::rgb(0xffffff, 46), std::round(1.5f * s_));
                const float end = sequence(b.kids, x + std::round(16 * s_), w - std::round(16 * s_), y, depth + 1, false, kQuote);
                (*items_)[barAt].h = std::max(0.0f, end - y);
                return end;
            }
            case md::Kind::List: return list(b, x, w, y, depth, tight);
            case md::Kind::Item: return sequence(b.kids, x, w, y, depth, true, base);
            case md::Kind::Rule: {
                y += std::round(8 * s_);
                rect(x, y, w, std::max(1.0f, std::round(s_)), theme::border);
                return y + std::round(8 * s_);
            }
            case md::Kind::Table: return table(b, x, w, y);
        }
        return y;
    }

    float heading(const md::Block& b, float x, float w, float y) {
        const int mode = b.level <= 2 ? 1 : 2;
        if (y > std::round(30 * s_) && b.level <= 2) y += std::round(bh_ * 0.35f);
        const float top = y;
        std::string slug = md::slug(md::plainText(b.spans));
        if (!slug.empty()) {
            const int n = slugs_[slug]++;
            out_.anchors.emplace_back(n ? slug + "-" + std::to_string(n) : slug, top);
        }
        Color c = b.level == 6 ? theme::dim : kHeading;
        std::vector<md::Span> spans = b.spans;
        y = flow(spans, x, x + w, y, c, mode);
        if (b.level <= 2) {
            y += std::round(5 * s_);
            rect(x, y, w, std::max(1.0f, std::round(s_)), theme::border);
            y += std::round(4 * s_);
        }
        return y;
    }

    float codeBlock(const md::Block& b, float x, float w, float y) {
        const float lineH = f_.lineHeight(), pad = std::round(12 * s_);
        std::vector<std::string_view> lines;
        std::string_view all = b.text;
        std::size_t p = 0;
        while (true) {
            const auto nl = all.find('\n', p);
            lines.push_back(all.substr(p, nl == std::string_view::npos ? std::string_view::npos : nl - p));
            if (nl == std::string_view::npos) break;
            p = nl + 1;
        }
        const float h = static_cast<float>(lines.size()) * lineH + 2 * pad;
        rect(x, y, w, h, kBlockBg, std::round(8 * s_));
        ring(x, y, w, h, Color::rgb(0xffffff, 26), std::round(8 * s_));
        if (!b.lang.empty()) {
            const float lw = f_.uiWidth(b.lang);
            text(MdItem::T::Text, x + w - lw - std::round(10 * s_), y + std::round(4 * s_), f_.uiLineHeight(), b.lang, theme::lineNum);
        }
        const Lang* lang = langByName(b.lang);
        std::uint8_t state = detail::StNormal;
        std::vector<Tok> toks;
        float ly = y + pad;
        const float cell = f_.cellW();
        for (auto sv : lines) {
            std::string line;
            for (char ch : sv) {
                if (ch == '\t') line.append(4 - utf8Length(line) % 4, ' ');
                else line += ch;
            }
            const float clip = x + w - pad;
            if (lang) {
                toks.assign(line.size(), Tok::Normal);
                state = detail::scanLine(lang, line, state, &toks);
                std::size_t i = 0;
                float cx = x + pad;
                while (i < line.size()) {
                    std::size_t j = i + 1;
                    while (j < line.size() && toks[j] == toks[i]) ++j;
                    while (j < line.size() && (static_cast<unsigned char>(line[j]) & 0xC0) == 0x80) ++j;   // keep code points whole
                    const std::string run = line.substr(i, j - i);
                    text(MdItem::T::Mono, cx, ly, lineH, run, theme::syntax(toks[i]), clip);
                    cx += static_cast<float>(utf8Length(run)) * cell;
                    i = j;
                }
            } else if (!line.empty()) {
                text(MdItem::T::Mono, x + pad, ly, lineH, line, Color::rgb(0xc9ccd3), clip);
            }
            ly += lineH;
        }
        return y + h;
    }

    float list(const md::Block& b, float x, float w, float y, int depth, bool tight) {
        const float numW = b.ordered ? f_.uiWidth(std::to_string(b.start + static_cast<int>(b.kids.size())) + ".") : 0;
        const float indent = std::max(std::round(26 * s_), numW + std::round(12 * s_));
        bool first = true;
        int n = b.start;
        for (const auto& item : b.kids) {
            if (!first) y += gap(true);
            first = false;
            const float cy = y + std::round(bh_ / 2);
            if (b.ordered) {
                const std::string label = std::to_string(n++) + ".";
                text(MdItem::T::Text, x + indent - std::round(8 * s_) - f_.uiWidth(label), y + std::round((bh_ - f_.uiLineHeight()) / 2), f_.uiLineHeight(), label, theme::dim);
            } else if (item.checked >= 0) {
                const float bs = std::round(14 * s_);
                const float bx = x + indent - std::round(10 * s_) - bs, by = cy - bs / 2;
                if (item.checked) {
                    rect(bx, by, bs, bs, theme::accent, std::round(3 * s_));
                    MdItem tick; tick.t = MdItem::T::Check; tick.x = bx; tick.y = by; tick.w = bs; tick.h = bs; tick.c = Color::rgb(0xffffff);
                    items_->push_back(std::move(tick));
                } else ring(bx, by, bs, bs, Color::rgb(0xffffff, 120), std::round(3 * s_));
            } else {
                const float d = std::round((depth % 3 == 1 ? 5.0f : 6.0f) * s_);
                const float bx = x + indent - std::round(11 * s_) - d / 2, by = cy - d / 2;
                if (depth % 3 == 0) rect(bx, by, d, d, theme::dim, d / 2);
                else if (depth % 3 == 1) ring(bx, by, d, d, theme::dim, d / 2);
                else rect(bx, by, d, d, theme::dim, std::round(s_));
            }
            y = block(item, x + indent, w - indent, y, depth + 1, true, theme::text);
        }
        (void)tight;
        return y;
    }

    float table(const md::Block& b, float x, float w, float y) {
        const std::size_t C = b.align.size();
        if (C == 0 || b.rows.empty()) return y;
        const float pad = std::round(10 * s_), vpad = std::round(5 * s_);
        std::vector<float> nat(C, 0.0f);
        for (std::size_t r = 0; r < b.rows.size(); ++r)
            for (std::size_t j = 0; j < C && j < b.rows[r].size(); ++j)
                nat[j] = std::max(nat[j], naturalWidth(b.rows[r][j], r == 0) + 2 * pad);
        std::vector<float> cw = nat;
        float total = std::accumulate(cw.begin(), cw.end(), 0.0f);
        if (total > w) {
            const float k = w / total;
            for (std::size_t j = 0; j < C; ++j) cw[j] = std::max(nat[j] * k, std::min(nat[j], std::round(56 * s_)));
            total = std::accumulate(cw.begin(), cw.end(), 0.0f);
            if (total > w) for (auto& v : cw) v *= w / total;
            total = std::accumulate(cw.begin(), cw.end(), 0.0f);
        }
        const float y0 = y;
        for (std::size_t r = 0; r < b.rows.size(); ++r) {
            std::vector<MdItem> cellItems;
            std::vector<MdLink> cellLinks;
            auto* savedItems = items_;
            auto* savedLinks = links_;
            items_ = &cellItems;
            links_ = &cellLinks;
            float rowEnd = y;
            float cx = x;
            for (std::size_t j = 0; j < C; ++j) {
                if (j < b.rows[r].size()) {
                    const auto& cell = b.rows[r][j];
                    const float left = cx + pad, right = cx + cw[j] - pad;
                    const float nw = naturalWidth(cell, r == 0);
                    float off = 0;
                    if (nw <= right - left) off = b.align[j] == 1 ? (right - left - nw) / 2 : b.align[j] == 2 ? right - left - nw : 0;
                    rowEnd = std::max(rowEnd, flow(cell, left, right, y + vpad, theme::text, 0, off, r == 0));
                }
                cx += cw[j];
            }
            items_ = savedItems;
            links_ = savedLinks;
            const float rowH = rowEnd - y + vpad;
            if (r == 0) rect(x, y, total, rowH, Color::rgb(0xffffff, 18));
            else if (r % 2 == 0) rect(x, y, total, rowH, Color::rgb(0xffffff, 7));
            if (r > 0) rect(x, y, total, std::max(1.0f, std::round(s_)), Color::rgb(0xffffff, 30));
            const int base = static_cast<int>(links_->size());   // cell items numbered their links in the temporary list
            for (auto& it : cellItems) {
                if (it.link >= 0) it.link += base;
                items_->push_back(std::move(it));
            }
            for (auto& l : cellLinks) links_->push_back(std::move(l));
            y += rowH;
        }
        float vx = x;
        for (std::size_t j = 0; j + 1 < C; ++j) {
            vx += cw[j];
            rect(vx, y0, std::max(1.0f, std::round(s_)), y - y0, Color::rgb(0xffffff, 30));
        }
        ring(x, y0, total, y - y0, Color::rgb(0xffffff, 44), std::round(6 * s_));
        return y;
    }
};

}  // namespace

MdLayout mdLayout(const std::vector<md::Block>& blocks, FontAtlas& font, float width, float scale) {
    return Builder(font, width, scale).run(blocks);
}

}  // namespace ide

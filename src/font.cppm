// ide.font — system font discovery, glyph rasterisation (stb_truetype) and a CPU-side
// R8 glyph atlas that the Vulkan renderer mirrors into a GPU texture.
//
// Two font roles share the atlas: the monospace *code* font (cell grid, used in the editor and
// terminal-like output) and a proportional *UI* font for all chrome, with real advances and kerning.
module;
#include "stb_truetype.h"
export module ide.font;
import std;
import ide.util;

namespace fs = std::filesystem;

export namespace ide {

struct Glyph {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  // atlas coordinates (0..1)
    float x = 0, y = 0;                    // bitmap offset from pen / baseline, in pixels
    float w = 0, h = 0;                    // bitmap size in pixels
    float advance = 0;                     // horizontal pen advance (unrounded)
    int index = 0, face = -1;              // glyph index within its face (for kerning)
    bool empty = true;                     // nothing to draw (space)
};

class FontAtlas {
public:
    static constexpr int Size = 2048;

    // Finds fonts on the system. Overrides: $IDE_FONT (code) and $IDE_UI_FONT (interface).
    std::expected<void, std::string> load() {
        std::vector<fs::path> all;
        for (const char* dir : {"/usr/share/fonts", "/usr/local/share/fonts"}) scan(dir, all);
        if (const char* home = std::getenv("HOME")) {
            scan(fs::path(home) / ".local/share/fonts", all);
            scan(fs::path(home) / ".fonts", all);
        }

        // ---- code font
        std::vector<fs::path> chosen;
        if (const char* env = std::getenv("IDE_FONT"); env && *env) chosen.emplace_back(env);
        else if (auto p = pick(all, {"jetbrainsmono-regular", "jetbrainsmono[", "cascadiacode", "firacode-regular",
                                     "firamono-regular", "sourcecodepro-regular", "sourcecodepro[",
                                     "adwaitamono-regular", "dejavusansmono.ttf", "liberationmono-regular",
                                     "notosansmono[", "notosansmono-regular", "hack-regular", "ubuntumono-r"}))
            chosen.push_back(*p);
        else if (auto q = pickAnyMono(all))
            chosen.push_back(*q);
        if (chosen.empty()) return std::unexpected("no monospace font found; set IDE_FONT=/path/to/font.ttf");
        addFallbacks(all, chosen);
        if (auto r = loadFaces(code_, chosen, true); !r) return r;
        fontPath_ = chosen.front().string();

        // ---- UI font: a modern humanist sans (Inter / Adwaita Sans first)
        std::vector<fs::path> ui;
        if (const char* env = std::getenv("IDE_UI_FONT"); env && *env) ui.emplace_back(env);
        else if (auto p = pick(all, {"adwaitasans-regular", "inter-regular", "inter[", "inter.", "cantarell-vf", "notosans[",
                                     "notosans-regular", "opensans-regular", "liberationsans-regular", "dejavusans.ttf"}))
            ui.push_back(*p);
        if (ui.empty()) ui = {chosen.front()};  // worst case: reuse the code font
        addFallbacks(all, ui);
        if (auto r = loadFaces(ui_, ui, false); !r) {
            ui = {chosen.front()};
            if (auto r2 = loadFaces(ui_, ui, false); !r2) return r2;
        }
        uiFontPath_ = ui.front().string();
        (void)loadFaces(title_, ui, false);   // same faces, rendered larger, for headings

        // ---- Material Design Icons font (Apache-2.0): all file / folder / toolbar / completion icons
        std::vector<fs::path> remix;
        for (const char* base : std::array<const char*, 3>{std::getenv("DAEMON_ASSETS"), DAEMON_ASSET_DIR, "/usr/share/daemon"}) {
            if (!base) continue;
            std::error_code ec;
            if (fs::is_regular_file(fs::path(base) / "materialdesignicons.ttf", ec)) { remix.push_back(fs::path(base) / "materialdesignicons.ttf"); break; }
        }
        if (!remix.empty()) (void)loadFaces(icons_, remix, false);
        setSizes(16, 13);
        return {};
    }

    const std::string& fontPath() const { return fontPath_; }
    const std::string& uiFontPath() const { return uiFontPath_; }

    // Sets em sizes in physical pixels. Invalidates every cached glyph.
    void setSizes(float codePx, float uiPx) {
        configure(code_, codePx, true);
        configure(ui_, uiPx, false);
        if (!title_.faces.empty()) configure(title_, uiPx * 1.85f, false);
        titleCache_.clear();
        cache_.clear();
        uiCache_.clear();
        images_.clear();
        atlas_.assign(static_cast<std::size_t>(Size) * Size, 0);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) atlas_[static_cast<std::size_t>(y) * Size + x] = 255;
        shelfX_ = 5; shelfY_ = 0; shelfH_ = 4;
        dirtyLo_ = 0; dirtyHi_ = Size;
        ++generation_;
    }
    void setPixelSize(float codePx) { setSizes(codePx, ui_.px); }
    void setUiPixelSize(float uiPx) { setSizes(code_.px, uiPx); }

    // ---- code font metrics (monospace grid)
    float pixelSize() const { return code_.px; }
    float cellW() const { return cellW_; }
    float lineHeight() const { return code_.lineH; }
    float baseline() const { return code_.baseline; }  // distance from line top to the baseline

    // ---- UI font metrics (proportional)
    float uiPixelSize() const { return ui_.px; }
    float uiLineHeight() const { return ui_.lineH; }
    float uiBaseline() const { return ui_.baseline; }

    // A pixel inside the solid white block, used for drawing plain rectangles.
    float whiteU() const { return 2.0f / Size; }
    float whiteV() const { return 2.0f / Size; }
    std::uint32_t generation() const { return generation_; }

    const Glyph& glyph(char32_t cp) {
        if (auto it = cache_.find(cp); it != cache_.end()) return it->second;
        return cache_.emplace(cp, rasterize(code_, cp)).first->second;
    }
    const Glyph& uiGlyph(char32_t cp) {
        if (auto it = uiCache_.find(cp); it != uiCache_.end()) return it->second;
        return uiCache_.emplace(cp, rasterize(ui_, cp)).first->second;
    }

    // A Material Design Icons glyph rasterised to a `px` em square. Offsets are relative to a baseline that sits
    // iconBaseline(px) below the top of the box, so (x, y + iconBaseline) places the box's top-left.
    bool hasIcons() const { return !icons_.faces.empty(); }
    float iconBaseline(int px) const {
        if (icons_.faces.empty()) return 0;
        const Face& f = *icons_.faces.front();
        int asc = 0, desc = 0, gap = 0;
        stbtt_GetFontVMetrics(&f.info, &asc, &desc, &gap);
        return std::round(static_cast<float>(asc) * stbtt_ScaleForMappingEmToPixels(&f.info, static_cast<float>(px)));
    }
    const Glyph& icon(char32_t cp, int px) {
        const std::uint64_t key = (1ull << 62) | (static_cast<std::uint64_t>(cp) << 8) | static_cast<std::uint64_t>(px);
        if (auto it = images_.find(key); it != images_.end()) return it->second;
        Glyph g;
        if (!icons_.faces.empty()) {
            Face& f = *icons_.faces.front();
            const float sc = stbtt_ScaleForMappingEmToPixels(&f.info, static_cast<float>(px));
            const int idx = stbtt_FindGlyphIndex(&f.info, static_cast<int>(cp));
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBox(&f.info, idx, sc, sc, &x0, &y0, &x1, &y1);
            const int w = x1 - x0, h = y1 - y0;
            int px0 = 0, py0 = 0;
            if (idx && w > 0 && h > 0 && place(w, h, px0, py0)) {
                std::vector<unsigned char> bmp(static_cast<std::size_t>(w) * h);
                stbtt_MakeGlyphBitmap(&f.info, bmp.data(), w, h, w, sc, sc, idx);
                for (int row = 0; row < h; ++row)
                    std::memcpy(&atlas_[static_cast<std::size_t>(py0 + row) * Size + px0], &bmp[static_cast<std::size_t>(row) * w], static_cast<std::size_t>(w));
                fillGlyph(g, px0, py0, w, h);
                g.x = static_cast<float>(x0);
                g.y = static_cast<float>(y0);
            }
        }
        return images_.emplace(key, g).first->second;
    }

    // ---- headings: the UI font at ~1.85x
    const Glyph& titleGlyph(char32_t cp) {
        if (title_.faces.empty()) return uiGlyph(cp);
        if (auto it = titleCache_.find(cp); it != titleCache_.end()) return it->second;
        return titleCache_.emplace(cp, rasterize(title_, cp)).first->second;
    }
    float titleLineHeight() const { return title_.faces.empty() ? ui_.lineH : title_.lineH; }
    float titleBaseline() const { return title_.faces.empty() ? ui_.baseline : title_.baseline; }
    float titleWidth(std::string_view s) {
        float w = 0;
        for (std::size_t i = 0; i < s.size();) {
            const char32_t cp = decodeUtf8(s, i);
            if (cp < 0x20 || cp == 0x7f) continue;
            w += titleGlyph(cp).advance;
        }
        return w;
    }

    // Kerning between two UI glyphs of the same face, in pixels.
    float uiKern(const Glyph& a, const Glyph& b) const {
        if (a.face < 0 || a.face != b.face) return 0;
        const Face& f = *ui_.faces[static_cast<std::size_t>(a.face)];
        return static_cast<float>(stbtt_GetGlyphKernAdvance(&f.info, a.index, b.index)) * f.scale;
    }

    // Width of a UI-font string in pixels.
    float uiWidth(std::string_view s) {
        float w = 0;
        const Glyph* prev = nullptr;
        for (std::size_t i = 0; i < s.size();) {
            char32_t cp = decodeUtf8(s, i);
            if (cp < 0x20 || cp == 0x7f) continue;
            const Glyph& g = uiGlyph(cp);
            if (prev) w += uiKern(*prev, g);
            w += g.advance;
            prev = &g;
        }
        return w;
    }

    // Shortens `s` with an ellipsis so it fits in `maxW` pixels.
    std::string uiEllipsize(std::string_view s, float maxW) {
        if (uiWidth(s) <= maxW) return std::string(s);
        const float dots = uiWidth("\xE2\x80\xA6");
        std::string out;
        float w = 0;
        for (std::size_t i = 0; i < s.size();) {
            std::size_t start = i;
            char32_t cp = decodeUtf8(s, i);
            float a = uiGlyph(cp).advance;
            if (w + a + dots > maxW) break;
            w += a;
            out.append(s.substr(start, i - start));
        }
        return out + "\xE2\x80\xA6";
    }

    // Caches an arbitrary w x h coverage bitmap (icons) under `key`; `render` fills w*h bytes.
    // The reference stays valid until the next setSizes().
    const Glyph& image(std::uint64_t key, int w, int h, const std::function<void(std::uint8_t*)>& render) {
        if (auto it = images_.find(key); it != images_.end()) return it->second;
        Glyph g;
        int px = 0, py = 0;
        if (place(w, h, px, py)) {
            std::vector<std::uint8_t> tmp(static_cast<std::size_t>(w) * h);
            render(tmp.data());
            for (int row = 0; row < h; ++row)
                std::memcpy(&atlas_[static_cast<std::size_t>(py + row) * Size + px], &tmp[static_cast<std::size_t>(row) * w],
                            static_cast<std::size_t>(w));
            fillGlyph(g, px, py, w, h);
        }
        return images_.emplace(key, g).first->second;
    }

    const std::uint8_t* pixels() const { return atlas_.data(); }

    // Returns the rows [y0, y1) modified since the last call.
    bool takeDirty(int& y0, int& y1) {
        if (dirtyLo_ >= dirtyHi_) return false;
        y0 = dirtyLo_; y1 = dirtyHi_;
        dirtyLo_ = Size; dirtyHi_ = 0;
        return true;
    }
    void markAllDirty() { dirtyLo_ = 0; dirtyHi_ = Size; }

private:
    struct Face {
        std::vector<char> data;
        stbtt_fontinfo info{};
        float scale = 1;
        int ascent = 0, descent = 0, lineGap = 0;
    };
    struct Role {
        std::vector<std::unique_ptr<Face>> faces;
        float px = 16, lineH = 20, baseline = 15;
    };

    static std::string lower(std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    static void scan(const fs::path& root, std::vector<fs::path>& out) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) return;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            auto ext = lower(it->path().extension().string());
            if (ext == ".ttf" || ext == ".otf") out.push_back(it->path());
        }
    }

    static std::optional<fs::path> pick(const std::vector<fs::path>& all, std::span<const std::string_view> names) {
        for (auto want : names)
            for (const auto& p : all)
                if (lower(p.filename().string()).find(want) != std::string::npos) return p;
        return std::nullopt;
    }
    static std::optional<fs::path> pick(const std::vector<fs::path>& all, std::initializer_list<std::string_view> names) {
        return pick(all, std::span<const std::string_view>(names.begin(), names.size()));
    }

    static std::optional<fs::path> pickAnyMono(const std::vector<fs::path>& all) {
        for (const auto& p : all) {
            auto n = lower(p.filename().string());
            if (n.find("mono") != std::string::npos && n.find("bold") == std::string::npos &&
                n.find("italic") == std::string::npos && n.find("oblique") == std::string::npos)
                return p;
        }
        return std::nullopt;
    }

    // Symbol fallbacks for glyphs the primary font lacks (arrows, check marks, ...).
    static void addFallbacks(const std::vector<fs::path>& all, std::vector<fs::path>& chosen) {
        const std::array<std::vector<std::string_view>, 2> fallbacks = {{
            {"dejavusansmono.ttf", "notosansmono[", "dejavusans.ttf"},
            {"symbola", "notosanssymbols"},
        }};
        for (const auto& names : fallbacks)
            if (auto p = pick(all, std::span<const std::string_view>(names)); p && std::ranges::find(chosen, *p) == chosen.end())
                chosen.push_back(*p);
    }

    std::expected<void, std::string> loadFaces(Role& role, const std::vector<fs::path>& paths, bool strict) {
        role.faces.clear();
        for (const auto& path : paths) {
            auto data = readFile(path);
            if (!data) {
                if (role.faces.empty()) return std::unexpected("cannot read font " + path.string());
                continue;
            }
            auto face = std::make_unique<Face>();
            face->data.assign(data->begin(), data->end());
            auto* bytes = reinterpret_cast<const unsigned char*>(face->data.data());
            int off = stbtt_GetFontOffsetForIndex(bytes, 0);
            if (off < 0 || !stbtt_InitFont(&face->info, bytes, off)) {
                if (role.faces.empty()) return std::unexpected("unsupported font file " + path.string());
                continue;
            }
            role.faces.push_back(std::move(face));
        }
        (void)strict;
        return {};
    }

    void configure(Role& r, float px, bool mono) {
        r.px = px;
        for (auto& f : r.faces) {
            f->scale = stbtt_ScaleForMappingEmToPixels(&f->info, px);
            stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->lineGap);
        }
        auto& p = *r.faces.front();
        const float natural = static_cast<float>(p.ascent - p.descent) * p.scale;
        if (mono) {
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&p.info, 'M', &adv, &lsb);
            cellW_ = std::max(1.0f, std::round(static_cast<float>(adv) * p.scale));
            r.lineH = std::round(px * 1.5f);  // airy, like current editors
            r.baseline = std::round((r.lineH - natural) / 2 + static_cast<float>(p.ascent) * p.scale);
        } else {
            r.lineH = std::ceil(natural);
            r.baseline = std::round(static_cast<float>(p.ascent) * p.scale);
        }
    }

    // Shelf packer: reserves a w x h rectangle (plus 1px padding) in the atlas.
    bool place(int w, int h, int& x, int& y) {
        if (shelfX_ + w + 1 > Size) { shelfX_ = 0; shelfY_ += shelfH_ + 1; shelfH_ = 0; }
        if (shelfY_ + h + 1 > Size) return false;
        shelfH_ = std::max(shelfH_, h);
        x = shelfX_;
        y = shelfY_;
        shelfX_ += w + 1;
        dirtyLo_ = std::min(dirtyLo_, y);
        dirtyHi_ = std::max(dirtyHi_, y + h);
        return true;
    }

    void fillGlyph(Glyph& g, int px, int py, int w, int h) const {
        g.empty = false;
        g.u0 = static_cast<float>(px) / Size;
        g.v0 = static_cast<float>(py) / Size;
        g.u1 = static_cast<float>(px + w) / Size;
        g.v1 = static_cast<float>(py + h) / Size;
        g.w = static_cast<float>(w);
        g.h = static_cast<float>(h);
    }

    Glyph rasterize(Role& role, char32_t cp) {
        Face* face = nullptr;
        int idx = 0, faceId = -1;
        for (std::size_t i = 0; i < role.faces.size(); ++i) {
            idx = stbtt_FindGlyphIndex(&role.faces[i]->info, static_cast<int>(cp));
            if (idx != 0) { face = role.faces[i].get(); faceId = static_cast<int>(i); break; }
        }
        if (!face) { face = role.faces.front().get(); idx = 0; faceId = 0; }

        Glyph g;
        g.index = idx;
        g.face = faceId;
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&face->info, idx, &adv, &lsb);
        g.advance = static_cast<float>(adv) * face->scale;

        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&face->info, idx, face->scale, face->scale, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0) return g;  // blank glyph (space): advance only

        int px = 0, py = 0;
        if (!place(w, h, px, py)) return g;  // atlas full: draw nothing

        std::vector<unsigned char> bmp(static_cast<std::size_t>(w) * h);
        stbtt_MakeGlyphBitmap(&face->info, bmp.data(), w, h, w, face->scale, face->scale, idx);
        static const auto lut = [] {  // slight coverage boost: light-on-dark text looks thin otherwise
            std::array<std::uint8_t, 256> t{};
            for (int i = 0; i < 256; ++i) t[i] = static_cast<std::uint8_t>(std::lround(255.0 * std::pow(i / 255.0, 0.80)));
            return t;
        }();
        for (int row = 0; row < h; ++row)
            for (int col = 0; col < w; ++col)
                atlas_[static_cast<std::size_t>(py + row) * Size + px + col] =
                    lut[bmp[static_cast<std::size_t>(row) * w + col]];

        fillGlyph(g, px, py, w, h);
        g.x = static_cast<float>(x0);
        g.y = static_cast<float>(y0);
        return g;
    }

    Role code_, ui_, icons_, title_;
    float cellW_ = 9;
    std::unordered_map<char32_t, Glyph> cache_, uiCache_, titleCache_;
    std::unordered_map<std::uint64_t, Glyph> images_;
    std::vector<std::uint8_t> atlas_;
    std::string fontPath_, uiFontPath_;
    int shelfX_ = 0, shelfY_ = 0, shelfH_ = 0;
    int dirtyLo_ = Size, dirtyHi_ = 0;
    std::uint32_t generation_ = 0;
};

}  // namespace ide

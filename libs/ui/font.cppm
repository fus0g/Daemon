// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.font — system font discovery, glyph rasterisation (stb_truetype) and a CPU-side
// R8 glyph atlas that the Vulkan renderer mirrors into a GPU texture.
//
// Two font roles share the atlas: the monospace *code* font (cell grid, used in the editor and
// terminal-like output) and a proportional *UI* font for all chrome, with real advances and kerning.
module;
#include "stb_truetype.h"
export module daemon.ui.font;
import std;
import daemon.base.util;

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
    std::expected<void, std::string> load();

    const std::string& fontPath() const { return fontPath_; }
    const std::string& uiFontPath() const { return uiFontPath_; }

    // Sets em sizes in physical pixels. Invalidates every cached glyph.
    void setSizes(float codePx, float uiPx);
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
    float iconBaseline(int px) const;
    const Glyph& icon(char32_t cp, int px);

    // ---- headings: the UI font at ~1.85x
    const Glyph& titleGlyph(char32_t cp) {
        if (title_.faces.empty()) return uiGlyph(cp);
        if (auto it = titleCache_.find(cp); it != titleCache_.end()) return it->second;
        return titleCache_.emplace(cp, rasterize(title_, cp)).first->second;
    }
    float titleLineHeight() const { return title_.faces.empty() ? ui_.lineH : title_.lineH; }
    float titleBaseline() const { return title_.faces.empty() ? ui_.baseline : title_.baseline; }
    float titleWidth(std::string_view s);

    // Kerning between two UI glyphs of the same face, in pixels.
    float uiKern(const Glyph& a, const Glyph& b) const {
        if (a.face < 0 || a.face != b.face) return 0;
        const Face& f = *ui_.faces[static_cast<std::size_t>(a.face)];
        return static_cast<float>(stbtt_GetGlyphKernAdvance(&f.info, a.index, b.index)) * f.scale;
    }

    // Width of a UI-font string in pixels.
    float uiWidth(std::string_view s);

    // Shortens `s` with an ellipsis so it fits in `maxW` pixels.
    std::string uiEllipsize(std::string_view s, float maxW);

    // Caches an arbitrary w x h coverage bitmap (icons) under `key`; `render` fills w*h bytes.
    // The reference stays valid until the next setSizes().
    const Glyph& image(std::uint64_t key, int w, int h, const std::function<void(std::uint8_t*)>& render);

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

    static void scan(const fs::path& root, std::vector<fs::path>& out);

    static std::optional<fs::path> pick(const std::vector<fs::path>& all, std::span<const std::string_view> names) {
        for (auto want : names)
            for (const auto& p : all)
                if (lower(p.filename().string()).find(want) != std::string::npos) return p;
        return std::nullopt;
    }
    static std::optional<fs::path> pick(const std::vector<fs::path>& all, std::initializer_list<std::string_view> names) {
        return pick(all, std::span<const std::string_view>(names.begin(), names.size()));
    }

    static std::optional<fs::path> pickAnyMono(const std::vector<fs::path>& all);

    // Symbol fallbacks for glyphs the primary font lacks (arrows, check marks, ...).
    static void addFallbacks(const std::vector<fs::path>& all, std::vector<fs::path>& chosen);

    std::expected<void, std::string> loadFaces(Role& role, const std::vector<fs::path>& paths, bool strict);

    void configure(Role& r, float px, bool mono);

    // Shelf packer: reserves a w x h rectangle (plus 1px padding) in the atlas.
    bool place(int w, int h, int& x, int& y);

    void fillGlyph(Glyph& g, int px, int py, int w, int h) const;

    Glyph rasterize(Role& role, char32_t cp);

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

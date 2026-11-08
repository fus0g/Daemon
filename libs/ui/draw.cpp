// draw - implementation of module daemon.ui.draw.
module daemon.ui.draw;
import std;
import daemon.base.util;
import daemon.ui.font;

namespace ide {

float DrawList::text(FontAtlas& font, float x, float y, std::string_view s, Color c,
           float maxX) {
    const float cw = font.cellW(), base = y + font.baseline();
    for (std::size_t i = 0; i < s.size();) {
        char32_t cp = decodeUtf8(s, i);
        if (cp < 0x20 || cp == 0x7f) cp = (cp == '\t') ? ' ' : 0xFFFD;
        if (x + cw > maxX) break;
        if (cp != ' ') glyph(font.glyph(cp), x, base, c);
        x += cw;
    }
    return x;
}

float DrawList::titleText(FontAtlas& font, float x, float y, std::string_view s, Color c) {
    const float base = std::round(y) + font.titleBaseline();
    for (std::size_t i = 0; i < s.size();) {
        char32_t cp = decodeUtf8(s, i);
        if (cp < 0x20 || cp == 0x7f) continue;
        const Glyph& g = font.titleGlyph(cp);
        glyph(g, x, base, c);
        x += g.advance;
    }
    return x;
}

float DrawList::uiText(FontAtlas& font, float x, float y, std::string_view s, Color c,
             float maxX) {
    const float base = std::round(y) + font.uiBaseline();
    const Glyph* prev = nullptr;
    for (std::size_t i = 0; i < s.size();) {
        char32_t cp = decodeUtf8(s, i);
        if (cp < 0x20 || cp == 0x7f) continue;
        const Glyph& g = font.uiGlyph(cp);
        if (prev) x += font.uiKern(*prev, g);
        if (x + g.advance > maxX + 0.5f) break;
        glyph(g, x, base, c);
        x += g.advance;
        prev = &g;
    }
    return x;
}

}  // namespace ide

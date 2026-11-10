// daemon.icons — vector icons for the file explorer and tabs.
//
// Icons are described on a 16x16 design grid with a few primitives (rounded rects, circles,
// capsules, polygons, rings), rasterised once per pixel size with 6x6 supersampling into the glyph
// atlas, and drawn as tinted masks. They stay crisp at any scale and cost nothing per frame.
export module daemon.ui.icons;
import std;
import daemon.ui.draw;
import daemon.ui.font;

namespace fs = std::filesystem;

export namespace ide {

enum class FileKind { Code, Config, Doc, Image, Build, Shader, Archive, Binary, Generic };

struct FileStyle {
    FileKind kind = FileKind::Generic;
    std::uint32_t color = 0x9aa0ab;  // 0xRRGGBB
};

FileStyle fileStyle(std::string_view name);

// Folder colour hints for well-known directory names.
std::uint32_t folderColor(std::string_view name);

namespace icon_detail {

struct V {
    float x, y;
};

struct Prim {
    enum Kind { RRect, Circle, Capsule, Poly, Ring } kind = Circle;
    bool sub = false;          // subtract from what has been drawn so far
    float a = 0, b = 0, c = 0, d = 0, e = 0;
    std::vector<V> pts;
};
using Layer = std::vector<Prim>;

// Builders (coordinates on the 16x16 design grid).
inline Prim rr(float cx, float cy, float hw, float hh, float r, bool sub = false) {
    return {Prim::RRect, sub, cx, cy, hw, hh, r, {}};
}
inline Prim circ(float cx, float cy, float r, bool sub = false) { return {Prim::Circle, sub, cx, cy, r, 0, 0, {}}; }
inline Prim ring(float cx, float cy, float r, float w) { return {Prim::Ring, false, cx, cy, r, w, 0, {}}; }
inline Prim cap(float x0, float y0, float x1, float y1, float r) { return {Prim::Capsule, false, 0, 0, 0, 0, r, {{x0, y0}, {x1, y1}}}; }
inline Prim poly(std::vector<V> pts, bool sub = false) { return {Prim::Poly, sub, 0, 0, 0, 0, 0, std::move(pts)}; }

bool inside(const Prim& p, float x, float y);

// Renders a layer into a size x size coverage mask (6x6 supersampling).
void rasterize(const Layer& layer, int size, std::uint8_t* out);

// ---- icon designs

inline Layer pageLayer() {
    return {rr(8, 8, 5.6f, 7.1f, 1.7f), poly({{8.3f, -0.5f}, {15.0f, -0.5f}, {15.0f, 6.2f}}, true)};
}
inline Layer foldLayer() { return {poly({{9.4f, 0.6f}, {9.4f, 4.4f}, {13.2f, 4.4f}})}; }

// Dark symbol cut into the page, by file kind.
Layer markLayer(FileKind kind);

inline Layer folderBack() { return {rr(8.0f, 8.9f, 7.2f, 5.1f, 1.7f), rr(4.8f, 3.6f, 3.7f, 1.5f, 1.1f)}; }
inline Layer folderFront() { return {rr(8.0f, 9.9f, 7.2f, 4.1f, 1.7f)}; }
inline Layer folderOpenFront() {
    return {poly({{3.2f, 6.9f}, {15.8f, 6.9f}, {13.8f, 14.2f}, {1.4f, 14.2f}}), rr(7.6f, 12.6f, 6.2f, 1.7f, 1.0f)};
}

inline Layer playLayer() {
    return {poly({{4.6f, 2.6f}, {13.0f, 8.0f}, {4.6f, 13.4f}}), cap(4.6f, 2.6f, 13.0f, 8.0f, 0.8f),
            cap(13.0f, 8.0f, 4.6f, 13.4f, 0.8f), cap(4.6f, 13.4f, 4.6f, 2.6f, 0.8f)};
}
inline Layer stopLayer() { return {rr(8.0f, 8.0f, 4.8f, 4.8f, 1.5f)}; }
inline Layer hammerLayer() {
    return {cap(4.4f, 13.4f, 9.8f, 6.2f, 1.3f), poly({{14.74f, 6.38f}, {12.54f, 9.46f}, {6.06f, 4.82f}, {8.26f, 1.74f}}),
            cap(14.74f, 6.38f, 12.54f, 9.46f, 0.5f), cap(8.26f, 1.74f, 6.06f, 4.82f, 0.5f)};
}
inline Layer reloadLayer() {
    return {ring(8.0f, 8.0f, 4.7f, 1.9f), poly({{8.0f, 8.0f}, {10.33f, -0.69f}, {16.7f, 5.67f}}, true),
            poly({{9.6f, 5.3f}, {15.4f, 5.3f}, {12.5f, 9.4f}})};
}
inline Layer panelLayer() {
    return {rr(8.0f, 8.0f, 6.4f, 5.4f, 1.6f), rr(8.0f, 8.0f, 5.1f, 4.1f, 0.8f, true), rr(8.0f, 11.3f, 5.1f, 1.7f, 0.3f)};
}

// Outline helpers: a rounded rect stroke of thickness t.
inline void outlineRR(Layer& l, float cx, float cy, float hw, float hh, float r, float t) {
    l.push_back(rr(cx, cy, hw, hh, r));
    l.push_back(rr(cx, cy, hw - t, hh - t, std::max(0.2f, r - t), true));
}
Layer filesLayer();
Layer terminalLayer();
inline Layer warningLayer() {
    return {poly({{8.0f, 1.6f}, {15.0f, 13.8f}, {1.0f, 13.8f}}),
            poly({{8.0f, 5.0f}, {12.2f, 12.2f}, {3.8f, 12.2f}}, true),
            cap(8.0f, 7.0f, 8.0f, 9.4f, 0.75f), circ(8.0f, 11.0f, 0.85f)};
}
inline Layer minimizeLayer() { return {cap(3.6f, 8.0f, 12.4f, 8.0f, 0.65f)}; }
inline Layer maximizeLayer() {
    Layer l;
    outlineRR(l, 8.0f, 8.0f, 4.8f, 4.8f, 1.2f, 1.3f);
    return l;
}
inline Layer closeLayer() { return {cap(4.0f, 4.0f, 12.0f, 12.0f, 0.75f), cap(12.0f, 4.0f, 4.0f, 12.0f, 0.75f)}; }
// The app mark: a dragon-winged war helm seen from the front - a smooth rounded dome with a ridge, bat-wing
// fins with scalloped edges, narrow slit eyes and a pointed chin guard.
inline Layer logoHelm() {
    auto mirror = [](std::vector<V> v) {
        for (auto& p : v) p.x = 16.0f - p.x;
        std::reverse(v.begin(), v.end());
        return v;
    };
    std::vector<V> wing = {{4.5f, 3.4f}, {3.7f, -0.3f}, {3.3f, 2.5f}, {1.5f, 0.1f}, {2.2f, 3.2f}, {0.0f, 2.2f}, {1.5f, 4.6f}, {4.1f, 6.6f}};
    std::vector<V> eye = {{4.6f, 6.3f}, {7.5f, 7.5f}, {7.4f, 8.5f}, {5.1f, 7.7f}};
    return {rr(8.0f, 7.0f, 3.9f, 5.4f, 3.6f), poly({{4.1f, 9.0f}, {11.9f, 9.0f}, {10.3f, 14.4f}, {8.0f, 15.5f}, {5.7f, 14.4f}}),
            poly(wing), poly(mirror(wing)),
            poly(eye, true), poly(mirror(eye), true),
            rr(8.0f, 11.3f, 0.5f, 2.3f, 0.3f, true), cap(8.0f, 1.9f, 8.0f, 5.0f, 0.2f)};
}

inline Layer chevronLayer(bool down) {
    if (down) return {cap(3.8f, 6.0f, 8.0f, 10.2f, 0.95f), cap(8.0f, 10.2f, 12.2f, 6.0f, 0.95f)};
    return {cap(6.0f, 3.8f, 10.2f, 8.0f, 0.95f), cap(10.2f, 8.0f, 6.0f, 12.2f, 0.95f)};
}

}  // namespace icon_detail

// Material Design Icons code points (private-use plane 15) for the font in third_party/fonts.
namespace mdi {
inline constexpr char32_t Folder = 0xF024B, FolderOpen = 0xF0770, File = 0xF0224, Doc = 0xF09EE, Code = 0xF102B,
    Image = 0xF0976, CogFile = 0xF107C, Cpp = 0xF0672, C = 0xF0671, Python = 0xF0320, Js = 0xF031E, Ts = 0xF06E6,
    Markdown = 0xF0354, Rust = 0xF1617, Go = 0xF07D3, Java = 0xF0B37, Html = 0xF031D, Css = 0xF031C, Lua = 0xF08B1,
    CSharp = 0xF031B, Kotlin = 0xF1219, Json = 0xF0626, Cog = 0xF08BB, Script = 0xF0BC3, Cube = 0xF01A7,
    Zip = 0xF0FFA, Xml = 0xF05C0, Bash = 0xF1183, Git = 0xF02A2, Docker = 0xF0868, Hammer = 0xF08EA,
    ChevronRight = 0xF0142, ChevronDown = 0xF0140, Play = 0xF040A, Stop = 0xF04DB, Refresh = 0xF0450,
    DockBottom = 0xF10A9, ConsoleLine = 0xF07B7, FileTree = 0xF13D2, Alert = 0xF002A, Minimize = 0xF05B0,
    Maximize = 0xF05AF, Close = 0xF0156, Database = 0xF1632;
}

char32_t fileGlyph(std::string_view name, FileKind kind);

enum class ToolIcon { Play, Stop, Build, Reload, Panel, Files, Terminal, Warning, Minimize, Maximize, Close };

class Icons {
public:
    // Draw a file icon with its top-left corner at (x, y); `size` is in pixels.
    static void file(DrawList& dl, FontAtlas& font, float x, float y, int size, std::string_view name, float alpha = 1.0f);

    static void folder(DrawList& dl, FontAtlas& font, float x, float y, int size, std::string_view name, bool open,
                       float alpha = 1.0f);

    static void tool(DrawList& dl, FontAtlas& font, float x, float y, int size, ToolIcon icon, Color color);

    // The app mark: a dragon-winged war helm in the accent colour (see logoHelm).
    static void logo(DrawList& dl, FontAtlas& font, float x, float y, int size, Color accent) {
        using namespace icon_detail;
        layer(dl, font, 72, 0, size, x, y, accent, logoHelm);
    }

    static void chevron(DrawList& dl, FontAtlas& font, float x, float y, int size, bool down, Color color) {
        using namespace icon_detail;
        if (font.hasIcons()) { glyph(dl, font, x, y, size, down ? mdi::ChevronDown : mdi::ChevronRight, color); return; }
        layer(dl, font, down ? 41 : 40, 0, size, x, y, color, [&] { return chevronLayer(down); });
    }

    // One Material Design Icons glyph in a size x size box with its top-left at (x, y).
    static void glyph(DrawList& dl, FontAtlas& font, float x, float y, int size, char32_t cp, Color color) {
        const Glyph& g = font.icon(cp, size);
        if (g.empty) return;
        dl.glyph(g, x, y + font.iconBaseline(size), color);
    }

private:
    template <class Make>
    static void layer(DrawList& dl, FontAtlas& font, std::uint32_t id, std::uint32_t variant, int size, float x, float y,
                      Color color, Make&& make) {
        const std::uint64_t key = (std::uint64_t{1} << 62) | (std::uint64_t{id} << 32) | (std::uint64_t{variant} << 16) |
                                  static_cast<std::uint64_t>(size);
        const Glyph& g = font.image(key, size, size, [&](std::uint8_t* px) { icon_detail::rasterize(make(), size, px); });
        if (g.empty) return;
        dl.glyph(g, x, y, color);  // glyph offsets are zero, so (x, y) is the top-left corner
    }
};

}  // namespace ide

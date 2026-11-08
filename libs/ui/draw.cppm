// daemon.draw — colours and the per-frame list of instanced quads handed to the renderer.
export module daemon.ui.draw;
import std;
import daemon.base.util;
import daemon.ui.font;

export namespace ide {

struct Color {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;

    static constexpr Color rgb(std::uint32_t hex, std::uint8_t alpha = 255) {
        return {static_cast<std::uint8_t>(hex >> 16), static_cast<std::uint8_t>(hex >> 8),
                static_cast<std::uint8_t>(hex), alpha};
    }
    constexpr Color alpha(std::uint8_t v) const { return {r, g, b, v}; }
    constexpr Color mix(Color o, float t) const {
        auto m = [t](std::uint8_t x, std::uint8_t y) {
            return static_cast<std::uint8_t>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t + 0.5f);
        };
        return {m(r, o.r), m(g, o.g), m(b, o.b), m(a, o.a)};
    }
    constexpr std::uint32_t packed() const {
        return std::uint32_t(r) | std::uint32_t(g) << 8 | std::uint32_t(b) << 16 | std::uint32_t(a) << 24;
    }
};

// One quad. Layout matches the vertex input state in ide.vulkan and quad.vert.
struct Instance {
    float x, y, w, h;
    float u0, v0, u1, v1;
    std::uint32_t rgba;
    float radius;  // corner radius (px); negative draws the complement, used to mask panel corners
};
static_assert(sizeof(Instance) == 40);

class DrawList {
public:
    enum class Mode : std::uint8_t { Normal, Erase, Glass };
    struct Seg { std::uint32_t at; Mode mode; };
    void clear() { items_.clear(); segs_.clear(); mode_ = Mode::Normal; }
    // Drawing modes for the quads that follow. Erase: replaces what is underneath (colour premultiplied, alpha 255),
    // used to carve translucent chrome out around opaque panels. Glass: blurs the scene behind the quad and tints it
    // with the quad's colour; everything from the first Glass quad on is drawn after the scene (menus, popups).
    void setMode(Mode m) {
        if (m == mode_) return;
        mode_ = m;
        segs_.push_back({static_cast<std::uint32_t>(items_.size()), m});
    }
    void erase(bool on) { setMode(on ? Mode::Erase : Mode::Normal); }
    void glass(bool on) { setMode(on ? Mode::Glass : Mode::Normal); }
    std::span<const Seg> segs() const { return segs_; }
    std::span<const Instance> items() const { return items_; }
    std::size_t size() const { return items_.size(); }

    void rect(const FontAtlas& font, float x, float y, float w, float h, Color c, float radius = 0) {
        if (w <= 0 || h <= 0 || c.a == 0) return;
        items_.push_back({x, y, w, h, font.whiteU(), font.whiteV(), font.whiteU(), font.whiteV(), c.packed(), radius});
    }

    // Rounded outline (a ring of `width` px just inside the rect). Drawn over the content below it.
    void ring(const FontAtlas& font, float x, float y, float w, float h, Color c, float radius, float width) {
        if (w <= 0 || h <= 0 || c.a == 0) return;
        items_.push_back({x, y, w, h, font.whiteU(), font.whiteV(), width, font.whiteV(), c.packed(), 1000.0f + radius});
    }

    // Draws one glyph with its pen at (x, baselineY). Coordinates are snapped to whole pixels.
    void glyph(const Glyph& g, float x, float baselineY, Color c) {
        if (g.empty) return;
        items_.push_back({std::round(x) + g.x, std::round(baselineY) + g.y, g.w, g.h, g.u0, g.v0, g.u1, g.v1,
                          c.packed(), 0.0f});
    }

    // Single-line text on a monospace grid. `y` is the top of the line box.
    // Returns the x coordinate after the last drawn cell.
    float text(FontAtlas& font, float x, float y, std::string_view s, Color c,
               float maxX = std::numeric_limits<float>::infinity());

    // Heading text (the larger UI face). `y` is the top of the heading line box.
    float titleText(FontAtlas& font, float x, float y, std::string_view s, Color c);

    // Proportional UI text. `y` is the top of the UI line box; returns the x after the last glyph.
    float uiText(FontAtlas& font, float x, float y, std::string_view s, Color c,
                 float maxX = std::numeric_limits<float>::infinity());

private:
    std::vector<Instance> items_;
    std::vector<Seg> segs_;
    Mode mode_ = Mode::Normal;
};

}  // namespace ide

// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app.mdview - lays a parsed Markdown document out for a given width: positioned text runs, rectangles and link
// boxes in document coordinates (origin at the top-left of the content). Drawing is a scan over the visible items.
export module daemon.app.mdview;
import std;
import daemon.ui.draw;
import daemon.ui.font;
import daemon.ui.geometry;
import daemon.text.markdown;

export namespace ide {

struct MdItem {
    enum class T : std::uint8_t { Text, Title, Mono, Rect, Ring, Check, Image } t = T::Rect;   // Check: a tick icon in the box x, y, w; Image: picture `s` in the box
    float x = 0, y = 0, w = 0, h = 0;   // Text / Title / Mono: w is the right clip limit (0 = none); Rect / Ring: the box
    float radius = 0;
    std::string s;
    Color c;
    bool bold = false;   // drawn twice, one pixel apart (the UI font has a single weight)
    int link = -1;       // index into MdLayout::links: the item brightens while that link is hovered
};

struct MdLink {
    Rect r;
    std::string url;
};

struct MdLayout {
    std::vector<MdItem> items;
    std::vector<MdLink> links;
    std::vector<std::pair<std::string, float>> anchors;   // heading slug -> y
    float height = 0;
};

// Natural size of the picture `src` in pixels: {w, h} when it is available, {-1, -1} while it is still loading,
// {0, 0} when it cannot be shown (the alt text is drawn instead).
using MdImageSize = std::function<std::pair<int, int>(const std::string& src)>;

// `width` is the viewport width in pixels, `scale` the UI scale factor.
MdLayout mdLayout(const std::vector<md::Block>& blocks, FontAtlas& font, float width, float scale, const MdImageSize& imageSize = {}, bool center = true);

}  // namespace ide

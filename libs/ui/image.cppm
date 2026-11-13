// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.ui.image — image decoding (PNG, JPEG, GIF, BMP via stb_image; SVG via NanoSVG), a premultiplied mip pyramid for
// crisp downscaling, and the CPU-side RGBA atlas the Vulkan renderer mirrors into a GPU texture.
export module daemon.ui.image;
import std;

export namespace ide {

struct Image {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;   // premultiplied alpha, w * h * 4 bytes
};

// A decoded picture and successively halved copies of it (level 0 is the largest, at most `maxSide` on its long edge).
struct ImageLevels {
    int naturalW = 0, naturalH = 0;   // size of the source (SVG: its declared size)
    std::vector<Image> levels;
};

// Font used for <text> in SVG files (shields.io badges and the like). Without one, SVGs containing text are refused.
void setSvgTextFont(const std::string& path);

// Decodes the bytes; on failure returns nullopt and sets `error`. Refuses absurd sizes (decompression bombs).
// SVG text is drawn by us (anchor, size, scale transform, textLength, fill); other SVG features follow NanoSVG
// (no clip paths, filters or CSS).
std::optional<ImageLevels> decodeImage(std::span<const std::uint8_t> bytes, int maxSide, std::string& error);

class ImageAtlas {
public:
    static constexpr int Size = 2048;

    struct Region {
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        int w = 0, h = 0;
    };

    // Packs the image (with a one-pixel extruded border so linear filtering never bleeds); nullopt when it does not fit.
    std::optional<Region> add(const Image& img);

    // Forgets every image (the pixel memory is released).
    void clear();

    bool empty() const { return pixels_.empty(); }
    const std::uint8_t* pixels() const { return pixels_.data(); }   // Size * Size * 4
    std::uint32_t epoch() const { return epoch_; }                   // changes whenever clear() dropped images

    // Rows [y0, y1) modified since the last call.
    bool takeDirty(int& y0, int& y1) {
        if (dirtyLo_ >= dirtyHi_) return false;
        y0 = dirtyLo_; y1 = dirtyHi_;
        dirtyLo_ = Size; dirtyHi_ = 0;
        return true;
    }
    void markAllDirty() { if (!pixels_.empty()) { dirtyLo_ = 0; dirtyHi_ = shelfY_ + shelfH_; } }

private:
    std::vector<std::uint8_t> pixels_;
    int shelfX_ = 0, shelfY_ = 0, shelfH_ = 0;
    int dirtyLo_ = Size, dirtyHi_ = 0;
    std::uint32_t epoch_ = 1;
};

}  // namespace ide

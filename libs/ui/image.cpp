// SPDX-License-Identifier: GPL-3.0-or-later
// image - implementation of module daemon.ui.image.
module;
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include "stb_truetype.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
module daemon.ui.image;
import std;
import daemon.base.util;

namespace ide {

namespace {

constexpr long long kMaxPixels = 64LL * 1024 * 1024;   // refuse bigger pictures outright

// Area-average resize of a premultiplied RGBA image (separable box filter with fractional coverage).
Image resize(const Image& src, int dw, int dh) {
    dw = std::max(1, dw);
    dh = std::max(1, dh);
    std::vector<float> tmp(static_cast<std::size_t>(dw) * static_cast<std::size_t>(src.h) * 4);
    const double sx = static_cast<double>(src.w) / dw;
    for (int y = 0; y < src.h; ++y) {
        const std::uint8_t* row = &src.rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(src.w) * 4];
        for (int x = 0; x < dw; ++x) {
            const double a = x * sx, b = a + sx;
            double acc[4] = {0, 0, 0, 0};
            for (int i = static_cast<int>(a); i < std::min(src.w, static_cast<int>(std::ceil(b))); ++i) {
                const double wgt = std::min<double>(b, i + 1) - std::max<double>(a, i);
                for (int c = 0; c < 4; ++c) acc[c] += wgt * row[static_cast<std::size_t>(i) * 4 + static_cast<std::size_t>(c)];
            }
            for (int c = 0; c < 4; ++c) tmp[(static_cast<std::size_t>(y) * static_cast<std::size_t>(dw) + static_cast<std::size_t>(x)) * 4 + static_cast<std::size_t>(c)] = static_cast<float>(acc[c] / sx);
        }
    }
    Image out;
    out.w = dw; out.h = dh;
    out.rgba.resize(static_cast<std::size_t>(dw) * static_cast<std::size_t>(dh) * 4);
    const double sy = static_cast<double>(src.h) / dh;
    for (int y = 0; y < dh; ++y) {
        const double a = y * sy, b = a + sy;
        for (int x = 0; x < dw; ++x) {
            double acc[4] = {0, 0, 0, 0};
            for (int j = static_cast<int>(a); j < std::min(src.h, static_cast<int>(std::ceil(b))); ++j) {
                const double wgt = std::min<double>(b, j + 1) - std::max<double>(a, j);
                for (int c = 0; c < 4; ++c) acc[c] += wgt * tmp[(static_cast<std::size_t>(j) * static_cast<std::size_t>(dw) + static_cast<std::size_t>(x)) * 4 + static_cast<std::size_t>(c)];
            }
            for (int c = 0; c < 4; ++c)
                out.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(dw) + static_cast<std::size_t>(x)) * 4 + static_cast<std::size_t>(c)] =
                    static_cast<std::uint8_t>(std::clamp(acc[c] / sy + 0.5, 0.0, 255.0));
        }
    }
    return out;
}

void premultiply(Image& img) {
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        const unsigned a = img.rgba[i + 3];
        if (a == 255) continue;
        for (std::size_t c = 0; c < 3; ++c) img.rgba[i + c] = static_cast<std::uint8_t>((img.rgba[i + c] * a + 127) / 255);
    }
}

// ---------------------------------------------------------------------------- SVG text

struct SvgFont {
    std::mutex mu;
    std::string path;
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool tried = false, ok = false;
};
SvgFont& svgFont() { static SvgFont f; return f; }

bool ensureSvgFont() {
    SvgFont& f = svgFont();
    std::lock_guard lock(f.mu);
    if (f.tried) return f.ok;
    f.tried = true;
    if (f.path.empty()) return false;
    auto bytes = readFile(f.path);
    if (!bytes) return false;
    f.data.assign(bytes->begin(), bytes->end());
    const int off = stbtt_GetFontOffsetForIndex(f.data.data(), 0);
    f.ok = off >= 0 && stbtt_InitFont(&f.info, f.data.data(), off) != 0;
    return f.ok;
}

std::string xmlAttr(std::string_view tag, std::string_view name) {
    std::size_t p = 0;
    while ((p = tag.find(name, p)) != std::string_view::npos) {
        const bool boundary = p > 0 && (tag[p - 1] == ' ' || tag[p - 1] == '\n' || tag[p - 1] == '\t');
        std::size_t q = p + name.size();
        if (boundary && q < tag.size() && tag[q] == '=' && q + 1 < tag.size() && (tag[q + 1] == '"' || tag[q + 1] == '\'')) {
            const auto e = tag.find(tag[q + 1], q + 2);
            if (e != std::string_view::npos) return std::string(tag.substr(q + 2, e - q - 2));
        }
        p += name.size();
    }
    return {};
}

float parseNumber(const std::string& s, float def) {
    if (s.empty()) return def;
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    return end == s.c_str() ? def : v;
}

// scale(k) / scale(kx ky) -> k (translate and the rest are ignored)
float parseScale(const std::string& t) {
    const auto p = t.find("scale(");
    if (p == std::string::npos) return 1.0f;
    return parseNumber(t.substr(p + 6), 1.0f);
}

bool parseColor(const std::string& s, std::uint8_t rgb[3]) {
    if (s.size() >= 4 && s[0] == '#') {
        auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (std::tolower(c) >= 'a' && std::tolower(c) <= 'f') ? std::tolower(c) - 'a' + 10 : 0; };
        if (s.size() == 4) { for (int i = 0; i < 3; ++i) rgb[i] = static_cast<std::uint8_t>(hex(s[static_cast<std::size_t>(i) + 1]) * 17); return true; }
        if (s.size() >= 7) { for (int i = 0; i < 3; ++i) rgb[i] = static_cast<std::uint8_t>(hex(s[static_cast<std::size_t>(i) * 2 + 1]) * 16 + hex(s[static_cast<std::size_t>(i) * 2 + 2])); return true; }
    }
    if (s == "white") { rgb[0] = rgb[1] = rgb[2] = 255; return true; }
    if (s == "black") { rgb[0] = rgb[1] = rgb[2] = 0; return true; }
    return false;
}

struct SvgText {
    float x = 0, y = 0, size = 16, scale = 1, textLength = 0, opacity = 1;
    int anchor = 0;   // 0 start, 1 middle, 2 end
    std::uint8_t rgb[3] = {0, 0, 0};
    std::u32string text;
};

std::vector<SvgText> collectSvgText(const std::string& svg) {
    struct Group { std::map<std::string, std::string> a; };
    std::vector<Group> stack;
    std::vector<SvgText> out;
    for (std::size_t i = 0; i < svg.size();) {
        const auto lt = svg.find('<', i);
        if (lt == std::string::npos) break;
        const auto gt = svg.find('>', lt);
        if (gt == std::string::npos) break;
        const std::string_view tag(svg.data() + lt, gt - lt + 1);
        i = gt + 1;
        if (tag.starts_with("<g") && tag.size() > 2 && (tag[2] == ' ' || tag[2] == '>' || tag[2] == '\n')) {
            if (tag.size() >= 2 && tag[tag.size() - 2] == '/') continue;   // empty group
            Group g;
            for (const char* name : {"fill", "fill-opacity", "font-size", "text-anchor", "transform", "opacity"}) {
                std::string v = xmlAttr(tag, name);
                if (!v.empty()) g.a[name] = v;
            }
            stack.push_back(std::move(g));
        } else if (tag.starts_with("</g")) {
            if (!stack.empty()) stack.pop_back();
        } else if (tag.starts_with("<text") && tag.size() > 5 && (tag[5] == ' ' || tag[5] == '>' || tag[5] == '\n')) {
            const auto close = svg.find("</text>", gt);
            if (close == std::string::npos) break;
            std::string content;
            bool inTag = false;
            for (std::size_t k = gt + 1; k < close; ++k) {   // drop nested tags such as <tspan>
                if (svg[k] == '<') inTag = true;
                else if (svg[k] == '>') inTag = false;
                else if (!inTag) content += svg[k];
            }
            i = close + 7;
            for (const auto& [from, to] : std::initializer_list<std::pair<std::string_view, std::string_view>>{{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}}) {
                std::size_t p = 0;
                while ((p = content.find(from, p)) != std::string::npos) { content.replace(p, from.size(), to); p += to.size(); }
            }
            std::map<std::string, std::string> eff;
            float scale = 1;
            for (const auto& g : stack) {
                for (const auto& [k, v] : g.a) { if (k == "transform") scale *= parseScale(v); else eff[k] = v; }
            }
            for (const char* name : {"fill", "fill-opacity", "font-size", "text-anchor", "transform", "opacity"}) {
                std::string v = xmlAttr(tag, name);
                if (v.empty()) continue;
                if (std::string_view(name) == "transform") scale *= parseScale(v); else eff[name] = v;
            }
            SvgText t;
            t.x = parseNumber(xmlAttr(tag, "x"), 0);
            t.y = parseNumber(xmlAttr(tag, "y"), 0);
            t.size = parseNumber(eff["font-size"], 16);
            t.scale = scale;
            t.textLength = parseNumber(xmlAttr(tag, "textLength"), 0);
            t.opacity = parseNumber(eff["fill-opacity"], 1) * parseNumber(eff["opacity"], 1);
            const std::string anchor = eff["text-anchor"];
            t.anchor = anchor == "middle" ? 1 : anchor == "end" ? 2 : 0;
            if (const std::string fill = eff.count("fill") ? eff["fill"] : "#000"; fill == "none") continue;
            else parseColor(fill, t.rgb);
            for (std::size_t k = 0; k < content.size();) t.text += decodeUtf8(content, k);
            if (!t.text.empty() && t.opacity > 0) out.push_back(std::move(t));
        }
    }
    return out;
}

// Draws the <text> elements over the rasterised SVG (straight-alpha RGBA, `k` raster pixels per SVG unit).
void drawSvgText(Image& img, const std::vector<SvgText>& texts, float k) {
    SvgFont& f = svgFont();
    for (const SvgText& t : texts) {
        const float px = t.size * t.scale * k;
        if (px < 1) continue;
        const float s = stbtt_ScaleForMappingEmToPixels(&f.info, px);
        float adv = 0;
        for (std::size_t i = 0; i < t.text.size(); ++i) {
            int a = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&f.info, static_cast<int>(t.text[i]), &a, &lsb);
            adv += static_cast<float>(a) * s;
            if (i + 1 < t.text.size()) adv += static_cast<float>(stbtt_GetCodepointKernAdvance(&f.info, static_cast<int>(t.text[i]), static_cast<int>(t.text[i + 1]))) * s;
        }
        const float target = t.textLength > 0 ? t.textLength * t.scale * k : adv;
        const float stretch = adv > 0 ? target / adv : 1.0f;
        float pen = t.x * t.scale * k - (t.anchor == 1 ? target / 2 : t.anchor == 2 ? target : 0.0f);
        const float baseY = t.y * t.scale * k;
        for (std::size_t i = 0; i < t.text.size(); ++i) {
            const int cp = static_cast<int>(t.text[i]);
            int a = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&f.info, cp, &a, &lsb);
            int gw = 0, gh = 0, xo = 0, yo = 0;
            const float fx = pen - std::floor(pen);
            unsigned char* bmp = stbtt_GetCodepointBitmapSubpixel(&f.info, s, s, fx, 0, cp, &gw, &gh, &xo, &yo);
            if (bmp) {
                const int ox = static_cast<int>(std::floor(pen)) + xo, oy = static_cast<int>(std::lround(baseY)) + yo;
                for (int y = 0; y < gh; ++y)
                    for (int x = 0; x < gw; ++x) {
                        const int dx = ox + x, dy = oy + y;
                        if (dx < 0 || dy < 0 || dx >= img.w || dy >= img.h) continue;
                        const float sa = bmp[y * gw + x] / 255.0f * t.opacity;
                        if (sa <= 0) continue;
                        std::uint8_t* d = &img.rgba[(static_cast<std::size_t>(dy) * static_cast<std::size_t>(img.w) + static_cast<std::size_t>(dx)) * 4];
                        const float da = d[3] / 255.0f;
                        const float oa = sa + da * (1 - sa);
                        for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>(std::clamp((t.rgb[c] * sa + d[c] * da * (1 - sa)) / oa + 0.5f, 0.0f, 255.0f));
                        d[3] = static_cast<std::uint8_t>(std::clamp(oa * 255.0f + 0.5f, 0.0f, 255.0f));
                    }
                stbtt_FreeBitmap(bmp, nullptr);
            }
            pen += static_cast<float>(a) * s * stretch;
            if (i + 1 < t.text.size()) pen += static_cast<float>(stbtt_GetCodepointKernAdvance(&f.info, cp, static_cast<int>(t.text[i + 1]))) * s * stretch;
        }
    }
}

std::optional<Image> decodeSvg(std::span<const std::uint8_t> bytes, int maxSide, int& natW, int& natH, std::string& error) {
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::vector<SvgText> texts;
    if (text.find("<text") != std::string::npos) {
        if (!ensureSvgFont()) { error = "SVG with text needs a font"; return std::nullopt; }
        texts = collectSvgText(text);
    }
    NSVGimage* svg = nsvgParse(text.data(), "px", 96.0f);
    if (!svg) { error = "invalid SVG"; return std::nullopt; }
    const float w = svg->width, h = svg->height;
    if (!(w > 0.5f && h > 0.5f) || w > 100000 || h > 100000) { nsvgDelete(svg); error = "SVG has no size"; return std::nullopt; }
    natW = static_cast<int>(std::ceil(w));
    natH = static_cast<int>(std::ceil(h));
    // Rasterise at about twice the declared size so it stays crisp on HiDPI, within the cap.
    int tw = std::clamp(static_cast<int>(std::lround(w * 2)), 64, maxSide);
    int th = std::max(1, static_cast<int>(std::lround(h * tw / w)));
    if (th > maxSide) { th = maxSide; tw = std::max(1, static_cast<int>(std::lround(w * th / h))); }
    Image img;
    img.w = tw; img.h = th;
    img.rgba.assign(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 4, 0);
    NSVGrasterizer* r = nsvgCreateRasterizer();
    if (!r) { nsvgDelete(svg); error = "out of memory"; return std::nullopt; }
    nsvgRasterize(r, svg, 0, 0, static_cast<float>(tw) / w, img.rgba.data(), tw, th, tw * 4);   // straight alpha
    if (!texts.empty()) drawSvgText(img, texts, static_cast<float>(tw) / w);
    nsvgDeleteRasterizer(r);
    nsvgDelete(svg);
    return img;
}

}  // namespace

void setSvgTextFont(const std::string& path) {
    SvgFont& f = svgFont();
    std::lock_guard lock(f.mu);
    if (f.path == path) return;
    f.path = path;
    f.tried = false;
    f.ok = false;
}

std::optional<ImageLevels> decodeImage(std::span<const std::uint8_t> bytes, int maxSide, std::string& error) {
    if (bytes.size() < 8) { error = "empty file"; return std::nullopt; }
    ImageLevels out;
    Image base;
    const std::string_view head(reinterpret_cast<const char*>(bytes.data()), std::min<std::size_t>(bytes.size(), 512));
    if (head.find("<svg") != std::string_view::npos) {
        auto svg = decodeSvg(bytes, maxSide, out.naturalW, out.naturalH, error);
        if (!svg) return std::nullopt;
        base = std::move(*svg);
    } else {
        int w = 0, h = 0, comp = 0;
        if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp)) { error = "unknown image format"; return std::nullopt; }
        if (w <= 0 || h <= 0 || static_cast<long long>(w) * h > kMaxPixels) { error = "image too large"; return std::nullopt; }
        unsigned char* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
        if (!px) { error = stbi_failure_reason() ? stbi_failure_reason() : "decode failed"; return std::nullopt; }
        base.w = w; base.h = h;
        base.rgba.assign(px, px + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
        stbi_image_free(px);
        out.naturalW = w; out.naturalH = h;
    }
    premultiply(base);
    if (std::max(base.w, base.h) > maxSide) {
        const double k = static_cast<double>(maxSide) / std::max(base.w, base.h);
        base = resize(base, static_cast<int>(std::lround(base.w * k)), static_cast<int>(std::lround(base.h * k)));
    }
    out.levels.push_back(std::move(base));
    while (out.levels.size() < 6 && std::min(out.levels.back().w, out.levels.back().h) >= 96) {
        const Image& prev = out.levels.back();
        Image next = resize(prev, prev.w / 2, prev.h / 2);
        out.levels.push_back(std::move(next));
    }
    return out;
}

std::optional<ImageAtlas::Region> ImageAtlas::add(const Image& img) {
    const int pw = img.w + 2, ph = img.h + 2;
    if (img.w <= 0 || img.h <= 0 || pw > Size || ph > Size) return std::nullopt;
    if (shelfX_ + pw > Size) { shelfY_ += shelfH_; shelfX_ = 0; shelfH_ = 0; }
    if (shelfY_ + ph > Size) return std::nullopt;
    if (pixels_.empty()) pixels_.assign(static_cast<std::size_t>(Size) * Size * 4, 0);
    const int px = shelfX_, py = shelfY_;
    shelfX_ += pw;
    shelfH_ = std::max(shelfH_, ph);
    for (int y = -1; y <= img.h; ++y) {   // the image plus a replicated one-pixel border
        const int sy = std::clamp(y, 0, img.h - 1);
        std::uint8_t* dst = &pixels_[(static_cast<std::size_t>(py + 1 + y) * Size + static_cast<std::size_t>(px)) * 4];
        const std::uint8_t* src = &img.rgba[static_cast<std::size_t>(sy) * static_cast<std::size_t>(img.w) * 4];
        std::memcpy(dst + 4, src, static_cast<std::size_t>(img.w) * 4);
        std::memcpy(dst, src, 4);
        std::memcpy(dst + (static_cast<std::size_t>(img.w) + 1) * 4, src + (static_cast<std::size_t>(img.w) - 1) * 4, 4);
    }
    dirtyLo_ = std::min(dirtyLo_, py);
    dirtyHi_ = std::max(dirtyHi_, py + ph);
    const float inv = 1.0f / Size;
    return Region{static_cast<float>(px + 1) * inv, static_cast<float>(py + 1) * inv, static_cast<float>(px + 1 + img.w) * inv,
                  static_cast<float>(py + 1 + img.h) * inv, img.w, img.h};
}

void ImageAtlas::clear() {
    pixels_.clear();
    pixels_.shrink_to_fit();
    shelfX_ = shelfY_ = shelfH_ = 0;
    dirtyLo_ = Size; dirtyHi_ = 0;
    ++epoch_;
}

}  // namespace ide

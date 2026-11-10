// SPDX-License-Identifier: GPL-3.0-or-later
// icons - implementation of module daemon.ui.icons.
module daemon.ui.icons;
import std;
import daemon.ui.draw;
import daemon.ui.font;
namespace fs = std::filesystem;

namespace ide {

FileStyle fileStyle(std::string_view name) {
    std::string lowerName(name);
    std::ranges::transform(lowerName, lowerName.begin(), [](unsigned char c) { return std::tolower(c); });
    using K = FileKind;
    static const std::unordered_map<std::string, FileStyle> byName = {
        {"cmakelists.txt", {K::Build, 0x6aab73}}, {"makefile", {K::Build, 0xcf8e6d}}, {"dockerfile", {K::Build, 0x56a8f5}},
        {".gitignore", {K::Config, 0xe06c5a}},    {".gitattributes", {K::Config, 0xe06c5a}},
        {"license", {K::Doc, 0xd7ba7d}},          {"readme.md", {K::Doc, 0x8fb4ff}},
        {"cargo.toml", {K::Build, 0xde7a52}},     {"package.json", {K::Build, 0x7fc97f}}};
    if (auto it = byName.find(lowerName); it != byName.end()) return it->second;
    std::string ext = fs::path(lowerName).extension().string();
    if (!ext.empty()) ext.erase(0, 1);
    static const std::unordered_map<std::string, FileStyle> byExt = {
        {"cpp", {K::Code, 0x56a8f5}}, {"cc", {K::Code, 0x56a8f5}}, {"cxx", {K::Code, 0x56a8f5}}, {"cppm", {K::Code, 0x56a8f5}},
        {"ixx", {K::Code, 0x56a8f5}}, {"hpp", {K::Code, 0x9d8cf0}}, {"hh", {K::Code, 0x9d8cf0}}, {"h", {K::Code, 0x9d8cf0}},
        {"inl", {K::Code, 0x9d8cf0}}, {"c", {K::Code, 0x6b8fd6}},   {"py", {K::Code, 0xe8c35a}}, {"rs", {K::Code, 0xde7a52}},
        {"go", {K::Code, 0x56c2d6}},  {"java", {K::Code, 0xe0775a}}, {"kt", {K::Code, 0xb07cdc}}, {"cs", {K::Code, 0x7fc97f}},
        {"js", {K::Code, 0xf0db4f}},  {"mjs", {K::Code, 0xf0db4f}}, {"jsx", {K::Code, 0x61dafb}}, {"ts", {K::Code, 0x3d8fd6}},
        {"tsx", {K::Code, 0x3d8fd6}}, {"sh", {K::Code, 0x7fc97f}},   {"bash", {K::Code, 0x7fc97f}}, {"zsh", {K::Code, 0x7fc97f}},
        {"lua", {K::Code, 0x5d7fd6}}, {"html", {K::Code, 0xe5734f}}, {"css", {K::Code, 0x5aa0e0}},
        {"json", {K::Config, 0xcbb956}}, {"toml", {K::Config, 0xcf8e6d}}, {"yaml", {K::Config, 0xcf8e6d}},
        {"yml", {K::Config, 0xcf8e6d}},  {"ini", {K::Config, 0xcf8e6d}},  {"xml", {K::Config, 0xe39f5a}},
        {"conf", {K::Config, 0x9aa0ab}}, {"cfg", {K::Config, 0x9aa0ab}},
        {"md", {K::Doc, 0x8fb4ff}},  {"txt", {K::Doc, 0x9aa0ab}}, {"pdf", {K::Doc, 0xe0605a}}, {"log", {K::Doc, 0x7a8190}},
        {"png", {K::Image, 0xb48ead}}, {"jpg", {K::Image, 0xb48ead}}, {"jpeg", {K::Image, 0xb48ead}}, {"gif", {K::Image, 0xb48ead}},
        {"svg", {K::Image, 0xe5a45a}}, {"webp", {K::Image, 0xb48ead}}, {"ico", {K::Image, 0xb48ead}},
        {"cmake", {K::Build, 0x6aab73}},
        {"glsl", {K::Shader, 0xc586c0}}, {"vert", {K::Shader, 0xc586c0}}, {"frag", {K::Shader, 0xc586c0}},
        {"comp", {K::Shader, 0xc586c0}}, {"spv", {K::Shader, 0x8a8e98}},
        {"zip", {K::Archive, 0xc9a45a}}, {"gz", {K::Archive, 0xc9a45a}}, {"tar", {K::Archive, 0xc9a45a}},
        {"xz", {K::Archive, 0xc9a45a}},  {"7z", {K::Archive, 0xc9a45a}},
        {"o", {K::Binary, 0x7a8190}}, {"a", {K::Binary, 0x7a8190}}, {"so", {K::Binary, 0x7a8190}}, {"bin", {K::Binary, 0x7a8190}},
        {"ttf", {K::Binary, 0x9aa0ab}}, {"otf", {K::Binary, 0x9aa0ab}}};
    if (auto it = byExt.find(ext); it != byExt.end()) return it->second;
    return {};
}

std::uint32_t folderColor(std::string_view name) {
    std::string n(name);
    std::ranges::transform(n, n.begin(), [](unsigned char c) { return std::tolower(c); });
    static const std::unordered_map<std::string, std::uint32_t> map = {
        {"src", 0x6fa8f5}, {"source", 0x6fa8f5}, {"lib", 0x6fa8f5}, {"include", 0x6fa8f5}, {"app", 0x6fa8f5},
        {"test", 0x7bc47f}, {"tests", 0x7bc47f}, {"spec", 0x7bc47f},
        {"docs", 0x9d8cf0}, {"doc", 0x9d8cf0},
        {"build", 0x8a909c}, {"dist", 0x8a909c}, {"out", 0x8a909c}, {"target", 0x8a909c}, {"bin", 0x8a909c}, {"obj", 0x8a909c},
        {".git", 0xe0775a}, {".github", 0xe0775a},
        {"assets", 0xc586c0}, {"images", 0xc586c0}, {"img", 0xc586c0}, {"icons", 0xc586c0}, {"res", 0xc586c0},
        {"shaders", 0xc586c0},
        {"third_party", 0x7a8190}, {"vendor", 0x7a8190}, {"node_modules", 0x7a8190}, {"deps", 0x7a8190},
        {".vscode", 0x56b6c2}, {".config", 0x56b6c2}, {"config", 0x56b6c2},
        {"tools", 0xe8a65a}, {"scripts", 0xe8a65a}};
    if (auto it = map.find(n); it != map.end()) return it->second;
    if (n.starts_with("cmake-build")) return 0x8a909c;
    return 0xdcb56d;
}

}  // namespace ide

namespace ide::icon_detail {

bool inside(const Prim& p, float x, float y) {
    switch (p.kind) {
        case Prim::RRect: {
            float qx = std::abs(x - p.a) - (p.c - p.e), qy = std::abs(y - p.b) - (p.d - p.e);
            return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - p.e < 0;
        }
        case Prim::Circle: return std::hypot(x - p.a, y - p.b) < p.c;
        case Prim::Ring: return std::abs(std::hypot(x - p.a, y - p.b) - p.c) < p.d / 2;
        case Prim::Capsule: {
            const V a = p.pts[0], b = p.pts[1];
            float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
            float t = l2 > 0 ? std::clamp(((x - a.x) * dx + (y - a.y) * dy) / l2, 0.0f, 1.0f) : 0.0f;
            return std::hypot(x - (a.x + t * dx), y - (a.y + t * dy)) < p.e;
        }
        case Prim::Poly: {
            bool in = false;
            for (std::size_t i = 0, j = p.pts.size() - 1; i < p.pts.size(); j = i++) {
                const V &a = p.pts[i], &b = p.pts[j];
                if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) in = !in;
            }
            return in;
        }
    }
    return false;
}

void rasterize(const Layer& layer, int size, std::uint8_t* out) {
    constexpr int SS = 6;
    const float scale = 16.0f / static_cast<float>(size);
    for (int py = 0; py < size; ++py)
        for (int px = 0; px < size; ++px) {
            int hits = 0;
            for (int sy = 0; sy < SS; ++sy)
                for (int sx = 0; sx < SS; ++sx) {
                    const float x = (static_cast<float>(px) + (static_cast<float>(sx) + 0.5f) / SS) * scale;
                    const float y = (static_cast<float>(py) + (static_cast<float>(sy) + 0.5f) / SS) * scale;
                    bool v = false;
                    for (const Prim& p : layer)
                        if (inside(p, x, y)) v = !p.sub;
                    hits += v;
                }
            out[py * size + px] = static_cast<std::uint8_t>((hits * 255 + SS * SS / 2) / (SS * SS));
        }
}

Layer markLayer(FileKind kind) {
    const float t = 0.78f;  // stroke half-width
    Layer l;
    switch (kind) {
        case FileKind::Code:  // < >
            l = {cap(6.7f, 7.7f, 4.5f, 9.9f, t), cap(4.5f, 9.9f, 6.7f, 12.1f, t),
                 cap(9.3f, 7.7f, 11.5f, 9.9f, t), cap(11.5f, 9.9f, 9.3f, 12.1f, t)};
            break;
        case FileKind::Config:  // list with bullets
            l = {circ(5.0f, 8.6f, 0.95f), cap(7.4f, 8.6f, 11.2f, 8.6f, t * 0.9f),
                 circ(5.0f, 11.8f, 0.95f), cap(7.4f, 11.8f, 10.2f, 11.8f, t * 0.9f)};
            break;
        case FileKind::Doc:  // text lines
            l = {cap(4.8f, 8.3f, 11.2f, 8.3f, t * 0.85f), cap(4.8f, 10.5f, 11.2f, 10.5f, t * 0.85f),
                 cap(4.8f, 12.7f, 8.6f, 12.7f, t * 0.85f)};
            break;
        case FileKind::Image:  // mountains + sun
            l = {poly({{4.2f, 12.9f}, {7.0f, 8.4f}, {8.9f, 11.0f}, {10.0f, 9.5f}, {11.9f, 12.9f}}), circ(10.9f, 7.9f, 1.0f)};
            break;
        case FileKind::Build:  // cog
            l = {ring(8.0f, 10.2f, 2.5f, 1.6f), circ(8.0f, 10.2f, 0.9f), cap(8.0f, 6.6f, 8.0f, 7.5f, 0.7f),
                 cap(8.0f, 12.9f, 8.0f, 13.8f, 0.7f), cap(4.4f, 10.2f, 5.3f, 10.2f, 0.7f), cap(10.7f, 10.2f, 11.6f, 10.2f, 0.7f)};
            break;
        case FileKind::Shader:  // diamond
            l = {poly({{8.0f, 7.0f}, {11.4f, 10.2f}, {8.0f, 13.4f}, {4.6f, 10.2f}}),
                 poly({{8.0f, 8.9f}, {9.6f, 10.2f}, {8.0f, 11.5f}, {6.4f, 10.2f}}, true)};
            break;
        case FileKind::Archive:  // zipper
            l = {rr(8.0f, 7.4f, 0.8f, 0.7f, 0.2f),  rr(8.0f, 9.0f, 0.8f, 0.7f, 0.2f), rr(8.0f, 10.6f, 0.8f, 0.7f, 0.2f),
                 rr(8.0f, 12.6f, 1.7f, 1.2f, 0.5f), rr(8.0f, 12.6f, 0.6f, 0.5f, 0.2f, true)};
            break;
        case FileKind::Binary:  // 2x2 grid
            l = {rr(5.9f, 8.7f, 1.2f, 1.2f, 0.4f), rr(10.1f, 8.7f, 1.2f, 1.2f, 0.4f), rr(5.9f, 12.1f, 1.2f, 1.2f, 0.4f),
                 rr(10.1f, 12.1f, 1.2f, 1.2f, 0.4f)};
            break;
        case FileKind::Generic:
            l = {cap(4.8f, 9.6f, 11.2f, 9.6f, t * 0.85f), cap(4.8f, 11.9f, 8.4f, 11.9f, t * 0.85f)};
            break;
    }
    return l;
}

Layer filesLayer() {  // two overlapping pages
    Layer l;
    outlineRR(l, 6.4f, 6.4f, 4.4f, 4.4f, 1.3f, 1.3f);
    l.push_back(rr(10.2f, 10.2f, 4.8f, 4.8f, 1.5f, true));  // knock out where the pages overlap
    outlineRR(l, 9.8f, 9.8f, 4.4f, 4.4f, 1.3f, 1.3f);
    return l;
}

Layer terminalLayer() {
    Layer l;
    outlineRR(l, 8.0f, 8.0f, 6.8f, 5.6f, 1.8f, 1.3f);
    l.push_back(cap(4.6f, 6.4f, 7.0f, 8.2f, 0.7f));
    l.push_back(cap(7.0f, 8.2f, 4.6f, 10.0f, 0.7f));
    l.push_back(cap(8.4f, 10.4f, 11.2f, 10.4f, 0.7f));
    return l;
}

}  // namespace ide::icon_detail

namespace ide {

char32_t fileGlyph(std::string_view name, FileKind kind) {
    std::string n(name);
    std::ranges::transform(n, n.begin(), [](unsigned char c) { return std::tolower(c); });
    if (n == "dockerfile") return mdi::Docker;
    if (n.starts_with(".git")) return mdi::Git;
    if (n == "cmakelists.txt" || n == "makefile") return mdi::Hammer;
    std::string ext = fs::path(n).extension().string();
    if (!ext.empty()) ext.erase(0, 1);
    static const std::unordered_map<std::string, char32_t> byExt = {
        {"cpp", mdi::Cpp}, {"cc", mdi::Cpp}, {"cxx", mdi::Cpp}, {"cppm", mdi::Cpp}, {"ixx", mdi::Cpp}, {"hpp", mdi::Cpp},
        {"hh", mdi::Cpp}, {"inl", mdi::Cpp}, {"c", mdi::C}, {"h", mdi::C}, {"py", mdi::Python}, {"js", mdi::Js},
        {"mjs", mdi::Js}, {"jsx", mdi::Js}, {"ts", mdi::Ts}, {"tsx", mdi::Ts}, {"md", mdi::Markdown}, {"rs", mdi::Rust},
        {"go", mdi::Go}, {"java", mdi::Java}, {"html", mdi::Html}, {"css", mdi::Css}, {"lua", mdi::Lua}, {"cs", mdi::CSharp},
        {"kt", mdi::Kotlin}, {"json", mdi::Json}, {"sh", mdi::Bash}, {"bash", mdi::Bash}, {"zsh", mdi::Bash},
        {"xml", mdi::Xml}, {"cmake", mdi::Hammer}, {"svg", mdi::Image}, {"log", mdi::Script}, {"txt", mdi::Doc}};
    if (auto it = byExt.find(ext); it != byExt.end()) return it->second;
    switch (kind) {
        case FileKind::Code: return mdi::Code;
        case FileKind::Config: return mdi::Cog;
        case FileKind::Doc: return mdi::Doc;
        case FileKind::Image: return mdi::Image;
        case FileKind::Build: return mdi::Hammer;
        case FileKind::Shader: return mdi::Code;
        case FileKind::Archive: return mdi::Zip;
        case FileKind::Binary: return mdi::Cube;
        default: return mdi::File;
    }
}

void Icons::file(DrawList& dl, FontAtlas& font, float x, float y, int size, std::string_view name, float alpha) {
    using namespace icon_detail;
    const FileStyle st = fileStyle(name);
    const Color c = Color::rgb(st.color);
    const auto a = [&](Color col) { return col.alpha(static_cast<std::uint8_t>(static_cast<float>(col.a) * alpha)); };
    if (font.hasIcons()) { glyph(dl, font, x, y, size, fileGlyph(name, st.kind), a(c)); return; }
    layer(dl, font, 1, 0, size, x, y, a(c), pageLayer);
    layer(dl, font, 2, 0, size, x, y, a(c.mix(Color::rgb(0xffffff), 0.38f)), foldLayer);
    layer(dl, font, 10 + static_cast<std::uint32_t>(st.kind), 0, size, x, y, a(Color{0x14, 0x16, 0x1a, 215}),
          [&] { return markLayer(st.kind); });
}

void Icons::folder(DrawList& dl, FontAtlas& font, float x, float y, int size, std::string_view name, bool open,
                   float alpha) {
    using namespace icon_detail;
    const Color c = Color::rgb(folderColor(name));
    const auto a = [&](Color col) { return col.alpha(static_cast<std::uint8_t>(static_cast<float>(col.a) * alpha)); };
    if (font.hasIcons()) { glyph(dl, font, x, y, size, open ? mdi::FolderOpen : mdi::Folder, a(c)); return; }
    layer(dl, font, 30, 0, size, x, y, a(c.mix(Color::rgb(0x000000), 0.30f)), folderBack);
    if (open) layer(dl, font, 32, 0, size, x, y, a(c.mix(Color::rgb(0xffffff), 0.14f)), folderOpenFront);
    else layer(dl, font, 31, 0, size, x, y, a(c), folderFront);
}

void Icons::tool(DrawList& dl, FontAtlas& font, float x, float y, int size, ToolIcon icon, Color color) {
    using namespace icon_detail;
    if (font.hasIcons()) {
        static constexpr char32_t map[] = {mdi::Play, mdi::Stop, mdi::Hammer, mdi::Refresh, mdi::DockBottom, mdi::FileTree,
                                           mdi::ConsoleLine, mdi::Alert, mdi::Minimize, mdi::Maximize, mdi::Close};
        glyph(dl, font, x, y, size, map[static_cast<std::size_t>(icon)], color);
        return;
    }
    const auto id = 50 + static_cast<std::uint32_t>(icon);
    layer(dl, font, id, 0, size, x, y, color, [&] {
        switch (icon) {
            case ToolIcon::Play: return playLayer();
            case ToolIcon::Stop: return stopLayer();
            case ToolIcon::Build: return hammerLayer();
            case ToolIcon::Reload: return reloadLayer();
            case ToolIcon::Panel: return panelLayer();
            case ToolIcon::Files: return filesLayer();
            case ToolIcon::Terminal: return terminalLayer();
            case ToolIcon::Warning: return warningLayer();
            case ToolIcon::Minimize: return minimizeLayer();
            case ToolIcon::Maximize: return maximizeLayer();
            case ToolIcon::Close: return closeLayer();
        }
        return Layer{};
    });
}

}  // namespace ide

// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the Markdown parser.
import std;
import daemon.text.markdown;
import daemon.text.buffer;
import daemon.ui.font;
import daemon.app.mdview;
import daemon.ui.image;
import daemon.base.util;
#include "check.hpp"

using namespace ide;

static bool has(const std::vector<md::Span>& v, std::string_view text, std::uint8_t style, std::string_view url = {}) {
    return std::ranges::any_of(v, [&](const md::Span& s) { return s.text == text && s.style == style && s.url == url && s.src.empty(); });
}

void testMarkdown() {
    {   // headings
        auto b = md::parse("# Title\n\n## Sub ##\n\nSetext\n======\n\n####### not a heading\n");
        CHECK(b.size() == 4);
        CHECK(b[0].kind == md::Kind::Heading && b[0].level == 1 && md::plainText(b[0].spans) == "Title");
        CHECK(b[1].level == 2 && md::plainText(b[1].spans) == "Sub");
        CHECK(b[2].kind == md::Kind::Heading && b[2].level == 1 && md::plainText(b[2].spans) == "Setext");
        CHECK(b[3].kind == md::Kind::Paragraph);
        CHECK(md::slug("Hello, World! v2") == "hello-world-v2");
    }
    {   // emphasis
        auto b = md::parse("plain **bold** and *it* and ***both*** ~~gone~~ `code` snake_case_word 2*3*4");
        CHECK(b.size() == 1);
        const auto& s = b[0].spans;
        CHECK(has(s, "bold", md::Bold));
        CHECK(has(s, "it", md::Italic));
        CHECK(has(s, "both", md::Bold | md::Italic));
        CHECK(has(s, "gone", md::Strike));
        CHECK(has(s, "code", md::Code));
        CHECK(md::plainText(s).find("snake_case_word") != std::string::npos);
    }
    {   // links, images, autolinks, references
        auto b = md::parse("[site](https://x.org \"t\") ![logo](a.png) <https://y.org> https://z.org/p.\n\n[ref][r] and [r]\n\n[r]: https://r.org\n");
        CHECK(b.size() == 2);
        const auto& s = b[0].spans;
        CHECK(has(s, "site", 0, "https://x.org"));
        CHECK(std::ranges::any_of(s, [](const md::Span& x) { return x.src == "a.png" && x.text == "logo"; }));
        CHECK(has(s, "https://y.org", 0, "https://y.org"));
        CHECK(has(s, "https://z.org/p", 0, "https://z.org/p"));
        CHECK(has(b[1].spans, "ref", 0, "https://r.org"));
        CHECK(has(b[1].spans, "r", 0, "https://r.org"));
    }
    {   // badge: image inside a link
        auto b = md::parse("[![CI](https://ci/badge.svg)](https://ci)");
        CHECK(b.size() == 1 && b[0].spans.size() == 1);
        CHECK(b[0].spans[0].src == "https://ci/badge.svg" && b[0].spans[0].url == "https://ci");
    }
    {   // code
        auto b = md::parse("```cpp\nint x;\n  y();\n```\n\n    indented\n    code\n\n~~~\nraw ``` inside\n~~~\n");
        CHECK(b.size() == 3);
        CHECK(b[0].kind == md::Kind::Code && b[0].lang == "cpp" && b[0].text == "int x;\n  y();");
        CHECK(b[1].kind == md::Kind::Code && b[1].text == "indented\ncode");
        CHECK(b[2].text == "raw ``` inside");
    }
    {   // lists
        auto b = md::parse("- a\n- b\n  - nested\n  - nested2\n- [x] done\n- [ ] todo\n\n3. three\n4. four\n");
        CHECK(b.size() == 2);
        CHECK(b[0].kind == md::Kind::List && !b[0].ordered && b[0].kids.size() == 4);
        CHECK(b[0].kids[1].kids.size() == 2 && b[0].kids[1].kids[1].kind == md::Kind::List);
        CHECK(b[0].kids[1].kids[1].kids.size() == 2);
        CHECK(b[0].kids[2].checked == 1 && b[0].kids[3].checked == 0);
        CHECK(b[1].ordered && b[1].start == 3 && b[1].kids.size() == 2);
    }
    {   // quotes and rules
        auto b = md::parse("> quoted\n> text\n>\n> > deeper\n\n---\n\n***\n");
        CHECK(b.size() == 3);
        CHECK(b[0].kind == md::Kind::Quote && b[0].kids.size() == 2 && b[0].kids[1].kind == md::Kind::Quote);
        CHECK(b[1].kind == md::Kind::Rule && b[2].kind == md::Kind::Rule);
    }
    {   // tables
        auto b = md::parse("| A | B |\n|:--|--:|\n| 1 | `x|y` |\n| 2 | 3 |\n\nafter");
        CHECK(b.size() == 2);
        CHECK(b[0].kind == md::Kind::Table && b[0].rows.size() == 3 && b[0].align.size() == 2);
        CHECK(b[0].align[0] == 0 && b[0].align[1] == 2);
        CHECK(b[1].kind == md::Kind::Paragraph);
    }
    {   // html
        auto b = md::parse("<div align=\"center\">\n<img src=\"logo.svg\" width=\"100\" alt=\"Logo\">\n\n# Name\n</div>\n\nline<br>break <b>bold</b> &amp; &copy;\n\n<!-- hidden -->\n");
        CHECK(b.size() == 3);
        CHECK(!b[0].spans.empty() && std::ranges::any_of(b[0].spans, [](const md::Span& x) { return x.src == "logo.svg" && x.text == "Logo"; }));
        CHECK(b[1].kind == md::Kind::Heading);
        CHECK(std::ranges::any_of(b[2].spans, [](const md::Span& x) { return x.brk; }));
        CHECK(has(b[2].spans, "bold", md::Bold));
        CHECK(md::plainText(b[2].spans).find("& \xC2\xA9") != std::string::npos);
    }
    {   // escapes and hard breaks
        auto b = md::parse("a \\*not em\\* and\\\nnext  \nline");
        CHECK(b.size() == 1);
        CHECK(md::plainText(b[0].spans).find("*not em*") != std::string::npos);
        CHECK(std::ranges::count_if(b[0].spans, [](const md::Span& x) { return x.brk; }) == 2);
    }
    {   // robustness: nothing may crash or hang on odd input
        for (std::string_view src : {"", "\n\n", "[", "![", "[a](", "**", "`", "<", "&", "| a |\n|", "- ", "1.", ">", "#", "```", "[a]: ", "* * *", "- - -\n-",
                                     "<a href=\"x\">unclosed", "***a**b*c", "_ a _", "a**b c**d", "~~~~~~~~~~~~~~~~~~", "\t\t\tx"}) {
            auto b = md::parse(src);
            (void)b;
        }
        std::string big;
        for (int i = 0; i < 2000; ++i) big += "- item *x* [l](u) `c`\n  - sub\n";
        CHECK(md::parse(big).size() == 1);
    }
}

void testMarkdownLayout() {
    FontAtlas atlas;
    if (!atlas.load()) return;   // no system fonts (headless CI): nothing to measure with
    atlas.setSizes(15, 14);
    const std::string src = "# Title\n\nSome *text* with a [link](https://x.org) and `code` that has to wrap over several lines "
                            "when the viewport is narrow enough to force it.\n\n- one\n- two\n\n| A | B |\n|---|---|\n| 1 | 2 |\n\n```cpp\nint x;\n```\n\n## Sub\n";
    auto blocks = md::parse(src);
    auto wide = mdLayout(blocks, atlas, 1000, 1.0f);
    auto narrow = mdLayout(blocks, atlas, 320, 1.0f);
    CHECK(!wide.items.empty() && wide.height > 100);
    CHECK(narrow.height > wide.height);   // wrapping makes it taller
    CHECK(wide.links.size() == 1 && wide.links[0].url == "https://x.org");
    CHECK(wide.anchors.size() == 2 && wide.anchors[0].first == "title" && wide.anchors[1].first == "sub");
    CHECK(wide.anchors[1].second > wide.anchors[0].second);
    for (const auto& l : narrow.links) CHECK(l.r.w > 0 && l.r.h > 0);
    CHECK(mdLayout({}, atlas, 500, 1.0f).items.empty());
    CHECK(mdLayout(blocks, atlas, 10, 1.0f).height > 0);   // absurdly narrow must not loop or crash
}

void testReadOnlyBuffer() {
    Buffer b;
    b.replace({0, 0}, {0, 0}, "abc", {}, false);
    b.readOnly = true;
    b.replace({0, 0}, {0, 0}, "zzz", {}, false);
    CHECK(b.line(0) == "abc");
    CHECK(!b.undo().has_value());
    b.readOnly = false;
    b.replace({0, 0}, {0, 0}, "x", {}, false);
    CHECK(b.line(0) == "xabc");
}

void testImages() {
    std::string err;
    {   // PNG round trip through our own writer: 2x2 with one transparent pixel
        std::vector<std::uint8_t> px = {255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 255,   10, 20, 30, 0};
        const auto path = std::filesystem::temp_directory_path() / "daemon_test_image.png";
        CHECK(writePng(path, 2, 2, px));
        auto bytes = readFile(path);
        CHECK(bytes.has_value());
        if (bytes) {
            auto img = decodeImage({reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size()}, 1400, err);
            CHECK(img.has_value());
            if (img) {
                CHECK(img->naturalW == 2 && img->naturalH == 2 && img->levels.size() == 1);
                CHECK(img->levels[0].rgba[0] == 255 && img->levels[0].rgba[3] == 255);   // red, opaque
                CHECK(img->levels[0].rgba[15] == 0);                                      // transparent
            }
        }
        std::filesystem::remove(path);
    }
    {   // SVG shapes
        const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"20\"><rect width=\"40\" height=\"20\" fill=\"#ff0000\"/></svg>";
        auto img = decodeImage({reinterpret_cast<const std::uint8_t*>(svg.data()), svg.size()}, 1400, err);
        CHECK(img.has_value());
        if (img) {
            CHECK(img->naturalW == 40 && img->naturalH == 20);
            CHECK(img->levels[0].w == 80 && img->levels[0].h == 40);   // drawn at twice the size for HiDPI
            const auto& px = img->levels[0].rgba;
            CHECK(px[(10 * 80 + 10) * 4] == 255 && px[(10 * 80 + 10) * 4 + 3] == 255);
        }
    }
    {   // junk and absurd input must fail cleanly
        const std::string junk = "this is not an image at all, just some text of reasonable length";
        CHECK(!decodeImage({reinterpret_cast<const std::uint8_t*>(junk.data()), junk.size()}, 1400, err).has_value());
        CHECK(!err.empty());
        CHECK(!decodeImage({}, 1400, err).has_value());
        std::string bomb = "\x89PNG\r\n\x1a\n\0\0\0\rIHDR\x7f\xff\xff\xff\x7f\xff\xff\xff\x08\x06\0\0\0";
        CHECK(!decodeImage({reinterpret_cast<const std::uint8_t*>(bomb.data()), bomb.size()}, 1400, err).has_value());
    }
    {   // atlas packing
        ImageAtlas atlas;
        Image a;
        a.w = 100; a.h = 50; a.rgba.assign(100 * 50 * 4, 255);
        auto r1 = atlas.add(a), r2 = atlas.add(a);
        CHECK(r1 && r2);
        if (r1 && r2) {
            CHECK(r1->u0 >= 0 && r1->u1 <= 1 && r1->v1 <= 1);
            CHECK(r2->u0 > r1->u1);   // next one sits to the right on the same shelf
        }
        int y0 = 0, y1 = 0;
        CHECK(atlas.takeDirty(y0, y1) && y1 > y0);
        CHECK(!atlas.takeDirty(y0, y1));
        Image huge;
        huge.w = 3000; huge.h = 10; huge.rgba.assign(3000 * 10 * 4, 0);
        CHECK(!atlas.add(huge).has_value());
        const auto epoch = atlas.epoch();
        atlas.clear();
        CHECK(atlas.empty() && atlas.epoch() != epoch);
    }
}

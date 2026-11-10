// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the ui libraries.
#include <cstdlib>
#include <poll.h>
import std;
import daemon.base.util;
import daemon.text.buffer;
import daemon.text.syntax;
import daemon.text.document;
import daemon.services.explorer;
import daemon.ui.icons;
import daemon.base.json;
import daemon.base.process;
import daemon.services.cmake;
import daemon.services.project;
import daemon.services.git;
import daemon.services.search;
import daemon.services.gdb;
import daemon.services.terminal;
import daemon.services.lsp;
import daemon.ui.font;
import daemon.ui.draw;
#include "check.hpp"

using namespace ide;

static void testIcons() {
    using namespace ide::icon_detail;
    // A rounded rect fills most of the grid; its corners and an empty layer stay clear.
    std::array<std::uint8_t, 18 * 18> px{};
    rasterize({rr(8, 8, 7, 7, 2)}, 18, px.data());
    CHECK(px[9 * 18 + 9] == 255);   // centre fully covered
    CHECK(px[0] < 40);              // rounded corner nearly empty
    int edge = 0;
    for (auto v : px) edge += (v > 0 && v < 255);
    CHECK(edge > 20);               // anti-aliased edge pixels exist
    // Subtraction: a hole in the middle.
    rasterize({rr(8, 8, 7, 7, 1), circ(8, 8, 3, true)}, 18, px.data());
    CHECK(px[9 * 18 + 9] == 0 && px[2 * 18 + 2] == 255);
    // The page icon rasterises into the atlas without overflowing and is cached.
    FontAtlas atlas;
    if (atlas.load()) {
        DrawList dl;
        Icons::file(dl, atlas, 0, 0, 18, "main.cpp");
        Icons::folder(dl, atlas, 0, 0, 18, "src", true);
        Icons::chevron(dl, atlas, 0, 0, 12, true, Color::rgb(0xffffff));
        const std::size_t perFile = atlas.hasIcons() ? 1 : 3;  // one font glyph, or three vector layers
        CHECK(dl.size() == perFile + (atlas.hasIcons() ? 1 : 2) + 1);
        std::size_t before = dl.size();
        Icons::file(dl, atlas, 0, 0, 18, "main.cpp");
        CHECK(dl.size() == before + perFile);
    }
}

void runUiTests() {
    testIcons();
}

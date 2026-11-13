// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.text.markdown — a small CommonMark / GitHub-flavoured Markdown parser for the preview.
//
// Supported: ATX and setext headings, paragraphs, block quotes, bullet / ordered / task lists (nested), fenced and
// indented code, thematic breaks, pipe tables with alignment, reference definitions, and the inline forms emphasis,
// strong, strikethrough, code spans, links, images (as alt-text chips), autolinks, escapes, entities, hard breaks and
// the handful of HTML tags READMEs use (<br>, <img>, <a>, <b>/<strong>, <i>/<em>, <code>, <kbd>, <del>). Everything else
// in raw HTML is dropped, its text kept.
export module daemon.text.markdown;
import std;

export namespace ide::md {

enum : std::uint8_t { Bold = 1, Italic = 2, Code = 4, Strike = 8 };

struct Span {
    std::string text;           // visible text; for an image its alt text
    std::uint8_t style = 0;
    std::string url;            // link target, empty when the span is not a link
    std::string src;            // non-empty: an image with this source
    bool brk = false;           // hard line break (text is empty)
};

enum class Kind { Paragraph, Heading, Code, Quote, List, Item, Rule, Table };

struct Block {
    Kind kind = Kind::Paragraph;
    int level = 0;              // heading level 1..6
    bool ordered = false;       // list
    int start = 1;              // ordered list: number of the first item
    int checked = -1;           // item: task list state (-1 none, 0 open, 1 done)
    std::vector<Span> spans;    // paragraph / heading content
    std::string text, lang;     // code block
    std::vector<Block> kids;    // quote content, list items, item content
    std::vector<std::vector<std::vector<Span>>> rows;   // table: rows of cells of spans; row 0 is the header
    std::vector<int> align;     // table columns: 0 left, 1 centre, 2 right
};

std::vector<Block> parse(std::string_view source);

// The text of the spans without styling (used for heading anchors and tooltips).
std::string plainText(const std::vector<Span>& spans);

// GitHub-style anchor of a heading: lower case, punctuation removed, spaces become '-'.
std::string slug(std::string_view heading);

}  // namespace ide::md

// daemon.app.theme - the IDE colour theme (colours of the islands, text and syntax).
export module daemon.app.theme;
import std;
import daemon.ui.draw;
import daemon.text.syntax;

export namespace ide {

// "Island" theme: a dark frame with floating rounded panels. Cool neutrals and a blue accent
// (Visual Studio) with warm keywords and soft pastel syntax colours (JetBrains).
namespace theme {
inline constexpr Color frame = Color::rgb(0x111218, 214);    // translucent chrome between islands (blurred by the compositor)
inline constexpr Color editorBg = Color::rgb(0x0a0b0d);     // code editor: near-black
inline constexpr Color bg = Color::rgb(0x1a1b1f);           // island (side panels) surface
inline constexpr Color lineHl = Color::rgb(0x15171b);
inline constexpr Color selection = Color::rgb(0x4d171c);
inline constexpr Color selectionIdle = Color::rgb(0x34363d);
inline constexpr Color text = Color::rgb(0xd1d4da);
inline constexpr Color lineNum = Color::rgb(0x565962);
inline constexpr Color lineNumActive = Color::rgb(0xaeb2bb);
inline constexpr Color guide = Color::rgb(0x1f2126);
inline constexpr Color caret = Color::rgb(0xe0454d);
inline constexpr Color titleBg = frame;
inline constexpr Color tabBg = Color::rgb(0x121316);
inline constexpr Color stripBg = Color::rgb(0x060709);      // editor tab strip: darker than the editor
inline constexpr Color sideBg = Color::rgb(0x0d0e11);       // explorer island
inline constexpr Color sideSel = Color::rgb(0x1a1c21);
inline constexpr Color sideHover = Color::rgb(0x121317);
inline constexpr Color panelBg = Color::rgb(0x0b0c0f);      // output panel island
inline constexpr Color panelHead = Color::rgb(0x07080a);        // tab strip inside the island
inline constexpr Color tabHover = Color::rgb(0x27292e);
inline constexpr Color tabActive = Color::rgb(0x2f3138);
inline constexpr Color statusBg = frame;
inline constexpr Color promptBg = Color::rgb(0x26282d);
inline constexpr Color border = Color::rgb(0x2e3036);
inline constexpr Color accent = Color::rgb(0xb01e23);   // Targaryen / Caraxes crimson
inline constexpr Color dim = Color::rgb(0x8a8e98);
inline constexpr Color match = Color::rgb(0x5a4a22);
inline constexpr Color bracket = Color::rgb(0x7d8494);
inline constexpr Color danger = Color::rgb(0xe5484d);
inline constexpr Color scrollbar = Color::rgb(0x7a8090, 110);

inline constexpr Color syntax(Tok t) {
    switch (t) {
        case Tok::Keyword: return Color::rgb(0xcf8e6d);   // warm orange
        case Tok::Type: return Color::rgb(0x4ec9b0);      // teal
        case Tok::String: return Color::rgb(0x6aab73);    // soft green
        case Tok::Number: return Color::rgb(0xb5cea8);    // pale green
        case Tok::Comment: return Color::rgb(0x6c7280);
        case Tok::Preproc: return Color::rgb(0xc586c0);   // magenta
        case Tok::Function: return Color::rgb(0x56a8f5);  // blue
        case Tok::Operator: return text;
        case Tok::Constant: return Color::rgb(0xc77dbb);  // violet
        case Tok::Key: return Color::rgb(0x7fb7ff);
        default: return text;
    }
}
}  // namespace theme

}  // namespace ide

// ide.input — platform-independent input event types.
export module ide.input;
import std;

export namespace ide {

enum class Key : std::uint8_t {
    None, Char, Left, Right, Up, Down, Home, End, PageUp, PageDown,
    Backspace, Delete, Enter, Tab, Escape, Insert, F1, F2, F3, F4, F5,
    F6, F7, F8, F9, F10, F11, F12,  // keep >= F6 contiguous: the app treats them as global shortcuts
};

struct KeyEvent {
    Key key = Key::None;
    char32_t ch = 0;   // for Key::Char: the lower-cased character of the key (for shortcuts)
    std::string text;  // text to insert (UTF-8); empty when ctrl/alt/super are held
    bool ctrl = false, shift = false, alt = false, super = false;
    bool repeat = false;
};

enum class Button : std::uint8_t { Left, Middle, Right };

enum class Cursor : std::uint8_t {
    Default, Text, ResizeN, ResizeS, ResizeW, ResizeE, ResizeNW, ResizeNE, ResizeSW, ResizeSE,
};

// Values match xdg_toplevel.resize_edge.
enum class Edge : std::uint8_t {
    None = 0, Top = 1, Bottom = 2, Left = 4, TopLeft = 5, BottomLeft = 6, Right = 8, TopRight = 9, BottomRight = 10,
};

}  // namespace ide

// daemon.util — UTF-8 helpers, file I/O and a tiny PNG writer (used for --screenshot).
export module daemon.base.util;
import std;

export namespace ide {

// ---------------------------------------------------------------- UTF-8

constexpr char32_t kReplacementChar = 0xFFFD;

// Decodes one code point starting at s[i] and advances i. Invalid input yields U+FFFD
// and consumes a single byte, so iteration always terminates.
char32_t decodeUtf8(std::string_view s, std::size_t& i);

void appendUtf8(std::string& out, char32_t c);

// Byte index of the code point that follows the one starting at i.
inline std::size_t utf8Next(std::string_view s, std::size_t i) {
    if (i >= s.size()) return s.size();
    decodeUtf8(s, i);
    return i;
}

// Byte index of the code point that precedes position i.
inline std::size_t utf8Prev(std::string_view s, std::size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

inline std::size_t utf8Length(std::string_view s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ++n) decodeUtf8(s, i);
    return n;
}

// Display column (in cells) of byte offset `byteCol`, expanding tabs.
int displayCol(std::string_view line, int byteCol, int tabWidth);

// Byte offset whose display column is closest to `dcol` (snapping to a boundary).
int byteColAt(std::string_view line, int dcol, int tabWidth);

// ---------------------------------------------------------------- Files

std::optional<std::string> readFile(const std::filesystem::path& p);

bool writeFile(const std::filesystem::path& p, std::string_view data);

// ---------------------------------------------------------------- PNG (stored deflate)

std::uint32_t crc32(std::uint32_t crc, const std::uint8_t* d, std::size_t n);

// Writes an 8-bit RGBA image. Uses uncompressed deflate blocks: big files, zero dependencies.
bool writePng(const std::filesystem::path& path, std::uint32_t w, std::uint32_t h,
                     std::span<const std::uint8_t> rgba);

}  // namespace ide

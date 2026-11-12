// SPDX-License-Identifier: GPL-3.0-or-later
// util - implementation of module daemon.base.util.
module daemon.base.util;
import std;

namespace ide {

void logLine(std::string_view msg) {
    static std::mutex mu;
    static std::ofstream file;
    static bool opened = false;
    std::lock_guard lock(mu);
    if (!opened) {
        opened = true;
        namespace fs = std::filesystem;
        fs::path dir;
        if (const char* x = std::getenv("XDG_STATE_HOME"); x && *x) dir = x;
        else if (const char* h = std::getenv("HOME"); h && *h) dir = fs::path(h) / ".local" / "state";
        if (!dir.empty()) {
            dir /= "daemon";
            std::error_code ec;
            fs::create_directories(dir, ec);
            auto path = dir / "daemon.log";
            auto size = fs::file_size(path, ec);
            file.open(path, !ec && size > 512 * 1024 ? std::ios::trunc : std::ios::app);
        }
    }
    auto now = std::chrono::system_clock::now();
    std::string line = std::format("{:%F %T} {}\n", std::chrono::floor<std::chrono::milliseconds>(now), msg);
    if (file.is_open()) { file << line; file.flush(); }
    std::cerr << line;
}

char32_t decodeUtf8(std::string_view s, std::size_t& i) {
    auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    auto cont = [&](std::size_t k) { return k < s.size() && (byte(k) & 0xC0) == 0x80; };
    const unsigned b = byte(i);
    if (b < 0x80) { ++i; return b; }
    if (b >= 0xC2 && b <= 0xDF && cont(i + 1)) {
        char32_t c = ((b & 0x1Fu) << 6) | (byte(i + 1) & 0x3Fu);
        i += 2;
        return c;
    }
    if (b >= 0xE0 && b <= 0xEF && cont(i + 1) && cont(i + 2)) {
        char32_t c = ((b & 0x0Fu) << 12) | ((byte(i + 1) & 0x3Fu) << 6) | (byte(i + 2) & 0x3Fu);
        if (c >= 0x800 && !(c >= 0xD800 && c <= 0xDFFF)) { i += 3; return c; }
    }
    if (b >= 0xF0 && b <= 0xF4 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
        char32_t c = ((b & 0x07u) << 18) | ((byte(i + 1) & 0x3Fu) << 12) |
                     ((byte(i + 2) & 0x3Fu) << 6) | (byte(i + 3) & 0x3Fu);
        if (c >= 0x10000 && c <= 0x10FFFF) { i += 4; return c; }
    }
    ++i;
    return kReplacementChar;
}

void appendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

int displayCol(std::string_view line, int byteCol, int tabWidth) {
    int col = 0;
    std::size_t i = 0;
    const std::size_t end = std::min<std::size_t>(byteCol, line.size());
    while (i < end) {
        char32_t c = decodeUtf8(line, i);
        col += (c == '\t') ? tabWidth - col % tabWidth : 1;
    }
    return col;
}

int byteColAt(std::string_view line, int dcol, int tabWidth) {
    int col = 0;
    std::size_t i = 0;
    while (i < line.size()) {
        std::size_t start = i;
        char32_t c = decodeUtf8(line, i);
        int next = col + ((c == '\t') ? tabWidth - col % tabWidth : 1);
        if (dcol < next) {
            // Choose the nearer boundary.
            return (dcol - col) * 2 >= (next - col) ? static_cast<int>(i) : static_cast<int>(start);
        }
        col = next;
    }
    return static_cast<int>(line.size());
}

std::optional<std::string> readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (f.bad()) return std::nullopt;
    return data;
}

bool writeFile(const std::filesystem::path& p, std::string_view data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
    return static_cast<bool>(f);
}

std::uint32_t crc32(std::uint32_t crc, const std::uint8_t* d, std::size_t n) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool writePng(const std::filesystem::path& path, std::uint32_t w, std::uint32_t h,
                     std::span<const std::uint8_t> rgba) {
    if (rgba.size() < static_cast<std::size_t>(w) * h * 4) return false;
    std::vector<std::uint8_t> raw;
    raw.reserve((static_cast<std::size_t>(w) * 4 + 1) * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        auto row = rgba.subspan(static_cast<std::size_t>(y) * w * 4, static_cast<std::size_t>(w) * 4);
        raw.insert(raw.end(), row.begin(), row.end());
    }

    std::vector<std::uint8_t> z{0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (std::size_t pos = 0; pos < raw.size() || pos == 0;) {
        std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(n & 0xFF); z.push_back(n >> 8);
        z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    std::uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back((adler >> s) & 0xFF);

    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto be32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        for (int s = 24; s >= 0; s -= 8) v.push_back((x >> s) & 0xFF);
    };
    auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        be32(out, static_cast<std::uint32_t>(data.size()));
        std::vector<std::uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        out.insert(out.end(), body.begin(), body.end());
        be32(out, crc32(0, body.data(), body.size()));
    };
    std::vector<std::uint8_t> ihdr;
    be32(ihdr, w); be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

}  // namespace ide

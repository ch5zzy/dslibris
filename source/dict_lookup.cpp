#include "dict_lookup.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace {

static const uint32_t kLemMaxCheckpoints = 2048;
static const uint32_t kLemMinBlockBytes = 4096;
static const size_t   kLemScanBufSize = 8192;

static bool IsRegularFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static bool IsDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool EndsWithCaseInsensitive(const std::string& s, const char* suffix) {
    size_t slen = strlen(suffix);
    if (s.size() < slen) return false;
    for (size_t i = 0; i < slen; i++) {
        unsigned char a = static_cast<unsigned char>(s[s.size() - slen + i]);
        unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static uint32_t ReadLE32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static int AsciiCaseCmp(const char* a, const char* b) {
    const unsigned char* pa = reinterpret_cast<const unsigned char*>(a);
    const unsigned char* pb = reinterpret_cast<const unsigned char*>(b);
    while (*pa && *pb) {
        unsigned char ca = (*pa >= 'A' && *pa <= 'Z') ? (*pa - 'A' + 'a') : *pa;
        unsigned char cb = (*pb >= 'A' && *pb <= 'Z') ? (*pb - 'A' + 'a') : *pb;
        if (ca != cb) return static_cast<int>(ca) - static_cast<int>(cb);
        pa++;
        pb++;
    }
    unsigned char ca = (*pa >= 'A' && *pa <= 'Z') ? (*pa - 'A' + 'a') : *pa;
    unsigned char cb = (*pb >= 'A' && *pb <= 'Z') ? (*pb - 'A' + 'a') : *pb;
    return static_cast<int>(ca) - static_cast<int>(cb);
}

static uint32_t CodepointToLower(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if ((cp >= 0x00C0 && cp <= 0x00D6) || (cp >= 0x00D8 && cp <= 0x00DE)) return cp + 32;
    if (cp >= 0x0100 && cp <= 0x012F) return (cp & 1) == 0 ? cp + 1 : cp;
    if (cp >= 0x0132 && cp <= 0x0137) return (cp & 1) == 0 ? cp + 1 : cp;
    if (cp >= 0x0139 && cp <= 0x0148) return (cp & 1) == 1 ? cp + 1 : cp;
    if (cp >= 0x014A && cp <= 0x0177) return (cp & 1) == 0 ? cp + 1 : cp;
    if (cp == 0x0178) return 0x00FF;
    if (cp >= 0x0179 && cp <= 0x017E) return (cp & 1) == 1 ? cp + 1 : cp;
    if (cp >= 0x0391 && cp <= 0x03A1) return cp + 32;
    if (cp >= 0x03A3 && cp <= 0x03AB) return cp + 32;
    if (cp == 0x0386) return 0x03AC;
    if (cp >= 0x0388 && cp <= 0x038A) return cp + 37;
    if (cp == 0x038C) return 0x03CC;
    if (cp == 0x038E) return 0x03CD;
    if (cp == 0x038F) return 0x03CE;
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 32;
    if (cp >= 0x0400 && cp <= 0x040F) return cp + 80;
    return cp;
}

static bool Contains(const std::vector<std::string>& vec, const std::string& val) {
    for (const auto& s : vec) {
        if (s == val) return true;
    }
    return false;
}

static void AddUnique(std::vector<std::string>& vec, const std::string& val) {
    if (!val.empty() && !Contains(vec, val)) {
        vec.push_back(val);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// DictText utilities
// ---------------------------------------------------------------------------

namespace DictText {

size_t Utf8Decode(const char* s, size_t len, uint32_t* cp) {
    if (len == 0) {
        *cp = 0;
        return 0;
    }
    unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (c0 < 0x80) {
        *cp = c0;
        return 1;
    } else if ((c0 & 0xE0) == 0xC0 && len >= 2) {
        *cp = ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(s[1]) & 0x3F);
        return 2;
    } else if ((c0 & 0xF0) == 0xE0 && len >= 3) {
        *cp = ((c0 & 0x0F) << 12) |
              ((static_cast<unsigned char>(s[1]) & 0x3F) << 6) |
              (static_cast<unsigned char>(s[2]) & 0x3F);
        return 3;
    } else if ((c0 & 0xF8) == 0xF0 && len >= 4) {
        *cp = ((c0 & 0x07) << 18) |
              ((static_cast<unsigned char>(s[1]) & 0x3F) << 12) |
              ((static_cast<unsigned char>(s[2]) & 0x3F) << 6) |
              (static_cast<unsigned char>(s[3]) & 0x3F);
        return 4;
    }
    *cp = c0;
    return 1;
}

void Utf8Encode(std::string& out, uint32_t cp) {
    if (cp == 0) return;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool IsCjkCodepoint(uint32_t cp) {
    return (cp >= 0x3040 && cp <= 0x30FF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xAC00 && cp <= 0xD7AF);
}

bool IsCombiningMark(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F) ||
           (cp >= 0x0591 && cp <= 0x05C7) ||
           (cp >= 0x064B && cp <= 0x065F);
}

bool IsPunctCodepoint(uint32_t cp) {
    if (cp <= 0x20) return true;
    if ((cp >= 0x21 && cp <= 0x2F) ||
        (cp >= 0x3A && cp <= 0x40) ||
        (cp >= 0x5B && cp <= 0x60) ||
        (cp >= 0x7B && cp <= 0x7E)) {
        return true;
    }
    if (cp == 0x00A0 || cp == 0x00A1 || cp == 0x00AB || cp == 0x00BB || cp == 0x00BF) return true;
    if (cp >= 0x2000 && cp <= 0x206F) return true;
    if (cp >= 0x3000 && cp <= 0x303F) return true;
    if (cp >= 0xFF01 && cp <= 0xFF0F) return true;
    if (cp >= 0xFF1A && cp <= 0xFF20) return true;
    if (cp >= 0xFF3B && cp <= 0xFF40) return true;
    if (cp >= 0xFF5B && cp <= 0xFF65) return true;
    return false;
}

std::string CleanToken(const std::string& raw) {
    std::string filtered;
    filtered.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == DICT_STYLE_BOLD_ON || c == DICT_STYLE_BOLD_OFF ||
            c == DICT_STYLE_ITALIC_ON || c == DICT_STYLE_ITALIC_OFF) {
            i++;
            continue;
        }
        uint32_t cp = 0;
        size_t step = Utf8Decode(raw.data() + i, raw.size() - i, &cp);
        if (cp == 0x2019 || cp == 0x2018) {
            filtered.push_back('\'');
        } else {
            filtered.append(raw.data() + i, step);
        }
        i += step;
    }

    size_t start = 0;
    while (start < filtered.size()) {
        uint32_t cp = 0;
        size_t step = Utf8Decode(filtered.data() + start, filtered.size() - start, &cp);
        if (!IsPunctCodepoint(cp)) break;
        start += step;
    }

    size_t end = start;
    size_t pos = start;
    while (pos < filtered.size()) {
        uint32_t cp = 0;
        size_t step = Utf8Decode(filtered.data() + pos, filtered.size() - pos, &cp);
        pos += step;
        if (!IsPunctCodepoint(cp)) {
            end = pos;
        }
    }

    return filtered.substr(start, end - start);
}

std::string ToLowerUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        size_t step = Utf8Decode(s.data() + i, s.size() - i, &cp);
        Utf8Encode(out, CodepointToLower(cp));
        i += step;
    }
    return out;
}

std::string StripCombiningMarks(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        size_t step = Utf8Decode(s.data() + i, s.size() - i, &cp);
        if (!IsCombiningMark(cp)) {
            out.append(s.data() + i, step);
        }
        i += step;
    }
    return out;
}

size_t Utf8CharCount(const std::string& s) {
    size_t count = 0;
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        i += Utf8Decode(s.data() + i, s.size() - i, &cp);
        count++;
    }
    return count;
}

std::string Utf8PrefixChars(const std::string& s, size_t maxChars) {
    size_t count = 0;
    size_t i = 0;
    while (i < s.size() && count < maxChars) {
        uint32_t cp = 0;
        i += Utf8Decode(s.data() + i, s.size() - i, &cp);
        count++;
    }
    return s.substr(0, i);
}

} // namespace DictText

// ---------------------------------------------------------------------------
// LemmaTable ("DLM1")
// ---------------------------------------------------------------------------


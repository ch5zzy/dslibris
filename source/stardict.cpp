#include "stardict.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include <zlib.h>

namespace {

static const uint32_t kMaxCheckpoints = 2048;
static const uint32_t kMinBlockBytes = 4096;
static const size_t   kScanBufSize = 8192;

static bool FileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static uint64_t GetFileSize(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
}

static inline unsigned char AsciiToLower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : c;
}

static uint32_t ReadBE32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           (static_cast<uint32_t>(p[3]));
}

static uint64_t ReadBE64(const unsigned char* p) {
    return (static_cast<uint64_t>(ReadBE32(p)) << 32) |
           static_cast<uint64_t>(ReadBE32(p + 4));
}

static uint16_t ReadLE16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static void AppendUtf8(std::string& out, uint32_t cp) {
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

static std::string TrimLine(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n'))
        start++;
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n'))
        end--;
    return s.substr(start, end - start);
}

static int AsciiStrCaseCmp(const char* a, const char* b) {
    const unsigned char* pa = reinterpret_cast<const unsigned char*>(a);
    const unsigned char* pb = reinterpret_cast<const unsigned char*>(b);
    while (*pa && *pb) {
        int ca = AsciiToLower(*pa);
        int cb = AsciiToLower(*pb);
        if (ca != cb) return ca - cb;
        pa++;
        pb++;
    }
    return static_cast<int>(AsciiToLower(*pa)) - static_cast<int>(AsciiToLower(*pb));
}

} // namespace

StarDict::StarDict()
    : idxFile(nullptr),
      dictFile(nullptr),
      synFile(nullptr),
      idxIndexed(false),
      synIndexed(false),
      dzChunkLen(0) {}

StarDict::~StarDict() {
    Close();
}

int StarDict::CompareKeys(const char* a, const char* b) {
    int cmp = AsciiStrCaseCmp(a, b);
    if (cmp != 0) return cmp;
    return strcmp(a, b);
}

bool StarDict::ParseIfo(const std::string& ifoPath, StarDictInfo& out) {
    out = StarDictInfo();
    out.ifoPath = ifoPath;

    FILE* fp = fopen(ifoPath.c_str(), "rb");
    if (!fp) return false;

    char lineBuf[512];
    bool validHeader = false;
    while (fgets(lineBuf, sizeof(lineBuf), fp)) {
        std::string line = TrimLine(lineBuf);
        if (line.empty()) continue;
        if (!validHeader) {
            // Strip optional UTF-8 BOM
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
            if (line == "StarDict's dict ifo file") {
                validHeader = true;
                continue;
            }
            fclose(fp);
            return false;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = TrimLine(line.substr(0, eq));
        std::string val = TrimLine(line.substr(eq + 1));

        if (key == "bookname") {
            out.bookName = val;
        } else if (key == "wordcount") {
            out.wordCount = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
        } else if (key == "synwordcount") {
            out.synWordCount = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
        } else if (key == "idxfilesize") {
            out.idxFileSize = static_cast<uint64_t>(strtoull(val.c_str(), nullptr, 10));
        } else if (key == "idxoffsetbits") {
            int bits = atoi(val.c_str());
            out.idxOffsetBits = (bits == 64) ? 64 : 32;
        } else if (key == "sametypesequence") {
            out.sameTypeSequence = val;
        }
    }
    fclose(fp);

    if (!validHeader || ifoPath.size() < 4) return false;

    std::string base = ifoPath.substr(0, ifoPath.size() - 4);
    out.idxPath = base + ".idx";
    if (!FileExists(out.idxPath)) return false;

    if (out.idxFileSize == 0) {
        out.idxFileSize = GetFileSize(out.idxPath);
    }

    std::string dzPath = base + ".dict.dz";
    std::string rawDictPath = base + ".dict";
    if (FileExists(dzPath)) {
        out.dictPath = dzPath;
        out.isDictZip = true;
    } else if (FileExists(rawDictPath)) {
        out.dictPath = rawDictPath;
        out.isDictZip = false;
    } else {
        return false;
    }

    std::string synCandidate = base + ".syn";
    if (FileExists(synCandidate)) {
        out.synPath = synCandidate;
        out.hasSyn = true;
    }

    if (out.bookName.empty()) {
        size_t slash = base.find_last_of("/\\");
        out.bookName = (slash == std::string::npos) ? base : base.substr(slash + 1);
    }

    return true;
}

bool StarDict::Open(const std::string& ifoPath) {
    Close();

    if (!ParseIfo(ifoPath, info)) return false;

    idxFile = fopen(info.idxPath.c_str(), "rb");
    if (!idxFile) {
        Close();
        return false;
    }

    dictFile = fopen(info.dictPath.c_str(), "rb");
    if (!dictFile) {
        Close();
        return false;
    }

    if (info.isDictZip) {
        if (!ParseDictZipHeader()) {
            Close();
            return false;
        }
    }

    if (info.hasSyn) {
        synFile = fopen(info.synPath.c_str(), "rb");
        if (!synFile) {
            info.hasSyn = false;
        }
    }

    return true;
}

void StarDict::Close() {
    if (idxFile) {
        fclose(idxFile);
        idxFile = nullptr;
    }
    if (dictFile) {
        fclose(dictFile);
        dictFile = nullptr;
    }
    if (synFile) {
        fclose(synFile);
        synFile = nullptr;
    }
    idxCheckpoints.clear();
    synCheckpoints.clear();
    dzChunkOffsets.clear();
    idxIndexed = false;
    synIndexed = false;
    dzChunkLen = 0;
}


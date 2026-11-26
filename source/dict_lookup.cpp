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

LemmaTable::LemmaTable() : fp(nullptr), dataStart(0), fileSize(0), indexed(false) {}

LemmaTable::~LemmaTable() {
    Close();
}

bool LemmaTable::Open(const std::string& path) {
    Close();
    fp = fopen(path.c_str(), "rb");
    if (!fp) return false;

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, fp) != 12) {
        Close();
        return false;
    }
    if (memcmp(hdr, "DLM1", 4) != 0) {
        Close();
        return false;
    }
    (void)ReadLE32(hdr + 8);

    struct stat st;
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 12) {
        Close();
        return false;
    }
    dataStart = 12;
    fileSize = static_cast<uint32_t>(st.st_size);
    return true;
}

void LemmaTable::Close() {
    if (fp) {
        fclose(fp);
        fp = nullptr;
    }
    checkpoints.clear();
    dataStart = 0;
    fileSize = 0;
    indexed = false;
}

bool LemmaTable::EnsureIndexed() {
    if (indexed) return !checkpoints.empty();
    if (!fp || fileSize <= dataStart) return false;

    checkpoints.clear();
    uint32_t dataLen = fileSize - dataStart;
    uint32_t blockBytes = dataLen / kLemMaxCheckpoints;
    if (blockBytes < kLemMinBlockBytes) blockBytes = kLemMinBlockBytes;

    if (fseek(fp, static_cast<long>(dataStart), SEEK_SET) != 0) return false;

    std::vector<unsigned char> buf(kLemScanBufSize);
    uint32_t pos = dataStart;
    uint32_t nextChk = dataStart;
    int nullsSeenInRecord = 0;
    bool atRecordStart = true;

    while (true) {
        size_t n = fread(buf.data(), 1, buf.size(), fp);
        if (n == 0) break;
        for (size_t i = 0; i < n; i++, pos++) {
            if (atRecordStart) {
                if (pos >= nextChk) {
                    checkpoints.push_back({pos});
                    nextChk = pos + blockBytes;
                }
                atRecordStart = false;
            }
            if (buf[i] == 0) {
                nullsSeenInRecord++;
                if (nullsSeenInRecord == 2) {
                    nullsSeenInRecord = 0;
                    atRecordStart = true;
                }
            }
        }
    }

    indexed = true;
    return !checkpoints.empty();
}

bool LemmaTable::ReadFirstWordAt(uint32_t offset, std::string& outWord) {
    if (fseek(fp, static_cast<long>(offset), SEEK_SET) != 0) return false;
    char tmp[260];
    size_t n = fread(tmp, 1, sizeof(tmp) - 1, fp);
    if (n == 0) return false;
    tmp[n] = '\0';
    outWord.assign(tmp);
    return true;
}

bool LemmaTable::FindLemmas(const std::string& word, std::vector<std::string>& outLemmas) {
    if (!fp || word.empty()) return false;
    if (!EnsureIndexed()) return false;

    int lo = 0;
    int hi = static_cast<int>(checkpoints.size()) - 1;
    int block = 0;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        std::string w;
        if (!ReadFirstWordAt(checkpoints[mid].fileOffset, w)) return false;
        int cmp = AsciiCaseCmp(w.c_str(), word.c_str());
        if (cmp <= 0) {
            block = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    while (block > 0) {
        std::string w;
        if (!ReadFirstWordAt(checkpoints[block].fileOffset, w)) break;
        if (AsciiCaseCmp(w.c_str(), word.c_str()) == 0) {
            block--;
        } else {
            break;
        }
    }

    bool found = false;
    for (int b = block; b < static_cast<int>(checkpoints.size()); b++) {
        uint32_t startOff = checkpoints[b].fileOffset;
        uint32_t endOff = (b + 1 < static_cast<int>(checkpoints.size()))
                              ? checkpoints[b + 1].fileOffset
                              : fileSize;
        if (endOff <= startOff) break;

        size_t span = endOff - startOff;
        std::vector<unsigned char> buf(span);
        if (fseek(fp, static_cast<long>(startOff), SEEK_SET) != 0) return false;
        size_t got = fread(buf.data(), 1, span, fp);

        size_t p = 0;
        bool passed = false;
        while (p < got) {
            size_t nul1 = p;
            while (nul1 < got && buf[nul1] != 0) nul1++;
            if (nul1 >= got) break;
            size_t nul2 = nul1 + 1;
            while (nul2 < got && buf[nul2] != 0) nul2++;
            if (nul2 >= got) break;

            const char* key = reinterpret_cast<const char*>(buf.data() + p);
            const char* lemma = reinterpret_cast<const char*>(buf.data() + nul1 + 1);
            int cmp = AsciiCaseCmp(key, word.c_str());
            if (cmp == 0) {
                AddUnique(outLemmas, std::string(lemma, nul2 - (nul1 + 1)));
                found = true;
            } else if (cmp > 0) {
                passed = true;
                break;
            }
            p = nul2 + 1;
        }
        if (passed) break;
        if (!found && b + 1 < static_cast<int>(checkpoints.size())) {
            std::string nextFirst;
            if (!ReadFirstWordAt(checkpoints[b + 1].fileOffset, nextFirst) ||
                AsciiCaseCmp(nextFirst.c_str(), word.c_str()) > 0) {
                break;
            }
        }
    }

    return found;
}

// ---------------------------------------------------------------------------
// DictionaryManager (single active dictionary, selected in Settings)
// ---------------------------------------------------------------------------

DictionaryManager::DictionaryManager() : activeIndex(-1) {}

DictionaryManager::~DictionaryManager() {
    CloseActive();
}

int DictionaryManager::ScanDirectory(const std::string& dictDir) {
    CloseActive();
    dictionaries.clear();
    activeIndex = -1;
    rootDir = dictDir;

    auto scanFolder = [&](const std::string& folder) {
        DIR* dp = opendir(folder.c_str());
        if (!dp) return;
        struct dirent* ent;
        while ((ent = readdir(dp)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string full = folder + "/" + ent->d_name;
            if (EndsWithCaseInsensitive(ent->d_name, ".ifo") && IsRegularFile(full)) {
                StarDictInfo info;
                if (StarDict::ParseIfo(full, info)) {
                    dictionaries.push_back(info);
                }
            }
        }
        closedir(dp);
    };

    scanFolder(dictDir);

    DIR* dp = opendir(dictDir.c_str());
    if (dp) {
        struct dirent* ent;
        while ((ent = readdir(dp)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string sub = dictDir + "/" + ent->d_name;
            if (IsDir(sub)) {
                scanFolder(sub);
            }
        }
        closedir(dp);
    }

    std::sort(dictionaries.begin(), dictionaries.end(),
              [](const StarDictInfo& a, const StarDictInfo& b) {
                  return AsciiCaseCmp(a.bookName.c_str(), b.bookName.c_str()) < 0;
              });

    if (!dictionaries.empty()) {
        activeIndex = 0;
    }
    return static_cast<int>(dictionaries.size());
}

void DictionaryManager::CloseActive() {
    activeDict.Close();
    activeLemma.Close();
}

bool DictionaryManager::SelectDictionary(int index) {
    if (index < 0 || index >= static_cast<int>(dictionaries.size())) return false;
    if (index != activeIndex) {
        CloseActive();
        activeIndex = index;
    }
    return true;
}

bool DictionaryManager::SelectByPath(const std::string& ifoPathOrName) {
    if (ifoPathOrName.empty()) return false;
    for (size_t i = 0; i < dictionaries.size(); i++) {
        const std::string& p = dictionaries[i].ifoPath;
        if (p == ifoPathOrName || dictionaries[i].bookName == ifoPathOrName) {
            return SelectDictionary(static_cast<int>(i));
        }
        size_t slash = p.find_last_of("/\\");
        std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
        if (base == ifoPathOrName) {
            return SelectDictionary(static_cast<int>(i));
        }
    }
    return false;
}

std::string DictionaryManager::GetActiveIfoPath() const {
    if (activeIndex < 0 || activeIndex >= static_cast<int>(dictionaries.size())) {
        return std::string();
    }
    return dictionaries[activeIndex].ifoPath;
}

std::string DictionaryManager::GetActiveBookName() const {
    if (activeIndex < 0 || activeIndex >= static_cast<int>(dictionaries.size())) {
        return std::string("none");
    }
    return dictionaries[activeIndex].bookName;
}

void DictionaryManager::TryOpenLemmaSidecar(const std::string& ifoPath) {
    activeLemma.Close();
    if (ifoPath.size() < 4) return;
    std::string sidecar = ifoPath.substr(0, ifoPath.size() - 4) + ".lem";
    if (IsRegularFile(sidecar)) {
        activeLemma.Open(sidecar);
        return;
    }
    if (!rootDir.empty()) {
        std::string fallback = rootDir + "/morph/default.lem";
        if (IsRegularFile(fallback)) {
            activeLemma.Open(fallback);
        }
    }
}

bool DictionaryManager::EnsureActiveOpen() {
    if (activeIndex < 0 || activeIndex >= static_cast<int>(dictionaries.size())) {
        return false;
    }
    if (activeDict.IsOpen()) return true;

    const std::string& ifo = dictionaries[activeIndex].ifoPath;
    if (!activeDict.Open(ifo)) return false;
    TryOpenLemmaSidecar(ifo);
    return true;
}

bool DictionaryManager::TryExactOrSyn(const std::string& candidate, StarDictEntry& entry,
                                      std::vector<std::string>* nearbyOut, bool captureNearby) {
    if (activeDict.FindWord(candidate, entry, captureNearby ? nearbyOut : nullptr, 5)) {
        return true;
    }
    if (activeDict.FindSynonym(candidate, entry)) {
        return true;
    }
    return false;
}

bool DictionaryManager::LookupExactHeadword(const std::string& headword, LookupResult& out) {
    out = LookupResult();
    out.queried = headword;
    if (!EnsureActiveOpen()) return false;

    StarDictEntry entry;
    if (activeDict.FindWord(headword, entry, &out.nearby, 5)) {
        out.found = true;
        out.headword = entry.headword;
        activeDict.ReadDefinition(entry, out.definition);
        return true;
    }
    return false;
}

bool DictionaryManager::Lookup(const std::string& rawToken, LookupResult& out, bool isCjkContext) {
    out = LookupResult();
    if (!EnsureActiveOpen()) return false;

    std::string cleaned = DictText::CleanToken(rawToken);
    if (cleaned.empty()) return false;
    out.queried = cleaned;

    uint32_t firstCp = 0;
    DictText::Utf8Decode(cleaned.data(), cleaned.size(), &firstCp);
    if (isCjkContext || DictText::IsCjkCodepoint(firstCp)) {
        size_t maxChars = std::min<size_t>(DictText::Utf8CharCount(cleaned), 8);
        StarDictEntry entry;
        for (size_t len = maxChars; len >= 1; len--) {
            std::string sub = DictText::Utf8PrefixChars(cleaned, len);
            bool capture = (len == maxChars);
            if (TryExactOrSyn(sub, entry, &out.nearby, capture)) {
                out.found = true;
                out.matchedViaFallback = (len < maxChars);
                out.queried = sub;
                out.headword = entry.headword;
                activeDict.ReadDefinition(entry, out.definition);
                return true;
            }
            if (activeLemma.IsOpen()) {
                std::vector<std::string> lemmas;
                if (activeLemma.FindLemmas(sub, lemmas)) {
                    for (const auto& lem : lemmas) {
                        if (TryExactOrSyn(lem, entry, nullptr, false)) {
                            out.found = true;
                            out.matchedViaFallback = true;
                            out.queried = sub;
                            out.headword = entry.headword;
                            activeDict.ReadDefinition(entry, out.definition);
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }

    std::vector<std::string> primaryCandidates;
    AddUnique(primaryCandidates, cleaned);

    std::string lower = DictText::ToLowerUtf8(cleaned);
    AddUnique(primaryCandidates, lower);

    size_t apos = lower.find('\'');
    if (apos != std::string::npos && apos <= 3 && apos + 1 < lower.size()) {
        AddUnique(primaryCandidates, lower.substr(apos + 1));
    }

    if (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "'s") == 0) {
        AddUnique(primaryCandidates, lower.substr(0, lower.size() - 2));
    } else if (lower.size() > 1 && lower.back() == '\'') {
        AddUnique(primaryCandidates, lower.substr(0, lower.size() - 1));
    }

    std::string strippedMarks = DictText::StripCombiningMarks(lower);
    AddUnique(primaryCandidates, strippedMarks);

    StarDictEntry entry;
    for (size_t i = 0; i < primaryCandidates.size(); i++) {
        bool capture = (i == 0 || (i == 1 && out.nearby.empty()));
        if (TryExactOrSyn(primaryCandidates[i], entry, &out.nearby, capture)) {
            out.found = true;
            out.matchedViaFallback = (i > 1);
            out.headword = entry.headword;
            activeDict.ReadDefinition(entry, out.definition);
            return true;
        }
    }

    if (activeLemma.IsOpen()) {
        for (const auto& cand : primaryCandidates) {
            std::vector<std::string> lemmas;
            if (activeLemma.FindLemmas(cand, lemmas)) {
                for (const auto& lem : lemmas) {
                    if (TryExactOrSyn(lem, entry, nullptr, false)) {
                        out.found = true;
                        out.matchedViaFallback = true;
                        out.headword = entry.headword;
                        activeDict.ReadDefinition(entry, out.definition);
                        return true;
                    }
                }
            }
        }
    }

    std::string base = strippedMarks.empty() ? lower : strippedMarks;
    size_t dash = base.find('-');
    if (dash != std::string::npos && dash > 0 && dash + 1 < base.size()) {
        std::string noDash;
        for (char ch : base) if (ch != '-') noDash.push_back(ch);
        if (TryExactOrSyn(noDash, entry, nullptr, false)) {
            out.found = true;
            out.matchedViaFallback = true;
            out.headword = entry.headword;
            activeDict.ReadDefinition(entry, out.definition);
            return true;
        }
    }

    size_t charLen = DictText::Utf8CharCount(base);
    if (charLen >= 4) {
        size_t minLen = std::max<size_t>(3, (charLen + 1) / 2);
        size_t maxTrim = std::min<size_t>(5, charLen - minLen);

        for (size_t trim = 1; trim <= maxTrim; trim++) {
            std::string stem = DictText::Utf8PrefixChars(base, charLen - trim);
            if (TryExactOrSyn(stem, entry, nullptr, false)) {
                out.found = true;
                out.matchedViaFallback = true;
                out.headword = entry.headword;
                activeDict.ReadDefinition(entry, out.definition);
                return true;
            }
            if (stem.size() >= 3 && static_cast<unsigned char>(stem.back()) < 0x80) {
                std::string withE = stem + "e";
                if (TryExactOrSyn(withE, entry, nullptr, false)) {
                    out.found = true;
                    out.matchedViaFallback = true;
                    out.headword = entry.headword;
                    activeDict.ReadDefinition(entry, out.definition);
                    return true;
                }
                if (stem.back() == 'i') {
                    std::string withY = stem.substr(0, stem.size() - 1) + "y";
                    if (TryExactOrSyn(withY, entry, nullptr, false)) {
                        out.found = true;
                        out.matchedViaFallback = true;
                        out.headword = entry.headword;
                        activeDict.ReadDefinition(entry, out.definition);
                        return true;
                    }
                }
                if (stem.size() >= 4 && stem[stem.size() - 1] == stem[stem.size() - 2]) {
                    std::string undoubled = stem.substr(0, stem.size() - 1);
                    if (TryExactOrSyn(undoubled, entry, nullptr, false)) {
                        out.found = true;
                        out.matchedViaFallback = true;
                        out.headword = entry.headword;
                        activeDict.ReadDefinition(entry, out.definition);
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

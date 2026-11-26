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

bool StarDict::ParseDictZipHeader() {
    if (!dictFile) return false;
    if (fseek(dictFile, 0, SEEK_SET) != 0) return false;

    unsigned char hdr[10];
    if (fread(hdr, 1, 10, dictFile) != 10) return false;
    if (hdr[0] != 0x1F || hdr[1] != 0x8B || hdr[2] != 8) return false;

    unsigned char flg = hdr[3];
    if ((flg & 0x04) == 0) return false; // FEXTRA required for dictzip

    unsigned char xlenBuf[2];
    if (fread(xlenBuf, 1, 2, dictFile) != 2) return false;
    uint16_t xlen = ReadLE16(xlenBuf);

    std::vector<unsigned char> extra(xlen);
    if (fread(extra.data(), 1, xlen, dictFile) != xlen) return false;

    bool foundRA = false;
    size_t pos = 0;
    std::vector<uint16_t> chunkSizes;
    while (pos + 4 <= extra.size()) {
        unsigned char si1 = extra[pos];
        unsigned char si2 = extra[pos + 1];
        uint16_t slen = ReadLE16(&extra[pos + 2]);
        pos += 4;
        if (pos + slen > extra.size()) break;

        if (si1 == 'R' && si2 == 'A' && slen >= 6) {
            uint16_t ver = ReadLE16(&extra[pos]);
            if (ver != 1) return false;
            dzChunkLen = ReadLE16(&extra[pos + 2]);
            uint16_t chcnt = ReadLE16(&extra[pos + 4]);
            if (dzChunkLen == 0 || static_cast<size_t>(6 + chcnt * 2) > slen) return false;

            chunkSizes.resize(chcnt);
            for (uint16_t i = 0; i < chcnt; i++) {
                chunkSizes[i] = ReadLE16(&extra[pos + 6 + i * 2]);
            }
            foundRA = true;
            break;
        }
        pos += slen;
    }

    if (!foundRA) return false;

    // Skip optional FNAME
    if (flg & 0x08) {
        int c;
        while ((c = fgetc(dictFile)) != EOF && c != 0) {}
        if (c == EOF) return false;
    }
    // Skip optional FCOMMENT
    if (flg & 0x10) {
        int c;
        while ((c = fgetc(dictFile)) != EOF && c != 0) {}
        if (c == EOF) return false;
    }
    // Skip optional FHCRC
    if (flg & 0x02) {
        if (fseek(dictFile, 2, SEEK_CUR) != 0) return false;
    }

    long dataStart = ftell(dictFile);
    if (dataStart < 0) return false;

    dzChunkOffsets.resize(chunkSizes.size() + 1);
    uint32_t cur = static_cast<uint32_t>(dataStart);
    for (size_t i = 0; i < chunkSizes.size(); i++) {
        dzChunkOffsets[i] = cur;
        cur += chunkSizes[i];
    }
    dzChunkOffsets[chunkSizes.size()] = cur;
    return true;
}

bool StarDict::EnsureIdxIndexed() {
    if (idxIndexed) return !idxCheckpoints.empty();
    if (!idxFile) return false;

    idxCheckpoints.clear();
    uint64_t fileSize = info.idxFileSize;
    if (fileSize == 0) {
        fileSize = GetFileSize(info.idxPath);
        info.idxFileSize = fileSize;
    }
    if (fileSize == 0) return false;

    uint32_t metaSize = (info.idxOffsetBits == 64) ? 12 : 8;
    uint32_t blockBytes = static_cast<uint32_t>(fileSize / kMaxCheckpoints);
    if (blockBytes < kMinBlockBytes) blockBytes = kMinBlockBytes;

    if (fseek(idxFile, 0, SEEK_SET) != 0) return false;

    std::vector<unsigned char> buf(kScanBufSize);
    uint32_t filePos = 0;
    uint32_t entryIdx = 0;
    uint32_t nextCheckpointOffset = 0;

    // State machine: 0 = scanning word until '\0', >0 = skipping remaining meta bytes
    uint32_t skipMeta = 0;
    bool atEntryStart = true;

    while (true) {
        size_t n = fread(buf.data(), 1, buf.size(), idxFile);
        if (n == 0) break;

        for (size_t i = 0; i < n; i++, filePos++) {
            if (skipMeta > 0) {
                skipMeta--;
                if (skipMeta == 0) {
                    entryIdx++;
                    atEntryStart = true;
                }
                continue;
            }
            if (atEntryStart) {
                if (filePos >= nextCheckpointOffset) {
                    idxCheckpoints.push_back({filePos, entryIdx});
                    nextCheckpointOffset = filePos + blockBytes;
                }
                atEntryStart = false;
            }
            if (buf[i] == 0) {
                skipMeta = metaSize;
            }
        }
    }

    idxIndexed = true;
    return !idxCheckpoints.empty();
}

bool StarDict::EnsureSynIndexed() {
    if (synIndexed) return !synCheckpoints.empty();
    if (!synFile) return false;

    synCheckpoints.clear();
    uint64_t fileSize = GetFileSize(info.synPath);
    if (fileSize == 0) return false;

    uint32_t blockBytes = static_cast<uint32_t>(fileSize / kMaxCheckpoints);
    if (blockBytes < kMinBlockBytes) blockBytes = kMinBlockBytes;

    if (fseek(synFile, 0, SEEK_SET) != 0) return false;

    std::vector<unsigned char> buf(kScanBufSize);
    uint32_t filePos = 0;
    uint32_t entryIdx = 0;
    uint32_t nextCheckpointOffset = 0;
    uint32_t skipMeta = 0;
    bool atEntryStart = true;

    while (true) {
        size_t n = fread(buf.data(), 1, buf.size(), synFile);
        if (n == 0) break;

        for (size_t i = 0; i < n; i++, filePos++) {
            if (skipMeta > 0) {
                skipMeta--;
                if (skipMeta == 0) {
                    entryIdx++;
                    atEntryStart = true;
                }
                continue;
            }
            if (atEntryStart) {
                if (filePos >= nextCheckpointOffset) {
                    synCheckpoints.push_back({filePos, entryIdx});
                    nextCheckpointOffset = filePos + blockBytes;
                }
                atEntryStart = false;
            }
            if (buf[i] == 0) {
                skipMeta = 4;
            }
        }
    }

    synIndexed = true;
    return !synCheckpoints.empty();
}

bool StarDict::ReadIdxWordAt(uint32_t fileOffset, std::string& outWord) {
    if (fseek(idxFile, static_cast<long>(fileOffset), SEEK_SET) != 0) return false;
    char tmp[260];
    size_t n = fread(tmp, 1, sizeof(tmp) - 1, idxFile);
    if (n == 0) return false;
    tmp[n] = '\0';
    outWord.assign(tmp);
    return true;
}

bool StarDict::ReadIdxEntryAt(uint32_t fileOffset, uint32_t entryIndex,
                              StarDictEntry& out, uint32_t& nextOffset) {
    if (fseek(idxFile, static_cast<long>(fileOffset), SEEK_SET) != 0) return false;
    unsigned char buf[280];
    size_t n = fread(buf, 1, sizeof(buf), idxFile);
    if (n == 0) return false;

    size_t nul = 0;
    while (nul < n && buf[nul] != 0) nul++;
    uint32_t metaSize = (info.idxOffsetBits == 64) ? 12 : 8;
    if (nul + 1 + metaSize > n) return false;

    out.headword.assign(reinterpret_cast<const char*>(buf), nul);
    const unsigned char* meta = buf + nul + 1;
    if (info.idxOffsetBits == 64) {
        out.dataOffset = ReadBE64(meta);
        out.dataSize = ReadBE32(meta + 8);
    } else {
        out.dataOffset = ReadBE32(meta);
        out.dataSize = ReadBE32(meta + 4);
    }
    out.entryIndex = entryIndex;
    nextOffset = fileOffset + static_cast<uint32_t>(nul + 1 + metaSize);
    return true;
}

bool StarDict::ReadSynEntryAt(uint32_t fileOffset, std::string& outWord,
                              uint32_t& outOrigIndex, uint32_t& nextOffset) {
    if (fseek(synFile, static_cast<long>(fileOffset), SEEK_SET) != 0) return false;
    unsigned char buf[270];
    size_t n = fread(buf, 1, sizeof(buf), synFile);
    if (n == 0) return false;

    size_t nul = 0;
    while (nul < n && buf[nul] != 0) nul++;
    if (nul + 5 > n) return false;

    outWord.assign(reinterpret_cast<const char*>(buf), nul);
    outOrigIndex = ReadBE32(buf + nul + 1);
    nextOffset = fileOffset + static_cast<uint32_t>(nul + 5);
    return true;
}

bool StarDict::GetEntryByIndex(uint32_t targetIndex, StarDictEntry& outEntry) {
    if (!EnsureIdxIndexed()) return false;

    // Binary search checkpoints by entryIndex.
    int lo = 0;
    int hi = static_cast<int>(idxCheckpoints.size()) - 1;
    int block = 0;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (idxCheckpoints[mid].entryIndex <= targetIndex) {
            block = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    uint32_t pos = idxCheckpoints[block].fileOffset;
    uint32_t idx = idxCheckpoints[block].entryIndex;
    uint32_t endPos = (block + 1 < static_cast<int>(idxCheckpoints.size()))
                          ? idxCheckpoints[block + 1].fileOffset
                          : static_cast<uint32_t>(info.idxFileSize);

    while (pos < endPos) {
        StarDictEntry cur;
        uint32_t nextPos = 0;
        if (!ReadIdxEntryAt(pos, idx, cur, nextPos)) return false;
        if (idx == targetIndex) {
            outEntry = cur;
            return true;
        }
        pos = nextPos;
        idx++;
    }
    return false;
}

void StarDict::CollectNearby(int blockIdx, uint32_t targetOffset,
                             std::vector<std::string>& nearbyOut, int maxNearby) {
    nearbyOut.clear();
    if (maxNearby <= 0 || idxCheckpoints.empty()) return;

    int startBlock = (blockIdx > 0) ? (blockIdx - 1) : 0;
    int endBlock = (blockIdx + 1 < static_cast<int>(idxCheckpoints.size()))
                       ? (blockIdx + 1)
                       : (static_cast<int>(idxCheckpoints.size()) - 1);

    uint32_t startOff = idxCheckpoints[startBlock].fileOffset;
    uint32_t endOff = (endBlock + 1 < static_cast<int>(idxCheckpoints.size()))
                          ? idxCheckpoints[endBlock + 1].fileOffset
                          : static_cast<uint32_t>(info.idxFileSize);

    if (endOff <= startOff) return;
    size_t span = endOff - startOff;
    // Cap memory usage for huge blocks
    if (span > 32768) {
        startOff = idxCheckpoints[blockIdx].fileOffset;
        endOff = (blockIdx + 1 < static_cast<int>(idxCheckpoints.size()))
                     ? idxCheckpoints[blockIdx + 1].fileOffset
                     : static_cast<uint32_t>(info.idxFileSize);
        if (endOff <= startOff) return;
        span = endOff - startOff;
    }

    std::vector<unsigned char> buf(span);
    if (fseek(idxFile, static_cast<long>(startOff), SEEK_SET) != 0) return;
    size_t got = fread(buf.data(), 1, span, idxFile);
    if (got == 0) return;

    uint32_t metaSize = (info.idxOffsetBits == 64) ? 12 : 8;
    std::vector<std::string> words;
    int targetPosIdx = -1;

    size_t p = 0;
    while (p < got) {
        uint32_t curAbs = startOff + static_cast<uint32_t>(p);
        size_t nul = p;
        while (nul < got && buf[nul] != 0) nul++;
        if (nul + 1 + metaSize > got) break;

        if (targetPosIdx < 0 && curAbs >= targetOffset) {
            targetPosIdx = static_cast<int>(words.size());
        }
        words.emplace_back(reinterpret_cast<const char*>(buf.data() + p), nul - p);
        p = nul + 1 + metaSize;
    }

    if (words.empty()) return;
    if (targetPosIdx < 0) targetPosIdx = static_cast<int>(words.size()) - 1;

    int half = maxNearby / 2;
    int begin = targetPosIdx - half;
    if (begin < 0) begin = 0;
    int end = begin + maxNearby;
    if (end > static_cast<int>(words.size())) {
        end = static_cast<int>(words.size());
        begin = std::max(0, end - maxNearby);
    }

    for (int i = begin; i < end; i++) {
        nearbyOut.push_back(words[i]);
    }
}

bool StarDict::FindWord(const std::string& word, StarDictEntry& outEntry,
                        std::vector<std::string>* nearbyOut, int maxNearby) {
    if (word.empty() || !EnsureIdxIndexed()) return false;

    // 1. Binary search checkpoints using ASCII case-insensitive comparison.
    // Find the last checkpoint whose first word <= word (case-insensitively).
    // If multiple entries share the same ASCII-lowercase key across a block boundary,
    // step back one block so we don't miss an earlier case variant.
    int lo = 0;
    int hi = static_cast<int>(idxCheckpoints.size()) - 1;
    int block = 0;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        std::string w;
        if (!ReadIdxWordAt(idxCheckpoints[mid].fileOffset, w)) return false;
        int cmp = AsciiStrCaseCmp(w.c_str(), word.c_str());
        if (cmp <= 0) {
            block = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    // Step back while previous block's start word has same case-insensitive key.
    while (block > 0) {
        std::string w;
        if (!ReadIdxWordAt(idxCheckpoints[block].fileOffset, w)) break;
        if (AsciiStrCaseCmp(w.c_str(), word.c_str()) == 0) {
            block--;
        } else {
            break;
        }
    }

    // 2. Scan the block (and if a case-insensitive match spans into the next block, check that too).
    bool foundCaseInsensitive = false;
    StarDictEntry bestEntry;
    uint32_t insertionOffset = idxCheckpoints[block].fileOffset;
    bool recordedInsertion = false;

    uint32_t metaSize = (info.idxOffsetBits == 64) ? 12 : 8;

    for (int b = block; b < static_cast<int>(idxCheckpoints.size()); b++) {
        uint32_t startOff = idxCheckpoints[b].fileOffset;
        uint32_t endOff = (b + 1 < static_cast<int>(idxCheckpoints.size()))
                              ? idxCheckpoints[b + 1].fileOffset
                              : static_cast<uint32_t>(info.idxFileSize);
        if (endOff <= startOff) break;

        size_t span = endOff - startOff;
        std::vector<unsigned char> buf(span);
        if (fseek(idxFile, static_cast<long>(startOff), SEEK_SET) != 0) return false;
        size_t got = fread(buf.data(), 1, span, idxFile);

        size_t p = 0;
        uint32_t idx = idxCheckpoints[b].entryIndex;
        bool passedTarget = false;

        while (p < got) {
            uint32_t curOff = startOff + static_cast<uint32_t>(p);
            size_t nul = p;
            while (nul < got && buf[nul] != 0) nul++;
            if (nul + 1 + metaSize > got) break;

            const char* entryWord = reinterpret_cast<const char*>(buf.data() + p);
            int ci = AsciiStrCaseCmp(entryWord, word.c_str());

            if (ci == 0) {
                StarDictEntry cand;
                cand.headword.assign(entryWord, nul - p);
                const unsigned char* meta = buf.data() + nul + 1;
                if (info.idxOffsetBits == 64) {
                    cand.dataOffset = ReadBE64(meta);
                    cand.dataSize = ReadBE32(meta + 8);
                } else {
                    cand.dataOffset = ReadBE32(meta);
                    cand.dataSize = ReadBE32(meta + 4);
                }
                cand.entryIndex = idx;

                if (!foundCaseInsensitive) {
                    bestEntry = cand;
                    foundCaseInsensitive = true;
                    insertionOffset = curOff;
                    recordedInsertion = true;
                }
                // Exact byte match wins immediately!
                if (cand.headword == word) {
                    outEntry = cand;
                    if (nearbyOut) CollectNearby(b, curOff, *nearbyOut, maxNearby);
                    return true;
                }
            } else if (ci > 0) {
                if (!recordedInsertion) {
                    insertionOffset = curOff;
                    recordedInsertion = true;
                }
                passedTarget = true;
                break;
            }

            p = nul + 1 + metaSize;
            idx++;
        }

        if (passedTarget) break;
        if (!foundCaseInsensitive && b + 1 < static_cast<int>(idxCheckpoints.size())) {
            std::string nextFirst;
            if (!ReadIdxWordAt(idxCheckpoints[b + 1].fileOffset, nextFirst) ||
                AsciiStrCaseCmp(nextFirst.c_str(), word.c_str()) > 0) {
                insertionOffset = endOff;
                recordedInsertion = true;
                break;
            }
        }
    }

    if (foundCaseInsensitive) {
        outEntry = bestEntry;
        if (nearbyOut) CollectNearby(block, insertionOffset, *nearbyOut, maxNearby);
        return true;
    }

    if (nearbyOut) {
        CollectNearby(block, insertionOffset, *nearbyOut, maxNearby);
    }
    return false;
}


#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

// Matches TEXT_BOLD_* / TEXT_ITALIC_* in text.h without pulling in libnds/FreeType.
#define DICT_STYLE_BOLD_ON    2
#define DICT_STYLE_BOLD_OFF   3
#define DICT_STYLE_ITALIC_ON  4
#define DICT_STYLE_ITALIC_OFF 5

struct StarDictInfo {
    std::string ifoPath;
    std::string idxPath;
    std::string dictPath;
    std::string synPath;
    std::string bookName;
    std::string sameTypeSequence;
    uint32_t wordCount = 0;
    uint32_t synWordCount = 0;
    uint64_t idxFileSize = 0;
    uint8_t idxOffsetBits = 32;
    bool isDictZip = false;
    bool hasSyn = false;
};

struct StarDictEntry {
    std::string headword;
    uint64_t dataOffset = 0;
    uint32_t dataSize = 0;
    uint32_t entryIndex = 0;
};

class StarDict {
public:
    StarDict();
    ~StarDict();

    // Parse .ifo metadata only (fast, suitable for scanning Dictionary/ folder).
    static bool ParseIfo(const std::string& ifoPath, StarDictInfo& info);

    // StarDict specification comparator: ASCII case-insensitive, tie-broken by strcmp.
    static int CompareKeys(const char* a, const char* b);

    // Open files and prepare lazy/compact indices (low RAM).
    bool Open(const std::string& ifoPath);
    void Close();
    bool IsOpen() const { return idxFile != nullptr && dictFile != nullptr; }
    const StarDictInfo& GetInfo() const { return info; }

    // Exact lookup in .idx (case-insensitive per StarDict spec, preferring exact case).
    bool FindWord(const std::string& word, StarDictEntry& outEntry,
                  std::vector<std::string>* nearbyOut = nullptr, int maxNearby = 5);

    // Exact lookup in .syn; resolves synonym index to the target .idx entry.
    bool FindSynonym(const std::string& word, StarDictEntry& outEntry);

    // Fetch entry by 0-based .idx entry index (used for .syn resolution).
    bool GetEntryByIndex(uint32_t index, StarDictEntry& outEntry);

    // Read raw definition bytes from .dict or .dict.dz.
    bool ReadRawDefinition(const StarDictEntry& entry, std::string& outRaw);

    // Convert raw StarDict entry bytes into UTF-8 text with DICT_STYLE_* markers.
    std::string FormatDefinition(const std::string& raw) const;

    // Convenience: read and format definition for an entry.
    bool ReadDefinition(const StarDictEntry& entry, std::string& outFormatted);

private:
    struct Checkpoint {
        uint32_t fileOffset;
        uint32_t entryIndex;
    };

    StarDictInfo info;
    FILE* idxFile;
    FILE* dictFile;
    FILE* synFile;

    // Sparse checkpoint tables (capped at <= 2048 entries each, ~16 KB max RAM).
    std::vector<Checkpoint> idxCheckpoints;
    std::vector<Checkpoint> synCheckpoints;
    bool idxIndexed;
    bool synIndexed;

    // Dictzip chunk metadata (only populated when info.isDictZip is true).
    uint16_t dzChunkLen;
    std::vector<uint32_t> dzChunkOffsets; // size = chunkCount + 1

    bool EnsureIdxIndexed();
    bool EnsureSynIndexed();
    bool ParseDictZipHeader();

    bool ReadIdxEntryAt(uint32_t fileOffset, uint32_t entryIndex,
                        StarDictEntry& out, uint32_t& nextOffset);
    bool ReadIdxWordAt(uint32_t fileOffset, std::string& outWord);
    bool ReadSynEntryAt(uint32_t fileOffset, std::string& outWord,
                        uint32_t& outOrigIndex, uint32_t& nextOffset);

    void CollectNearby(int blockIdx, uint32_t targetOffset,
                       std::vector<std::string>& nearbyOut, int maxNearby);

    bool ReadDictZipRange(uint64_t offset, uint32_t size, std::string& out);
    static std::string StripMarkup(const char* data, size_t len, char typeChar);
};

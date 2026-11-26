#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "stardict.h"

// Compact disk-backed lemma table ("DLM1"): sorted (inflected\0lemma\0) pairs.
// Can be generated for any language using etc/tool/make_lemma.py.
class LemmaTable {
public:
    LemmaTable();
    ~LemmaTable();

    bool Open(const std::string& path);
    void Close();
    bool IsOpen() const { return fp != nullptr; }

    // Look up an inflected form; appends matching lemma(s) to outLemmas.
    bool FindLemmas(const std::string& word, std::vector<std::string>& outLemmas);

private:
    struct Checkpoint {
        uint32_t fileOffset;
    };

    FILE* fp;
    uint32_t dataStart;
    uint32_t fileSize;
    std::vector<Checkpoint> checkpoints;
    bool indexed;

    bool EnsureIndexed();
    bool ReadFirstWordAt(uint32_t offset, std::string& outWord);
};

struct LookupResult {
    bool found = false;
    bool matchedViaFallback = false;
    std::string queried;             // Normalized token that was looked up
    std::string headword;            // Matched dictionary headword
    std::string definition;          // Formatted UTF-8 definition (with DICT_STYLE_* markers)
    std::vector<std::string> nearby; // Nearby .idx headwords for quick-jump / "Did you mean?"
};

// Language-agnostic text utilities (UTF-8 normalization, case folding, CJK detection).
namespace DictText {
    size_t Utf8Decode(const char* s, size_t len, uint32_t* cp);
    void Utf8Encode(std::string& out, uint32_t cp);
    bool IsCjkCodepoint(uint32_t cp);
    bool IsPunctCodepoint(uint32_t cp);
    bool IsCombiningMark(uint32_t cp);
    std::string CleanToken(const std::string& raw);
    std::string ToLowerUtf8(const std::string& s);
    std::string StripCombiningMarks(const std::string& s);
    size_t Utf8CharCount(const std::string& s);
    std::string Utf8PrefixChars(const std::string& s, size_t maxChars);
}

class DictionaryManager {
public:
    DictionaryManager();
    ~DictionaryManager();

    // Scan dictDir (flat files and 1-level subdirectories) for *.ifo dictionaries.
    int ScanDirectory(const std::string& dictDir);

    const std::vector<StarDictInfo>& GetDictionaries() const { return dictionaries; }
    int GetActiveIndex() const { return activeIndex; }
    bool HasDictionary() const { return !dictionaries.empty(); }

    // Select active dictionary by index or by .ifo path (or basename).
    bool SelectDictionary(int index);
    bool SelectByPath(const std::string& ifoPathOrName);
    std::string GetActiveIfoPath() const;
    std::string GetActiveBookName() const;

    // Close active open handles (if any).
    void CloseActive();

    // Language-agnostic lookup in the currently active dictionary:
    //   rawToken: the selected word (or CJK slice starting at tapped character).
    //   isCjkContext: true when rawToken is a spaceless CJK window from the cursor.
    bool Lookup(const std::string& rawToken, LookupResult& out, bool isCjkContext = false);

    // Direct lookup of a specific headword (e.g., when browsing "Nearby" suggestions).
    bool LookupExactHeadword(const std::string& headword, LookupResult& out);

private:
    std::string rootDir;
    std::vector<StarDictInfo> dictionaries;
    int activeIndex;
    StarDict activeDict;
    LemmaTable activeLemma;

    bool EnsureActiveOpen();
    void TryOpenLemmaSidecar(const std::string& ifoPath);
    bool TryExactOrSyn(const std::string& candidate, StarDictEntry& entry,
                       std::vector<std::string>* nearbyOut, bool captureNearby);
};

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <zlib.h>

#include "dict_lookup.h"
#include "stardict.h"

namespace {

void WriteBE32(std::string& buf, uint32_t v) {
    buf.push_back(static_cast<char>((v >> 24) & 0xFF));
    buf.push_back(static_cast<char>((v >> 16) & 0xFF));
    buf.push_back(static_cast<char>((v >> 8) & 0xFF));
    buf.push_back(static_cast<char>(v & 0xFF));
}

void WriteLE16(std::string& buf, uint16_t v) {
    buf.push_back(static_cast<char>(v & 0xFF));
    buf.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void WriteLE32(std::string& buf, uint32_t v) {
    buf.push_back(static_cast<char>(v & 0xFF));
    buf.push_back(static_cast<char>((v >> 8) & 0xFF));
    buf.push_back(static_cast<char>((v >> 16) & 0xFF));
    buf.push_back(static_cast<char>((v >> 24) & 0xFF));
}

// Compress raw data into a valid RFC 1952 dictzip (.dict.dz) file with small chunks
// so we explicitly exercise multi-chunk boundary reads.
std::string MakeDictZip(const std::string& raw, uint16_t chunkLen) {
    std::vector<std::string> compChunks;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t take = std::min<size_t>(chunkLen, raw.size() - pos);
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        int rc = deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        assert(rc == Z_OK);

        std::string comp(take + 128, '\0');
        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(raw.data() + pos));
        zs.avail_in = static_cast<uInt>(take);
        zs.next_out = reinterpret_cast<Bytef*>(&comp[0]);
        zs.avail_out = static_cast<uInt>(comp.size());

        rc = deflate(&zs, Z_FULL_FLUSH);
        assert(rc == Z_OK);
        size_t outLen = comp.size() - zs.avail_out;
        deflateEnd(&zs);
        comp.resize(outLen);
        compChunks.push_back(comp);
        pos += take;
    }

    // Build RA extra subfield
    std::string ra;
    WriteLE16(ra, 1); // VER = 1
    WriteLE16(ra, chunkLen);
    WriteLE16(ra, static_cast<uint16_t>(compChunks.size()));
    for (const auto& c : compChunks) {
        WriteLE16(ra, static_cast<uint16_t>(c.size()));
    }

    std::string extra;
    extra.push_back('R');
    extra.push_back('A');
    WriteLE16(extra, static_cast<uint16_t>(ra.size()));
    extra.append(ra);

    std::string dz;
    // Gzip header (10 bytes): ID1, ID2, CM=8, FLG=FEXTRA|FNAME, MTIME(4), XFL, OS
    dz.push_back(static_cast<char>(0x1F));
    dz.push_back(static_cast<char>(0x8B));
    dz.push_back(static_cast<char>(8));
    dz.push_back(static_cast<char>(0x04 | 0x08)); // FEXTRA | FNAME
    WriteLE32(dz, 0);
    dz.push_back(0);
    dz.push_back(3);

    WriteLE16(dz, static_cast<uint16_t>(extra.size()));
    dz.append(extra);
    dz.append("test.dict");
    dz.push_back('\0');

    for (const auto& c : compChunks) {
        dz.append(c);
    }
    return dz;
}

void WriteFile(const std::string& path, const std::string& data) {
    FILE* fp = fopen(path.c_str(), "wb");
    assert(fp != nullptr);
    if (!data.empty()) {
        size_t n = fwrite(data.data(), 1, data.size(), fp);
        assert(n == data.size());
    }
    fclose(fp);
}

struct TestWord {
    std::string key;
    std::string def;
};

void BuildTestDict(const std::string& dir, const std::string& name,
                   const std::string& bookName, const std::string& sameTypeSeq,
                   std::vector<TestWord> words,
                   const std::vector<std::pair<std::string, std::string>>& syns,
                   bool useDictZip) {
    std::sort(words.begin(), words.end(),
              [](const TestWord& a, const TestWord& b) {
                  return StarDict::CompareKeys(a.key.c_str(), b.key.c_str()) < 0;
              });

    std::string dictData;
    std::string idxData;
    for (const auto& w : words) {
        uint32_t off = static_cast<uint32_t>(dictData.size());
        uint32_t sz = static_cast<uint32_t>(w.def.size());
        dictData.append(w.def);
        idxData.append(w.key);
        idxData.push_back('\0');
        WriteBE32(idxData, off);
        WriteBE32(idxData, sz);
    }

    std::string base = dir + "/" + name;
    WriteFile(base + ".idx", idxData);

    if (useDictZip) {
        // Use tiny 128-byte chunks so entries cross chunk boundaries!
        WriteFile(base + ".dict.dz", MakeDictZip(dictData, 128));
    } else {
        WriteFile(base + ".dict", dictData);
    }

    if (!syns.empty()) {
        std::vector<std::pair<std::string, uint32_t>> resolved;
        for (const auto& s : syns) {
            for (uint32_t i = 0; i < words.size(); i++) {
                if (words[i].key == s.second) {
                    resolved.push_back({s.first, i});
                    break;
                }
            }
        }
        std::sort(resolved.begin(), resolved.end(),
                  [](const std::pair<std::string, uint32_t>& a,
                     const std::pair<std::string, uint32_t>& b) {
                      int c = StarDict::CompareKeys(a.first.c_str(), b.first.c_str());
                      return c != 0 ? (c < 0) : (a.second < b.second);
                  });
        std::string synData;
        for (const auto& r : resolved) {
            synData.append(r.first);
            synData.push_back('\0');
            WriteBE32(synData, r.second);
        }
        WriteFile(base + ".syn", synData);
    }

    std::string ifo = "StarDict's dict ifo file\n";
    ifo += "version=2.4.2\n";
    ifo += "wordcount=" + std::to_string(words.size()) + "\n";
    if (!syns.empty()) ifo += "synwordcount=" + std::to_string(syns.size()) + "\n";
    ifo += "idxfilesize=" + std::to_string(idxData.size()) + "\n";
    ifo += "bookname=" + bookName + "\n";
    if (!sameTypeSeq.empty()) ifo += "sametypesequence=" + sameTypeSeq + "\n";
    WriteFile(base + ".ifo", ifo);
}

void TestDictTextNormalization() {
    assert(DictText::CleanToken("“Whispered,”") == "Whispered");
    assert(DictText::CleanToken("«l’homme»!") == "l'homme");
    assert(DictText::CleanToken(std::string(1, DICT_STYLE_BOLD_ON) + "hello." + std::string(1, DICT_STYLE_BOLD_OFF)) == "hello");
    assert(DictText::ToLowerUtf8("Été") == "été");
    assert(DictText::ToLowerUtf8("HÄUSER") == "häuser");
    assert(DictText::ToLowerUtf8("ПРИВЕТ") == "привет");
    assert(DictText::IsCjkCodepoint(0x56F3)); // 図
    assert(!DictText::IsCjkCodepoint('A'));
    std::cout << "[PASS] DictText normalization\n";
}


} // namespace

int main() {
    TestDictTextNormalization();
    std::cout << "NORMALIZATION TESTS PASSED\n";
    return 0;
}

#include "dict_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app.h"
#include "book.h"
#include "page.h"
#include "text.h"

namespace {

static const int kHeaderHeight = 28;
static const int kFooterHeight = 24;
static const int kDefMarginLeft = 8;
static const int kDefMarginRight = 10;

static u16 MakeColor(u8 r, u8 g, u8 b) {
    return RGB15(r, g, b) | BIT(15);
}

static u16 BlendHighlight(u16 pixel, bool invert) {
    u8 r = pixel & 0x1F;
    u8 g = (pixel >> 5) & 0x1F;
    u8 b = (pixel >> 10) & 0x1F;
    if (!invert) {
        // Warm yellow-amber tint on light background (preserves antialiased black text)
        u8 nr = static_cast<u8>((r * 3 + 31) / 4);
        u8 ng = static_cast<u8>((g * 3 + 29) / 4);
        u8 nb = static_cast<u8>((b * 2 + 10) / 4);
        return MakeColor(nr, ng, nb);
    } else {
        // Cool teal-blue tint on dark background (preserves light text)
        u8 nr = static_cast<u8>((r * 2 + 4) / 4);
        u8 ng = static_cast<u8>((g * 3 + 14) / 4);
        u8 nb = static_cast<u8>((b * 3 + 22) / 4);
        return MakeColor(nr, ng, nb);
    }
}

// Fallback ASCII/Latin transcription ONLY when the active font lacks a glyph
// (e.g., IPA symbols in Liberation fonts). If the user installs an IPA-capable
// font like DejaVu Serif or Gentium, FT_Get_Char_Index() succeeds and real IPA renders.
static const char* MissingGlyphFallback(uint32_t cp) {
    if (DictText::IsCombiningMark(cp)) return "";
    switch (cp) {
        case 0x0250: return "a";   // ɐ
        case 0x0251: return "a";   // ɑ
        case 0x0252: return "o";   // ɒ
        case 0x0254: return "o";   // ɔ
        case 0x0259: return "e";   // ə
        case 0x025A: return "er";  // ɚ
        case 0x025B: return "e";   // ɛ
        case 0x025C: return "er";  // ɜ
        case 0x025D: return "er";  // ɝ
        case 0x0261: return "g";   // ɡ
        case 0x0268: return "i";   // ɨ
        case 0x026A: return "i";   // ɪ
        case 0x026B: return "l";   // ɫ
        case 0x026F: return "u";   // ɯ
        case 0x0272: return "ny";  // ɲ
        case 0x0275: return "o";   // ɵ
        case 0x0279: return "r";   // ɹ
        case 0x027E: return "r";   // ɾ
        case 0x0281: return "r";   // ʁ
        case 0x0283: return "sh";  // ʃ
        case 0x0289: return "u";   // ʉ
        case 0x028A: return "u";   // ʊ
        case 0x028B: return "v";   // ʋ
        case 0x028C: return "u";   // ʌ
        case 0x028E: return "ly";  // ʎ
        case 0x028F: return "y";   // ʏ
        case 0x0292: return "zh";  // ʒ
        case 0x0294: return "'";   // ʔ
        case 0x02B0: return "h";   // ʰ
        case 0x02B2: return "y";   // ʲ
        case 0x02B7: return "w";   // ʷ
        case 0x02C8: return "'";   // ˈ (primary stress)
        case 0x02CC: return ",";   // ˌ (secondary stress)
        case 0x02D0: return ":";   // ː (long vowel)
        case 0x02D1: return ":";   // ˑ (half-long)
        case 0x03B8: return "th";  // θ
        case 0x00F0: return "dh";  // ð
        case 0x014B: return "ng";  // ŋ
        case 0x00E6: return "ae";  // æ
        case 0x0153: return "oe";  // œ
        case 0x00F8: return "oe";  // ø
        case 0x03B2: return "b";   // β
        case 0x0263: return "g";   // ɣ
        case 0x03C7: return "kh";  // χ
        case 0x2013: return "-";   // –
        case 0x2014: return "--";  // —
        case 0x2022: return "-";   // •
        case 0x2026: return "..."; // …
        default: return nullptr;
    }
}

static bool HasGlyph(Text* ts, uint32_t cp, u8 style) {
    FT_Face face = ts->GetFace(style);
    if (!face) return true;
    return FT_Get_Char_Index(face, cp) != 0;
}

static int MeasureCodepoint(Text* ts, uint32_t cp, u8 style) {
    if (cp >= 0x80 && !HasGlyph(ts, cp, style)) {
        const char* fb = MissingGlyphFallback(cp);
        if (fb) {
            int w = 0;
            for (const char* p = fb; *p; p++) {
                w += ts->GetAdvance(static_cast<u32>(static_cast<unsigned char>(*p)), style);
            }
            return w;
        }
    }
    return ts->GetAdvance(cp, style);
}

} // namespace

DictUI::DictUI() {
    Reset();
}

void DictUI::Reset() {
    touchActive = false;
    touchHoldFrames = 0;
    touchStartX = touchStartY = touchLastX = touchLastY = 0;
    yHoldActive = false;
    yHoldFrames = 0;

    enteredByTouch = false;
    cancelled = false;
    targetScreenIdx = 1;
    selectedWordIdx = -1;
    selectHoldDir = 0;
    selectHoldFrames = 0;

    hasHighlight = false;
    hlScreenIdx = 0;
    hlX = hlY = hlW = hlH = 0;

    currentResult = LookupResult();
    defScreenIdx = 0;
    nearbyCursor = -1;
    wrappedLines.clear();
    scrollLine = 0;
    viewportLines = 10;
    scrollHoldDir = 0;
    scrollHoldFrames = 0;
    defDirty = false;

    viewTouchDown = false;
    viewTouchStartX = viewTouchStartY = 0;
    viewTouchStartScroll = 0;
    viewTouchDragged = false;
}


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

u8 DictUI::GetTouchScreenIdx(const App* app) const {
    // When orientation == false (Turned Left), Sub (screenright = 1) is the physical touch screen.
    // When orientation == true (Turned Right), lcdSwap() puts Main (screenleft = 0) on the physical touch screen.
    return app->orientation ? 0 : 1;
}

bool DictUI::IsInInnerGutter(const App* app, u16 ux) const {
    if (app->orientation) {
        // Touch screen is left page; hinge/gutter is on its right edge.
        return ux >= static_cast<u16>(app->ts->display.width - app->ts->margin.right - 2);
    } else {
        // Touch screen is right page; hinge/gutter is on its left edge.
        return ux <= static_cast<u16>(app->ts->margin.left + 2);
    }
}

bool DictUI::IsInOuterEdge(const App* app, u16 ux) const {
    if (app->orientation) {
        // Touch screen is left page; outer edge away from hinge is on its left edge.
        return ux <= static_cast<u16>(app->ts->margin.left);
    } else {
        // Touch screen is right page; outer edge away from hinge is on its right edge.
        return ux >= static_cast<u16>(app->ts->display.width - app->ts->margin.right);
    }
}

bool DictUI::IsInCancelZone(const App* app, u16 uy) const {
    return uy >= static_cast<u16>(app->ts->display.height - app->ts->margin.bottom);
}

int DictUI::FindNearestWord(const App* app, u8 screenIdx, u16 ux, u16 uy) const {
    int bestIdx = -1;
    int bestDist = 0x7FFFFFFF;

    for (size_t i = 0; i < app->pageWords.size(); i++) {
        const WordBox& wb = app->pageWords[i];
        if (wb.screenIdx != screenIdx) continue;

        int x0 = wb.x;
        int x1 = wb.x + wb.w;
        int y0 = wb.y;
        int y1 = wb.y + wb.h;

        int dx = 0;
        if (ux < x0) dx = x0 - ux;
        else if (ux > x1) dx = ux - x1;

        int dy = 0;
        if (uy < y0) dy = y0 - uy;
        else if (uy > y1) dy = uy - y1;

        // Direct hit inside bounding box wins immediately.
        if (dx == 0 && dy == 0) return static_cast<int>(i);

        // Weight vertical distance slightly higher so dragging stays on the intended line.
        int dist = dx * dx + (dy * 2) * (dy * 2);
        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = static_cast<int>(i);
        }
    }
    return bestIdx;
}

void DictUI::ClearHighlight(App* app) {
    if (!hasHighlight) return;
    u16* fb = (hlScreenIdx == 0) ? app->ts->screenleft : app->ts->screenright;
    int stride = app->ts->display.height; // 256
    int idx = 0;
    for (u16 y = hlY; y < hlY + hlH; y++) {
        for (u16 x = hlX; x < hlX + hlW; x++) {
            fb[y * stride + x] = savedPixels[idx++];
        }
    }
    hasHighlight = false;
}

void DictUI::ApplyHighlight(App* app, int wordIdx) {
    if (wordIdx == selectedWordIdx && hasHighlight) return;
    ClearHighlight(app);
    selectedWordIdx = wordIdx;
    if (wordIdx < 0 || wordIdx >= static_cast<int>(app->pageWords.size())) return;

    const WordBox& wb = app->pageWords[wordIdx];
    u16 x0 = (wb.x > 1) ? (wb.x - 1) : 0;
    u16 y0 = (wb.y > 0) ? wb.y : 0;
    u16 x1 = static_cast<u16>(std::min<int>(app->ts->display.width, wb.x + wb.w + 1));
    u16 y1 = static_cast<u16>(std::min<int>(app->ts->display.height, wb.y + wb.h));
    if (x1 <= x0 || y1 <= y0) return;

    u16 w = x1 - x0;
    u16 h = y1 - y0;
    if (static_cast<int>(w) * static_cast<int>(h) > kMaxHighlightPx) {
        h = static_cast<u16>(kMaxHighlightPx / w);
    }

    u16* fb = (wb.screenIdx == 0) ? app->ts->screenleft : app->ts->screenright;
    int stride = app->ts->display.height;
    bool inv = app->ts->GetInvert();

    int idx = 0;
    for (u16 y = y0; y < y0 + h; y++) {
        for (u16 x = x0; x < x0 + w; x++) {
            u16 orig = fb[y * stride + x];
            savedPixels[idx++] = orig;
            fb[y * stride + x] = BlendHighlight(orig, inv);
        }
    }

    hlScreenIdx = wb.screenIdx;
    hlX = x0;
    hlY = y0;
    hlW = w;
    hlH = h;
    hasHighlight = true;
}

void DictUI::DrawSelectHint(App* app, const char* msg) {
    u16* prevScreen = app->ts->GetScreen();
    int prevStyle = app->ts->GetStyle();
    bool prevInv = app->ts->GetInvert();

    // Draw hint in bottom status strip of screenleft (same area as UpdateClock/PrintStatus).
    app->ts->SetScreen(app->ts->screenleft);
    app->ts->SetStyle(TEXT_STYLE_BROWSER);
    app->ts->ClearRect(0, 240, app->ts->display.width, app->ts->display.height);
    app->ts->SetPen(6, 252);
    if (msg && msg[0]) {
        bool b = false, it = false;
        DrawStyledText(app, app->ts->screenleft, 6, 252, msg, app->ts->display.width - 4, false, false, b, it);
    }

    app->ts->SetInvert(prevInv);
    app->ts->SetStyle(prevStyle);
    app->ts->SetScreen(prevScreen);
}

bool DictUI::HandleBookInput(App* app, bool& outTurnPrev, bool& outTurnNext, bool& outBookmark) {
    outTurnPrev = false;
    outTurnNext = false;
    outBookmark = false;

    u32 down = keysDown();
    u32 held = keysHeld();
    u32 up = keysUp();

    // --- 1. Stylus tap vs hold-to-lookup ---
    if (down & KEY_TOUCH) {
        touchPosition coord = app->TouchRead();
        touchActive = true;
        touchHoldFrames = 0;
        touchStartX = coord.py;
        touchStartY = coord.px;
        touchLastX = coord.py;
        touchLastY = coord.px;
        return true;
    }

    if (touchActive) {
        if (held & KEY_TOUCH) {
            touchPosition coord = app->TouchRead();
            touchLastX = coord.py;
            touchLastY = coord.px;
            touchHoldFrames++;

            if (touchHoldFrames == kHoldFrames) {
                if (!app->dictManager.HasDictionary()) {
                    DrawSelectHint(app, "no dict in Dictionary/");
                } else if (!app->pageWords.empty()) {
                    touchActive = false;
                    BeginTouchSelection(app, touchLastX, touchLastY);
                    return true;
                }
            }
            return true;
        }
        if (up & KEY_TOUCH) {
            touchActive = false;
            if (touchHoldFrames < kHoldFrames) {
                // Short tap: preserve original page-turn gesture (coord.py < 95).
                if (touchStartX < 95) outTurnPrev = true;
                else outTurnNext = true;
            } else {
                // Held with no dictionary loaded; clear hint.
                DrawSelectHint(app, "");
            }
            return true;
        }
    }

    // --- 2. Y button: short press = bookmark, hold >= 15 frames = D-pad lookup ---
    if (down & KEY_Y) {
        yHoldActive = true;
        yHoldFrames = 0;
        return true;
    }
    if (yHoldActive) {
        if (held & KEY_Y) {
            yHoldFrames++;
            if (yHoldFrames == kHoldFrames) {
                yHoldActive = false;
                if (!app->dictManager.HasDictionary()) {
                    DrawSelectHint(app, "no dict in Dictionary/");
                } else if (!app->pageWords.empty()) {
                    BeginDpadSelection(app);
                }
            }
            return true;
        }
        if (up & KEY_Y) {
            yHoldActive = false;
            if (yHoldFrames < kHoldFrames) {
                outBookmark = true;
            }
            return true;
        }
    }

    return false;
}

void DictUI::BeginTouchSelection(App* app, u16 ux, u16 uy) {
    enteredByTouch = true;
    cancelled = false;
    selectedWordIdx = -1;
    hasHighlight = false;

    u8 touchIdx = GetTouchScreenIdx(app);
    targetScreenIdx = IsInInnerGutter(app, ux) ? static_cast<u8>(1 - touchIdx) : touchIdx;

    int idx = FindNearestWord(app, targetScreenIdx, ux, uy);
    if (idx < 0) {
        // Fallback to the other screen if the current screen has no words.
        targetScreenIdx = 1 - targetScreenIdx;
        idx = FindNearestWord(app, targetScreenIdx, ux, uy);
    }

    ApplyHighlight(app, idx);
    app->mode = APP_MODE_DICT_SELECT;
    DrawSelectHint(app, "release:lookup  bottom:cancel");
}

void DictUI::BeginDpadSelection(App* app) {
    enteredByTouch = false;
    cancelled = false;
    selectedWordIdx = -1;
    selectHoldDir = 0;
    selectHoldFrames = 0;
    hasHighlight = false;

    targetScreenIdx = app->pageWords[0].screenIdx;
    ApplyHighlight(app, 0);
    app->mode = APP_MODE_DICT_SELECT;
    DrawSelectHint(app, "D-pad:move  A:lookup  B:cancel");
}

void DictUI::CancelSelection(App* app) {
    ClearHighlight(app);
    selectedWordIdx = -1;
    selectHoldDir = 0;
    selectHoldFrames = 0;
    cancelled = false;
    DrawSelectHint(app, "");
    app->mode = APP_MODE_BOOK;
}

void DictUI::HandleSelectEvent(App* app) {
    u32 down = keysDown();
    u32 held = keysHeld();
    u32 up = keysUp();

    if (down & KEY_B) {
        CancelSelection(app);
        return;
    }

    if (enteredByTouch) {
        if (held & KEY_TOUCH) {
            touchPosition coord = app->TouchRead();
            u16 ux = coord.py;
            u16 uy = coord.px;

            if (IsInCancelZone(app, uy)) {
                if (!cancelled) {
                    cancelled = true;
                    ClearHighlight(app);
                    selectedWordIdx = -1;
                    DrawSelectHint(app, "release to cancel");
                }
                return;
            }

            if (cancelled) {
                cancelled = false;
                DrawSelectHint(app, "release:lookup  bottom:cancel");
            }

            u8 touchIdx = GetTouchScreenIdx(app);
            // Dragging into the inner hinge switches to the opposite page;
            // dragging into the outer edge away from the hinge switches back to the touch page.
            if (targetScreenIdx == touchIdx && IsInInnerGutter(app, ux)) {
                targetScreenIdx = 1 - touchIdx;
            } else if (targetScreenIdx != touchIdx && IsInOuterEdge(app, ux)) {
                targetScreenIdx = touchIdx;
            }

            int idx = FindNearestWord(app, targetScreenIdx, ux, uy);
            if (idx >= 0) {
                ApplyHighlight(app, idx);
            }
            return;
        }

        if (up & KEY_TOUCH || !(held & KEY_TOUCH)) {
            if (cancelled || selectedWordIdx < 0) {
                CancelSelection(app);
            } else {
                OpenDefinitionForSelected(app);
            }
            return;
        }
    } else {
        // D-pad / button selection mode (also allows stylus tap to jump directly).
        if (down & KEY_A) {
            if (selectedWordIdx >= 0) {
                OpenDefinitionForSelected(app);
            } else {
                CancelSelection(app);
            }
            return;
        }

        int n = static_cast<int>(app->pageWords.size());
        if (n == 0) {
            CancelSelection(app);
            return;
        }

        // Note: app->key.* are in pre-rotation hardware space (matching app_book/app_browser/font):
        //   Visual RIGHT = app->key.down, Visual LEFT = app->key.up,
        //   Visual DOWN  = app->key.left, Visual UP   = app->key.right.
        int curDir = 0;
        if (held & app->key.down) curDir = 1;       // visual right (next word)
        else if (held & app->key.up) curDir = 2;    // visual left (prev word)
        else if (held & app->key.left) curDir = 3;  // visual down (next line)
        else if (held & app->key.right) curDir = 4; // visual up (prev line)

        int triggerDir = 0;
        if (down & app->key.down) { triggerDir = 1; selectHoldDir = 1; selectHoldFrames = 0; }
        else if (down & app->key.up) { triggerDir = 2; selectHoldDir = 2; selectHoldFrames = 0; }
        else if (down & app->key.left) { triggerDir = 3; selectHoldDir = 3; selectHoldFrames = 0; }
        else if (down & app->key.right) { triggerDir = 4; selectHoldDir = 4; selectHoldFrames = 0; }
        else if (curDir != 0 && curDir == selectHoldDir) {
            selectHoldFrames++;
            int interval = (curDir <= 2) ? kSelectWordInterval : kSelectLineInterval;
            if (selectHoldFrames >= kSelectDelayFrames &&
                ((selectHoldFrames - kSelectDelayFrames) % interval) == 0) {
                triggerDir = curDir;
            }
        } else {
            selectHoldDir = curDir;
            selectHoldFrames = 0;
        }

        int nextIdx = selectedWordIdx;
        if (triggerDir == 1) {
            nextIdx = (selectedWordIdx + 1 < n) ? (selectedWordIdx + 1) : 0;
        } else if (triggerDir == 2) {
            nextIdx = (selectedWordIdx > 0) ? (selectedWordIdx - 1) : (n - 1);
        } else if (triggerDir == 3) {
            // Jump to first word on next visual line
            const WordBox& cur = app->pageWords[selectedWordIdx];
            for (int i = selectedWordIdx + 1; i < n; i++) {
                if (app->pageWords[i].screenIdx != cur.screenIdx ||
                    app->pageWords[i].y > cur.y + cur.h / 2) {
                    nextIdx = i;
                    break;
                }
            }
        } else if (triggerDir == 4) {
            // Jump to word on previous visual line
            const WordBox& cur = app->pageWords[selectedWordIdx];
            for (int i = selectedWordIdx - 1; i >= 0; i--) {
                if (app->pageWords[i].screenIdx != cur.screenIdx ||
                    app->pageWords[i].y + app->pageWords[i].h / 2 < cur.y) {
                    nextIdx = i;
                    break;
                }
            }
        } else if (down & (app->key.l | app->key.r)) {
            // Toggle between left and right screens
            u8 other = 1 - app->pageWords[selectedWordIdx].screenIdx;
            for (int i = 0; i < n; i++) {
                if (app->pageWords[i].screenIdx == other) {
                    nextIdx = i;
                    break;
                }
            }
        } else if (down & KEY_TOUCH) {
            // Switch seamlessly to stylus drag from D-pad mode
            touchPosition coord = app->TouchRead();
            enteredByTouch = true;
            u8 touchIdx = GetTouchScreenIdx(app);
            targetScreenIdx = IsInInnerGutter(app, coord.py) ? static_cast<u8>(1 - touchIdx) : touchIdx;
            int idx = FindNearestWord(app, targetScreenIdx, coord.py, coord.px);
            if (idx >= 0) nextIdx = idx;
        }

        if (nextIdx != selectedWordIdx && nextIdx >= 0) {
            ApplyHighlight(app, nextIdx);
        }
    }
}

void DictUI::OpenDefinitionForSelected(App* app) {
    if (!app->bookcurrent || selectedWordIdx < 0 ||
        selectedWordIdx >= static_cast<int>(app->pageWords.size())) {
        CancelSelection(app);
        return;
    }

    const WordBox& wb = app->pageWords[selectedWordIdx];
    Page* page = app->bookcurrent->GetPage();
    if (!page) {
        CancelSelection(app);
        return;
    }

    std::string rawWord = page->ExtractWord(wb);
    u8 targetDefScreen = 1 - wb.screenIdx; // Opposite screen keeps sentence visible!

    DrawSelectHint(app, "looking up...");
    LookupResult res;
    app->dictManager.Lookup(rawWord, res, wb.isCjk);

    ShowLookupResult(app, res, targetDefScreen);
}

void DictUI::ShowLookupResult(App* app, const LookupResult& res, u8 targetDefScreen) {
    currentResult = res;
    defScreenIdx = targetDefScreen;
    nearbyCursor = -1;
    scrollLine = 0;
    scrollHoldDir = 0;
    scrollHoldFrames = 0;
    viewTouchDown = false;
    viewTouchDragged = false;

    WrapDefinitionLines(app);
    if (defScreenIdx != 0) {
        DrawSelectHint(app, "");
    }
    app->mode = APP_MODE_DICT_VIEW;
    defDirty = true;
    DrawDefinitionView(app);
}

void DictUI::CloseDefinition(App* app) {
    ClearHighlight(app);
    selectedWordIdx = -1;
    app->mode = APP_MODE_BOOK;
    if (app->bookcurrent && app->bookcurrent->GetPage()) {
        app->bookcurrent->GetPage()->Draw(app->ts);
    }
}

void DictUI::WrapDefinitionLines(App* app) {
    wrappedLines.clear();
    Text* ts = app->ts;
    int maxW = ts->display.width - kDefMarginLeft - kDefMarginRight;

    // Save font style state
    int savedStyle = ts->GetStyle();
    ts->SetStyle(TEXT_STYLE_REGULAR);

    std::string source;
    if (currentResult.found && !currentResult.definition.empty()) {
        source = currentResult.definition;
    } else if (!currentResult.found) {
        source = "No exact entry found.";
        if (!currentResult.nearby.empty()) {
            source += "\n\nNearby headwords (L/R to browse):";
            for (size_t i = 0; i < currentResult.nearby.size(); i++) {
                source += "\n- ";
                source += currentResult.nearby[i];
            }
        }
    } else {
        source = "(empty definition)";
    }

    bool bold = false;
    bool italic = false;

    auto currentFaceStyle = [&]() -> u8 {
        if (bold && italic) return TEXT_STYLE_BOLDITALIC;
        if (bold) return TEXT_STYLE_BOLD;
        if (italic) return TEXT_STYLE_ITALIC;
        return TEXT_STYLE_REGULAR;
    };

    auto prefixStyleBytes = [&]() -> std::string {
        std::string p;
        if (bold) p.push_back(static_cast<char>(TEXT_BOLD_ON));
        if (italic) p.push_back(static_cast<char>(TEXT_ITALIC_ON));
        return p;
    };

    std::string curLine = prefixStyleBytes();
    int curW = 0;
    size_t i = 0;

    while (i < source.size()) {
        unsigned char c = static_cast<unsigned char>(source[i]);
        if (c == '\n') {
            wrappedLines.push_back(curLine);
            curLine = prefixStyleBytes();
            curW = 0;
            i++;
            continue;
        }
        if (c == TEXT_BOLD_ON) {
            bold = true;
            curLine.push_back(static_cast<char>(c));
            i++;
            continue;
        }
        if (c == TEXT_BOLD_OFF) {
            bold = false;
            curLine.push_back(static_cast<char>(c));
            i++;
            continue;
        }
        if (c == TEXT_ITALIC_ON) {
            italic = true;
            curLine.push_back(static_cast<char>(c));
            i++;
            continue;
        }
        if (c == TEXT_ITALIC_OFF) {
            italic = false;
            curLine.push_back(static_cast<char>(c));
            i++;
            continue;
        }

        if (c == ' ') {
            u8 adv = ts->GetAdvance(' ', currentFaceStyle());
            if (curW > 0 && curW + adv <= maxW) {
                curLine.push_back(' ');
                curW += adv;
            }
            i++;
            continue;
        }

        // Measure next word (up to whitespace, newline, or CJK break)
        size_t wordStart = i;
        int wordW = 0;
        bool simBold = bold;
        bool simItalic = italic;
        size_t j = i;
        while (j < source.size()) {
            unsigned char ch = static_cast<unsigned char>(source[j]);
            if (ch == ' ' || ch == '\n') break;
            if (ch == TEXT_BOLD_ON) { simBold = true; j++; continue; }
            if (ch == TEXT_BOLD_OFF) { simBold = false; j++; continue; }
            if (ch == TEXT_ITALIC_ON) { simItalic = true; j++; continue; }
            if (ch == TEXT_ITALIC_OFF) { simItalic = false; j++; continue; }

            uint32_t cp = 0;
            size_t step = DictText::Utf8Decode(source.data() + j, source.size() - j, &cp);
            if (step == 0) step = 1;
            u8 st = (simBold && simItalic) ? TEXT_STYLE_BOLDITALIC
                  : (simBold ? TEXT_STYLE_BOLD : (simItalic ? TEXT_STYLE_ITALIC : TEXT_STYLE_REGULAR));
            int adv = MeasureCodepoint(ts, cp, st);

            // Allow wrapping between CJK chars if word exceeds width
            if (DictText::IsCjkCodepoint(cp) && j > wordStart) break;
            if (wordW + adv > maxW && j > wordStart) break;

            wordW += adv;
            j += step;
            if (DictText::IsCjkCodepoint(cp)) break;
        }

        if (curW > 0 && curW + wordW > maxW) {
            wrappedLines.push_back(curLine);
            curLine = prefixStyleBytes();
            curW = 0;
        }

        // Append word bytes and update bold/italic state
        for (size_t k = wordStart; k < j; k++) {
            unsigned char ch = static_cast<unsigned char>(source[k]);
            if (ch == TEXT_BOLD_ON) bold = true;
            else if (ch == TEXT_BOLD_OFF) bold = false;
            else if (ch == TEXT_ITALIC_ON) italic = true;
            else if (ch == TEXT_ITALIC_OFF) italic = false;
            curLine.push_back(source[k]);
        }
        curW += wordW;
        i = j;
    }

    if (!curLine.empty()) {
        wrappedLines.push_back(curLine);
    }

    int lineH = ts->GetHeight();
    if (lineH < 8) lineH = 12;
    int bodyH = ts->display.height - kHeaderHeight - kFooterHeight - 6;
    viewportLines = std::max(1, bodyH / lineH);

    ts->SetStyle(savedStyle);
}

void DictUI::DrawHLine(u16* fb, u16 x0, u16 x1, u16 y, u16 color) {
    int stride = 256;
    for (u16 x = x0; x < x1; x++) {
        fb[y * stride + x] = color;
    }
}

void DictUI::FillRect(u16* fb, u16 x0, u16 y0, u16 x1, u16 y1, u16 color) {
    int stride = 256;
    for (u16 y = y0; y < y1; y++) {
        for (u16 x = x0; x < x1; x++) {
            fb[y * stride + x] = color;
        }
    }
}

void DictUI::DrawStyledText(App* app, u16* fb, u16 x, u16 y, const std::string& text,
                            int maxX, bool initialBold, bool initialItalic,
                            bool& outBold, bool& outItalic) {
    Text* ts = app->ts;
    ts->SetScreen(fb);
    ts->SetPen(x, y);

    bool bold = initialBold;
    bool italic = initialItalic;

    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == TEXT_BOLD_ON) { bold = true; i++; continue; }
        if (c == TEXT_BOLD_OFF) { bold = false; i++; continue; }
        if (c == TEXT_ITALIC_ON) { italic = true; i++; continue; }
        if (c == TEXT_ITALIC_OFF) { italic = false; i++; continue; }

        uint32_t cp = 0;
        size_t step = DictText::Utf8Decode(text.data() + i, text.size() - i, &cp);
        if (step == 0) step = 1;
        i += step;

        u8 st = (bold && italic) ? TEXT_STYLE_BOLDITALIC
              : (bold ? TEXT_STYLE_BOLD : (italic ? TEXT_STYLE_ITALIC : TEXT_STYLE_REGULAR));
        if (cp >= 0x80 && !HasGlyph(ts, cp, st)) {
            const char* fb = MissingGlyphFallback(cp);
            if (fb) {
                for (const char* p = fb; *p; p++) {
                    u32 fcp = static_cast<u32>(static_cast<unsigned char>(*p));
                    u8 adv = ts->GetAdvance(fcp, st);
                    if (ts->GetPenX() + adv > maxX) break;
                    ts->PrintChar(fcp, st);
                }
                continue;
            }
        }
        u8 adv = ts->GetAdvance(cp, st);
        if (ts->GetPenX() + adv > maxX) break;
        ts->PrintChar(cp, st);
    }

    outBold = bold;
    outItalic = italic;
}

void DictUI::DrawDefinitionView(App* app) {
    Text* ts = app->ts;
    u16* prevScreen = ts->GetScreen();
    int prevStyle = ts->GetStyle();
    bool inv = ts->GetInvert();

    u16* dstFb = (defScreenIdx == 0) ? ts->screenleft : ts->screenright;
    u16* fb = ts->offscreen;
    ts->SetScreen(fb);
    ts->ClearScreen();

    u16 borderCol = inv ? MakeColor(10, 10, 12) : MakeColor(20, 20, 22);
    u16 barBg = inv ? MakeColor(4, 4, 6) : MakeColor(28, 28, 30);

    // --- Header bar ---
    FillRect(fb, 0, 0, ts->display.width, kHeaderHeight, barBg);
    DrawHLine(fb, 0, ts->display.width, kHeaderHeight, borderCol);

    std::string headerText;
    if (currentResult.found) {
        headerText.push_back(static_cast<char>(TEXT_BOLD_ON));
        headerText += currentResult.headword;
        headerText.push_back(static_cast<char>(TEXT_BOLD_OFF));
        if (!currentResult.queried.empty() && currentResult.queried != currentResult.headword) {
            headerText += " ";
            headerText.push_back(static_cast<char>(TEXT_ITALIC_ON));
            headerText += "(" + currentResult.queried + ")";
            headerText.push_back(static_cast<char>(TEXT_ITALIC_OFF));
        }
    } else {
        headerText.push_back(static_cast<char>(TEXT_BOLD_ON));
        headerText += currentResult.queried.empty() ? "Dictionary" : currentResult.queried;
        headerText.push_back(static_cast<char>(TEXT_BOLD_OFF));
    }

    bool dummyB = false, dummyI = false;
    DrawStyledText(app, fb, kDefMarginLeft, 18, headerText, ts->display.width - 8, false, false, dummyB, dummyI);

    // --- Scrollable body ---
    ts->SetStyle(TEXT_STYLE_REGULAR);
    int lineH = ts->GetHeight();
    if (lineH < 8) lineH = 12;

    int maxScroll = std::max(0, static_cast<int>(wrappedLines.size()) - viewportLines);
    if (scrollLine > maxScroll) scrollLine = maxScroll;
    if (scrollLine < 0) scrollLine = 0;

    int y = kHeaderHeight + lineH + 2;
    int endLine = std::min<int>(static_cast<int>(wrappedLines.size()), scrollLine + viewportLines);
    for (int ln = scrollLine; ln < endLine; ln++) {
        bool b = false, it = false;
        DrawStyledText(app, fb, kDefMarginLeft, static_cast<u16>(y), wrappedLines[ln],
                       ts->display.width - kDefMarginRight, false, false, b, it);
        y += lineH;
    }

    // --- Scrollbar (right edge) ---
    if (static_cast<int>(wrappedLines.size()) > viewportLines) {
        int trackTop = kHeaderHeight + 4;
        int trackBottom = ts->display.height - kFooterHeight - 4;
        int trackH = trackBottom - trackTop;
        int thumbH = std::max(10, (trackH * viewportLines) / static_cast<int>(wrappedLines.size()));
        int thumbY = trackTop + ((trackH - thumbH) * scrollLine) / std::max(1, maxScroll);
        FillRect(fb, ts->display.width - 4, trackTop, ts->display.width - 2, trackBottom, barBg);
        FillRect(fb, ts->display.width - 4, thumbY, ts->display.width - 2, thumbY + thumbH, borderCol);
    }

    // --- Footer bar ---
    int footerY = ts->display.height - kFooterHeight;
    FillRect(fb, 0, footerY, ts->display.width, ts->display.height, barBg);
    DrawHLine(fb, 0, ts->display.width, footerY, borderCol);

    std::string footer;
    if (!currentResult.nearby.empty()) {
        footer = "B:close  Up/Dn:scroll  L/R:near";
    } else {
        footer = "B:close  Up/Dn:scroll";
    }
    ts->SetStyle(TEXT_STYLE_BROWSER);
    DrawStyledText(app, fb, 6, static_cast<u16>(footerY + 15), footer, ts->display.width - 6, false, false, dummyB, dummyI);

    ts->CopyScreen(fb, dstFb);

    ts->SetStyle(prevStyle);
    ts->SetScreen(prevScreen);
    defDirty = false;
}

void DictUI::HandleViewEvent(App* app) {
    u32 down = keysDown();
    u32 held = keysHeld();
    u32 up = keysUp();

    if (down & KEY_B) {
        CloseDefinition(app);
        return;
    }

    int maxScroll = std::max(0, static_cast<int>(wrappedLines.size()) - viewportLines);

    // Note: app->key.* are in pre-rotation hardware space (DS held vertically like a book):
    //   Visual DOWN  = app->key.left   (scroll down)
    //   Visual UP    = app->key.right  (scroll up)
    //   Visual RIGHT = app->key.down   (next nearby)
    //   Visual LEFT  = app->key.up     (prev nearby)
    bool wantDown = (held & (app->key.left | KEY_A)) != 0;
    bool wantUp = (held & app->key.right) != 0;
    int curDir = wantDown ? 1 : (wantUp ? -1 : 0);

    if (down & (app->key.left | KEY_A)) {
        scrollHoldDir = 1;
        scrollHoldFrames = 0;
        if (scrollLine < maxScroll) {
            scrollLine = std::min(maxScroll, scrollLine + 2);
            defDirty = true;
        }
    } else if (down & app->key.right) {
        scrollHoldDir = -1;
        scrollHoldFrames = 0;
        if (scrollLine > 0) {
            scrollLine = std::max(0, scrollLine - 2);
            defDirty = true;
        }
    } else if (curDir != 0 && curDir == scrollHoldDir) {
        scrollHoldFrames++;
        if (scrollHoldFrames >= kScrollDelayFrames &&
            ((scrollHoldFrames - kScrollDelayFrames) % kScrollIntervalFrames) == 0) {
            if (curDir > 0 && scrollLine < maxScroll) {
                scrollLine++;
                defDirty = true;
            } else if (curDir < 0 && scrollLine > 0) {
                scrollLine--;
                defDirty = true;
            }
        }
    } else {
        scrollHoldDir = curDir;
        scrollHoldFrames = 0;
    }

    if ((down & (app->key.r | app->key.down)) && !currentResult.nearby.empty()) {
        // Jump to next nearby headword
        nearbyCursor = (nearbyCursor + 1) % static_cast<int>(currentResult.nearby.size());
        std::vector<std::string> savedNearby = currentResult.nearby;
        LookupResult nextRes;
        app->dictManager.LookupExactHeadword(savedNearby[nearbyCursor], nextRes);
        nextRes.nearby = savedNearby;
        currentResult = nextRes;
        scrollLine = 0;
        WrapDefinitionLines(app);
        defDirty = true;
    } else if ((down & (app->key.l | app->key.up)) && !currentResult.nearby.empty()) {
        // Jump to previous nearby headword
        int cnt = static_cast<int>(currentResult.nearby.size());
        nearbyCursor = (nearbyCursor <= 0) ? (cnt - 1) : (nearbyCursor - 1);
        std::vector<std::string> savedNearby = currentResult.nearby;
        LookupResult prevRes;
        app->dictManager.LookupExactHeadword(savedNearby[nearbyCursor], prevRes);
        prevRes.nearby = savedNearby;
        currentResult = prevRes;
        scrollLine = 0;
        WrapDefinitionLines(app);
        defDirty = true;
    }

    // Touch handling inside definition view
    u8 touchIdx = GetTouchScreenIdx(app);
    if (down & KEY_TOUCH) {
        touchPosition coord = app->TouchRead();
        viewTouchDown = true;
        viewTouchStartX = coord.py;
        viewTouchStartY = coord.px;
        viewTouchStartScroll = scrollLine;
        viewTouchDragged = false;
    } else if (viewTouchDown && (held & KEY_TOUCH)) {
        if (defScreenIdx == touchIdx) {
            touchPosition coord = app->TouchRead();
            int dy = static_cast<int>(viewTouchStartY) - static_cast<int>(coord.px);
            if (dy > 6 || dy < -6) {
                viewTouchDragged = true;
                int lineH = std::max<int>(8, app->ts->GetHeight());
                int newScroll = viewTouchStartScroll + (dy / lineH);
                newScroll = std::max(0, std::min(maxScroll, newScroll));
                if (newScroll != scrollLine) {
                    scrollLine = newScroll;
                    defDirty = true;
                }
            }
        }
    } else if (viewTouchDown && (up & KEY_TOUCH)) {
        viewTouchDown = false;
        if (!viewTouchDragged) {
            if (defScreenIdx != touchIdx) {
                // Tapping the reading screen dismisses the opposite-screen definition.
                CloseDefinition(app);
                return;
            } else {
                // On the definition touch screen: tap footer closes, tap upper/lower halves scroll.
                if (viewTouchStartY >= static_cast<u16>(app->ts->display.height - kFooterHeight)) {
                    CloseDefinition(app);
                    return;
                } else if (viewTouchStartY < static_cast<u16>(app->ts->display.height / 2)) {
                    if (scrollLine > 0) {
                        scrollLine = std::max(0, scrollLine - viewportLines + 1);
                        defDirty = true;
                    }
                } else {
                    if (scrollLine < maxScroll) {
                        scrollLine = std::min(maxScroll, scrollLine + viewportLines - 1);
                        defDirty = true;
                    }
                }
            }
        }
    }

    if (defDirty) {
        DrawDefinitionView(app);
    }
}

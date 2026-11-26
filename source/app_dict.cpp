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


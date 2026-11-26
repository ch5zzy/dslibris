#pragma once

#include <nds.h>
#include <string>
#include <vector>
#include "dict_lookup.h"

class App;
struct WordBox;

// Manages stylus tap-hold-drag word selection, D-pad word stepping,
// flicker-free VRAM word highlighting, and the opposite-screen definition view.
class DictUI {
public:
    DictUI();

    void Reset();

    // Called each frame from App::HandleEventInBook() to distinguish
    // short tap (page turn) vs hold (enter word selection), and short Y (bookmark)
    // vs hold Y (D-pad word selection).
    // Returns true if the touch/Y input was consumed (page-turn or mode transition).
    bool HandleBookInput(App* app, bool& outTurnPrev, bool& outTurnNext, bool& outBookmark);

    // Event loop handlers for APP_MODE_DICT_SELECT and APP_MODE_DICT_VIEW.
    void HandleSelectEvent(App* app);
    void HandleViewEvent(App* app);

private:
    static const int kHoldFrames = 15;       // ~250ms at 60Hz
    static const int kScrollDelayFrames = 30;  // 0.5s at 60Hz before continuous scroll
    static const int kScrollIntervalFrames = 8; // ~7.5 lines/sec while holding
    static const int kSelectDelayFrames = 24;   // ~0.4s at 60Hz before continuous word stepping
    static const int kSelectWordInterval = 5;   // ~12 words/sec while holding left/right
    static const int kSelectLineInterval = 7;   // ~8.5 lines/sec while holding up/down
    static const int kMaxHighlightPx = 192 * 28;

    // Book-mode hold trackers
    bool touchActive;
    int touchHoldFrames;
    u16 touchStartX;
    u16 touchStartY;
    u16 touchLastX;
    u16 touchLastY;

    bool yHoldActive;
    int yHoldFrames;

    // Selection-mode state
    bool enteredByTouch;
    bool cancelled;
    u8 targetScreenIdx;   // 0 = screenleft, 1 = screenright
    int selectedWordIdx;
    int selectHoldDir;    // 0 = none, 1 = right, 2 = left, 3 = down, 4 = up
    int selectHoldFrames;

    // Flicker-free VRAM highlight save buffer
    bool hasHighlight;
    u8 hlScreenIdx;
    u16 hlX, hlY, hlW, hlH;
    u16 savedPixels[kMaxHighlightPx];

    // Definition-view state
    LookupResult currentResult;
    u8 defScreenIdx;      // Screen displaying the definition (opposite of selected word)
    int nearbyCursor;     // Index into currentResult.nearby when browsing nearby words
    std::vector<std::string> wrappedLines;
    int scrollLine;
    int viewportLines;
    int scrollHoldDir;    // -1 = up, +1 = down, 0 = none
    int scrollHoldFrames;
    bool defDirty;

    // Definition touch-scroll state
    bool viewTouchDown;
    u16 viewTouchStartX;
    u16 viewTouchStartY;
    int viewTouchStartScroll;
    bool viewTouchDragged;

    u8 GetTouchScreenIdx(const App* app) const;
    bool IsInInnerGutter(const App* app, u16 ux) const;
    bool IsInOuterEdge(const App* app, u16 ux) const;
    bool IsInCancelZone(const App* app, u16 uy) const;
    int FindNearestWord(const App* app, u8 screenIdx, u16 ux, u16 uy) const;

    void ClearHighlight(App* app);
    void ApplyHighlight(App* app, int wordIdx);
    void DrawSelectHint(App* app, const char* msg);

    void BeginTouchSelection(App* app, u16 ux, u16 uy);
    void BeginDpadSelection(App* app);
    void CancelSelection(App* app);
    void OpenDefinitionForSelected(App* app);
    void ShowLookupResult(App* app, const LookupResult& res, u8 targetDefScreen);
    void CloseDefinition(App* app);

    void WrapDefinitionLines(App* app);
    void DrawDefinitionView(App* app);
    void DrawStyledText(App* app, u16* fb, u16 x, u16 y, const std::string& text,
                        int maxX, bool initialBold, bool initialItalic,
                        bool& outBold, bool& outItalic);
    void DrawHLine(u16* fb, u16 x0, u16 x1, u16 y, u16 color);
    void FillRect(u16* fb, u16 x0, u16 y0, u16 x1, u16 y1, u16 color);
};

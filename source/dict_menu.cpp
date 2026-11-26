#include "dict_menu.h"

#include "app.h"
#include "button.h"

DictMenu::DictMenu(App* _app) : Menu(_app) {
    Rebuild();
}

DictMenu::~DictMenu() {
    for (auto button : buttons) {
        delete button;
    }
    buttons.clear();
}

void DictMenu::Rebuild() {
    for (auto button : buttons) {
        delete button;
    }
    buttons.clear();

    const auto& dicts = app->dictManager.GetDictionaries();
    for (size_t i = 0; i < dicts.size(); i++) {
        Button* b = new Button(app->ts);
        b->Init();
        b->Move(0, (buttons.size() % pagesize) * b->GetHeight());
        b->SetLabel1(dicts[i].bookName);
        if (dicts[i].wordCount > 0) {
            char sub[32];
            snprintf(sub, sizeof(sub), "%lu words", static_cast<unsigned long>(dicts[i].wordCount));
            b->SetLabel2(std::string(sub));
        }
        buttons.push_back(b);
    }

    int active = app->dictManager.GetActiveIndex();
    selected = (active >= 0 && active < static_cast<int>(buttons.size())) ? static_cast<u8>(active) : 0;
    page = selected / pagesize;
    dirty = true;
}

void DictMenu::handleInput() {
    auto keys = keysDown();
    auto key = app->key;

    if (keys & KEY_B) {
        app->ShowSettingsView();
    } else if (keys & (key.r | key.right)) {
        if (selected > 0) {
            if (selected == page * pagesize) {
                previousPage();
            } else {
                selectPrevious();
            }
        }
    } else if (keys & (key.l | key.left)) {
        if (!buttons.empty() && selected < buttons.size() - 1) {
            if (selected == page * pagesize + (pagesize - 1)) {
                nextPage();
            } else {
                selectNext();
            }
        }
    } else if (keys & KEY_A) {
        handleButtonPress();
    } else if (keys & KEY_TOUCH) {
        handleTouchInput();
    }
}

void DictMenu::handleTouchInput() {
    touchPosition coord = app->TouchRead();

    if (app->buttonprefs.EnclosesPoint(coord.py, coord.px)) {
        app->ShowSettingsView();
    } else if (app->buttonnext.EnclosesPoint(coord.py, coord.px)) {
        nextPage();
    } else if (app->buttonprev.EnclosesPoint(coord.py, coord.px)) {
        previousPage();
    } else {
        for (u8 i = page * pagesize;
             (i < buttons.size()) && (i < (page + 1) * pagesize);
             i++) {
            if (buttons[i]->EnclosesPoint(coord.py, coord.px)) {
                selected = i;
                handleButtonPress();
                break;
            }
        }
    }
}

void DictMenu::draw() {
    bool invert = app->ts->GetInvert();
    u16* screen = app->ts->GetScreen();
    int style = app->ts->GetStyle();

    app->ts->SetScreen(app->ts->screenright);
    app->ts->SetInvert(false);
    app->ts->SetStyle(TEXT_STYLE_BROWSER);
    app->ts->ClearScreen();

    if (buttons.empty()) {
        app->ts->SetPen(10, 40);
        app->ts->PrintString("No StarDict files found.");
        app->ts->SetPen(10, 64);
        app->ts->PrintString("Add .ifo/.idx/.dict(.dz)");
        app->ts->SetPen(10, 82);
        app->ts->PrintString("to the Dictionary/ folder.");
    } else {
        for (u8 i = page * pagesize;
             (i < buttons.size()) && (i < (page + 1) * pagesize);
             i++) {
            buttons[i]->Draw(app->ts->screenright, i == selected);
        }
        if (page > 0) app->buttonprev.Draw(app->ts->screenright, false);
        if (page < GetPageCount() - 1) app->buttonnext.Draw(app->ts->screenright, false);
    }
    app->buttonprefs.Label("cancel");
    app->buttonprefs.Draw(app->ts->screenright, false);

    app->ts->SetStyle(style);
    app->ts->SetInvert(invert);
    app->ts->SetScreen(screen);

    dirty = false;
}

void DictMenu::selectNext() {
    if (static_cast<size_t>(selected) + 1 < buttons.size()) {
        selected++;
        dirty = true;
    }
}

void DictMenu::selectPrevious() {
    if (selected > 0) {
        selected--;
        dirty = true;
    }
}

void DictMenu::nextPage() {
    if (static_cast<size_t>(page + 1) * pagesize < buttons.size()) {
        page++;
        selected = page * pagesize;
        dirty = true;
    }
}

void DictMenu::previousPage() {
    if (page > 0) {
        page--;
        selected = page * pagesize + (pagesize - 1);
        dirty = true;
    }
}

void DictMenu::handleButtonPress() {
    if (buttons.empty() || selected >= buttons.size()) {
        app->ShowSettingsView();
        return;
    }

    if (app->dictManager.SelectDictionary(selected)) {
        app->dictfile = app->dictManager.GetActiveIfoPath();
        app->PrefsRefreshButton(PREFS_BUTTON_DICTIONARY);
        app->prefs->Write();
    }
    app->ShowSettingsView();
}

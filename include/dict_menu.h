#pragma once

#include <nds.h>
#include <vector>
#include "button.h"
#include "menu.h"

class DictMenu : public Menu {
public:
    DictMenu(App* app);
    ~DictMenu();

    void Rebuild();
    void draw();
    void Draw() override { draw(); }
    void HandleInput(u16 keys) override { (void)keys; handleInput(); }
    void handleInput();
    inline bool isDirty() const { return dirty; }
    inline void setDirty(bool d = true) { dirty = d; }

private:
    void handleButtonPress();
    void handleTouchInput();
    void nextPage();
    void previousPage();
    void selectNext();
    void selectPrevious();
};

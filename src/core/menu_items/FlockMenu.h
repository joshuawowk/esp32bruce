#ifndef __FLOCK_MENU_H__
#define __FLOCK_MENU_H__

#if !defined(LITE_VERSION)
#include <MenuItemInterface.h>

class FlockMenu : public MenuItemInterface {
public:
    FlockMenu() : MenuItemInterface("Flock-You") {}

    void optionsMenu(void);
    void drawIcon(float scale);
    bool hasTheme() { return false; }
    // Themeless: return a reference to a persistent empty String (never a
    // temporary). checkTheme() short-circuits on hasTheme()==false, so this
    // is effectively unused, but must still be a valid reference.
    const String &themePath() override {
        static const String empty;
        return empty;
    }
};

#endif // LITE_VERSION
#endif // __FLOCK_MENU_H__

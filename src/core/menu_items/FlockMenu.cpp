#include "FlockMenu.h"

#if !defined(LITE_VERSION)
#include "core/display.h"
#include "core/utils.h"
#include "modules/flock/flockyou.h"
#include <globals.h>

void FlockMenu::optionsMenu() {
    options = {
        {"Scan All (W+B)", [=]() { flockyou_run_all(); } },
        {"BLE only",       [=]() { flockyou_run_ble(); } },
        {"WiFi link only", [=]() { flockyou_run_wifi(); }},
        {"Export to SD",   [=]() { flockyou_export(); }  },
        {"Clear session",  [=]() { flockyou_clear(); }   },
    };

    addOptionToMainMenu();
    loopOptions(options, MENU_TYPE_SUBMENU, "Flock-You");
}

void FlockMenu::drawIcon(float scale) {
    clearIconArea();

    int r = scale * 8;

    // Stylized ALPR camera on a pole.
    tft.drawRect(iconCenterX - 2 * r, iconCenterY - r, 4 * r, 2 * r, bruceConfig.priColor);
    tft.fillCircle(iconCenterX, iconCenterY, r / 2, bruceConfig.priColor);   // lens
    tft.drawLine(
        iconCenterX, iconCenterY + r, iconCenterX, iconCenterY + 3 * r, bruceConfig.priColor
    ); // pole
    // Two "flash" ticks to hint at active detection.
    tft.drawLine(
        iconCenterX + 2 * r, iconCenterY - r, iconCenterX + 3 * r, iconCenterY - 2 * r,
        bruceConfig.priColor
    );
    tft.drawLine(
        iconCenterX + 2 * r, iconCenterY, iconCenterX + 3 * r, iconCenterY, bruceConfig.priColor
    );
}

#endif // LITE_VERSION

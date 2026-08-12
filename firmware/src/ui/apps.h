#pragma once

#include <Arduino.h>

#include "../proto_gen/smartknob.pb.h"

/** Vector glyph drawn for an app or a settings row (see ui_icons.h). */
enum class AppIcon : uint8_t {
    VOLUME,
    BRIGHTNESS,
    BULB,
    FAN,
    SCROLL,
    TIMER,
    SETTINGS,
    STRENGTH,
    PRESS,
    CLICK,
    LED,
    CALIBRATE,
    PET,
};

/** How the app's position is presented on its page. */
enum class ValueStyle : uint8_t {
    PERCENT,  // 0-100 with a "%" suffix
    ONOFF,    // two positions, shown as ON / OFF
    STEPS,    // discrete positions, shown using step_labels
    COUNT,    // raw position, no unit
    MINUTES,  // raw position with a "MIN" suffix
};

/** What a short press does while the app page is open. */
enum class PressAction : uint8_t {
    NONE,        // visual acknowledgement only
    TOGGLE,      // flip between min and max position
    CYCLE,       // advance one position, wrapping at max
    MUTE,        // jump to min, remembering the previous position
    START_STOP,  // timer: start the countdown, or pause/resume a running one
    PET_MOOD,    // pet: move the creature on to its next mood
};

/** Which OS-side control, if any, mirrors this app's value over the cable. */
enum class HostChannel : uint8_t {
    NONE,
    VOLUME,
    BRIGHTNESS,
};

/** Selects the page behaviour and the renderer used for an app. */
enum class AppKind : uint8_t {
    VALUE,     // position maps straight to a value
    TIMER,     // position sets a duration that then counts down
    SETTINGS,  // opens the settings list instead of a value page
    PET,       // rotation pats a creature; the page has no value at all
};

struct AppDescriptor {
    const char* name;
    const char* caption;
    AppIcon icon;
    ValueStyle value_style;
    PressAction press_action;
    const char* const* step_labels;
    uint8_t step_label_count;
    AppKind kind;
    HostChannel host_channel;
    PB_SmartKnobConfig config;
};

/** Indices into APPS. Menu-visible entries come first, hidden ones after. */
enum : uint8_t {
    APP_VOLUME = 0,
    APP_BRIGHTNESS,
    APP_TIMER,
    APP_PET,
    APP_SETTINGS,
    APP_LIGHT,
    APP_FAN,
    APP_SCROLL,
};

extern const AppDescriptor APPS[];
extern const uint8_t APP_COUNT;

/**
 * Apps offered in the carousel, as indices into APPS. Anything left out keeps
 * its descriptor and stays reachable from code, it just is not listed; adding
 * it back to the menu is a one-line change.
 */
extern const uint8_t MENU_ITEMS[];
extern const uint8_t MENU_ITEM_COUNT;

/** Upper bound on APP_COUNT, so callers can size per-app arrays statically. */
constexpr uint8_t APP_SLOTS = 8;

/** Haptic config for the menu carousel itself, opened at the given slot. */
PB_SmartKnobConfig menuConfig(uint8_t selected);

/** Label for a position of a STEPS/ONOFF app, or nullptr if the style has no labels. */
const char* appStepLabel(const AppDescriptor& app, int32_t position);

/** Menu slot showing the given app, or 0 if the app is hidden. */
uint8_t menuSlotForApp(uint8_t app_index);

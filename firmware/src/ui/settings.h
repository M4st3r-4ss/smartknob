#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "apps.h"

/** Rows of the settings list, in the order they scroll past. */
enum class SettingId : uint8_t {
    KNOB_STRENGTH,
    CLICK_FORCE,
    MIN_BRIGHTNESS,
    LED_RING,
    LED_BRIGHTNESS,
    STRAIN_CALIBRATE,
    RESET,
};

constexpr uint8_t SETTING_COUNT = 7;

enum class SettingKind : uint8_t {
    PERCENT,  // integer with a "%" suffix
    ONOFF,    // 0 or 1, shown as ON / OFF
    ACTION,   // no value; a press runs it
};

struct SettingDescriptor {
    const char* name;
    const char* caption;
    AppIcon icon;
    SettingKind kind;
    int16_t min_value;
    int16_t max_value;
    int16_t default_value;
    /** Knob travel per step while the row is being edited. */
    float step_degrees;
};

/**
 * Press threshold as a fraction of the calibrated press force. This used to be a
 * row of its own; it is now fixed at the lightest setting that row offered,
 * which is the one worth having by default.
 */
constexpr float PRESS_THRESHOLD = 0.40f;

extern const SettingDescriptor SETTINGS[];

const SettingDescriptor& settingAt(uint8_t index);

/**
 * Settings are kept in NVS rather than in PB_PersistentConfiguration, so adding
 * one does not mean regenerating the checked-in nanopb sources.
 */
class SettingsStore {
    public:
        void begin();

        int16_t get(SettingId id) const;
        /**
         * Clamps to the descriptor's range and takes effect immediately, but only
         * in RAM: editing a row sweeps through dozens of values, so the flash
         * write waits for commit().
         */
        void set(SettingId id, int16_t value);
        /** Persists any values changed since the last commit. */
        void commit();
        /** Restores every value row to its default and persists immediately. */
        void resetToDefaults();

        /** Position of the value within its range, 0-1, for arc drawing. */
        float unit(SettingId id) const;
        bool isOn(SettingId id) const;

        /** Multiplier folded into every detent_strength_unit. */
        float knobStrengthScale() const;
        /** Multiplier folded into played haptic clicks. */
        float clickForceScale() const;
        /** Floor for the display backlight, in the 0-65535 backlight domain. */
        uint16_t minBacklight() const;
        /** LED ring brightness, 0-255, already zeroed when the ring is off. */
        uint8_t ledBrightness() const;

    private:
        int16_t values_[SETTING_COUNT] = {};
        int16_t saved_[SETTING_COUNT] = {};
        Preferences prefs_;
        bool ready_ = false;
};

// Both return unscaled detent strengths; InterfaceTask folds in
// knobStrengthScale() for every locally-applied config in one place.

/** Haptic config for scrolling the settings list itself. */
PB_SmartKnobConfig settingsListConfig(uint8_t selected);
/** Haptic config for editing one row, entered at its current value. */
PB_SmartKnobConfig settingEditConfig(SettingId id, int16_t value);

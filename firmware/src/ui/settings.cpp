#include "settings.h"

// Persisted under short keys so they fit NVS's 15-character limit comfortably.
static const char* const NVS_NAMESPACE = "sk_ui";
// Keys are looked up by name, so dropping a row leaves the remaining ones
// pointing at the same entries they were saved under. The two action rows never
// touch NVS; their slots are placeholders to keep this array the same length.
static const char* const NVS_KEYS[SETTING_COUNT] = {
    "knob_str", "click_str", "min_bright", "led_ring", "led_bright", "unused", "unused2",
};

const SettingDescriptor SETTINGS[] = {
    {"STRENGTH", "Detent force", AppIcon::STRENGTH, SettingKind::PERCENT, 25, 200, 100, 3},
    {"CLICK", "Press feedback", AppIcon::CLICK, SettingKind::PERCENT, 25, 200, 100, 3},
    {"MIN BRIGHT", "Screen floor", AppIcon::BRIGHTNESS, SettingKind::PERCENT, 2, 100, 2, 3.6},
    {"LED RING", "Knob backlight", AppIcon::BULB, SettingKind::ONOFF, 0, 1, 1, 60},
    {"LED LEVEL", "Ring brightness", AppIcon::LED, SettingKind::PERCENT, 5, 100, 100, 3.6},
    {"CALIBRATE", "Strain sensor", AppIcon::CALIBRATE, SettingKind::ACTION, 0, 0, 0, 20},
    {"RESET", "Restore defaults", AppIcon::RESET, SettingKind::ACTION, 0, 0, 0, 20},
};

const SettingDescriptor& settingAt(uint8_t index) {
    return SETTINGS[index < SETTING_COUNT ? index : 0];
}

void SettingsStore::begin() {
    ready_ = prefs_.begin(NVS_NAMESPACE, false);
    for (uint8_t i = 0; i < SETTING_COUNT; i++) {
        const SettingDescriptor& d = SETTINGS[i];
        int16_t value = d.default_value;
        if (ready_ && d.kind != SettingKind::ACTION) {
            value = prefs_.getShort(NVS_KEYS[i], d.default_value);
        }
        values_[i] = constrain(value, d.min_value, d.max_value);
        saved_[i] = values_[i];
    }
}

int16_t SettingsStore::get(SettingId id) const {
    return values_[(uint8_t)id];
}

void SettingsStore::set(SettingId id, int16_t value) {
    uint8_t i = (uint8_t)id;
    const SettingDescriptor& d = SETTINGS[i];
    if (d.kind == SettingKind::ACTION) {
        return;
    }
    values_[i] = constrain(value, d.min_value, d.max_value);
}

void SettingsStore::resetToDefaults() {
    for (uint8_t i = 0; i < SETTING_COUNT; i++) {
        if (SETTINGS[i].kind == SettingKind::ACTION) {
            continue;
        }
        values_[i] = SETTINGS[i].default_value;
    }
    // Written straight through rather than left for the next commit(): a reset is
    // deliberate, and the list it returns to has no confirming press of its own.
    commit();
}

void SettingsStore::commit() {
    if (!ready_) {
        return;
    }
    for (uint8_t i = 0; i < SETTING_COUNT; i++) {
        if (SETTINGS[i].kind == SettingKind::ACTION || values_[i] == saved_[i]) {
            continue;
        }
        prefs_.putShort(NVS_KEYS[i], values_[i]);
        saved_[i] = values_[i];
    }
}

float SettingsStore::unit(SettingId id) const {
    const SettingDescriptor& d = SETTINGS[(uint8_t)id];
    if (d.max_value <= d.min_value) {
        return 0;
    }
    return (float)(values_[(uint8_t)id] - d.min_value) / (float)(d.max_value - d.min_value);
}

bool SettingsStore::isOn(SettingId id) const {
    return values_[(uint8_t)id] != 0;
}

float SettingsStore::knobStrengthScale() const {
    return get(SettingId::KNOB_STRENGTH) / 100.0f;
}

float SettingsStore::clickForceScale() const {
    return get(SettingId::CLICK_FORCE) / 100.0f;
}

uint16_t SettingsStore::minBacklight() const {
    uint32_t scaled = (uint32_t)get(SettingId::MIN_BRIGHTNESS) * UINT16_MAX / 100;
    return (uint16_t)max(scaled, (uint32_t)1);
}

uint8_t SettingsStore::ledBrightness() const {
    if (!isOn(SettingId::LED_RING)) {
        return 0;
    }
    return (uint8_t)(get(SettingId::LED_BRIGHTNESS) * 255 / 100);
}

PB_SmartKnobConfig settingsListConfig(uint8_t selected) {
    PB_SmartKnobConfig config = {
        selected, 0, 0,
        0, SETTING_COUNT - 1, 26 * PI / 180,
        1.1, 1, 1.1,
        "Settings",
        0, {}, 0,
        44,
    };
    return config;
}

PB_SmartKnobConfig settingEditConfig(SettingId id, int16_t value) {
    const SettingDescriptor& d = SETTINGS[(uint8_t)id];
    float detent = d.kind == SettingKind::ONOFF ? 1.6f : 0.4f;
    PB_SmartKnobConfig config = {
        value, 0, 0,
        d.min_value, d.max_value, (float)(d.step_degrees * PI / 180),
        detent, 1, d.kind == SettingKind::ONOFF ? 0.55f : 1.1f,
        "",
        0, {}, 0,
        44,
    };
    strlcpy(config.text, d.name, sizeof(config.text));
    return config;
}

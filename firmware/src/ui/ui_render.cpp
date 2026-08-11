#if SK_DISPLAY
#include "ui_render.h"

#include "../font/roboto_light_60.h"
#include "../util.h"
#include "apps.h"
#include "settings.h"
#include "ui_icons.h"
#include "ui_theme.h"

namespace ui {
namespace {

/** Page change animation, and the tap ripple after a short press. */
constexpr uint32_t TRANSITION_MS = 340;
constexpr uint32_t RIPPLE_MS = 420;

/** Horizontal pitch between carousel items, in pixels. */
constexpr float CAROUSEL_PITCH = 96;

struct Anim {
    bool initialised = false;
    uint32_t last_frame_ms = 0;
    /** Smoothed carousel offset, in item indices. */
    float carousel = 0;
    /** Smoothed 0-1 value of the open app, for the arc and glyph. */
    float value_unit = 0;
    /** Smoothed settings-list offset, in rows. */
    float list = 0;
    uint8_t mode_nonce = 0;
    uint8_t press_nonce = 0;
    uint32_t transition_start_ms = 0;
    uint32_t ripple_start_ms = 0;
};

Anim anim;

/** Exponential smoothing that is stable regardless of frame time. */
float approach(float current, float target, float dt, float rate) {
    return current + (target - current) * (1 - expf(-rate * dt));
}

/** Copies text uppercased, for the tracked-caps labels. */
void upperCopy(char* dest, size_t dest_size, const char* src) {
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < dest_size; i++) {
        dest[i] = toupper((unsigned char)src[i]);
    }
    dest[i] = '\0';
}

int32_t clampPosition(const PB_SmartKnobState& state, int32_t position) {
    if (state.config.max_position < state.config.min_position) {
        return position;
    }
    return CLAMP(position, state.config.min_position, state.config.max_position);
}

/** 0-1 position of the knob within its configured range. */
float positionUnit(const PB_SmartKnobState& state) {
    int32_t span = state.config.max_position - state.config.min_position;
    if (span <= 0) {
        return 0;
    }
    return clamp01((float)(clampPosition(state, state.current_position) - state.config.min_position) / span);
}

/** Ripple progress, or -1 when no ripple is playing. */
float rippleProgress(uint32_t now_ms) {
    if (anim.ripple_start_ms == 0) {
        return -1;
    }
    uint32_t elapsed = now_ms - anim.ripple_start_ms;
    if (elapsed >= RIPPLE_MS) {
        return -1;
    }
    return (float)elapsed / RIPPLE_MS;
}

/** Expanding gold hairline that acknowledges a press. */
void drawRipple(TFT_eSprite& spr, float progress) {
    if (progress < 0) {
        return;
    }
    float eased = easeOutCubic(progress);
    float radius = lerpf(18, BEZEL_RADIUS - 4, eased);
    float strength = (1 - eased) * 0.9f;
    ring(spr, radius, mix(COLOR_BG, COLOR_GOLD, strength));
    ring(spr, radius - 1, mix(COLOR_BG, COLOR_GOLD_DEEP, strength * 0.6f));
}

/** Bezel arc that fills while the knob is held, hinting at the way back. */
void drawHoldIndicator(TFT_eSprite& spr, float progress) {
    if (progress <= 0.02f) {
        return;
    }
    float sweep = 360 * clamp01(progress);
    arc(spr, BEZEL_RADIUS - 5, BEZEL_RADIUS - 1, 90, 90 - sweep, COLOR_GOLD);
    glow(spr, BEZEL_RADIUS - 3, 4, COLOR_GOLD_SOFT, progress * 0.5f);

    char label[16];
    upperCopy(label, sizeof(label), progress > 0.75f ? "release" : "hold to exit");
    trackedText(spr, label, CENTER_X, CENTER_Y + 78, &FreeSans9pt7b,
                mix(COLOR_TEXT_DIM, COLOR_TEXT, progress), 2);
}

}  // namespace

namespace {

/** Even spread of selection dots across the bottom of the bezel. */
void drawSelectionDots(TFT_eSprite& spr, float carousel, float fade) {
    if (MENU_ITEM_COUNT < 2) {
        return;
    }
    constexpr float SPREAD_DEG = 46;
    float step = SPREAD_DEG / (MENU_ITEM_COUNT - 1);
    float start = 270 - SPREAD_DEG / 2;
    for (uint8_t i = 0; i < MENU_ITEM_COUNT; i++) {
        float distance = clamp01(fabsf(carousel - i));
        float weight = 1 - distance;
        uint16_t color = mix(dim(COLOR_HAIRLINE, fade), dim(COLOR_GOLD, fade), weight);
        dot(spr, start + step * i, BEZEL_RADIUS - 14, 1 + weight, color);
    }
}

/**
 * The carousel: the selected glyph sits large and gold in the middle, its
 * neighbours shrink and cool off toward the rim. Everything is placed from the
 * fractional offset so the whole row slides with the knob.
 */
void drawMenu(TFT_eSprite& spr, const UiState& ui_state, uint32_t now_ms, float enter) {
    (void)ui_state;
    backdrop(spr, now_ms, enter);

    // The selected glyph sits on the screen centre. The glow ring below is drawn
    // about the centre too, so it now reads as a halo around the icon instead of
    // a ring offset from it. The label pair hangs off icon_y so it follows.
    const float icon_y = CENTER_Y;
    const int8_t nearest = (int8_t)lroundf(anim.carousel);

    for (int8_t i = 0; i < (int8_t)MENU_ITEM_COUNT; i++) {
        float delta = i - anim.carousel;
        if (fabsf(delta) > 2.2f) {
            continue;
        }
        float distance = clamp01(fabsf(delta));
        float x = CENTER_X + delta * CAROUSEL_PITCH;
        // Neighbours ride a shallow arc so the row reads as a dial, not a list.
        float y = icon_y + delta * delta * 5;
        float size = lerpf(30, 13, easeInOutCubic(distance)) * lerpf(0.82f, 1, enter);
        uint16_t color = mix(dim(COLOR_GOLD, enter), dim(COLOR_GOLD_DEEP, enter), distance);
        if (fabsf(delta) < 0.5f) {
            glow(spr, size + 12, 8, COLOR_GOLD_DEEP, (1 - distance * 2) * 0.45f * enter);
        }
        icon(spr, APPS[MENU_ITEMS[i]].icon, (int16_t)x, (int16_t)y, size, color, 0.68f, now_ms * 0.04f);
    }

    // Label pair for the nearest app, fading out while the row is mid-slide.
    if (nearest >= 0 && nearest < (int8_t)MENU_ITEM_COUNT) {
        const AppDescriptor& app = APPS[MENU_ITEMS[nearest]];
        float settle = clamp01(1 - fabsf(anim.carousel - nearest) * 2.4f) * enter;
        if (settle > 0.01f) {
            char name[24];
            upperCopy(name, sizeof(name), app.name);
            // Measured from the icon, not the screen: the gap under the glyph is
            // what the eye reads, and the caption still clears the selection dots.
            trackedText(spr, name, CENTER_X, (int16_t)(icon_y + 56), &FreeSansBold9pt7b,
                        mix(COLOR_BG, COLOR_TEXT, settle), 3);
            centeredText(spr, app.caption, CENTER_X, (int16_t)(icon_y + 76), &FreeSans9pt7b,
                         mix(COLOR_BG, COLOR_TEXT_DIM, settle));
        }
    }

    drawSelectionDots(spr, anim.carousel, enter);
    drawRipple(spr, rippleProgress(now_ms));
}

}  // namespace

namespace {

/** Writes the headline value for an app into buf, and its unit into unit_buf. */
void formatValue(const AppDescriptor& app, const PB_SmartKnobState& state,
                 char* buf, size_t buf_size, char* unit_buf, size_t unit_size) {
    int32_t position = clampPosition(state, state.current_position);
    unit_buf[0] = '\0';
    switch (app.value_style) {
        case ValueStyle::PERCENT:
            snprintf(buf, buf_size, "%ld", (long)position);
            snprintf(unit_buf, unit_size, "%%");
            break;
        case ValueStyle::MINUTES:
            snprintf(buf, buf_size, "%ld", (long)position);
            snprintf(unit_buf, unit_size, "MIN");
            break;
        case ValueStyle::ONOFF:
        case ValueStyle::STEPS: {
            const char* label = appStepLabel(app, position);
            upperCopy(buf, buf_size, label != nullptr ? label : "");
            break;
        }
        case ValueStyle::COUNT:
        default:
            snprintf(buf, buf_size, "%ld", (long)position);
            break;
    }
}

/** Value arc, tick marks and travelling tip shared by the app and remote pages. */
void drawValueArc(TFT_eSprite& spr, const PB_SmartKnobState& state, float unit, float enter) {
    const float r_out = BEZEL_RADIUS - 8;
    const float r_in = r_out - 6;
    const float sweep = ARC_SWEEP_DEG * enter;
    const float start = ARC_START_DEG;

    arc(spr, r_in, r_out, start, start - sweep, dim(COLOR_TRACK, 0.55f * enter));

    int32_t span = state.config.max_position - state.config.min_position;
    if (span > 0 && span <= 24) {
        for (int32_t i = 0; i <= span; i++) {
            float a = start - sweep * ((float)i / span);
            tick(spr, a, r_in - 5, r_in - 2, 1, dim(COLOR_HAIRLINE, enter));
        }
    }

    float filled = sweep * clamp01(unit);
    if (filled > 0.5f) {
        arc(spr, r_in, r_out, start, start - filled, dim(COLOR_GOLD, enter));
        arc(spr, r_out, r_out + 1, start, start - filled, dim(COLOR_SPECULAR, 0.5f * enter));
    }

    float tip = start - filled;
    glow(spr, (r_in + r_out) / 2, 8, COLOR_GOLD_SOFT, 0.5f * enter);
    dot(spr, tip, (r_in + r_out) / 2, 3.5f, dim(COLOR_SPECULAR, enter));
}

/**
 * An open app: the glyph settles up top while the arc sweeps in, so the page
 * feels like it grew out of the carousel item rather than replacing it.
 */
void drawApp(TFT_eSprite& spr, const PB_SmartKnobState& state, const UiState& ui_state,
             uint32_t now_ms, float enter) {
    const AppDescriptor& app = APPS[ui_state.app_index < APP_COUNT ? ui_state.app_index : 0];

    backdrop(spr, now_ms, enter);
    drawValueArc(spr, state, anim.value_unit, enter);

    char name[24];
    upperCopy(name, sizeof(name), app.name);
    trackedText(spr, name, CENTER_X, CENTER_Y - 78, &FreeSans9pt7b,
                mix(COLOR_BG, COLOR_GOLD, enter), 3);

    float icon_size = lerpf(30, 15, easeOutCubic(enter));
    float icon_y = lerpf(CENTER_Y - 18, CENTER_Y - 50, easeOutCubic(enter));
    icon(spr, app.icon, CENTER_X, (int16_t)icon_y, icon_size,
         dim(COLOR_GOLD, 0.55f + 0.45f * enter), anim.value_unit, now_ms * 0.12f);

    char value[16];
    char unit[8];
    bool counting = app.kind == AppKind::TIMER && (ui_state.timer_running || ui_state.timer_remaining_s > 0);
    if (counting) {
        // A running countdown reads as MM:SS rather than as a knob position.
        uint32_t remaining = ui_state.timer_remaining_s;
        snprintf(value, sizeof(value), "%lu:%02lu", (unsigned long)(remaining / 60), (unsigned long)(remaining % 60));
        unit[0] = '\0';
    } else {
        formatValue(app, state, value, sizeof(value), unit, sizeof(unit));
    }

    bool wordy = app.value_style == ValueStyle::ONOFF || app.value_style == ValueStyle::STEPS;
    if (wordy) {
        trackedText(spr, value, CENTER_X, CENTER_Y + 14, &FreeSansBold12pt7b,
                    mix(COLOR_BG, COLOR_TEXT, enter), 4);
    } else {
        spr.setTextDatum(CC_DATUM);
        spr.setFreeFont(&Roboto_Light_60);
        spr.setTextColor(mix(COLOR_BG, COLOR_TEXT, enter));
        spr.drawString(value, CENTER_X, CENTER_Y + 10, 1);
        if (unit[0] != '\0') {
            int16_t half = spr.textWidth(value, 1) / 2;
            centeredText(spr, unit, CENTER_X + half + 14, CENTER_Y + 24, &FreeSans9pt7b,
                         mix(COLOR_BG, COLOR_GOLD_SOFT, enter));
        }
    }

    const char* caption = app.caption;
    if (app.kind == AppKind::TIMER) {
        if (ui_state.timer_elapsed) {
            caption = "Time up";
        } else if (ui_state.timer_running) {
            caption = "Press to pause";
        } else if (ui_state.timer_remaining_s > 0) {
            caption = "Paused";
        }
    }
    centeredText(spr, caption, CENTER_X, CENTER_Y + 52, &FreeSans9pt7b,
                 mix(COLOR_BG, COLOR_TEXT_DIM, enter * 0.9f));

    // A finished countdown pulses the bezel until the press that clears it.
    if (ui_state.timer_elapsed) {
        float pulse = 0.5f + 0.5f * sinf(now_ms * 0.006f);
        ring(spr, BEZEL_RADIUS - 3, mix(COLOR_BG, COLOR_GOLD, 0.35f + 0.55f * pulse));
        glow(spr, BEZEL_RADIUS - 6, 6, COLOR_GOLD_SOFT, 0.35f * pulse);
    }

    drawRipple(spr, rippleProgress(now_ms));
    drawHoldIndicator(spr, ui_state.hold_progress);
}

}  // namespace

namespace {

/** A host is driving the knob: show its text and range in the same dress. */
void drawRemote(TFT_eSprite& spr, const PB_SmartKnobState& state, uint32_t now_ms, float enter) {
    backdrop(spr, now_ms, enter);
    drawValueArc(spr, state, anim.value_unit, enter);

    trackedText(spr, "REMOTE", CENTER_X, CENTER_Y - 78, &FreeSans9pt7b,
                mix(COLOR_BG, COLOR_GOLD, enter), 3);

    spr.setTextDatum(CC_DATUM);
    spr.setFreeFont(&Roboto_Light_60);
    spr.setTextColor(mix(COLOR_BG, COLOR_TEXT, enter));
    spr.drawNumber(state.current_position, CENTER_X, CENTER_Y - 4, 1);

    // config.text is free-form and may carry newlines; render it line by line.
    int16_t line_y = CENTER_Y + 44;
    const char* start = state.config.text;
    const char* end = start + strnlen(state.config.text, sizeof(state.config.text));
    while (start < end && line_y < CENTER_Y + 84) {
        const char* newline = strchr(start, '\n');
        if (newline == nullptr || newline > end) {
            newline = end;
        }
        char buf[sizeof(state.config.text)] = {};
        strncat(buf, start, min(sizeof(buf) - 1, (size_t)(newline - start)));
        centeredText(spr, buf, CENTER_X, line_y, &FreeSans9pt7b, mix(COLOR_BG, COLOR_TEXT_DIM, enter));
        start = newline + 1;
        line_y += 18;
    }

    drawRipple(spr, rippleProgress(now_ms));
}

}  // namespace

namespace {

/** Vertical pitch between settings rows, in pixels. */
constexpr float ROW_PITCH = 46;

/** Writes a settings row's value, or an empty string for action rows. */
void formatSetting(const SettingDescriptor& d, int16_t value, char* buf, size_t buf_size) {
    switch (d.kind) {
        case SettingKind::ONOFF:
            strlcpy(buf, value != 0 ? "ON" : "OFF", buf_size);
            break;
        case SettingKind::ACTION:
            strlcpy(buf, "GO", buf_size);
            break;
        case SettingKind::PERCENT:
        default:
            snprintf(buf, buf_size, "%d%%", (int)value);
            break;
    }
}

/**
 * The settings list scrolls vertically, which is what sets it apart from the
 * horizontal app carousel. Each row is an icon on the left and its title on the
 * right; rows follow the chord of the round face so nothing runs off the glass.
 */
void drawSettings(TFT_eSprite& spr, const UiState& ui_state, uint32_t now_ms, float enter) {
    backdrop(spr, now_ms, enter);

    const float chord_radius = BEZEL_RADIUS - 8;

    for (uint8_t i = 0; i < SETTING_COUNT; i++) {
        float dy = (i - anim.list) * ROW_PITCH;
        if (fabsf(dy) > 96) {
            continue;
        }
        float focus = clamp01(1 - fabsf(dy) / ROW_PITCH);
        float fade = (0.30f + 0.70f * focus) * enter;
        int16_t y = (int16_t)(CENTER_Y + dy);

        // Half-width of the face at this row, so the hairlines follow the glass.
        float half = sqrtf(max(0.0f, chord_radius * chord_radius - dy * dy));
        int16_t left = (int16_t)(CENTER_X - half);
        int16_t right = (int16_t)(CENTER_X + half);

        // Text keeps one shared column instead of following the curve: on the
        // outer rows the chord is too narrow for a long title and its value.
        float text_half = sqrtf(max(0.0f, chord_radius * chord_radius - ROW_PITCH * ROW_PITCH));
        int16_t text_left = (int16_t)(CENTER_X - min(half, text_half));
        int16_t text_right = (int16_t)(CENTER_X + min(half, text_half));

        const SettingDescriptor& d = SETTINGS[i];
        uint16_t accent = mix(COLOR_GOLD_DEEP, COLOR_GOLD, focus);

        if (focus > 0.5f) {
            // Hairline cradle behind the row in focus.
            float weight = (focus - 0.5f) * 2 * enter;
            spr.drawFastHLine(left + 8, y - 20, right - left - 16, dim(COLOR_HAIRLINE, weight * 0.7f));
            spr.drawFastHLine(left + 8, y + 20, right - left - 16, dim(COLOR_HAIRLINE, weight * 0.7f));
        }

        icon(spr, d.icon, text_left + 14, y, 11 + 3 * focus, dim(accent, fade),
             ui_state.setting_values[i] != 0 ? 0.8f : 0.1f, now_ms * 0.03f);

        char name[20];
        upperCopy(name, sizeof(name), d.name);
        int16_t name_width = trackedTextWidth(spr, name, &FreeSans9pt7b, 1);
        trackedText(spr, name, text_left + 32 + name_width / 2, y, &FreeSans9pt7b,
                    mix(COLOR_BG, COLOR_TEXT, fade), 1);

        char value[12];
        formatSetting(d, ui_state.setting_values[i], value, sizeof(value));
        if (d.kind == SettingKind::ACTION && ui_state.calibration_step > 0) {
            // Guides the two-phase strain calibration without leaving the list.
            strlcpy(value, ui_state.calibration_step == 1 ? "LET GO" : "PUSH", sizeof(value));
        }
        int16_t value_width = trackedTextWidth(spr, value, &FreeSans9pt7b, 0);
        trackedText(spr, value, text_right - 8 - value_width / 2, y, &FreeSans9pt7b,
                    mix(COLOR_BG, accent, fade), 0);
    }

    trackedText(spr, "SETTINGS", CENTER_X, 22, &FreeSansBold9pt7b,
                mix(COLOR_BG, COLOR_GOLD, enter), 3);

    // Scroll position on the right rim, so the list reads as taller than the face.
    if (SETTING_COUNT > 1) {
        float span = 96;
        float track_top = CENTER_Y - span / 2;
        float thumb = track_top + span * (anim.list / (SETTING_COUNT - 1));
        spr.drawFastVLine(CENTER_X + 104, (int16_t)track_top, (int16_t)span, dim(COLOR_HAIRLINE, enter * 0.6f));
        spr.fillRect(CENTER_X + 103, (int16_t)thumb - 5, 3, 10, dim(COLOR_GOLD, enter));
    }

    drawRipple(spr, rippleProgress(now_ms));
    drawHoldIndicator(spr, ui_state.hold_progress);
}

/** Editing one row: the same value page dress as an app, minus the carousel. */
void drawSettingEdit(TFT_eSprite& spr, const PB_SmartKnobState& state, const UiState& ui_state,
                     uint32_t now_ms, float enter) {
    const SettingDescriptor& d = settingAt(ui_state.setting_index);

    backdrop(spr, now_ms, enter);
    drawValueArc(spr, state, anim.value_unit, enter);

    char name[20];
    upperCopy(name, sizeof(name), d.name);
    trackedText(spr, name, CENTER_X, CENTER_Y - 78, &FreeSans9pt7b,
                mix(COLOR_BG, COLOR_GOLD, enter), 3);

    icon(spr, d.icon, CENTER_X, CENTER_Y - 46, 15,
         dim(COLOR_GOLD, 0.55f + 0.45f * enter), anim.value_unit, now_ms * 0.12f);

    char value[12];
    formatSetting(d, ui_state.setting_values[ui_state.setting_index], value, sizeof(value));
    if (d.kind == SettingKind::ONOFF) {
        trackedText(spr, value, CENTER_X, CENTER_Y + 14, &FreeSansBold12pt7b,
                    mix(COLOR_BG, COLOR_TEXT, enter), 4);
    } else {
        spr.setTextDatum(CC_DATUM);
        spr.setFreeFont(&Roboto_Light_60);
        spr.setTextColor(mix(COLOR_BG, COLOR_TEXT, enter));
        spr.drawString(value, CENTER_X, CENTER_Y + 10, 1);
    }

    centeredText(spr, d.caption, CENTER_X, CENTER_Y + 52, &FreeSans9pt7b,
                 mix(COLOR_BG, COLOR_TEXT_DIM, enter * 0.9f));

    drawRipple(spr, rippleProgress(now_ms));
    drawHoldIndicator(spr, ui_state.hold_progress);
}

}  // namespace

void renderInit(TFT_eSprite& spr) {
    spr.setTextWrap(false);
    spr.setTextDatum(CC_DATUM);
    anim = Anim();
}

void render(TFT_eSprite& spr, const PB_SmartKnobState& state, const UiState& ui_state, uint32_t now_ms) {
    float carousel_target = menuSlotForApp(ui_state.app_index);
    if (ui_state.mode == UiMode::MENU) {
        // The state can still describe the app we just left for a frame or two,
        // so bound this to the carousel itself rather than to state.config.
        carousel_target = CLAMP(state.current_position, (int32_t)0, (int32_t)(MENU_ITEM_COUNT - 1)) +
                          CLAMP(state.sub_position_unit, (float)-0.55, (float)0.55);
    }

    float list_target = ui_state.setting_index;
    if (ui_state.mode == UiMode::SETTINGS) {
        list_target = CLAMP(state.current_position, (int32_t)0, (int32_t)(SETTING_COUNT - 1)) +
                      CLAMP(state.sub_position_unit, (float)-0.55, (float)0.55);
    }

    float value_target = positionUnit(state);
    if (ui_state.mode == UiMode::APP && ui_state.timer_total_s > 0 &&
        APPS[ui_state.app_index < APP_COUNT ? ui_state.app_index : 0].kind == AppKind::TIMER) {
        // While counting down, the arc tracks time left rather than knob position.
        value_target = clamp01((float)ui_state.timer_remaining_s / ui_state.timer_total_s);
    }

    if (!anim.initialised) {
        anim.initialised = true;
        anim.last_frame_ms = now_ms;
        anim.mode_nonce = ui_state.mode_nonce;
        anim.press_nonce = ui_state.press_nonce;
        anim.transition_start_ms = now_ms;
        anim.carousel = carousel_target;
        anim.value_unit = value_target;
        anim.list = list_target;
    }

    float dt = CLAMP((now_ms - anim.last_frame_ms) / 1000.0f, (float)0, (float)0.1);
    anim.last_frame_ms = now_ms;

    if (ui_state.mode_nonce != anim.mode_nonce) {
        anim.mode_nonce = ui_state.mode_nonce;
        anim.transition_start_ms = now_ms;
        // The incoming state may still carry the previous page's config, so start
        // the arc from empty and let it sweep up to the real value.
        anim.value_unit = 0;
    }
    if (ui_state.press_nonce != anim.press_nonce) {
        anim.press_nonce = ui_state.press_nonce;
        anim.ripple_start_ms = now_ms;
    }

    anim.carousel = approach(anim.carousel, carousel_target, dt, 16);
    anim.value_unit = approach(anim.value_unit, value_target, dt, 18);
    anim.list = approach(anim.list, list_target, dt, 15);

    float enter = easeOutCubic(clamp01((float)(now_ms - anim.transition_start_ms) / TRANSITION_MS));

    spr.fillSprite(COLOR_BG);
    switch (ui_state.mode) {
        case UiMode::APP:
            drawApp(spr, state, ui_state, now_ms, enter);
            break;
        case UiMode::REMOTE:
            drawRemote(spr, state, now_ms, enter);
            break;
        case UiMode::SETTINGS:
            drawSettings(spr, ui_state, now_ms, enter);
            break;
        case UiMode::SETTING_EDIT:
            drawSettingEdit(spr, state, ui_state, now_ms, enter);
            break;
        case UiMode::MENU:
        default:
            drawMenu(spr, ui_state, now_ms, enter);
            break;
    }
}
}  // namespace ui

#endif

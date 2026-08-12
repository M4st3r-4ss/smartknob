#if SK_DISPLAY
#include "ui_render.h"

#include "../font/roboto_light_60.h"
#include "../util.h"
#include "apps.h"
#include "pet.h"
#include "settings.h"
#include "ui_icons.h"
#include "ui_theme.h"

namespace ui {
namespace {

/** Page change animation, and the tap ripple after a short press. */
constexpr uint32_t TRANSITION_MS = 340;
constexpr uint32_t RIPPLE_MS = 420;

/** How long the reset row reads "DONE" after it runs. */
constexpr uint32_t RESET_FLASH_MS = 1400;

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
    /** Start of the "DONE" flash on the reset row, and the nonce that armed it. */
    uint32_t settings_reset_ms = 0;
    uint8_t settings_reset_nonce = 0;

    /** Smoothed 0-1 "is being patted", so the face eases in and out of a pat. */
    float pet_petting = 0;
    /** Start of the after-pat emote, and the last mood change. */
    uint32_t pet_reaction_start_ms = 0;
    uint32_t pet_mood_start_ms = 0;
    uint8_t pet_reaction_nonce = 0;
    uint8_t pet_mood_nonce = 0;
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

/** True while the reset row should still be reporting that it ran. */
bool resetFlashing(uint32_t now_ms) {
    return anim.settings_reset_ms != 0 && now_ms - anim.settings_reset_ms < RESET_FLASH_MS;
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

    // Name of the nearest app, fading out while the row is mid-slide. No caption
    // under it: the menu reads cleaner as icon plus name. Nothing draws
    // app.caption any more, but the descriptors keep it as documentation and as
    // a one-line way to put the subtitles back.
    if (nearest >= 0 && nearest < (int8_t)MENU_ITEM_COUNT) {
        const AppDescriptor& app = APPS[MENU_ITEMS[nearest]];
        float settle = clamp01(1 - fabsf(anim.carousel - nearest) * 2.4f) * enter;
        if (settle > 0.01f) {
            char name[24];
            upperCopy(name, sizeof(name), app.name);
            // Measured from the icon, not the screen: the gap under the glyph is
            // what the eye reads.
            trackedText(spr, name, CENTER_X, (int16_t)(icon_y + 56), &FreeSansBold9pt7b,
                        mix(COLOR_BG, COLOR_TEXT, settle), 3);
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

    // Name, glyph and value share the face evenly now that the subtitle is gone:
    // the trio is centred in the ring instead of being packed into its top half.
    // The name also clears the arc better here - at the old height the widest
    // one ("BRIGHTNESS") reached almost exactly as far as the arc's inner edge.
    char name[24];
    upperCopy(name, sizeof(name), app.name);
    trackedText(spr, name, CENTER_X, CENTER_Y - 66, &FreeSans9pt7b,
                mix(COLOR_BG, COLOR_GOLD, enter), 3);

    // The glyph starts where the carousel left it, which is the centre now that
    // the menu icon sits there, and rises into place as the page settles.
    float icon_size = lerpf(30, 15, easeOutCubic(enter));
    float icon_y = lerpf(CENTER_Y, CENTER_Y - 30, easeOutCubic(enter));
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

    // No fixed subtitle on the page. The timer keeps its line because that one is
    // live state, not decoration: without it a paused countdown looks exactly
    // like a running one, since both just show MM:SS.
    const char* status = nullptr;
    if (app.kind == AppKind::TIMER) {
        if (ui_state.timer_elapsed) {
            status = "Time up";
        } else if (ui_state.timer_running) {
            status = "Press to pause";
        } else if (ui_state.timer_remaining_s > 0) {
            status = "Paused";
        }
    }

    // One element more to fit when the timer is live, so the value moves up to
    // make room rather than crowding the status line into the hold indicator.
    int16_t value_y = CENTER_Y + (status != nullptr ? 8 : 20);

    bool wordy = app.value_style == ValueStyle::ONOFF || app.value_style == ValueStyle::STEPS;
    if (wordy) {
        trackedText(spr, value, CENTER_X, value_y, &FreeSansBold12pt7b,
                    mix(COLOR_BG, COLOR_TEXT, enter), 4);
    } else {
        spr.setTextDatum(CC_DATUM);
        spr.setFreeFont(&Roboto_Light_60);
        spr.setTextColor(mix(COLOR_BG, COLOR_TEXT, enter));
        spr.drawString(value, CENTER_X, value_y, 1);
        if (unit[0] != '\0') {
            int16_t half = spr.textWidth(value, 1) / 2;
            centeredText(spr, unit, CENTER_X + half + 14, value_y + 14, &FreeSans9pt7b,
                         mix(COLOR_BG, COLOR_GOLD_SOFT, enter));
        }
    }

    if (status != nullptr) {
        centeredText(spr, status, CENTER_X, CENTER_Y + 56, &FreeSans9pt7b,
                     mix(COLOR_BG, COLOR_TEXT_DIM, enter * 0.9f));
    }

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

    // Rows used to be cut off at a fixed distance, which left the outermost one
    // sitting on top of the SETTINGS header. They now fade out over the last
    // stretch and are gone before they reach it, which also removes the pop as
    // rows appear and disappear during a scroll.
    constexpr float ROW_FADE_START = ROW_PITCH;
    constexpr float ROW_CUTOFF = 78;

    for (uint8_t i = 0; i < SETTING_COUNT; i++) {
        float dy = (i - anim.list) * ROW_PITCH;
        float edge = clamp01((ROW_CUTOFF - fabsf(dy)) / (ROW_CUTOFF - ROW_FADE_START));
        if (edge <= 0.01f) {
            continue;
        }
        float focus = clamp01(1 - fabsf(dy) / ROW_PITCH);
        float fade = (0.30f + 0.70f * focus) * enter * edge;
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
        // Both prompts are pinned to their own row: there is more than one action
        // row now, so keying off SettingKind::ACTION would caption both of them.
        if ((SettingId)i == SettingId::STRAIN_CALIBRATE && ui_state.calibration_step > 0) {
            // Guides the two-phase strain calibration without leaving the list.
            strlcpy(value, ui_state.calibration_step == 1 ? "LET GO" : "TAP", sizeof(value));
        } else if ((SettingId)i == SettingId::RESET && resetFlashing(now_ms)) {
            strlcpy(value, "DONE", sizeof(value));
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

    // Same three-element spacing as an app page, so stepping from one to the
    // other doesn't shift the furniture around.
    char name[20];
    upperCopy(name, sizeof(name), d.name);
    trackedText(spr, name, CENTER_X, CENTER_Y - 66, &FreeSans9pt7b,
                mix(COLOR_BG, COLOR_GOLD, enter), 3);

    icon(spr, d.icon, CENTER_X, CENTER_Y - 30, 15,
         dim(COLOR_GOLD, 0.55f + 0.45f * enter), anim.value_unit, now_ms * 0.12f);

    char value[12];
    formatSetting(d, ui_state.setting_values[ui_state.setting_index], value, sizeof(value));
    if (d.kind == SettingKind::ONOFF) {
        trackedText(spr, value, CENTER_X, CENTER_Y + 20, &FreeSansBold12pt7b,
                    mix(COLOR_BG, COLOR_TEXT, enter), 4);
    } else {
        spr.setTextDatum(CC_DATUM);
        spr.setFreeFont(&Roboto_Light_60);
        spr.setTextColor(mix(COLOR_BG, COLOR_TEXT, enter));
        spr.drawString(value, CENTER_X, CENTER_Y + 20, 1);
    }

    // Subtitle removed here too: the row's own name above the value says enough.
    // d.caption is now unread, like app.caption, and kept for the same reason.

    drawRipple(spr, rippleProgress(now_ms));
    drawHoldIndicator(spr, ui_state.hold_progress);
}

}  // namespace

namespace {

/** How long the after-pat emote holds before the face settles back. */
constexpr uint32_t PET_REACTION_MS = 1100;

/**
 * Head start the loving mood's left heart has over its right one, as a fraction
 * of the emote. Written as a delay in milliseconds over the whole beat so it
 * stays 0.3s if PET_REACTION_MS is ever retuned.
 */
constexpr float PET_HEART_DELAY = 300.0f / PET_REACTION_MS;

/**
 * The face, as numbers. Every mood, the pat itself and the after-pat emote all
 * work by moving these, so the three never fight over the same pixels: the mood
 * sets them, a pat bends them, and the reaction bends them again.
 *
 * There is no head here, and nothing is drawn that a mood does not ask for. The
 * bezel is the face, the two ovals are the whole of it at rest, and the mouth
 * only exists while the pet is answering a pat. Everything a brow used to say is
 * said by tilting the eyes instead, which is one shape doing two jobs rather
 * than two shapes competing for a small screen.
 */
struct PetFace {
    // The resting oval is about two and a half times as tall as it is wide, set
    // a third of the way out from the centre. That is the whole silhouette.
    float eye_rx = 11.5f;    // oval half-width
    float eye_ry = 28;       // oval half-height; tall ovals are the whole face
    float left_open = 1;     // per-eye vertical scale, so one can wink or blink
    float right_open = 1;
    float lid = 0;           // 0-1, how far the upper lid has come down
    float tilt = 0;          // radians; +ve drops the inner corners into a scowl
    float arch = 0;          // a shut eye's curve: +1 a happy arch, -1 a sleepy valley
    float spread = 1;        // multiplier on the gap between the eyes
    float mouth = 0.5f;      // +1 smile through to -1 frown
    float mouth_width = 30;
    float mouth_open = 0;    // 0-1, opens the curve out into a filled shape
    float mouth_show = 0;    // 0-1; at 0 there is no mouth at all, which is the rest state
    float blush = 0;         // 0-1 cheeks
    float bounce = 0;        // vertical offset, breathing or bouncing
    float wobble = 0;        // sideways shake
};

/** A blink, as a 0-1 openness. Returns 1 for all but a moment of each period. */
float blinkOpenness(uint32_t now_ms, uint16_t period_ms, uint16_t close_ms) {
    uint32_t phase = now_ms % period_ms;
    if (phase >= close_ms) {
        return 1;
    }
    // Shuts and opens again over close_ms, fastest through the middle.
    return 1 - sinf(PI * (float)phase / close_ms);
}

PetFace petFaceFor(PetMood mood, float petting, float reaction, uint32_t now_ms) {
    PetFace f;

    switch (mood) {
        case PetMood::CALM:
            // The rest state, and deliberately the plainest: two clean ovals and
            // a slow breath, which is the whole of the face at its quietest.
            f.bounce = sinf(now_ms * 0.0018f) * 2.0f;
            break;
        case PetMood::CURIOUS:
            // Tall, lifted and set a little wider: a face leaning towards you.
            f.eye_rx = 12;
            f.eye_ry = 32;
            f.spread = 1.05f;
            f.tilt = -0.06f;
            f.bounce = sinf(now_ms * 0.0032f) * 1.5f;
            break;
        case PetMood::HAPPY:
            f.eye_ry = 31;
            f.arch = 1;
            f.blush = 0.35f;
            f.bounce = sinf(now_ms * 0.006f) * 3.0f;
            break;
        case PetMood::LOVING:
            f.eye_ry = 25;
            f.lid = 0.42f;
            f.arch = 1;
            f.blush = 0.8f;
            f.bounce = sinf(now_ms * 0.0024f) * 2.5f;
            break;
        case PetMood::PLAYFUL:
            f.eye_ry = 29;
            f.arch = 1;
            f.blush = 0.3f;
            // A slow wink; the timing is deliberately not the bounce's.
            f.right_open = sinf(now_ms * 0.0021f) > 0.75f ? 0 : 1;
            f.bounce = sinf(now_ms * 0.011f) * 4.0f;
            f.wobble = sinf(now_ms * 0.008f) * 2.0f;
            break;
        case PetMood::SKITTISH:
            // Outer corners down rather than inner: worried, not cross.
            f.eye_rx = 12;
            f.eye_ry = 31;
            f.spread = 1.08f;
            f.tilt = -0.16f;
            f.arch = -1;
            f.wobble = sinf(now_ms * 0.022f) * 2.0f;
            break;
        case PetMood::ANNOYED:
            f.eye_ry = 24;
            f.lid = 0.40f;
            f.tilt = 0.17f;
            f.spread = 0.97f;
            f.wobble = sinf(now_ms * 0.014f) * 0.8f;
            break;
        case PetMood::GRUMPY:
            f.eye_ry = 23;
            f.lid = 0.52f;
            f.tilt = 0.30f;
            f.spread = 0.94f;
            break;
        case PetMood::SLEEPY:
        default:
            f.eye_ry = 6;
            f.lid = 0.90f;
            f.arch = -1;
            f.bounce = sinf(now_ms * 0.0012f) * 3.5f;
            break;
    }

    // A blink every few seconds. Nothing else costs so little and does so much to
    // make the ovals read as an animal rather than a graphic; a jumpy pet blinks
    // more often than a settled one. Sleepy is already shut.
    if (mood != PetMood::SLEEPY) {
        float open = blinkOpenness(now_ms, mood == PetMood::SKITTISH ? 1900 : 4300, 130);
        f.left_open *= open;
        f.right_open *= open;
    }

    // Being patted, per mood. The happy family screws its eyes shut and grins,
    // the cross ones narrow further, sleepy stirs half awake.
    if (petting > 0.01f) {
        switch (mood) {
            case PetMood::ANNOYED:
            case PetMood::GRUMPY:
                f.lid += 0.22f * petting;
                f.tilt += 0.18f * petting;
                f.mouth = -0.7f;
                f.wobble += sinf(now_ms * 0.05f) * 2.5f * petting;
                break;
            case PetMood::SKITTISH:
                f.eye_ry *= 1 + 0.12f * petting;
                f.mouth = -0.25f;
                f.wobble += sinf(now_ms * 0.06f) * 3.0f * petting;
                break;
            case PetMood::SLEEPY:
                f.eye_ry += 9 * petting;
                f.lid -= 0.5f * petting;
                f.mouth = 0.2f;
                break;
            default:
                f.left_open *= 1 - 0.94f * petting;
                f.right_open *= 1 - 0.94f * petting;
                f.arch = 1;
                f.mouth = 1;
                f.blush = max(f.blush, 0.55f * petting);
                f.wobble += sinf(now_ms * 0.03f) * 2.0f * petting;
                break;
        }
        f.mouth_show = max(f.mouth_show, petting);
    }

    // The beat after the hand lifts, held while the haptic reaction plays. This is
    // the one moment the mouth is worth drawing, so each mood gets its own.
    if (reaction > 0.01f) {
        switch (mood) {
            case PetMood::CURIOUS:
                f.eye_ry *= 1 + 0.18f * reaction;
                f.tilt -= 0.08f * reaction;
                f.mouth = 0.45f;
                f.mouth_width = 22;
                break;
            case PetMood::LOVING:
                f.left_open *= 1 - reaction;
                f.right_open *= 1 - reaction;
                f.arch = 1;
                f.blush = 1;
                f.mouth = 0.9f;
                break;
            case PetMood::HAPPY:
                f.mouth = 1;
                f.mouth_width = 38;
                f.mouth_open = 0.55f;
                f.blush = 0.6f;
                break;
            case PetMood::PLAYFUL:
                f.left_open = 1;
                f.right_open = 1;
                f.eye_ry *= 1 + 0.2f * reaction;
                f.mouth = 1;
                f.mouth_width = 34;
                f.mouth_open = 0.6f;
                f.bounce -= 5 * reaction;
                break;
            case PetMood::SKITTISH:
                // Eyes snap wide and the mouth goes small: a gasp. The widening is
                // held back deliberately - past about a fifth the ovals stop
                // reading as startled eyes and start reading as tall slots.
                f.eye_rx *= 1 + 0.10f * reaction;
                f.eye_ry *= 1 + 0.18f * reaction;
                f.lid = 0;
                f.mouth = -0.4f;
                f.mouth_width = 20;
                f.mouth_open = 0.7f;
                f.wobble += sinf(now_ms * 0.08f) * 4.0f * reaction;
                break;
            case PetMood::ANNOYED:
                f.tilt += 0.20f * reaction;
                f.mouth = -0.6f;
                f.mouth_width = 26;
                f.wobble += sinf(now_ms * 0.07f) * 2.0f * reaction;
                break;
            case PetMood::GRUMPY:
                // The lid lifts so it can properly glare at you.
                f.lid -= 0.30f * reaction;
                f.eye_ry *= 1 + 0.25f * reaction;
                f.tilt += 0.24f * reaction;
                f.mouth = -1;
                f.mouth_width = 26;
                f.mouth_open = 0.45f;
                f.wobble += sinf(now_ms * 0.06f) * 3.0f * reaction;
                break;
            case PetMood::SLEEPY:
                // A yawn, which is the only round mouth on the page.
                f.mouth = 0.1f;
                f.mouth_width = 20;
                f.mouth_open = 1;
                break;
            case PetMood::CALM:
            default:
                f.left_open *= 1 - 0.85f * reaction;
                f.right_open *= 1 - 0.85f * reaction;
                f.arch = 1;
                f.mouth = 0.7f;
                break;
        }
        f.mouth_show = max(f.mouth_show, reaction);
    }

    f.mouth = CLAMP(f.mouth, (float)-1, (float)1);
    f.lid = CLAMP(f.lid, (float)0, (float)0.95f);
    f.mouth_show = clamp01(f.mouth_show);
    f.blush = clamp01(f.blush);
    return f;
}

}  // namespace

namespace {

/**
 * A filled oval that can be flattened across the top and turned on its axis.
 * A lidded eye and an open grin are the same shape, and the sprite has no
 * rotated primitive, so both are walked out as a fan of triangles. Straight,
 * unlidded ovals skip all of this and go through fillEllipse, which is both
 * smoother and cheaper - and that is the shape the face wears most of the time.
 */
void fillOval(TFT_eSprite& spr, float cx, float cy, float rx, float ry, float cut, float tilt, uint16_t color) {
    if (rx < 1 || ry < 1) {
        return;
    }
    if (cut < 0.02f && fabsf(tilt) < 0.02f) {
        spr.fillEllipse((int32_t)cx, (int32_t)cy, (int32_t)rx, (int32_t)ry, color);
        return;
    }

    constexpr uint8_t SEGMENTS = 28;
    float top = -ry + 2 * ry * cut;
    float sin_t = sinf(tilt);
    float cos_t = cosf(tilt);
    // Any interior point will do for the fan; the shape stays convex.
    float mid = (top + ry) / 2;
    float hub_x = cx - mid * sin_t;
    float hub_y = cy + mid * cos_t;

    float prev_x = 0;
    float prev_y = 0;
    for (uint8_t i = 0; i <= SEGMENTS; i++) {
        float a = i * (2 * PI / SEGMENTS);
        float ex = rx * cosf(a);
        float ey = ry * sinf(a);
        if (ey < top) {
            ey = top;
        }
        float x = cx + ex * cos_t - ey * sin_t;
        float y = cy + ex * sin_t + ey * cos_t;
        if (i > 0) {
            spr.fillTriangle(hub_x, hub_y, prev_x, prev_y, x, y, color);
        }
        prev_x = x;
        prev_y = y;
    }
}

/** One eye: a tall oval, or a curve once it shuts. */
void drawPetEye(TFT_eSprite& spr, float cx, float cy, const PetFace& f, float open, float tilt, uint16_t color) {
    float rx = f.eye_rx;
    float ry = f.eye_ry * open;
    float visible = 2 * ry * (1 - f.lid);

    // Anything thinner than this becomes the curve rather than a squashed oval: a
    // few pixels of flattened ellipse reads as a dash lying on the face, where the
    // curve reads as a shut eye. The swap happens within a frame or two of a
    // blink, so it is never seen mid-change.
    if (rx < 1 || visible < 9) {
        // Shut, and the curve is the mood's: an arch reads happy, a valley reads
        // sleepy, and a flat line is just a blink passing through.
        float w = rx * 1.25f;
        if (f.arch > 0.5f) {
            strokeArc(spr, cx, cy + w * 0.55f, w, 3, 22, 158, color);
        } else if (f.arch < -0.5f) {
            strokeArc(spr, cx, cy - w * 0.75f, w * 1.3f, 3, 214, 326, color);
        } else {
            stroke(spr, cx - w, cy, cx + w, cy, color);
        }
        return;
    }

    fillOval(spr, cx, cy, rx, ry, f.lid, tilt, color);

    // Catchlight: a hole punched up and to the right, on both eyes rather than
    // mirrored, as though one light were on the face. It is most of what makes
    // two ovals read as alive.
    float top = -ry + 2 * ry * f.lid;
    if (ry - top > 8 && rx >= 4) {
        float ox = rx * 0.30f;
        float oy = top + (ry - top) * 0.18f;
        float sin_t = sinf(tilt);
        float cos_t = cosf(tilt);
        spr.fillCircle((int32_t)(cx + ox * cos_t - oy * sin_t),
                       (int32_t)(cy + ox * sin_t + oy * cos_t),
                       (int32_t)max(1.5f, rx * 0.18f), COLOR_BG);
    }
}

/**
 * The mouth, which exists only while the pet is answering: a curve most of the
 * time, or a filled shape when a mood opens it. Kept off the face at rest, so
 * the resting look stays the two ovals and nothing else.
 */
void drawPetMouth(TFT_eSprite& spr, float cx, float cy, const PetFace& f, uint16_t color) {
    float w = f.mouth_width;
    if (f.mouth_open > 0.05f) {
        float rx = w * 0.34f;
        float ry = rx * (0.9f + 1.1f * f.mouth_open);
        if (f.mouth > 0.35f) {
            // A grin: flat along the top, round underneath.
            fillOval(spr, cx, cy, rx, ry, 0.5f, 0, color);
        } else {
            fillOval(spr, cx, cy, rx * 0.8f, ry * 0.8f, 0, 0, color);
        }
        return;
    }

    float curve = f.mouth;
    if (fabsf(curve) < 0.06f) {
        stroke(spr, cx - w / 2, cy, cx + w / 2, cy, color);
        return;
    }
    float radius = w / (0.55f + 0.75f * fabsf(curve));
    if (curve > 0) {
        strokeArc(spr, cx, cy - radius * 0.72f, radius, 3, 214, 326, color);
    } else {
        strokeArc(spr, cx, cy + radius * 0.72f, radius, 3, 34, 146, color);
    }
}

/**
 * Cheeks, drawn as the short diagonal strokes a comic uses rather than as a soft
 * patch. The sprite is 8 bits per pixel: a dim wash quantises to a muddy blob,
 * while a stroke at full colour stays the colour it was meant to be. How flushed
 * the pet is shows in how many strokes there are, which survives that too.
 */
void drawPetBlush(TFT_eSprite& spr, float cx, float cy, float amount, uint16_t color) {
    // Never one: a single stroke on a bare cheek reads as a stray scratch rather
    // than as colour in the face.
    uint8_t lines = amount > 0.6f ? 3 : 2;
    for (int8_t side = -1; side <= 1; side += 2) {
        float x = cx + side * 62;
        for (uint8_t i = 0; i < lines; i++) {
            float ox = (i - (lines - 1) / 2.0f) * 6;
            stroke(spr, x + ox - side * 4, cy + 5, x + ox + side * 4, cy - 5, color);
        }
    }
}

void drawPetHeart(TFT_eSprite& spr, float cx, float cy, float s, uint16_t color) {
    spr.fillCircle(cx - s * 0.5f, cy - s * 0.35f, s * 0.55f, color);
    spr.fillCircle(cx + s * 0.5f, cy - s * 0.35f, s * 0.55f, color);
    spr.fillTriangle(cx - s, cy - s * 0.1f, cx + s, cy - s * 0.1f, cx, cy + s, color);
}

void drawPetZ(TFT_eSprite& spr, float cx, float cy, float s, uint16_t color) {
    stroke(spr, cx - s, cy - s, cx + s, cy - s, color);
    stroke(spr, cx + s, cy - s, cx - s, cy + s, color);
    stroke(spr, cx - s, cy + s, cx + s, cy + s, color);
}

void drawPetSpark(TFT_eSprite& spr, float cx, float cy, float s, uint16_t color) {
    stroke(spr, cx - s, cy, cx + s, cy, color);
    stroke(spr, cx, cy - s, cx, cy + s, color);
    stroke(spr, cx - s * 0.6f, cy - s * 0.6f, cx + s * 0.6f, cy + s * 0.6f, color);
    stroke(spr, cx - s * 0.6f, cy + s * 0.6f, cx + s * 0.6f, cy - s * 0.6f, color);
}

}  // namespace

namespace {

// The pet is the one page where colour carries meaning rather than decoration,
// so grumpy and sleepy step outside the champagne family the rest of the UI
// keeps to. Both survive the sprite's 3-3-2 quantisation.
constexpr uint16_t COLOR_PET_CROSS = rgb565(225, 95, 60);
constexpr uint16_t COLOR_PET_DOZE = rgb565(120, 150, 220);

uint16_t petAccent(PetMood mood) {
    switch (mood) {
        case PetMood::CURIOUS:
            return rgb565(120, 205, 225);
        case PetMood::LOVING:
            return rgb565(235, 105, 165);
        case PetMood::SKITTISH:
            return rgb565(240, 170, 75);
        case PetMood::ANNOYED:
            return rgb565(235, 105, 70);
        case PetMood::GRUMPY:
            return COLOR_PET_CROSS;
        case PetMood::SLEEPY:
            return COLOR_PET_DOZE;
        case PetMood::HAPPY:
        case PetMood::PLAYFUL:
            return COLOR_GOLD;
        case PetMood::CALM:
        default:
            return COLOR_GOLD_SOFT;
    }
}

/**
 * The mood's own after-pat emote, floating up beside the face as it fades. The
 * eyes are wide now that the head has gone, so these sit further out than they
 * used to: clear of the ovals, and still well inside the bezel at full rise.
 */
void drawPetEmote(TFT_eSprite& spr, PetMood mood, float face_y, float progress, uint16_t accent) {
    float rise = easeOutCubic(progress) * 26;
    float fade = 1 - progress;
    uint16_t color = mix(COLOR_BG, accent, fade);

    switch (mood) {
        case PetMood::CURIOUS:
            // A question mark asks back: the curious pet is listening for more.
            stroke(spr, CENTER_X + 60, face_y - 36 - rise,
                   CENTER_X + 71, face_y - 45 - rise, color);
            stroke(spr, CENTER_X + 71, face_y - 45 - rise,
                   CENTER_X + 77, face_y - 35 - rise, color);
            stroke(spr, CENTER_X + 77, face_y - 35 - rise,
                   CENTER_X + 68, face_y - 23 - rise, color);
            spr.fillCircle(CENTER_X + 67, face_y - 15 - rise, 2, color);
            break;
        case PetMood::LOVING: {
            // A pair, one per side, rather than a single heart over the middle of
            // the face: that one rose straight through where the mood title now
            // sits. They are flown at different heights and the right one is held
            // back, so the two read as a flutter rather than one symmetrical pop.
            drawPetHeart(spr, CENTER_X - 70, face_y - 34 - rise,
                         11 - 3 * progress, color);

            // Rescaled onto the beat that is left rather than simply offset: a
            // plain offset would still be mid-rise and half-lit when the emote
            // stops being drawn, so the late heart would vanish in mid-air.
            float late = (progress - PET_HEART_DELAY) / (1 - PET_HEART_DELAY);
            if (late > 0) {
                drawPetHeart(spr, CENTER_X + 70, face_y - 18 - easeOutCubic(late) * 26,
                             11 - 3 * late, mix(COLOR_BG, accent, 1 - late));
            }
            break;
        }
        case PetMood::SKITTISH:
            // Sided the same way and for the same reason as the playful sparks:
            // the middle dash of the shiver used to cross the title line.
            for (uint8_t i = 0; i < 3; i++) {
                float side = i == 1 ? 1.0f : -1.0f;
                float x = CENTER_X + side * 68;
                stroke(spr, x - 5, face_y - 40 - rise + i * 5,
                       x + 5, face_y - 46 - rise + i * 5, color);
            }
            break;
        case PetMood::ANNOYED:
            for (uint8_t i = 0; i < 2; i++) {
                float side = i == 0 ? -1.0f : 1.0f;
                float x = CENTER_X + side * 68;
                stroke(spr, x - side * 7, face_y - 35 - rise,
                       x + side * 7, face_y - 25 - rise, color);
                stroke(spr, x - side * 5, face_y - 20 - rise,
                       x + side * 5, face_y - 14 - rise, color);
            }
            break;
        case PetMood::HAPPY:
            for (uint8_t i = 0; i < 2; i++) {
                float side = i == 0 ? -1.0f : 1.0f;
                drawPetHeart(spr, CENTER_X + side * 70, face_y - 22 - rise - i * 10,
                             7 - 2 * progress, color);
            }
            break;
        case PetMood::PLAYFUL:
            // All three sparks are thrown out to a side, alternating, rather than
            // one of them going up the middle: the middle one rose straight
            // through the mood title and blotted out a square of it. Alternating
            // heights keep them reading as a scatter rather than a column.
            for (uint8_t i = 0; i < 3; i++) {
                float side = i == 1 ? 1.0f : -1.0f;
                float height = 40 - i * 14;
                drawPetSpark(spr, CENTER_X + side * 70, face_y - height - rise,
                             8 - 3 * progress, color);
            }
            break;
        case PetMood::SLEEPY:
            for (uint8_t i = 0; i < 2; i++) {
                drawPetZ(spr, CENTER_X + 58 + i * 13, face_y - 36 - rise - i * 14,
                         6 - 2 * progress, color);
            }
            break;
        case PetMood::GRUMPY:
            // Two short bars, the way a comic shows a huff.
            for (uint8_t i = 0; i < 2; i++) {
                float side = i == 0 ? -1.0f : 1.0f;
                float x = CENTER_X + side * 66;
                stroke(spr, x, face_y - 30 - rise, x + side * 9, face_y - 38 - rise, color);
                stroke(spr, x - side * 2, face_y - 20 - rise, x + side * 7, face_y - 26 - rise, color);
            }
            break;
        case PetMood::CALM:
        default:
            // Purr: soft arcs washing outward from both cheeks.
            for (uint8_t i = 0; i < 2; i++) {
                float side = i == 0 ? 180.0f : 0.0f;
                strokeArc(spr, CENTER_X, face_y, 72 + rise, 2,
                          side - 26, side + 26, mix(COLOR_BG, accent, fade * 0.7f));
            }
            break;
    }
}

/**
 * The pet page. There is no value here: rotation is a pat, so the face and the
 * two feedback channels are the whole readout. The attention arc along the
 * bottom is the only number, and it is what moves the mood along.
 */
void drawPet(TFT_eSprite& spr, const UiState& ui_state, uint32_t now_ms, float enter) {
    PetMood mood = (PetMood)(ui_state.pet_mood < PET_MOOD_COUNT ? ui_state.pet_mood : 0);
    const PetMoodDescriptor& d = petMoodAt(mood);
    uint16_t accent = petAccent(mood);

    backdrop(spr, now_ms, enter);

    float reaction = 0;
    if (anim.pet_reaction_start_ms != 0) {
        uint32_t elapsed = now_ms - anim.pet_reaction_start_ms;
        if (elapsed < PET_REACTION_MS) {
            reaction = 1 - (float)elapsed / PET_REACTION_MS;
        }
    }
    // A pat in progress owns the face; the reaction only takes over once it ends.
    float petting = anim.pet_petting;
    reaction *= 1 - petting;

    PetFace f = petFaceFor(mood, petting, reaction, now_ms);

    // A fresh mood pops in, so a change is felt on screen as well as in the knob.
    float mood_age = clamp01((float)(now_ms - anim.pet_mood_start_ms) / 420);
    float pop = lerpf(0.86f, 1, easeOutBack(mood_age)) * lerpf(0.80f, 1, enter);

    // No head, no ears, no outline: the bezel is the face, and the two ovals get
    // the whole of it. Everything below is placed off the screen centre rather
    // than off a drawn head, so the face fills the glass the way it should.
    float face_x = CENTER_X + f.wobble;
    float face_y = CENTER_Y - 6 + f.bounce;
    f.eye_rx *= pop;
    f.eye_ry *= pop;

    if (f.blush > 0.15f) {
        drawPetBlush(spr, face_x, face_y + 26, f.blush,
                     mix(COLOR_BG, accent, 0.85f * enter));
    }

    // The two eyes lean opposite ways: a positive tilt leans each oval outward at
    // the top, which slopes its upper edge down towards the middle and reads as a
    // scowl. Negative does the reverse and reads as worry.
    float eye_dx = 41 * f.spread * pop;
    uint16_t eye_color = mix(COLOR_BG, COLOR_TEXT, enter);
    drawPetEye(spr, face_x - eye_dx, face_y, f, f.left_open, -f.tilt, eye_color);
    drawPetEye(spr, face_x + eye_dx, face_y, f, f.right_open, f.tilt, eye_color);

    // The mouth is the pet answering, so it is drawn only while it has something
    // to say and fades out with the pat.
    if (f.mouth_show > 0.02f) {
        drawPetMouth(spr, face_x, face_y + 42 * pop, f,
                     mix(COLOR_BG, COLOR_TEXT, enter * f.mouth_show));
    }

    if (reaction > 0.01f) {
        drawPetEmote(spr, mood, face_y, 1 - reaction, accent);
    }

    // Attention: a short arc along the bottom rim rather than the full value
    // sweep, so the page never reads as something with a value to set.
    const float r_out = BEZEL_RADIUS - 8;
    float sweep = 84 * enter;
    arc(spr, r_out - 5, r_out, 270 - sweep / 2, 270 + sweep / 2, dim(COLOR_TRACK, 0.5f * enter));
    float filled = sweep * clamp01(anim.value_unit);
    if (filled > 0.5f) {
        arc(spr, r_out - 5, r_out, 270 - sweep / 2, 270 - sweep / 2 + filled, dim(accent, enter));
    }

    char name[16];
    upperCopy(name, sizeof(name), d.name);
    // A line lower than it used to sit: 22px, which is this font's yAdvance, so
    // the drop is exactly the height of the text itself.
    trackedText(spr, name, CENTER_X, 46, &FreeSansBold9pt7b, mix(COLOR_BG, accent, enter), 3);

    // One live line: what it is doing now, or how it answered the last pat. The
    // hint only shows while the pet has been left alone.
    const char* status = nullptr;
    if (petting > 0.35f) {
        status = d.while_petting;
    } else if (reaction > 0.05f) {
        status = d.after_petting;
    } else if (anim.pet_reaction_start_ms == 0) {
        status = "Rotate to pat";
    }
    // Sits lower than the other pages': the face is bigger now, and an open mouth
    // reaches further down than the old one did.
    if (status != nullptr && ui_state.hold_progress <= 0.02f) {
        centeredText(spr, status, CENTER_X, CENTER_Y + 72, &FreeSans9pt7b,
                     mix(COLOR_BG, COLOR_TEXT_DIM, enter * 0.9f));
    }

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

    const AppDescriptor& open_app = APPS[ui_state.app_index < APP_COUNT ? ui_state.app_index : 0];
    bool pet_page = ui_state.mode == UiMode::APP && open_app.kind == AppKind::PET;

    float value_target = positionUnit(state);
    if (ui_state.mode == UiMode::APP && ui_state.timer_total_s > 0 && open_app.kind == AppKind::TIMER) {
        // While counting down, the arc tracks time left rather than knob position.
        value_target = clamp01((float)ui_state.timer_remaining_s / ui_state.timer_total_s);
    } else if (pet_page) {
        // The pet page has no position to show; its arc tracks attention instead.
        value_target = clamp01(ui_state.pet_charge);
    }

    if (!anim.initialised) {
        anim.initialised = true;
        anim.last_frame_ms = now_ms;
        anim.mode_nonce = ui_state.mode_nonce;
        anim.press_nonce = ui_state.press_nonce;
        // Adopted rather than compared, so the first frame does not replay a
        // reaction or a mood change that happened before the display woke up.
        anim.pet_reaction_nonce = ui_state.pet_reaction_nonce;
        anim.pet_mood_nonce = ui_state.pet_mood_nonce;
        anim.pet_petting = ui_state.pet_petting ? 1.0f : 0.0f;
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
    if (ui_state.settings_reset_nonce != anim.settings_reset_nonce) {
        anim.settings_reset_nonce = ui_state.settings_reset_nonce;
        anim.settings_reset_ms = now_ms;
    }

    if (ui_state.pet_reaction_nonce != anim.pet_reaction_nonce) {
        anim.pet_reaction_nonce = ui_state.pet_reaction_nonce;
        anim.pet_reaction_start_ms = now_ms;
    }
    if (ui_state.pet_mood_nonce != anim.pet_mood_nonce) {
        anim.pet_mood_nonce = ui_state.pet_mood_nonce;
        anim.pet_mood_start_ms = now_ms;
    }
    if (!pet_page) {
        // Leaving the page clears the reaction, so returning to it shows the idle
        // hint rather than replaying a stale emote.
        anim.pet_reaction_start_ms = 0;
    }
    // Patting eases in fast and out slowly: the face should not snap straight back
    // the instant the knob stops moving.
    anim.pet_petting = approach(anim.pet_petting, ui_state.pet_petting ? 1.0f : 0.0f,
                                dt, ui_state.pet_petting ? 20 : 6);

    anim.carousel = approach(anim.carousel, carousel_target, dt, 16);
    anim.value_unit = approach(anim.value_unit, value_target, dt, 18);
    anim.list = approach(anim.list, list_target, dt, 15);

    float enter = easeOutCubic(clamp01((float)(now_ms - anim.transition_start_ms) / TRANSITION_MS));

    spr.fillSprite(COLOR_BG);
    switch (ui_state.mode) {
        case UiMode::APP:
            if (pet_page) {
                drawPet(spr, ui_state, now_ms, enter);
            } else {
                drawApp(spr, state, ui_state, now_ms, enter);
            }
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

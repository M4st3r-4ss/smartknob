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
 * The face, as numbers. Every mood, the pat itself and the after-pat emote all
 * work by moving these, so the three never fight over the same pixels: the mood
 * sets them, a pat bends them, and the reaction bends them again.
 */
struct PetFace {
    float eye_rx;      // oval half-width
    float eye_ry;      // oval half-height; the ovals are what read as eyes
    float left_open;   // per-eye vertical scale, so one can wink
    float right_open;
    float lid;         // 0-1, how much a curved lid replaces the oval
    float brow;        // >0 lowered and angry, <0 raised
    float mouth;       // +1 smile through to -1 frown
    float mouth_width;
    float bounce;      // body offset, breathing or bouncing
    float wobble;      // sideways shake
};

PetFace petFaceFor(PetMood mood, float petting, float reaction, uint32_t now_ms) {
    PetFace f = {13, 15, 1, 1, 0, 0, 0.5f, 28, 0, 0};

    switch (mood) {
        case PetMood::CALM:
            f.eye_ry = 13;
            f.lid = 0.15f;
            f.mouth = 0.45f;
            f.bounce = sinf(now_ms * 0.0018f) * 2.0f;
            break;
        case PetMood::CURIOUS:
            f.eye_rx = 12;
            f.eye_ry = 17;
            f.brow = -0.65f;
            f.mouth = 0.15f;
            f.mouth_width = 20;
            f.bounce = sinf(now_ms * 0.0032f) * 1.5f;
            break;
        case PetMood::HAPPY:
            f.eye_ry = 18;
            f.mouth = 1.0f;
            f.mouth_width = 34;
            f.bounce = sinf(now_ms * 0.006f) * 3.0f;
            break;
        case PetMood::LOVING:
            f.eye_ry = 11;
            f.lid = 0.5f;
            f.mouth = 0.9f;
            f.mouth_width = 32;
            f.bounce = sinf(now_ms * 0.0024f) * 2.5f;
            break;
        case PetMood::PLAYFUL:
            f.eye_ry = 17;
            // A slow alternating wink; the timing is deliberately not the bounce's.
            f.right_open = sinf(now_ms * 0.0021f) > 0.75f ? 0.15f : 1;
            f.mouth = 0.85f;
            f.mouth_width = 31;
            f.bounce = sinf(now_ms * 0.011f) * 4.0f;
            f.wobble = sinf(now_ms * 0.008f) * 2.0f;
            break;
        case PetMood::SKITTISH:
            f.eye_rx = 11;
            f.eye_ry = 14;
            f.left_open = sinf(now_ms * 0.018f) > 0.82f ? 0.35f : 1;
            f.right_open = sinf(now_ms * 0.021f + 1.4f) > 0.86f ? 0.35f : 1;
            f.mouth = -0.15f;
            f.mouth_width = 19;
            f.wobble = sinf(now_ms * 0.022f) * 2.0f;
            break;
        case PetMood::ANNOYED:
            f.eye_rx = 14;
            f.eye_ry = 10;
            f.brow = 0.7f;
            f.mouth = -0.35f;
            f.mouth_width = 26;
            f.wobble = sinf(now_ms * 0.014f) * 0.8f;
            break;
        case PetMood::GRUMPY:
            f.eye_rx = 14;
            f.eye_ry = 9;
            f.brow = 1;
            f.mouth = -0.8f;
            f.mouth_width = 24;
            break;
        case PetMood::SLEEPY:
        default:
            f.eye_ry = 4;
            f.lid = 0.85f;
            f.mouth = 0.15f;
            f.mouth_width = 16;
            f.bounce = sinf(now_ms * 0.0012f) * 3.5f;
            break;
    }

    // Being patted, per mood. The happy family screws its eyes shut and smiles
    // wider, grumpy narrows and scowls harder, sleepy stirs half awake.
    if (petting > 0.01f) {
        switch (mood) {
            case PetMood::GRUMPY:
                f.eye_ry *= 1 - 0.35f * petting;
                f.brow += petting;
                f.mouth -= 0.3f * petting;
                f.wobble += sinf(now_ms * 0.05f) * 2.5f * petting;
                break;
            case PetMood::SLEEPY:
                f.eye_ry += 7 * petting;
                f.lid -= 0.55f * petting;
                f.mouth += 0.25f * petting;
                break;
            default:
                f.eye_ry *= 1 - 0.7f * petting;
                f.lid = f.lid + (1 - f.lid) * 0.85f * petting;
                f.left_open = 1;
                f.right_open = 1;
                f.mouth += 0.4f * petting;
                f.wobble += sinf(now_ms * 0.03f) * 2.0f * petting;
                break;
        }
        f.mouth_width += 4 * petting;
    }

    // The beat after the hand lifts, held while the haptic reaction plays.
    if (reaction > 0.01f) {
        switch (mood) {
            case PetMood::CURIOUS:
                f.eye_ry *= 1 + 0.35f * reaction;
                f.brow -= 0.4f * reaction;
                f.mouth += 0.25f * reaction;
                break;
            case PetMood::LOVING:
                f.lid = 0.9f;
                f.eye_ry *= 0.7f;
                f.mouth += 0.35f * reaction;
                break;
            case PetMood::SKITTISH:
                f.eye_ry *= 1 + 0.55f * reaction;
                f.wobble += sinf(now_ms * 0.08f) * 4.0f * reaction;
                break;
            case PetMood::ANNOYED:
                f.eye_ry *= 1 + 0.45f * reaction;
                f.brow += 0.45f * reaction;
                f.wobble += sinf(now_ms * 0.07f) * 2.0f * reaction;
                break;
            case PetMood::GRUMPY:
                f.eye_ry *= 1 + 1.1f * reaction;
                f.brow += 0.5f * reaction;
                f.wobble += sinf(now_ms * 0.06f) * 3.0f * reaction;
                break;
            case PetMood::SLEEPY:
                f.eye_ry *= 1 - 0.6f * reaction;
                f.lid += 0.4f * reaction;
                break;
            case PetMood::PLAYFUL:
                f.left_open = 1;
                f.right_open = 1;
                f.eye_ry *= 1 + 0.35f * reaction;
                f.bounce -= 5 * reaction;
                break;
            default:
                // Content: eyes stay creased shut a moment longer.
                f.lid = f.lid + (1 - f.lid) * 0.75f * reaction;
                f.eye_ry *= 1 - 0.55f * reaction;
                f.mouth += 0.35f * reaction;
                break;
        }
    }

    f.mouth = CLAMP(f.mouth, (float)-1, (float)1.4f);
    return f;
}

}  // namespace

namespace {

/** One eye: an oval, or a curved lid once it closes. */
void drawPetEye(TFT_eSprite& spr, float cx, float cy, const PetFace& f, float open, uint16_t color) {
    float rx = f.eye_rx;
    float ry = f.eye_ry * open;

    if (f.lid > 0.55f || ry < 2.5f) {
        // Closed: a lid arc curving the way the mood does, so a happy squeeze and
        // a sleepy doze do not look alike.
        float curve = f.mouth >= 0 ? 1.0f : -1.0f;
        strokeArc(spr, cx, cy - curve * rx * 0.5f, rx * 1.15f, 2,
                  curve > 0 ? 200 : 20, curve > 0 ? 340 : 160, color);
        return;
    }

    if (rx >= 1 && ry >= 1) {
        spr.fillEllipse((int32_t)cx, (int32_t)cy, (int32_t)rx, (int32_t)ry, color);
        if (f.lid > 0.05f) {
            // A partly dropped lid, drawn by cutting the top of the oval away.
            int32_t cut = (int32_t)(ry * 2 * f.lid);
            spr.fillRect((int32_t)(cx - rx - 1), (int32_t)(cy - ry - 1),
                         (int32_t)(rx * 2 + 3), cut, COLOR_BG);
        }
        // Catchlight, which is most of what makes the ovals read as alive.
        if (ry > 5) {
            spr.fillCircle((int32_t)(cx - rx * 0.35f), (int32_t)(cy - ry * 0.35f),
                           max(1.0f, rx * 0.22f), COLOR_SPECULAR);
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

/** The mood's own after-pat emote, floating up around the head as it fades. */
void drawPetEmote(TFT_eSprite& spr, PetMood mood, float head_y, float progress, uint16_t accent) {
    float rise = easeOutCubic(progress) * 26;
    float fade = 1 - progress;
    uint16_t color = mix(COLOR_BG, accent, fade);

    switch (mood) {
        case PetMood::CURIOUS:
            // A question mark asks back: the curious pet is listening for more.
            stroke(spr, CENTER_X + 56, head_y - 36 - rise,
                   CENTER_X + 67, head_y - 45 - rise, color);
            stroke(spr, CENTER_X + 67, head_y - 45 - rise,
                   CENTER_X + 73, head_y - 35 - rise, color);
            stroke(spr, CENTER_X + 73, head_y - 35 - rise,
                   CENTER_X + 64, head_y - 23 - rise, color);
            spr.fillCircle(CENTER_X + 63, head_y - 15 - rise, 2, color);
            break;
        case PetMood::LOVING:
            drawPetHeart(spr, CENTER_X, head_y - 34 - rise,
                         13 - 4 * progress, color);
            break;
        case PetMood::SKITTISH:
            for (uint8_t i = 0; i < 3; i++) {
                float side = i == 1 ? 0 : (i == 0 ? -1.0f : 1.0f);
                float x = CENTER_X + side * (i == 1 ? 0 : 59);
                stroke(spr, x - 5, head_y - 27 - rise + i * 5,
                       x + 5, head_y - 33 - rise + i * 5, color);
            }
            break;
        case PetMood::ANNOYED:
            for (uint8_t i = 0; i < 2; i++) {
                float side = i == 0 ? -1.0f : 1.0f;
                float x = CENTER_X + side * 60;
                stroke(spr, x - side * 7, head_y - 35 - rise,
                       x + side * 7, head_y - 25 - rise, color);
                stroke(spr, x - side * 5, head_y - 20 - rise,
                       x + side * 5, head_y - 14 - rise, color);
            }
            break;
        case PetMood::HAPPY:
            for (uint8_t i = 0; i < 2; i++) {
                float side = i == 0 ? -1.0f : 1.0f;
                drawPetHeart(spr, CENTER_X + side * 62, head_y - 18 - rise - i * 10,
                             7 - 2 * progress, color);
            }
            break;
        case PetMood::PLAYFUL:
            for (uint8_t i = 0; i < 3; i++) {
                float side = i == 1 ? 0 : (i == 0 ? -1.0f : 1.0f);
                drawPetSpark(spr, CENTER_X + side * 60, head_y - 34 - rise + i * 6,
                             8 - 3 * progress, color);
            }
            break;
        case PetMood::SLEEPY:
            for (uint8_t i = 0; i < 2; i++) {
                drawPetZ(spr, CENTER_X + 52 + i * 14, head_y - 34 - rise - i * 14,
                         6 - 2 * progress, color);
            }
            break;
        case PetMood::GRUMPY:
            // Two short bars, the way a comic shows a huff.
            for (uint8_t i = 0; i < 2; i++) {
                float side = i == 0 ? -1.0f : 1.0f;
                float x = CENTER_X + side * 58;
                stroke(spr, x, head_y - 30 - rise, x + side * 9, head_y - 38 - rise, color);
                stroke(spr, x - side * 2, head_y - 20 - rise, x + side * 7, head_y - 26 - rise, color);
            }
            break;
        case PetMood::CALM:
        default:
            // Purr: soft arcs washing outward from both cheeks.
            for (uint8_t i = 0; i < 2; i++) {
                float side = i == 0 ? 180.0f : 0.0f;
                strokeArc(spr, CENTER_X, head_y, 62 + rise, 2,
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
    float pop = lerpf(0.88f, 1, easeOutBack(mood_age));

    float head_r = 50 * pop * lerpf(0.85f, 1, enter);
    float head_x = CENTER_X + f.wobble;
    float head_y = CENTER_Y - 4 + f.bounce;
    uint16_t line = mix(COLOR_BG, accent, enter);

    // Ears, then the head outline over their bases.
    for (int8_t side = -1; side <= 1; side += 2) {
        float ex = head_x + side * head_r * 0.72f;
        spr.fillTriangle(ex - head_r * 0.26f, head_y - head_r * 0.70f,
                         ex + head_r * 0.26f, head_y - head_r * 0.60f,
                         ex + side * head_r * 0.14f, head_y - head_r * 1.34f, line);
    }
    strokeArc(spr, head_x, head_y, head_r, 2, 0, 360, line);
    strokeArc(spr, head_x, head_y, head_r - 4, 1, 0, 360, mix(COLOR_BG, dim(accent, 0.35f), enter));

    float eye_dx = head_r * 0.42f;
    float eye_y = head_y - head_r * 0.10f;
    uint16_t eye_color = mix(COLOR_BG, COLOR_TEXT, enter);
    drawPetEye(spr, head_x - eye_dx, eye_y, f, f.left_open, eye_color);
    drawPetEye(spr, head_x + eye_dx, eye_y, f, f.right_open, eye_color);

    if (f.brow > 0.01f) {
        // Brows only exist when a mood needs them, which is what makes them read.
        float drop = 6 + 4 * clamp01(f.brow);
        for (int8_t side = -1; side <= 1; side += 2) {
            float bx = head_x + side * eye_dx;
            stroke(spr, bx - side * f.eye_rx, eye_y - drop - 6,
                   bx + side * f.eye_rx, eye_y - drop, line);
        }
    }

    // Nose, then the mouth: one arc, curving up or down with the mood.
    spr.fillCircle(head_x, head_y + head_r * 0.24f, 2.5f, line);
    float mouth_y = head_y + head_r * 0.40f;
    float curve = f.mouth;
    if (fabsf(curve) < 0.06f) {
        stroke(spr, head_x - f.mouth_width / 2, mouth_y, head_x + f.mouth_width / 2, mouth_y, line);
    } else {
        float radius = f.mouth_width / (0.55f + 0.75f * fabsf(curve));
        if (curve > 0) {
            strokeArc(spr, head_x, mouth_y - radius * 0.72f, radius, 2, 214, 326, line);
        } else {
            strokeArc(spr, head_x, mouth_y + radius * 0.72f, radius, 2, 34, 146, line);
        }
    }

    if (reaction > 0.01f) {
        drawPetEmote(spr, mood, head_y, 1 - reaction, accent);
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
    trackedText(spr, name, CENTER_X, 24, &FreeSansBold9pt7b, mix(COLOR_BG, accent, enter), 3);

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
    if (status != nullptr && ui_state.hold_progress <= 0.02f) {
        centeredText(spr, status, CENTER_X, CENTER_Y + 66, &FreeSans9pt7b,
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

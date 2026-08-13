#pragma once

#include <Arduino.h>

enum class UiMode : uint8_t {
    /** Rotate to browse the app carousel, press to open. */
    MENU,
    /** A local app page is open; hold to go back to the menu. */
    APP,
    /** A config arrived over serial, so a host is driving the knob. */
    REMOTE,
    /** Vertical settings list; rotate to scroll, press to edit a row. */
    SETTINGS,
    /** One settings row is being adjusted; press or hold to leave. */
    SETTING_EDIT,
};

struct UiState {
    UiMode mode = UiMode::MENU;
    /** Index into APPS while mode == APP. */
    uint8_t app_index = 0;
    /** 0-1 progress of the press-and-hold that returns to the menu. */
    float hold_progress = 0;
    /** Bumped on every mode change so the renderer can start a transition. */
    uint8_t mode_nonce = 0;
    /** Bumped on every short press so the renderer can play a tap ripple. */
    uint8_t press_nonce = 0;

    /** Highlighted row while mode is SETTINGS or SETTING_EDIT. */
    uint8_t setting_index = 0;
    /**
     * Value of each settings row, so the list can show all of them at once.
     * Sized to SETTING_COUNT, which InterfaceTask asserts at startup; settings.h
     * cannot be included here without a cycle, so the two are kept in step by
     * that assert rather than by the type.
     */
    int16_t setting_values[12] = {};

    /** Seconds left on the timer app, or 0 when it is not counting down. */
    uint32_t timer_remaining_s = 0;
    /** Duration the running countdown started from, for the progress arc. */
    uint32_t timer_total_s = 0;
    bool timer_running = false;
    /** Set when a countdown reaches zero, cleared once acknowledged. */
    bool timer_elapsed = false;

    /** Strain calibration phase, 0 when not calibrating (see InterfaceTask). */
    uint8_t calibration_step = 0;
    /** Bumped when settings are restored to defaults, so the row can say so. */
    uint8_t settings_reset_nonce = 0;

    /** Pet page: the creature's mood, as a PetMood. */
    uint8_t pet_mood = 0;
    /** 0-1 attention banked from patting, which is what moves the mood along. */
    float pet_charge = 0;
    /** 0-1 agitation caused by hurried or overly forceful strokes. */
    float pet_agitation = 0;
    /** Smoothed patting speed in degrees per second. */
    float pet_speed = 0;
    /** True while pats are still arriving, so the face can react mid-stroke. */
    bool pet_petting = false;
    /** Bumped when a pat ends, so the renderer plays the after-pat emote. */
    uint8_t pet_reaction_nonce = 0;
    /** Bumped on every mood change, so the new face can animate in. */
    uint8_t pet_mood_nonce = 0;
};

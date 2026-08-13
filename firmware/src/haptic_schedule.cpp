#include "haptic_schedule.h"

#include "motors/motor_config.h"
#include "util.h"

// Stiffness per unit of detent (or endstop) strength. Carried over unchanged:
// this is the factor the strengths in every app config were chosen against.
static const float STIFFNESS_PER_STRENGTH_UNIT = 4.0f;

// Damping, as a function of detent width. Fine detents want more of it: their
// angular errors are small, so the spring is weak there and there is little to
// stop the knob ringing. Coarse detents want less, because a large D on a coarse
// detent just amplifies sensor noise into audible motor buzz.
static const float DAMPING_LOWER_PER_STRENGTH_UNIT = 0.08f;
static const float DAMPING_UPPER_PER_STRENGTH_UNIT = 0.02f;
static const float DAMPING_WIDTH_LOWER_RADIANS = 3 * PI / 180;
static const float DAMPING_WIDTH_UPPER_RADIANS = 8 * PI / 180;

// The click. On a fine detent the spring cannot produce one: at the moment of
// crossing, the angular error is a fraction of a degree, so the torque step is
// too small to feel. A short burst in the direction of travel is what makes the
// new value register in the hand. By the time detents are this wide the spring is
// doing it unaided and the burst would read as a double click.
static const float CLICK_WIDTH_FADE_RADIANS = 10 * PI / 180;
static const float CLICK_TORQUE_PER_STRENGTH_UNIT = 2.0f;
static const uint32_t CLICK_DURATION_MICROS = 3000;

// The integral is for the last fraction of a degree of cogging droop, so it is
// confined to a fraction of one detent. Past that the knob is being turned, not
// settling.
static const float INTEGRAL_BAND_DETENT_FRACTION = 0.3f;
static const float INTEGRAL_BAND_MAX_RADIANS = 3 * PI / 180;

/** 1 below the fade window, 0 above it, linear between. */
static float velocityFade(float velocity) {
    float speed = fabsf(velocity);
    if (speed <= VELOCITY_FADE_START_RAD_PER_SEC) {
        return 1;
    }
    if (speed >= VELOCITY_FADE_END_RAD_PER_SEC) {
        return 0;
    }
    return 1 - (speed - VELOCITY_FADE_START_RAD_PER_SEC)
            / (VELOCITY_FADE_END_RAD_PER_SEC - VELOCITY_FADE_START_RAD_PER_SEC);
}

HapticSchedule scheduleHaptics(const HapticContext& context, const HapticGainScale& scale) {
    const PB_SmartKnobConfig& config = context.config;

    HapticSchedule out;
    out.gains.limit = FOC_PID_LIMIT;
    out.gains.ramp = FOC_PID_OUTPUT_RAMP;

    if (!context.spring_active) {
        // Free-spinning between intermittent detents: nothing pushes back, and
        // nothing drags either.
        return out;
    }

    float fade = velocityFade(context.velocity);
    if (fade <= 0) {
        return out;
    }

    float strength = context.out_of_bounds ? config.endstop_strength_unit : config.detent_strength_unit;

    // Damping: piecewise linear in detent width, clamped to the band its two
    // endpoints describe so widths outside the window get the nearer endpoint
    // rather than an extrapolation.
    float damping_lower = config.detent_strength_unit * DAMPING_LOWER_PER_STRENGTH_UNIT;
    float damping_upper = config.detent_strength_unit * DAMPING_UPPER_PER_STRENGTH_UNIT;
    float damping_raw = damping_lower
            + (damping_upper - damping_lower)
              / (DAMPING_WIDTH_UPPER_RADIANS - DAMPING_WIDTH_LOWER_RADIANS)
              * (config.position_width_radians - DAMPING_WIDTH_LOWER_RADIANS);
    float damping = CLAMP(damping_raw, min(damping_lower, damping_upper), max(damping_lower, damping_upper));

    out.gains.p = strength * STIFFNESS_PER_STRENGTH_UNIT * scale.p * fade;
    out.gains.d = damping * scale.d * fade;

    // No integral at an endstop: the error there is the user leaning on a wall we
    // are deliberately holding, and an integral would read that as droop to be
    // corrected, wind up against them, and lunge on release.
    if (!context.out_of_bounds) {
        out.gains.i = config.detent_strength_unit * HAPTIC_INTEGRAL_BASE * scale.i * fade;
        out.integral_band_radians = fminf(
            config.position_width_radians * INTEGRAL_BAND_DETENT_FRACTION,
            INTEGRAL_BAND_MAX_RADIANS);
    }

    // Clicks are the detents announcing themselves, so an endstop gets none, and
    // neither does the gap between intermittent detents - the caller only crosses
    // into a real detent there, and petConfig-style textures play their own.
    if (!context.out_of_bounds && config.detent_positions_count == 0
            && config.position_width_radians < CLICK_WIDTH_FADE_RADIANS) {
        float taper = 1 - config.position_width_radians / CLICK_WIDTH_FADE_RADIANS;
        out.click_torque = config.detent_strength_unit * CLICK_TORQUE_PER_STRENGTH_UNIT * taper * fade;
        out.click_duration_micros = CLICK_DURATION_MICROS;
    }

    return out;
}

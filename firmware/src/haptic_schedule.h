#pragma once

#include "haptic_pid.h"
#include "proto_gen/smartknob.pb.h"

/**
 * Turns a haptic config into controller gains.
 *
 * The gains used to be poked at from three places: compile-time FOC_PID_*
 * defines, a piecewise-linear D recompute that ran whenever a config arrived,
 * and a P overwrite on every pass of the motor loop. Nothing owned the result,
 * so there was no single place to read off what the knob was about to feel like.
 * This is that place.
 *
 * The numbers below reproduce the behaviour that was tuned by hand over the old
 * arrangement, with two deliberate departures, both of which the split between
 * damping and click made possible:
 *
 *  - Damping is no longer switched off for intermittent detents. It was only
 *    switched off there because D also produced the click, and the clicks were
 *    unwanted near an intermittent detent; damping itself was never the problem.
 *
 *  - The high-velocity cutout is a taper rather than a cliff. It ends up in the
 *    same place - zero torque by VELOCITY_FADE_END_RAD_PER_SEC, which is the
 *    speed the old hard cutout triggered at - but it gets there smoothly instead
 *    of dropping the spring in one step under the user's hand.
 */

/** What the schedule is being asked about. */
struct HapticContext {
    /** The config now in force. */
    PB_SmartKnobConfig config;
    /** True when the knob is pushing past min_position or max_position. */
    bool out_of_bounds = false;
    /**
     * False when the spring should not act at all: intermittent detents are
     * configured and the current position is not one of them. The knob free-spins
     * there, so every gain including damping goes to zero - anything else would
     * add drag to a movement that is meant to feel unresisted.
     */
    bool spring_active = true;
    /** Measured shaft velocity in rad/s, for the high-speed taper. */
    float velocity = 0;
};

struct HapticSchedule {
    HapticGains gains;
    /** Torque for the detent-crossing click; 0 when the spring can do it alone. */
    float click_torque = 0;
    uint32_t click_duration_micros = 0;
    /** Radius around centre the integral may act in; 0 disables it. */
    float integral_band_radians = 0;
};

/**
 * Base integral gain, in torque per radian-second per unit of detent strength.
 * The integral row defaults to 0%, so out of the box the controller is the same
 * PD that was tuned before and this number does nothing; it sets what 100% on
 * that row means once someone turns it up.
 */
constexpr float HAPTIC_INTEGRAL_BASE = 30.0f;

/** Speeds between which every gain fades out, to head off a runaway. */
constexpr float VELOCITY_FADE_START_RAD_PER_SEC = 40.0f;
constexpr float VELOCITY_FADE_END_RAD_PER_SEC = 60.0f;

HapticSchedule scheduleHaptics(const HapticContext& context, const HapticGainScale& scale);

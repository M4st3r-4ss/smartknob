#pragma once

#include <Arduino.h>

/**
 * The haptic torque controller.
 *
 * This replaces the borrowed SimpleFOC PID_velocity object the loop used to run
 * on. That worked, but it left three problems that could not be fixed from the
 * outside:
 *
 *  - Its derivative was taken on the error signal. The error jumps by a whole
 *    position width every time the knob crosses into the next detent, so every
 *    crossing fed the derivative a step, and the resulting one-loop torque spike
 *    was what produced the "click" on fine detents. That made damping and click
 *    strength the same number, so neither could be set without wrecking the
 *    other, and it also meant the derivative amplified sensor noise all the rest
 *    of the time. Here the derivative is taken on measured velocity, and the
 *    click is an explicit impulse (see kick()).
 *
 *  - It had no guard on its own timestep. Playing a haptic blocks the motor loop
 *    for ~6ms, and the next update divided a normal error change by that stale
 *    dt, so every click was followed by a derivative kick nobody asked for.
 *
 *  - Its integral was unusable here: nothing gated or leaked it, so any sustained
 *    hold off-centre wound it up and the knob lunged when released. The integral
 *    below is gated to small errors and leaks continuously, which is what makes
 *    it safe to use for taking out cogging droop.
 *
 * Torque is in the same arbitrary volts-ish units SimpleFOC's move() takes, so
 * the gain magnitudes carried over from the old FOC_PID_* defines still mean
 * roughly what they used to.
 */

/** Gains for one haptic mode, in the units the loop actually works in. */
struct HapticGains {
    /** Torque per radian of error: the stiffness of the detent spring. */
    float p = 0;
    /** Torque per radian-second of accumulated error; takes out cogging droop. */
    float i = 0;
    /** Torque per radian/second of velocity: damping, and nothing else. */
    float d = 0;
    /** Clamp on the returned torque. */
    float limit = 10;
    /**
     * Largest torque change per second the spring terms may make. The click
     * impulse deliberately bypasses this - a rate-limited impulse is not an
     * impulse. 0 disables the limit.
     */
    float ramp = 10000;
};

/**
 * User-facing multipliers on the scheduled gains, from the settings rows and
 * from serial tuning. 1 means "whatever the schedule chose".
 *
 * Deliberately left without default member initialisers: this travels inside the
 * motor task's command union, and a union member with a non-trivial default
 * constructor deletes the union's own. Callers use HAPTIC_GAIN_SCALE_DEFAULT.
 */
struct HapticGainScale {
    float p;
    float i;
    float d;
};

constexpr HapticGainScale HAPTIC_GAIN_SCALE_DEFAULT = {1, 1, 1};

/**
 * One update's worth of input. The three terms deliberately do not all read the
 * same signal:
 *
 *  - P reads the dead-zoned error, so it stays quiet within a degree of centre
 *    instead of hunting there.
 *  - I reads the raw error, because the droop it exists to remove is smaller than
 *    the dead zone and is invisible to a dead-zoned signal. It is gated to near
 *    standstill instead (see INTEGRAL_VELOCITY_GATE_RAD_PER_SEC), which is what
 *    keeps it from fighting a hand that is holding the knob off-centre.
 *  - D reads measured velocity rather than differentiating anything.
 */
struct HapticInput {
    /** Dead-zoned error in radians; positive means short of centre. */
    float error = 0;
    /** Same error before the dead zone was applied. */
    float integral_error = 0;
    /** Measured shaft velocity in rad/s, in the same frame as the error. */
    float velocity = 0;
    uint32_t now_micros = 0;
};

/** One update's worth of the controller's internals, for tuning and telemetry. */
struct HapticTrace {
    /** Error fed in, in radians; positive means the knob is short of centre. */
    float error = 0;
    float velocity = 0;
    float p_term = 0;
    float i_term = 0;
    float d_term = 0;
    /** Click impulse still being played, included in output. */
    float impulse = 0;
    /** Torque actually returned, after ramp and clamp. */
    float output = 0;
    /** Timestep used, in seconds. */
    float dt = 0;
    /** Set when the clamp was hit, so the integral was held. */
    bool saturated = false;
    /** Set when dt was implausible and the I/D terms were skipped for it. */
    bool dt_rejected = false;
};

class HapticPid {
    public:
        /**
         * Largest timestep worth differentiating over. The motor loop normally
         * runs every ~1ms; anything past this means it was blocked (a click, a
         * config change, calibration) and the history either side of the gap is
         * not comparable.
         */
        static constexpr float MAX_DT_SECONDS = 0.008f;
        /** Floor, so a double-call in the same microsecond cannot divide by zero. */
        static constexpr float MIN_DT_SECONDS = 0.00005f;

        /**
         * The integral only exists to take out the last fraction of a radian of
         * cogging droop, so it is gated off entirely outside this multiple of the
         * error scale it was given, and bled away at this rate whatever happens.
         * Together these are what stop a sustained hold from winding it up.
         */
        static constexpr float INTEGRAL_LEAK_PER_SECOND = 1.2f;
        /** Cap on the integral's own share of the output limit. */
        static constexpr float INTEGRAL_AUTHORITY = 0.35f;
        /**
         * The integral only accumulates below this speed. Above it the knob is
         * being turned, and a term whose whole job is to correct where the knob
         * comes to rest has nothing to say about where it is passing through.
         */
        static constexpr float INTEGRAL_VELOCITY_GATE_RAD_PER_SEC = 0.5f;

        void setGains(const HapticGains& gains) { gains_ = gains; }
        const HapticGains& gains() const { return gains_; }

        /**
         * Error band the integral is allowed to work in, in radians. Set from the
         * detent width: past a fraction of one detent the knob is being moved,
         * not settling, and an integral has no business there.
         */
        void setIntegralBand(float radians) { integral_band_ = radians; }

        /**
         * Drops all history: integral, derivative and any impulse in flight. Call
         * when the setpoint moves for a reason of our own (new config, new page,
         * calibration) so the discontinuity is not read as an error the user made.
         */
        void reset();

        /**
         * Adds a torque impulse decaying to zero over duration_micros, in the
         * direction of sign(torque). This is the detent "click": on fine detents
         * the spring alone cannot produce one, because the angular error at the
         * moment of crossing is too small to feel.
         *
         * A second kick replaces whatever is still playing rather than summing
         * with it, so spinning the knob fast gives one click per detent instead of
         * an accumulating shove.
         */
        void kick(float torque, uint32_t duration_micros);

        /** Runs one update and returns the torque to apply. */
        float update(const HapticInput& in);

        const HapticTrace& trace() const { return trace_; }

    private:
        HapticGains gains_;
        float integral_band_ = 0;

        bool have_history_ = false;
        uint32_t last_micros_ = 0;
        float integral_ = 0;
        /** Previous spring output, for the slew limit. Excludes the impulse. */
        float last_spring_ = 0;

        /** Impulse in flight: its starting torque, and when it began and ends. */
        float impulse_torque_ = 0;
        uint32_t impulse_start_micros_ = 0;
        uint32_t impulse_duration_micros_ = 0;

        HapticTrace trace_;

        /** Impulse contribution at now_micros, decaying linearly to zero. */
        float impulseAt(uint32_t now_micros);
};

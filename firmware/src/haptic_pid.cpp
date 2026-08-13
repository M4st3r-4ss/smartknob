#include "haptic_pid.h"

#include "util.h"

void HapticPid::reset() {
    have_history_ = false;
    integral_ = 0;
    last_spring_ = 0;
    impulse_torque_ = 0;
    impulse_duration_micros_ = 0;
    trace_ = HapticTrace();
}

void HapticPid::kick(float torque, uint32_t duration_micros) {
    if (duration_micros == 0 || torque == 0) {
        return;
    }
    // Replaced rather than summed: see the header. One click per detent, however
    // fast the detents go past.
    impulse_torque_ = CLAMP(torque, -gains_.limit, gains_.limit);
    impulse_duration_micros_ = duration_micros;
    impulse_start_micros_ = micros();
}

float HapticPid::impulseAt(uint32_t now_micros) {
    if (impulse_duration_micros_ == 0) {
        return 0;
    }
    // Unsigned subtraction, so this stays right across a micros() wrap.
    uint32_t elapsed = now_micros - impulse_start_micros_;
    if (elapsed >= impulse_duration_micros_) {
        impulse_torque_ = 0;
        impulse_duration_micros_ = 0;
        return 0;
    }
    float remaining = 1.0f - (float)elapsed / (float)impulse_duration_micros_;
    return impulse_torque_ * remaining;
}

float HapticPid::update(const HapticInput& in) {
    trace_ = HapticTrace();
    trace_.error = in.error;
    trace_.velocity = in.velocity;

    float dt = 0;
    bool dt_ok = false;
    if (have_history_) {
        uint32_t elapsed = in.now_micros - last_micros_;
        dt = elapsed * 1e-6f;
        dt_ok = dt >= MIN_DT_SECONDS && dt <= MAX_DT_SECONDS;
    }
    last_micros_ = in.now_micros;
    have_history_ = true;
    trace_.dt = dt;
    trace_.dt_rejected = !dt_ok;

    float p_term = gains_.p * in.error;

    // Integral. Kept out of the way unless the knob is genuinely settling: gated
    // to a band around centre and to near standstill, leaked continuously, and
    // capped to a share of the output limit so it can never be the loudest term.
    float integral_before = integral_;
    float i_term = 0;
    if (gains_.i <= 0) {
        integral_ = 0;
    } else {
        bool settling = fabsf(in.velocity) <= INTEGRAL_VELOCITY_GATE_RAD_PER_SEC
                && (integral_band_ <= 0 || fabsf(in.integral_error) <= integral_band_);
        if (!settling) {
            integral_ = 0;
        } else if (dt_ok) {
            integral_ += in.integral_error * dt;
            integral_ -= integral_ * CLAMP(INTEGRAL_LEAK_PER_SECOND * dt, 0.0f, 1.0f);
            float authority = gains_.limit * INTEGRAL_AUTHORITY / gains_.i;
            integral_ = CLAMP(integral_, -authority, authority);
        }
        i_term = gains_.i * integral_;
    }

    // Damping, straight off measured velocity. Needs no timestep of its own,
    // which is why it survives the blocked loop a click causes.
    float d_term = -gains_.d * in.velocity;

    float spring = p_term + i_term + d_term;

    // Slew limit, on the spring terms only.
    if (dt_ok && gains_.ramp > 0) {
        float max_step = gains_.ramp * dt;
        spring = CLAMP(spring, last_spring_ - max_step, last_spring_ + max_step);
    }
    last_spring_ = spring;

    float impulse = impulseAt(in.now_micros);
    float output = spring + impulse;
    float clamped = CLAMP(output, -gains_.limit, gains_.limit);

    if (clamped != output) {
        trace_.saturated = true;
        // Clamping anti-windup: the integral does not get to keep what the clamp
        // just threw away, or it would carry on growing while the output cannot.
        integral_ = integral_before;
        i_term = gains_.i * integral_;
    }

    trace_.p_term = p_term;
    trace_.i_term = i_term;
    trace_.d_term = d_term;
    trace_.impulse = impulse;
    trace_.output = clamped;
    return clamped;
}

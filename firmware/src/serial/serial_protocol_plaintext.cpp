#include "../proto_gen/smartknob.pb.h"

#include "serial_protocol_plaintext.h"

void SerialProtocolPlaintext::handleState(const PB_SmartKnobState& state) {
    bool substantial_change = (latest_state_.current_position != state.current_position)
        || (latest_state_.config.detent_strength_unit != state.config.detent_strength_unit)
        || (latest_state_.config.endstop_strength_unit != state.config.endstop_strength_unit)
        || (latest_state_.config.min_position != state.config.min_position)
        || (latest_state_.config.max_position != state.config.max_position);
    latest_state_ = state;

    if (substantial_change) {
        stream_.printf("STATE: %d [%d, %d]  (detent strength: %0.2f, width: %0.0f deg, endstop strength: %0.2f)\n", 
            state.current_position,
            state.config.min_position,
            state.config.max_position,
            state.config.detent_strength_unit,
            degrees(state.config.position_width_radians),
            state.config.endstop_strength_unit);
    }
}

void SerialProtocolPlaintext::log(const char* msg) {
    stream_.print("LOG: ");
    stream_.println(msg);
}

void SerialProtocolPlaintext::sendHostCommand(const char* verb, const char* channel) {
    stream_.printf("@%s %s\n", verb, channel);
}

void SerialProtocolPlaintext::sendHostCommand(const char* verb, const char* channel, int32_t value) {
    stream_.printf("@%s %s %ld\n", verb, channel, (long)value);
}

void SerialProtocolPlaintext::sendTraceHeader() {
    // Sent once when the stream starts, so a capture describes its own columns
    // and the tuning tool does not carry a copy of this order that can drift.
    stream_.println("TRACE_HEADER: ms,pos,sub,angle,center,err,vel,p,i,d,imp,out,dt,flags,gp,gi,gd");
}

void SerialProtocolPlaintext::sendTrace(const HapticTelemetry& telemetry) {
    // The two booleans share one column as bits (1 = clamped, 2 = timestep
    // rejected). At 200 samples a second the line length is what decides whether
    // the link keeps up, and the decimals below are already past where the sensor
    // stops being meaningful.
    uint8_t flags = (telemetry.pid.saturated ? 1 : 0) | (telemetry.pid.dt_rejected ? 2 : 0);
    stream_.printf("TRACE: %lu,%ld,%.3f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.5f,%u,%.3f,%.3f,%.3f\n",
        (unsigned long)telemetry.millis,
        (long)telemetry.position,
        telemetry.sub_position_unit,
        telemetry.angle,
        telemetry.detent_center,
        telemetry.pid.error,
        telemetry.pid.velocity,
        telemetry.pid.p_term,
        telemetry.pid.i_term,
        telemetry.pid.d_term,
        telemetry.pid.impulse,
        telemetry.pid.output,
        telemetry.pid.dt,
        (unsigned)flags,
        telemetry.gains.p,
        telemetry.gains.i,
        telemetry.gains.d);
}

/** Parses "@VAL", "@PID" and "@TRACE"; anything else is ignored. */
void SerialProtocolPlaintext::handleLine() {
    if (strncmp(line_, "VAL ", 4) == 0) {
        char* channel = line_ + 4;
        char* space = strchr(channel, ' ');
        if (space == nullptr) {
            return;
        }
        *space = '\0';
        if (channel[0] == '\0' || !host_value_callback_) {
            return;
        }
        host_value_callback_(channel, (int32_t)strtol(space + 1, nullptr, 10));
        return;
    }

    // "@PID <p> <i> <d>": all three percentages every time. Accepting them one at
    // a time would mean holding the other two here, and the settings store is
    // already the place that remembers them.
    if (strncmp(line_, "PID ", 4) == 0) {
        if (!haptic_tune_callback_) {
            return;
        }
        char* cursor = line_ + 4;
        long values[3];
        for (uint8_t i = 0; i < 3; i++) {
            char* end = nullptr;
            values[i] = strtol(cursor, &end, 10);
            // strtol returns 0 both for "0" and for a word that is not a number;
            // an unmoved cursor is what tells the two apart. A half-parsed line
            // applies nothing rather than a partial set of gains.
            if (end == cursor) {
                return;
            }
            cursor = end;
        }
        haptic_tune_callback_((int16_t)values[0], (int16_t)values[1], (int16_t)values[2]);
        return;
    }

    if (strncmp(line_, "TRACE ", 6) == 0) {
        if (trace_callback_) {
            trace_callback_(strtol(line_ + 6, nullptr, 10) != 0);
        }
        return;
    }
}

void SerialProtocolPlaintext::loop() {
    while (stream_.available() > 0) {
        int b = stream_.read();

        // Host-agent commands arrive as "@..." lines; single-byte commands below
        // keep working because only a leading '@' enters line mode.
        if (in_line_) {
            if (b == '\n' || b == '\r') {
                line_[line_len_] = '\0';
                in_line_ = false;
                line_len_ = 0;
                handleLine();
            } else if (line_len_ < (uint8_t)(sizeof(line_) - 1)) {
                line_[line_len_++] = (char)b;
            }
            continue;
        }
        if (b == '@') {
            in_line_ = true;
            line_len_ = 0;
            continue;
        }

        if (b == 0) {
            if (protocol_change_callback_) {
                protocol_change_callback_(SERIAL_PROTOCOL_PROTO);
            }
            break;
        }
        if (b == ' ') {
            if (press_callback_) {
                press_callback_();
            }
        } else if (b == 'B' || b == 'b') {
            if (back_callback_) {
                back_callback_();
            }
        } else if (b == 'C') {
            motor_calibration_callback_();
        } else if (b == 'S') {
            if (strain_calibration_callback_) {
                strain_calibration_callback_();
            }
        }
    }
}

void SerialProtocolPlaintext::init(PressCallback press_callback, BackCallback back_callback, StrainCalibrationCallback strain_calibration_callback) {
    press_callback_ = press_callback;
    back_callback_ = back_callback;
    strain_calibration_callback_ = strain_calibration_callback;
    stream_.println("SmartKnob starting!\n\nSerial mode: plaintext\nPress 'C' at any time to calibrate motor/sensor.\nPress 'S' at any time to calibrate strain sensors.\nPress <Space> to select (same as pressing the knob).\nPress 'B' to go back to the menu (same as holding the knob).\nHost agent: knob sends '@SET <channel> <value>' and '@GET <channel>'; reply with '@VAL <channel> <value>'.\nHaptic tuning: '@PID <p> <i> <d>' sets the three multipliers in percent; '@TRACE 1' streams the loop.");
}

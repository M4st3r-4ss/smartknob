#include <SimpleFOC.h>

#include "motor_task.h"
#if SENSOR_MT6701
#include "mt6701_sensor.h"
#elif SENSOR_TLV
#include "tlv_sensor.h"
#elif SENSOR_MAQ430
#include "maq430_sensor.h"
#endif

#include "motors/motor_config.h"
#include "util.h"

static const float DEAD_ZONE_DETENT_PERCENT = 0.2;
static const float DEAD_ZONE_RAD = 1 * _PI / 180;

static const float IDLE_VELOCITY_EWMA_ALPHA = 0.001;
static const float IDLE_VELOCITY_RAD_PER_SEC = 0.05;
static const uint32_t IDLE_CORRECTION_DELAY_MILLIS = 500;
static const float IDLE_CORRECTION_MAX_ANGLE_RAD = 5 * PI / 180;
static const float IDLE_CORRECTION_RATE_ALPHA = 0.0005;

// Filter on the velocity SimpleFOC reports. This used to be a detail of the
// sensor read that nothing much depended on; it is now the damping term's input
// signal, so it is set explicitly rather than left at whichever default the
// linked SimpleFOC version happens to ship. Raise it if damping sounds gritty,
// lower it if damping feels like it arrives late.
#ifndef FOC_LPF_VELOCITY
#define FOC_LPF_VELOCITY 0.005
#endif


MotorTask::MotorTask(const uint8_t task_core, Configuration& configuration) : Task("Motor", 4000, 1, task_core), configuration_(configuration) {
    queue_ = xQueueCreate(5, sizeof(Command));
    assert(queue_ != NULL);
}

MotorTask::~MotorTask() {}


#if SENSOR_TLV
    TlvSensor encoder = TlvSensor();
#elif SENSOR_MT6701
    MT6701Sensor encoder = MT6701Sensor();
#elif SENSOR_MAQ430
    MagneticSensorSPI encoder = MagneticSensorSPI(MAQ430_SPI, PIN_MAQ_SS);
#endif

void MotorTask::run() {

    driver.voltage_power_supply = 5;
    driver.init();

    #if SENSOR_TLV
    encoder.init(&Wire, false);
    #elif SENSOR_MT6701
    encoder.init();
    #elif SENSOR_MAQ430
    SPIClass* spi = new SPIClass(HSPI);
    spi->begin(PIN_MAQ_SCK, PIN_MAQ_MISO, PIN_MAQ_MOSI, PIN_MAQ_SS);
    encoder.init(spi);
    #endif

    motor.linkDriver(&driver);

    motor.controller = MotionControlType::torque;
    motor.voltage_limit = FOC_VOLTAGE_LIMIT;
    motor.velocity_limit = 10000;
    motor.linkSensor(&encoder);

    // The haptic torque loop runs on HapticPid (see haptic_pid.h), not on
    // SimpleFOC's own PID objects. SimpleFOC is left doing what it is good at:
    // commutation, and the sensor filtering below.
    #ifdef FOC_LPF
    motor.LPF_angle.Tf = FOC_LPF;
    #endif
    // Damping reads this, so the filter on it is part of the haptic tuning.
    motor.LPF_velocity.Tf = FOC_LPF_VELOCITY;

    motor.init();

    encoder.update();
    delay(10);

    PB_PersistentConfiguration c = configuration_.get();
    motor.pole_pairs = c.motor.calibrated ? c.motor.pole_pairs : 7;
    motor.initFOC(c.motor.zero_electrical_offset, c.motor.direction_cw ? Direction::CW : Direction::CCW);

    motor.monitor_downsample = 0; // disable monitor at first - optional

    // disableCore0WDT();

    float current_detent_center = motor.shaft_angle;
    PB_SmartKnobConfig config = {
        .position = 0,
        .sub_position_unit = 0,
        .position_nonce = 0,
        .min_position = 0,
        .max_position = 1,
        .position_width_radians = 60 * _PI / 180,
        .detent_strength_unit = 0,
    };
    int32_t current_position = 0;
    float latest_sub_position_unit = 0;

    float idle_check_velocity_ewma = 0;
    uint32_t last_idle_start = 0;
    uint32_t last_publish = 0;

    while (1) {
        motor.loopFOC();

        // Check queue for pending requests from other tasks
        Command command;
        if (xQueueReceive(queue_, &command, 0) == pdTRUE) {
            switch (command.command_type) {
                case CommandType::CALIBRATE:
                    calibrate();
                    break;
                case CommandType::CONFIG: {
                    // Check new config for validity
                    PB_SmartKnobConfig& new_config = command.data.config;
                    if (new_config.detent_strength_unit < 0) {
                        log("Ignoring invalid config: detent_strength_unit cannot be negative");
                        break;
                    }
                    if (new_config.endstop_strength_unit < 0) {
                        log("Ignoring invalid config: endstop_strength_unit cannot be negative");
                        break;
                    }
                    if (new_config.snap_point < 0.5) {
                        log("Ignoring invalid config: snap_point must be >= 0.5 for stability");
                        break;
                    }
                    if (new_config.detent_positions_count > COUNT_OF(new_config.detent_positions)) {
                        log("Ignoring invalid config: detent_positions_count is too large");
                        break;
                    }
                    if (new_config.snap_point_bias < 0) {
                        log("Ignoring invalid config: snap_point_bias cannot be negative or there is risk of instability");
                        break;
                    }

                    // Change haptic input mode
                    bool position_updated = false;
                    if (new_config.position != config.position
                            || new_config.sub_position_unit != config.sub_position_unit
                            || new_config.position_nonce != config.position_nonce) {
                        log("applying position change");
                        current_position = new_config.position;
                        position_updated = true;
                    }

                    if (new_config.min_position <= new_config.max_position) {
                        // Only check bounds if min/max indicate bounds are active (min >= max)
                        if (current_position < new_config.min_position) {
                            current_position = new_config.min_position;
                            log("adjusting position to min");
                        } else if (current_position > new_config.max_position) {
                            current_position = new_config.max_position;
                            log("adjusting position to max");
                        }
                    }

                    if (position_updated || new_config.position_width_radians != config.position_width_radians) {
                        log("adjusting detent center");
                        float new_sub_position = position_updated ? new_config.sub_position_unit : latest_sub_position_unit;
                        #if SK_INVERT_ROTATION
                            float shaft_angle = -motor.shaft_angle;
                        #else
                            float shaft_angle = motor.shaft_angle;
                        #endif
                        current_detent_center = shaft_angle + new_sub_position * new_config.position_width_radians;
                    }
                    config = new_config;
                    log("Got new config");

                    // Gains are no longer poked at from here: scheduleHaptics()
                    // derives them from this config on every pass of the loop
                    // below, which is also what lets the velocity taper work.
                    //
                    // The controller's history is dropped instead. A new config
                    // usually moves the detent centre, and that step is our own
                    // doing - carrying an integral or a slew-limited output across
                    // it would show up as the knob shoving back at a page change.
                    if (position_updated) {
                        pid_.reset();
                    }
                    break;
                }
                case CommandType::HAPTIC: {
                    // Play a haptic "click", scaled by the user's click-force setting.
                    float scale = command.data.haptic.strength_scale;
                    if (!(scale > 0)) {
                        scale = 1;
                    }
                    float strength = (command.data.haptic.press ? 5 : 1.5) * scale;
                    strength = CLAMP(strength, (float)-FOC_VOLTAGE_LIMIT, (float)FOC_VOLTAGE_LIMIT);
                    motor.move(strength);
                    for (uint8_t i = 0; i < 3; i++) {
                        motor.loopFOC();
                        delay(1);
                    }
                    motor.move(-strength);
                    for (uint8_t i = 0; i < 3; i++) {
                        motor.loopFOC();
                        delay(1);
                    }
                    motor.move(0);
                    motor.loopFOC();
                    // That drove the motor directly for ~6ms, so the controller's
                    // idea of the last torque applied is now wrong, and its
                    // timestep spans the whole click. Both are dropped rather than
                    // slewed away from: the motor is at zero torque here, which is
                    // exactly the state a fresh controller assumes.
                    pid_.reset();
                    break;
                }
                case CommandType::GAIN_SCALE:
                    gain_scale_ = command.data.gain_scale;
                    snprintf(buf_, sizeof(buf_), "Gain scale: P=%.2f I=%.2f D=%.2f",
                        gain_scale_.p, gain_scale_.i, gain_scale_.d);
                    log(buf_);
                    // The integral was earned under the old gains, so it does not
                    // carry over to the new ones.
                    pid_.reset();
                    break;
                case CommandType::TRACE:
                    trace_enabled_ = command.data.trace_enabled;
                    log(trace_enabled_ ? "Trace on" : "Trace off");
                    break;
            }
        }

        // Everything below works in the "position frame": the direction the
        // position counts up, which on an inverted build is the opposite of the
        // way the sensor counts. current_detent_center is already in this frame.
        //
        // Velocity is carried into it too, because the damping term is about to be
        // subtracted from a torque expressed in it; getting that sign wrong turns
        // damping into positive feedback.
        #if SK_INVERT_ROTATION
            const float position_sign = -1;
        #else
            const float position_sign = 1;
        #endif
        float shaft_angle_in_frame = position_sign * motor.shaft_angle;
        float velocity_in_frame = position_sign * motor.shaft_velocity;

        // If we are not moving and we're close to the center (but not exactly there), slowly adjust the centerpoint to match the current position
        idle_check_velocity_ewma = velocity_in_frame * IDLE_VELOCITY_EWMA_ALPHA + idle_check_velocity_ewma * (1 - IDLE_VELOCITY_EWMA_ALPHA);
        if (fabsf(idle_check_velocity_ewma) > IDLE_VELOCITY_RAD_PER_SEC) {
            last_idle_start = 0;
        } else {
            if (last_idle_start == 0) {
                last_idle_start = millis();
            }
        }
        // Both sides in the position frame. This used to compare the raw sensor
        // angle against a centre held in the position frame, so on an inverted
        // build the two had opposite signs, the test all but never passed, and
        // idle correction silently did nothing.
        if (last_idle_start > 0 && millis() - last_idle_start > IDLE_CORRECTION_DELAY_MILLIS && fabsf(shaft_angle_in_frame - current_detent_center) < IDLE_CORRECTION_MAX_ANGLE_RAD) {
            current_detent_center = shaft_angle_in_frame * IDLE_CORRECTION_RATE_ALPHA + current_detent_center * (1 - IDLE_CORRECTION_RATE_ALPHA);
        }

        // Check where we are relative to the current nearest detent; update our position if we've moved far enough to snap to another detent
        float angle_to_detent_center = shaft_angle_in_frame - current_detent_center;

        float snap_point_radians = config.position_width_radians * config.snap_point;
        float bias_radians = config.position_width_radians * config.snap_point_bias;
        float snap_point_radians_decrease = snap_point_radians + (current_position <= 0 ? bias_radians : -bias_radians);
        float snap_point_radians_increase = -snap_point_radians + (current_position >= 0 ? -bias_radians : bias_radians); 

        int32_t num_positions = config.max_position - config.min_position + 1;
        int32_t crossed = 0;
        if (angle_to_detent_center > snap_point_radians_decrease && (num_positions <= 0 || current_position > config.min_position)) {
            current_detent_center += config.position_width_radians;
            angle_to_detent_center -= config.position_width_radians;
            current_position--;
            crossed = -1;
        } else if (angle_to_detent_center < snap_point_radians_increase && (num_positions <= 0 || current_position < config.max_position)) {
            current_detent_center -= config.position_width_radians;
            angle_to_detent_center += config.position_width_radians;
            current_position++;
            crossed = 1;
        }

        latest_sub_position_unit = -angle_to_detent_center / config.position_width_radians;

        float dead_zone_adjustment = CLAMP(
            angle_to_detent_center,
            fmaxf(-config.position_width_radians*DEAD_ZONE_DETENT_PERCENT, -DEAD_ZONE_RAD),
            fminf(config.position_width_radians*DEAD_ZONE_DETENT_PERCENT, DEAD_ZONE_RAD));

        bool out_of_bounds = num_positions > 0 && ((angle_to_detent_center > 0 && current_position == config.min_position) || (angle_to_detent_center < 0 && current_position == config.max_position));

        // Intermittent detents: only the listed positions push back at all.
        bool spring_active = true;
        if (!out_of_bounds && config.detent_positions_count > 0) {
            spring_active = false;
            for (uint8_t i = 0; i < config.detent_positions_count; i++) {
                if (config.detent_positions[i] == current_position) {
                    spring_active = true;
                    break;
                }
            }
        }

        // Assigned field by field rather than brace-initialised: these structs
        // carry default member initialisers, which stops them being aggregates
        // under the gnu++11 the older ESP32 platform builds with.
        HapticContext context;
        context.config = config;
        context.out_of_bounds = out_of_bounds;
        context.spring_active = spring_active;
        context.velocity = velocity_in_frame;

        HapticSchedule schedule = scheduleHaptics(context, gain_scale_);
        pid_.setGains(schedule.gains);
        pid_.setIntegralBand(schedule.integral_band_radians);

        // A detent was just crossed, so announce it. On anything but the finest
        // detents the schedule asks for no impulse and the spring's own step is
        // the click; see haptic_schedule.cpp.
        if (crossed != 0 && schedule.click_torque > 0) {
            pid_.kick(crossed * schedule.click_torque, schedule.click_duration_micros);
        }

        // The old hard cutout at 60 rad/s lives on as the top of the schedule's
        // velocity taper, which reaches zero gain at the same speed. Nothing needs
        // to special-case it here any more: past that point every term is zero, so
        // the torque below is zero too, without a step change under the hand.
        HapticInput input;
        // P works on the dead-zoned error, I on the raw one (see haptic_pid.h).
        input.error = -angle_to_detent_center + dead_zone_adjustment;
        input.integral_error = -angle_to_detent_center;
        input.velocity = velocity_in_frame;
        input.now_micros = micros();

        float torque = pid_.update(input);
        motor.move(position_sign * torque);

        if (trace_enabled_ && millis() - last_trace_ > TRACE_INTERVAL_MILLIS) {
            HapticTelemetry telemetry;
            telemetry.millis = millis();
            telemetry.angle = shaft_angle_in_frame;
            telemetry.detent_center = current_detent_center;
            telemetry.position = current_position;
            telemetry.sub_position_unit = latest_sub_position_unit;
            telemetry.pid = pid_.trace();
            telemetry.gains = schedule.gains;
            publishTelemetry(telemetry);
            last_trace_ = millis();
        }

        // Publish current status to other registered tasks periodically
        if (millis() - last_publish > 5) {
            publish({
                .current_position = current_position,
                .sub_position_unit = latest_sub_position_unit,
                .has_config = true,
                .config = config,
            });
            last_publish = millis();
        }

        delay(1);
    }
}

void MotorTask::setConfig(const PB_SmartKnobConfig& config) {
    Command command = {
        .command_type = CommandType::CONFIG,
        .data = {
            .config = config,
        }
    };
    xQueueSend(queue_, &command, portMAX_DELAY);
}


void MotorTask::playHaptic(bool press, float strength_scale) {
    Command command = {
        .command_type = CommandType::HAPTIC,
        .data = {
            .haptic = {
                .press = press,
                .strength_scale = strength_scale,
            },
        }
    };
    xQueueSend(queue_, &command, portMAX_DELAY);
}

void MotorTask::runCalibration() {
    Command command = {
        .command_type = CommandType::CALIBRATE,
        .data = {
            .unused = 0,
        }
    };
    xQueueSend(queue_, &command, portMAX_DELAY);
}

void MotorTask::setGainScale(const HapticGainScale& scale) {
    Command command = {
        .command_type = CommandType::GAIN_SCALE,
        .data = {
            .gain_scale = scale,
        }
    };
    xQueueSend(queue_, &command, portMAX_DELAY);
}

void MotorTask::setTraceEnabled(bool enabled) {
    Command command = {
        .command_type = CommandType::TRACE,
        .data = {
            .trace_enabled = enabled,
        }
    };
    xQueueSend(queue_, &command, portMAX_DELAY);
}


void MotorTask::addListener(QueueHandle_t queue) {
    listeners_.push_back(queue);
}

void MotorTask::addTelemetryListener(QueueHandle_t queue) {
    telemetry_listeners_.push_back(queue);
}

void MotorTask::publish(const PB_SmartKnobState& state) {
    for (auto listener : listeners_) {
        xQueueOverwrite(listener, &state);
    }
}

void MotorTask::publishTelemetry(const HapticTelemetry& telemetry) {
    for (auto listener : telemetry_listeners_) {
        // Sent rather than overwritten, and with a zero timeout: the consumer
        // wants every sample, but not at the price of this loop waiting for it.
        // A full queue drops the sample here and the trace shows the gap.
        xQueueSend(listener, &telemetry, 0);
    }
}

void MotorTask::calibrate() {
    // SimpleFOC is supposed to be able to determine this automatically (if you omit params to initFOC), but
    // it seems to have a bug (or I've misconfigured it) that gets both the offset and direction very wrong!
    // So this value is based on experimentation.
    // TODO: dig into SimpleFOC calibration and find/fix the issue

    log("\n\n\nStarting calibration, please DO NOT TOUCH MOTOR until complete!");
    delay(1000);

    motor.controller = MotionControlType::angle_openloop;
    motor.pole_pairs = 1;
    motor.initFOC(0, Direction::CW);

    float a = 0;

    motor.voltage_limit = FOC_VOLTAGE_LIMIT;
    motor.move(a);

    // #### Determine direction motor rotates relative to angle sensor
    for (uint8_t i = 0; i < 200; i++) {
        encoder.update();
        motor.move(a);
        delay(1);
    }
    float start_sensor = encoder.getAngle();

    for (; a < 3 * _2PI; a += 0.01) {
        encoder.update();
        motor.move(a);
        delay(1);
    }

    for (uint8_t i = 0; i < 200; i++) {
        encoder.update();
        delay(1);
    }
    float end_sensor = encoder.getAngle();


    motor.voltage_limit = 0;
    motor.move(a);

    log("");

    float movement_angle = fabsf(end_sensor - start_sensor);
    if (movement_angle < radians(30) || movement_angle > radians(180)) {
        snprintf(buf_, sizeof(buf_), "ERROR! Unexpected sensor change: start=%.2f end=%.2f", start_sensor, end_sensor);
        log(buf_);
        return;
    }

    log("Sensor measures positive for positive motor rotation:");
    if (end_sensor > start_sensor) {
        log("YES, Direction=CW");
        motor.initFOC(0, Direction::CW);
    } else {
        log("NO, Direction=CCW");
        motor.initFOC(0, Direction::CCW);
    }
    snprintf(buf_, sizeof(buf_), "  (start was %.1f, end was %.1f)", start_sensor, end_sensor);
    log(buf_);


    // #### Determine pole-pairs
    // Rotate 20 electrical revolutions and measure mechanical angle traveled, to calculate pole-pairs
    uint8_t electrical_revolutions = 20;
    snprintf(buf_, sizeof(buf_), "Going to measure %d electrical revolutions...", electrical_revolutions);
    log(buf_);
    motor.voltage_limit = FOC_VOLTAGE_LIMIT;
    motor.move(a);
    log("Going to electrical zero...");
    float destination = a + _2PI;
    for (; a < destination; a += 0.03) {
        encoder.update();
        motor.move(a);
        delay(1);
    }
    log("pause..."); // Let momentum settle...
    for (uint16_t i = 0; i < 1000; i++) {
        encoder.update();
        delay(1);
    }
    log("Measuring...");

    start_sensor = motor.sensor_direction * encoder.getAngle();
    destination = a + electrical_revolutions * _2PI;
    for (; a < destination; a += 0.03) {
        encoder.update();
        motor.move(a);
        delay(1);
    }
    for (uint16_t i = 0; i < 1000; i++) {
        encoder.update();
        motor.move(a);
        delay(1);
    }
    end_sensor = motor.sensor_direction * encoder.getAngle();
    motor.voltage_limit = 0;
    motor.move(a);

    if (fabsf(motor.shaft_angle - motor.target) > 1 * PI / 180) {
        log("ERROR: motor did not reach target!");
        while(1) {}
    }

    float electrical_per_mechanical = electrical_revolutions * _2PI / (end_sensor - start_sensor);
    snprintf(buf_, sizeof(buf_), "Electrical angle / mechanical angle (i.e. pole pairs) = %.2f", electrical_per_mechanical);
    log(buf_);

    if (electrical_per_mechanical < 3 || electrical_per_mechanical > 12) {
        snprintf(buf_, sizeof(buf_), "ERROR! Unexpected calculated pole pairs: %.2f", electrical_per_mechanical);
        log(buf_);
        return;
    }

    int measured_pole_pairs = (int)round(electrical_per_mechanical);
    snprintf(buf_, sizeof(buf_), "Pole pairs set to %d", measured_pole_pairs);
    log(buf_);

    delay(1000);


    // #### Determine mechanical offset to electrical zero
    // Measure mechanical angle at every electrical zero for several revolutions
    motor.voltage_limit = FOC_VOLTAGE_LIMIT;
    motor.move(a);
    float offset_x = 0;
    float offset_y = 0;
    float destination1 = (floor(a / _2PI) + measured_pole_pairs / 2.) * _2PI;
    float destination2 = (floor(a / _2PI)) * _2PI;
    for (; a < destination1; a += 0.4) {
        motor.move(a);
        delay(100);
        for (uint8_t i = 0; i < 100; i++) {
            encoder.update();
            delay(1);
        }
        float real_electrical_angle = _normalizeAngle(a);
        float measured_electrical_angle = _normalizeAngle( (float)(motor.sensor_direction * measured_pole_pairs) * encoder.getMechanicalAngle()  - 0);

        float offset_angle = measured_electrical_angle - real_electrical_angle;
        offset_x += cosf(offset_angle);
        offset_y += sinf(offset_angle);

        snprintf(buf_, sizeof(buf_), "%.2f, %.2f, %.2f", degrees(real_electrical_angle), degrees(measured_electrical_angle), degrees(_normalizeAngle(offset_angle)));
        log(buf_);
    }
    for (; a > destination2; a -= 0.4) {
        motor.move(a);
        delay(100);
        for (uint8_t i = 0; i < 100; i++) {
            encoder.update();
            delay(1);
        }
        float real_electrical_angle = _normalizeAngle(a);
        float measured_electrical_angle = _normalizeAngle( (float)(motor.sensor_direction * measured_pole_pairs) * encoder.getMechanicalAngle()  - 0);

        float offset_angle = measured_electrical_angle - real_electrical_angle;
        offset_x += cosf(offset_angle);
        offset_y += sinf(offset_angle);

        snprintf(buf_, sizeof(buf_), "%.2f, %.2f, %.2f", degrees(real_electrical_angle), degrees(measured_electrical_angle), degrees(_normalizeAngle(offset_angle)));
        log(buf_);
    }
    motor.voltage_limit = 0;
    motor.move(a);

    float avg_offset_angle = atan2f(offset_y, offset_x);


    // #### Apply settings
    motor.pole_pairs = measured_pole_pairs;
    motor.zero_electric_angle = avg_offset_angle + _3PI_2;
    motor.voltage_limit = FOC_VOLTAGE_LIMIT;
    motor.controller = MotionControlType::torque;
    // Calibration spent the last several seconds driving the motor open-loop, so
    // the controller's history describes a machine that no longer exists.
    pid_.reset();

    log("");
    log("RESULTS:");
    snprintf(buf_, sizeof(buf_), "  ZERO_ELECTRICAL_OFFSET: %.2f", motor.zero_electric_angle);
    log(buf_);
    if (motor.sensor_direction == Direction::CW) {
        log("  FOC_DIRECTION: Direction::CW");
    } else {
        log("  FOC_DIRECTION: Direction::CCW");
    }
    snprintf(buf_, sizeof(buf_), "  MOTOR_POLE_PAIRS: %d", motor.pole_pairs);
    log(buf_);

    log("");
    log("Saving to persistent configuration...");
    PB_MotorCalibration calibration = {
        .calibrated = true,
        .zero_electrical_offset = motor.zero_electric_angle,
        .direction_cw = motor.sensor_direction == Direction::CW,
        .pole_pairs = motor.pole_pairs,
    };
    if (configuration_.setMotorCalibrationAndSave(calibration)) {
        log("Success!");
    }
}

void MotorTask::checkSensorError() {
#if SENSOR_TLV
    if (encoder.getAndClearError()) {
        log("LOCKED!");
    }
#elif SENSOR_MT6701
    MT6701Error error = encoder.getAndClearError();
    if (error.error) {
        snprintf(buf_, sizeof(buf_), "CRC error. Received %d; calculated %d", error.received_crc, error.calculated_crc);
        log(buf_);
    }
#endif
}

void MotorTask::setLogger(Logger* logger) {
    logger_ = logger;
}

void MotorTask::log(const char* msg) {
    if (logger_ != nullptr) {
        logger_->log(msg);
    }
}

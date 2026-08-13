#pragma once

#include <Arduino.h>
#include <SimpleFOC.h>
#include <vector>

#include "configuration.h"
#include "haptic_pid.h"
#include "haptic_schedule.h"
#include "logger.h"
#include "proto_gen/smartknob.pb.h"
#include "task.h"


enum class CommandType {
    CALIBRATE,
    CONFIG,
    HAPTIC,
    /** New P/I/D multipliers, from the settings rows or from serial tuning. */
    GAIN_SCALE,
    /** Turns the telemetry stream on or off. */
    TRACE,
};

struct HapticData {
    bool press;
    /** Multiplier on the click torque, so the settings page can tune the feel. */
    float strength_scale;
};

struct Command {
    CommandType command_type;
    union CommandData {
        uint8_t unused;
        PB_SmartKnobConfig config;
        HapticData haptic;
        HapticGainScale gain_scale;
        bool trace_enabled;
    };
    CommandData data;
};

/**
 * One sampled pass of the haptic loop, for the tuning tools. Published on its own
 * queue rather than folded into PB_SmartKnobState: the state message is a
 * checked-in nanopb type, and regenerating it needs the nanopb submodule that
 * building the firmware deliberately does without.
 */
struct HapticTelemetry {
    uint32_t millis = 0;
    /** Shaft angle and detent centre, both in the position frame (see run()). */
    float angle = 0;
    float detent_center = 0;
    int32_t position = 0;
    float sub_position_unit = 0;
    /** What the controller did with all that. */
    HapticTrace pid;
    /** What the schedule asked for, so a trace can be read without guessing. */
    HapticGains gains;
};

class MotorTask : public Task<MotorTask> {
    friend class Task<MotorTask>; // Allow base Task to invoke protected run()

    public:
        MotorTask(const uint8_t task_core, Configuration& configuration);
        ~MotorTask();

        void setConfig(const PB_SmartKnobConfig& config);
        void playHaptic(bool press, float strength_scale = 1);
        void runCalibration();
        /** Applies new P/I/D multipliers on top of the scheduled gains. */
        void setGainScale(const HapticGainScale& scale);
        /** Starts or stops the telemetry stream. */
        void setTraceEnabled(bool enabled);

        void addListener(QueueHandle_t queue);
        /**
         * Registers a queue for telemetry. Depth-1 and written with an overwrite,
         * like the state queue: a consumer that falls behind should skip samples
         * rather than stall the motor loop.
         */
        void addTelemetryListener(QueueHandle_t queue);
        void setLogger(Logger* logger);

    protected:
        void run();

    private:
        Configuration& configuration_;
        QueueHandle_t queue_;
        Logger* logger_;
        std::vector<QueueHandle_t> listeners_;
        std::vector<QueueHandle_t> telemetry_listeners_;
        char buf_[72];

        /** The haptic torque controller, and the multipliers riding on top. */
        HapticPid pid_;
        HapticGainScale gain_scale_ = HAPTIC_GAIN_SCALE_DEFAULT;
        bool trace_enabled_ = false;
        /** Throttle for telemetry, so tracing cannot flood the serial link. */
        uint32_t last_trace_ = 0;
        static const uint32_t TRACE_INTERVAL_MILLIS = 5;

        // BLDC motor & driver instance
        BLDCMotor motor = BLDCMotor(1);
        BLDCDriver6PWM driver = BLDCDriver6PWM(PIN_UH, PIN_UL, PIN_VH, PIN_VL, PIN_WH, PIN_WL);

        void publish(const PB_SmartKnobState& state);
        void publishTelemetry(const HapticTelemetry& telemetry);
        void calibrate();
        void checkSensorError();
        void log(const char* msg);
};

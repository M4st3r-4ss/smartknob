#pragma once

#include <AceButton.h>
#include <Arduino.h>

#include "configuration.h"
#include "display_task.h"
#include "logger.h"
#include "motor_task.h"
#include "serial/serial_protocol_plaintext.h"
#include "serial/serial_protocol_protobuf.h"
#include "serial/uart_stream.h"
#include "task.h"
#include "ui/apps.h"
#include "ui/settings.h"
#include "ui/ui_state.h"

#ifndef SK_FORCE_UART_STREAM
    #define SK_FORCE_UART_STREAM 0
#endif

class InterfaceTask : public Task<InterfaceTask>, public Logger {
    friend class Task<InterfaceTask>; // Allow base Task to invoke protected run()

    public:
        InterfaceTask(const uint8_t task_core, MotorTask& motor_task, DisplayTask* display_task);
        virtual ~InterfaceTask();

        void log(const char* msg) override;
        void setConfiguration(Configuration* configuration);

    protected:
        void run();

    private:
    #if defined(CONFIG_IDF_TARGET_ESP32S3) && !SK_FORCE_UART_STREAM
        HWCDC stream_;
    #else
        UartStream stream_;
    #endif
        MotorTask& motor_task_;
        DisplayTask* display_task_;
        char buf_[128];

        SemaphoreHandle_t mutex_;
        Configuration* configuration_ = nullptr; // protected by mutex_

        PB_PersistentConfiguration configuration_value_;
        bool configuration_loaded_ = false;

        uint8_t strain_calibration_step_ = 0;
        int32_t strain_reading_ = 0;

        SerialProtocol* current_protocol_ = nullptr;
        bool remote_controlled_ = false;
        uint8_t press_count_ = 1;

        /** How long the knob must be held on an app page to return to the menu. */
        static const uint32_t HOLD_TO_EXIT_MS = 600;


        UiState ui_state_ = {};
        /** Slot in MENU_ITEMS, not an index into APPS. */
        uint8_t menu_selection_ = 0;
        /** Last value of each app, so reopening one resumes where it was left. */
        int32_t app_position_[APP_SLOTS] = {};
        /** Position remembered by a MUTE press, restored by the next press. */
        int32_t muted_position_ = 0;
        /** Nonce for locally-applied configs, so the motor honours our position. */
        uint8_t local_nonce_ = 1;

        SettingsStore settings_;
        /** Highlighted settings row, and the row being edited in SETTING_EDIT. */
        uint8_t setting_selection_ = 0;

        /** Timer app: absolute deadline while running, plus the paused remainder. */
        uint32_t timer_deadline_ms_ = 0;
        uint32_t timer_paused_s_ = 0;
        uint32_t timer_last_shown_s_ = 0;

        /** Throttle for value updates pushed to the host agent. */
        static const uint32_t HOST_SEND_INTERVAL_MS = 45;
        bool host_send_pending_ = false;
        uint32_t host_send_last_ms_ = 0;
        int32_t host_sent_value_ = INT32_MIN;
        HostChannel host_sent_channel_ = HostChannel::NONE;


        bool knob_pressed_ = false;
        uint32_t press_started_ms_ = 0;
        /** Set once a hold has fired, to keep the release from also acting. */
        bool hold_consumed_ = false;

        PB_SmartKnobState latest_state_ = {};
        PB_SmartKnobConfig latest_config_ = {};

        QueueHandle_t log_queue_;
        QueueHandle_t knob_state_queue_;
        SerialProtocolPlaintext plaintext_protocol_;
        SerialProtocolProtobuf proto_protocol_;

        void openMenu();
        void openApp(uint8_t index);
        /** The vertical settings list. */
        void openSettings();
        /** Adjusting one settings row, or running it when it is an action. */
        void openSettingEdit(uint8_t index);
        /** Short press: open the highlighted app, or run the app's press action. */
        void handlePress();
        /** Hold, or 'B' over serial: back out one level. */
        void handleBack();
        void publishUiState();

        /** Copies the stored settings into ui_state_ so the list can draw them. */
        void publishSettingValues();
        /** Re-applies settings that affect the motor for the page now open. */
        void reapplyCurrentConfig();
        /**
         * Strain calibration, runnable from the knob alone: step 1 samples the
         * idle reading once the knob is released, step 2 records the peak of one
         * firm push and saves when it is let go.
         */
        void startStrainCalibration();
        void updateStrainCalibration();
        int32_t calib_peak_ = 0;
        uint32_t calib_phase_ms_ = 0;
        /** Smallest peak-above-idle worth trusting, in raw HX711 counts. */
        static const int32_t CALIB_MIN_PRESS_DELTA = 1000;
        static const uint32_t CALIB_SETTLE_MS = 900;
        static const uint32_t CALIB_PUSH_TIMEOUT_MS = 10000;

        /** Timer app: arm, pause/resume, and count down. */
        void toggleTimer();
        void tickTimer();
        /**
         * True while a countdown is running or waiting to be acknowledged. The
         * dial is ignored in those states so a knock cannot wipe out a timer.
         */
        bool timerActive() const;
        /** Drops any countdown, running, paused or finished. */
        void clearTimer();
        /** Non-blocking haptic alert pulses, used when a countdown ends. */
        void tickAlert();
        uint8_t alert_pulses_left_ = 0;
        uint32_t alert_next_ms_ = 0;

        /** Queues the open app's value for the host, honouring the throttle. */
        void noteHostValue();
        void flushHostValue();
        /** Asks the host for a channel's current OS-side value. */
        void requestHostValue(HostChannel channel);
        /** Handles an inbound "@VAL <channel> <value>" line from the host. */
        void handleHostValue(const char* channel, int32_t value);
        const AppDescriptor& currentApp() const;

        void updateHardware();
        void publishState();
        void applyConfig(PB_SmartKnobConfig& config, bool from_remote);
        /**
         * Applies a config we own: folds in the knob-strength setting and takes a
         * fresh nonce so the motor snaps to the position we asked for.
         */
        void applyLocalConfig(PB_SmartKnobConfig config);
        /** Wire name for a host channel, or nullptr for HostChannel::NONE. */
        static const char* channelName(HostChannel channel);
};

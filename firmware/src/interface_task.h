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
#include "ui/pet.h"
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
        /** Restores every settings row to its default, from the list itself. */
        void resetSettings();
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
        /**
         * Non-blocking haptic patterns: a run of distinct beats rather than one
         * buzz, driven from the interface loop so serial and press detection
         * keep running throughout. The timer alarm and the pet's after-pat
         * reactions are both played this way.
         *
         * cancel_on_rotation is what the alarm wants, where any deliberate knob
         * action should silence it. The pet clears it: its own beats can nudge
         * the knob far enough to register, and that must not cut the reaction
         * short. Patting again stops the pattern explicitly instead.
         */
        void startPattern(uint8_t beats, uint32_t kick_ms, uint32_t gap_ms, float strength, bool cancel_on_rotation);
        void tickPattern();
        /** Stops a pattern early, releasing a kick that is still out. */
        void stopPattern();
        /** The alarm for a finished countdown: enough rings to carry a room. */
        void startAlert();
        /** Beats still owed, counting the one in progress. */
        uint8_t pattern_beats_left_ = 0;
        /** False while a beat's kick is out and its release is still owed. */
        bool pattern_settled_ = true;
        uint32_t pattern_next_ms_ = 0;
        uint32_t pattern_kick_ms_ = 0;
        uint32_t pattern_gap_ms_ = 0;
        float pattern_strength_ = 1;
        bool pattern_cancel_on_rotation_ = true;
        /** Rings for one alarm, and the shape of a single ring. */
        static const uint8_t ALERT_RINGS = 10;
        static const uint32_t ALERT_KICK_MS = 90;
        static const uint32_t ALERT_GAP_MS = 400;

        /**
         * Pet page. Pats are measured in degrees turned rather than in detents
         * crossed: detent width is part of the mood's texture, and a coarse coat
         * must not bank affection faster than a fine one for the same movement.
         */
        void notePetStroke(int32_t detents);
        void tickPet();
        /** A pat has ended: play the mood's reaction, then let the mood move on. */
        void endPetPat();
        void setPetMood(PetMood mood);
        /**
         * The texture's own feedback during a stroke: grain fires off distance
         * covered, tremble off the clock. Both are one-shot bumps that leave the
         * pattern player's kick/release bookkeeping alone, so they can be dropped
         * in mid-rotation without owing a release afterwards.
         */
        void tickPetTexture();
        void playPetPulse(float strength);
        /**
         * Floor on the gap between texture pulses. The motor's haptic command
         * blocks its loop for ~6ms, so an unthrottled grain on a fast spin would
         * starve the FOC loop and turn the whole page to mush.
         */
        static const uint32_t PET_PULSE_MIN_MS = 45;
        PetMood pet_mood_ = PetMood::CALM;
        /**
         * A mood earned by a pat waits for that pat's reaction to finish playing,
         * so the face and the feel that answer a pat are the ones that were being
         * patted, and the new mood arrives as its own moment.
         */
        PetMood pet_pending_mood_ = PetMood::CALM;
        bool pet_mood_pending_ = false;
        /** Affection banked by gentle patting, and agitation by rough, both 0-1. */
        float pet_charge_ = 0;
        float pet_agitation_ = 0;
        uint32_t pet_last_stroke_ms_ = 0;
        uint32_t pet_tick_ms_ = 0;
        /** Degrees turned in the pat now in progress. */
        float pet_pat_degrees_ = 0;
        bool pet_pat_open_ = false;
        /** Smoothed stroke speed in degrees/second, which sets gentle vs rough. */
        float pet_speed_ = 0;
        /** Degrees since the last grain pulse, and the tremble's own clock. */
        float pet_grain_deg_ = 0;
        uint32_t pet_tremble_ms_ = 0;
        /** Rate limit shared by grain and tremble (see PET_PULSE_MIN_MS). */
        uint32_t pet_pulse_last_ms_ = 0;

        /**
         * Last position seen from the motor, used to spot a rotation even on
         * pages that otherwise ignore the dial.
         */
        int32_t last_state_position_ = 0;

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
        /**
         * Config for the open app page: the mood's feel on the pet page, the
         * remembered value elsewhere. Also re-zeroes the pet's stroke baseline,
         * since petConfig() always re-labels the current spot as position 0.
         */
        PB_SmartKnobConfig currentAppConfig();
        /** Wire name for a host channel, or nullptr for HostChannel::NONE. */
        static const char* channelName(HostChannel channel);
};

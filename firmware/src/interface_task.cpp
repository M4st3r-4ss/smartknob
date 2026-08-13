#if SK_LEDS
#include <FastLED.h>
#endif

#if SK_STRAIN
#include <HX711.h>
#endif

#if SK_ALS
#include <Adafruit_VEML7700.h>
#endif

#include "interface_task.h"
#include "semaphore_guard.h"
#include "util.h"

#if SK_LEDS
CRGB leds[NUM_LEDS];
#endif

#if SK_STRAIN
HX711 scale;
#endif

#if SK_ALS
Adafruit_VEML7700 veml = Adafruit_VEML7700();
#endif

InterfaceTask::InterfaceTask(const uint8_t task_core, MotorTask& motor_task, DisplayTask* display_task) : 
        Task("Interface", 3400, 1, task_core),
        stream_(),
        motor_task_(motor_task),
        display_task_(display_task),
        plaintext_protocol_(stream_, [this] () {
            motor_task_.runCalibration();
        }),
        proto_protocol_(stream_, [this] (PB_SmartKnobConfig& config) {
            applyConfig(config, true);
        }) {
    #if SK_DISPLAY
        assert(display_task != nullptr);
    #endif


    log_queue_ = xQueueCreate(10, sizeof(std::string *));
    assert(log_queue_ != NULL);

    knob_state_queue_ = xQueueCreate(1, sizeof(PB_SmartKnobState));
    assert(knob_state_queue_ != NULL);

    // Depth 1 and overwritten, like the state queue: if this task falls behind,
    // the tuning tool should miss samples rather than the motor loop stall.
    telemetry_queue_ = xQueueCreate(1, sizeof(HapticTelemetry));
    assert(telemetry_queue_ != NULL);

    mutex_ = xSemaphoreCreateMutex();
    assert(mutex_ != NULL);
}

InterfaceTask::~InterfaceTask() {
    vSemaphoreDelete(mutex_);
}

void InterfaceTask::run() {
    stream_.begin();
    
    #if SK_LEDS
        FastLED.addLeds<SK6812, PIN_LED_DATA, GRB>(leds, NUM_LEDS);
    #endif

    #if SK_ALS && PIN_SDA >= 0 && PIN_SCL >= 0
        Wire.begin(PIN_SDA, PIN_SCL);
        Wire.setClock(400000);
    #endif
    #if SK_STRAIN
        scale.begin(PIN_STRAIN_DO, PIN_STRAIN_SCK);
    #endif

    #if SK_ALS
        if (veml.begin()) {
            veml.setGain(VEML7700_GAIN_2);
            veml.setIntegrationTime(VEML7700_IT_400MS);
        } else {
            log("ALS sensor not found!");
        }
    #endif

    // app_position_ is sized from APP_SLOTS, but APP_COUNT is only known at link time.
    assert(APP_COUNT > 0 && APP_COUNT <= APP_SLOTS);
    assert(SETTING_COUNT <= COUNT_OF(ui_state_.setting_values));

    settings_.begin();
    publishSettingValues();

    for (uint8_t i = 0; i < APP_COUNT; i++) {
        app_position_[i] = APPS[i].config.position;
    }

    motor_task_.addListener(knob_state_queue_);
    motor_task_.addTelemetryListener(telemetry_queue_);
    // The stored PID rows have to reach the motor before the first page is
    // applied, or the knob spends its first moments on the schedule's defaults.
    applyHapticSettings();
    openMenu();

    plaintext_protocol_.init([this] () {
        handlePress();
    }, [this] () {
        handleBack();
    }, [this] () {
        startStrainCalibration();
    });

    plaintext_protocol_.setHostValueCallback([this] (const char* channel, int32_t value) {
        handleHostValue(channel, value);
    });

    // Start in legacy protocol mode
    current_protocol_ = &plaintext_protocol_;

    ProtocolChangeCallback protocol_change_callback = [this] (uint8_t protocol) {
        switch (protocol) {
            case SERIAL_PROTOCOL_LEGACY:
                current_protocol_ = &plaintext_protocol_;
                break;
            case SERIAL_PROTOCOL_PROTO:
                current_protocol_ = &proto_protocol_;
                break;
            default:
                log("Unknown protocol requested");
                break;
        }
    };

    plaintext_protocol_.setProtocolChangeCallback(protocol_change_callback);
    proto_protocol_.setProtocolChangeCallback(protocol_change_callback);

    // Interface loop:
    while (1) {
        if (xQueueReceive(knob_state_queue_, &latest_state_, 0) == pdTRUE) {
            publishState();

            // States already in flight when a config is applied still carry the
            // old position. Acting on those would undo the move we just asked
            // for, so wait until the motor echoes the config we last sent.
            bool position_is_current = !latest_state_.has_config
                    || latest_state_.config.position_nonce == latest_config_.position_nonce;

            if (position_is_current) {
                // Turning the knob silences a ringing alarm, wherever we are and
                // even on a page that ignores the dial. Checked here rather than
                // in the per-mode branches below because the timer page locks the
                // dial while the alarm runs, so a rotation reaches nothing else.
                if (latest_state_.current_position != last_state_position_ && pattern_cancel_on_rotation_) {
                    stopPattern();
                }
                last_state_position_ = latest_state_.current_position;
            }

            if (!position_is_current) {
                // Nothing to do until the motor catches up.
            } else if (ui_state_.mode == UiMode::MENU) {
                menu_selection_ = (uint8_t)CLAMP(latest_state_.current_position, (int32_t)0, (int32_t)(MENU_ITEM_COUNT - 1));
                ui_state_.app_index = MENU_ITEMS[menu_selection_];
                publishUiState();
            } else if (ui_state_.mode == UiMode::APP) {
                // A live countdown ignores the dial, so brushing the knob can
                // no longer wipe it out; the dial is re-seated when it stops.
                bool dial_locked = currentApp().kind == AppKind::TIMER && timerActive();
                int32_t previous = app_position_[ui_state_.app_index];
                if (!dial_locked && latest_state_.current_position != previous) {
                    int32_t delta = latest_state_.current_position - previous;
                    app_position_[ui_state_.app_index] = latest_state_.current_position;
                    if (currentApp().kind == AppKind::PET) {
                        // Rotation is a pat here rather than a value: which way
                        // the knob went does not matter, only how far.
                        notePetStroke(delta < 0 ? -delta : delta);
                    } else if (currentApp().kind == AppKind::TIMER) {
                        // Only a paused countdown can still be holding a
                        // remainder here, and dialling a new duration replaces it.
                        clearTimer();
                    }
                    noteHostValue();
                    publishUiState();
                }
            } else if (ui_state_.mode == UiMode::SETTINGS) {
                setting_selection_ = (uint8_t)CLAMP(latest_state_.current_position, (int32_t)0, (int32_t)(SETTING_COUNT - 1));
                ui_state_.setting_index = setting_selection_;
                publishUiState();
            } else if (ui_state_.mode == UiMode::SETTING_EDIT) {
                SettingId id = (SettingId)setting_selection_;
                int16_t before = settings_.get(id);
                settings_.set(id, (int16_t)latest_state_.current_position);
                if (settings_.get(id) != before) {
                    publishSettingValues();
                    publishUiState();
                    // The haptic rows are live: the point of tuning by feel is
                    // that the knob changes under your hand as you turn it, so
                    // they go to the motor now rather than on the confirming
                    // press. applyHapticSettings() ignores unrelated rows.
                    applyHapticSettings();
                }
            }
        }

        tickPattern();
        tickTimer();
        tickPet();
        tickTelemetry();
        flushHostValue();

        current_protocol_->loop();

        std::string* log_string;
        while (xQueueReceive(log_queue_, &log_string, 0) == pdTRUE) {
            current_protocol_->log(log_string->c_str());
            delete log_string;
        }

        updateHardware();

        if (!configuration_loaded_) {
            SemaphoreGuard lock(mutex_);
            if (configuration_ != nullptr) {
                configuration_value_ = configuration_->get();
                configuration_loaded_ = true;
            }
        }

        delay(1);
    }
}

void InterfaceTask::log(const char* msg) {
    // Allocate a string for the duration it's in the queue; it is free'd by the queue consumer
    std::string* msg_str = new std::string(msg);

    // Put string in queue (or drop if full to avoid blocking)
    xQueueSendToBack(log_queue_, &msg_str, 0);
}

void InterfaceTask::openMenu() {
    ui_state_.mode = UiMode::MENU;
    ui_state_.app_index = MENU_ITEMS[menu_selection_];
    ui_state_.hold_progress = 0;
    ui_state_.mode_nonce++;
    publishUiState();

    applyLocalConfig(menuConfig(menu_selection_));
}

void InterfaceTask::openApp(uint8_t index) {
    const AppDescriptor& app = APPS[index];

    if (app.kind == AppKind::SETTINGS) {
        openSettings();
        return;
    }

    bool entering = ui_state_.mode != UiMode::APP || ui_state_.app_index != index;

    ui_state_.mode = UiMode::APP;
    ui_state_.app_index = index;
    ui_state_.hold_progress = 0;
    if (entering) {
        // Only a real page change animates; in-app value changes must not restart it.
        ui_state_.mode_nonce++;
    }

    if (app.kind == AppKind::PET) {
        // The mood carries over between visits, but nothing else does: no pat is
        // in progress, and the doze-off clock starts from this moment rather than
        // from whenever the page was last closed.
        ui_state_.pet_mood = (uint8_t)pet_mood_;
        ui_state_.pet_charge = pet_charge_;
        ui_state_.pet_agitation = pet_agitation_;
        ui_state_.pet_petting = false;
        pet_pat_open_ = false;
        pet_pat_degrees_ = 0;
        pet_speed_ = 0;
        pet_grain_deg_ = 0;
        pet_mood_pending_ = false;
        pet_last_stroke_ms_ = millis();
        pet_tick_ms_ = millis();
        pet_tremble_ms_ = millis();
    }

    publishUiState();
    applyLocalConfig(currentAppConfig());

    if (entering) {
        // Open on whatever the OS currently has, not on a stale local value.
        requestHostValue(app.host_channel);
        snprintf(buf_, sizeof(buf_), "Opening %s", app.name);
        log(buf_);
    } else {
        noteHostValue();
    }
}

void InterfaceTask::openSettings() {
    ui_state_.mode = UiMode::SETTINGS;
    ui_state_.app_index = APP_SETTINGS;
    ui_state_.setting_index = setting_selection_;
    ui_state_.hold_progress = 0;
    ui_state_.mode_nonce++;
    publishSettingValues();
    publishUiState();

    applyLocalConfig(settingsListConfig(setting_selection_));
}

void InterfaceTask::openSettingEdit(uint8_t index) {
    const SettingDescriptor& d = settingAt(index);
    if (d.kind == SettingKind::ACTION) {
        if ((SettingId)index == SettingId::STRAIN_CALIBRATE) {
            startStrainCalibration();
        } else if ((SettingId)index == SettingId::RESET) {
            resetSettings();
        }
        return;
    }

    setting_selection_ = index;
    ui_state_.mode = UiMode::SETTING_EDIT;
    ui_state_.setting_index = index;
    ui_state_.hold_progress = 0;
    ui_state_.mode_nonce++;
    publishUiState();

    applyLocalConfig(settingEditConfig((SettingId)index, settings_.get((SettingId)index)));
}

void InterfaceTask::handlePress() {
    // The alarm outlives the timer page, so a press anywhere silences it. On the
    // timer page itself this is redundant with clearTimer(), which is fine: the
    // press still does its own job below.
    stopPattern();

    if (remote_controlled_) {
        // The host owns the screen; it sees press_nonce and decides what to do.
        // Still ripple, so the tap gets acknowledged locally either way.
        ui_state_.press_nonce++;
        publishUiState();
        return;
    }

    if (ui_state_.mode == UiMode::MENU) {
        ui_state_.press_nonce++;
        openApp(MENU_ITEMS[menu_selection_]);
        return;
    }

    if (ui_state_.mode == UiMode::SETTINGS) {
        ui_state_.press_nonce++;
        openSettingEdit(setting_selection_);
        return;
    }

    if (ui_state_.mode == UiMode::SETTING_EDIT) {
        // A press confirms the value and drops back to the list.
        ui_state_.press_nonce++;
        settings_.commit();
        openSettings();
        return;
    }

    // Inside an app, a short press runs that app's own action.
    uint8_t index = ui_state_.app_index;
    const AppDescriptor& app = APPS[index];
    int32_t current = app_position_[index];
    bool changed = false;

    switch (app.press_action) {
        case PressAction::TOGGLE:
            app_position_[index] = current != app.config.min_position
                    ? app.config.min_position
                    : app.config.max_position;
            changed = true;
            break;
        case PressAction::CYCLE:
            app_position_[index] = current >= app.config.max_position
                    ? app.config.min_position
                    : current + 1;
            changed = true;
            break;
        case PressAction::MUTE:
            if (current != app.config.min_position) {
                muted_position_ = current;
                app_position_[index] = app.config.min_position;
            } else {
                app_position_[index] = muted_position_;
            }
            changed = true;
            break;
        case PressAction::START_STOP:
            toggleTimer();
            break;
        case PressAction::PET_MOOD:
            // Nudges the pet on to its next mood, so every mood and every pair of
            // feedbacks can be tried without waiting for one to be earned.
            setPetMood((PetMood)(((uint8_t)pet_mood_ + 1) % PET_MOOD_COUNT));
            break;
        case PressAction::NONE:
            break;
    }

    ui_state_.press_nonce++;
    if (changed) {
        // Re-send the config to snap the motor to the new position.
        openApp(index);
        noteHostValue();
    } else {
        publishUiState();
    }
}

void InterfaceTask::handleBack() {
    stopPattern();
    if (ui_state_.mode == UiMode::MENU) {
        return;
    }
    motor_task_.playHaptic(false, settings_.clickForceScale());

    if (ui_state_.mode == UiMode::SETTING_EDIT) {
        // One level at a time: edit -> list -> menu.
        settings_.commit();
        openSettings();
        return;
    }

    remote_controlled_ = false;
    openMenu();
}

void InterfaceTask::publishUiState() {
    #if SK_DISPLAY
        display_task_->setUiState(ui_state_);
    #endif
}

const AppDescriptor& InterfaceTask::currentApp() const {
    return APPS[ui_state_.app_index < APP_COUNT ? ui_state_.app_index : 0];
}

PB_SmartKnobConfig InterfaceTask::currentAppConfig() {
    uint8_t index = ui_state_.app_index < APP_COUNT ? ui_state_.app_index : 0;
    const AppDescriptor& app = APPS[index];

    if (app.kind == AppKind::PET) {
        // Feedback 1 lives here: each mood has its own detent width and strength,
        // so how the knob pushes back is the mood. petConfig() re-labels wherever
        // the knob is now as position 0, so the stroke baseline resets with it.
        app_position_[index] = 0;
        return petConfig(pet_mood_);
    }

    PB_SmartKnobConfig config = app.config;
    config.position = app_position_[index];
    config.sub_position_unit = 0;
    return config;
}

void InterfaceTask::publishSettingValues() {
    for (uint8_t i = 0; i < SETTING_COUNT; i++) {
        ui_state_.setting_values[i] = settings_.get((SettingId)i);
    }
}

void InterfaceTask::applyHapticSettings() {
    HapticGainScale scale = settings_.gainScale();
    bool trace = settings_.traceEnabled();

    // Both are sent only when they change. Every edit of any row comes through
    // here, and each send is a queue message the motor task answers by dropping
    // its integral - doing that on an unrelated row would be felt.
    bool gains_changed = !haptic_settings_sent_
            || scale.p != sent_gain_scale_.p
            || scale.i != sent_gain_scale_.i
            || scale.d != sent_gain_scale_.d;
    if (gains_changed) {
        motor_task_.setGainScale(scale);
        sent_gain_scale_ = scale;
    }
    if (!haptic_settings_sent_ || trace != sent_trace_enabled_) {
        motor_task_.setTraceEnabled(trace);
        sent_trace_enabled_ = trace;
    }
    haptic_settings_sent_ = true;
}

void InterfaceTask::tickTelemetry() {
    HapticTelemetry telemetry;
    if (xQueueReceive(telemetry_queue_, &telemetry, 0) != pdTRUE) {
        return;
    }
    // Only the plaintext protocol carries traces: the protobuf channel would need
    // a new message type, and generating one means the nanopb submodule that
    // building the firmware otherwise does without.
    if (current_protocol_ == &plaintext_protocol_) {
        plaintext_protocol_.sendTrace(telemetry);
    }
}

void InterfaceTask::reapplyCurrentConfig() {
    switch (ui_state_.mode) {
        case UiMode::MENU:
            applyLocalConfig(menuConfig(menu_selection_));
            break;
        case UiMode::SETTINGS:
            applyLocalConfig(settingsListConfig(setting_selection_));
            break;
        case UiMode::SETTING_EDIT: {
            SettingId id = (SettingId)setting_selection_;
            applyLocalConfig(settingEditConfig(id, settings_.get(id)));
            break;
        }
        case UiMode::APP:
            applyLocalConfig(currentAppConfig());
            break;
        case UiMode::REMOTE:
            break;
    }
}

void InterfaceTask::resetSettings() {
    settings_.resetToDefaults();
    publishSettingValues();
    ui_state_.settings_reset_nonce++;
    publishUiState();
    // Puts the PID multipliers back to the schedule's own numbers, and stops any
    // trace the reset just switched off.
    applyHapticSettings();

    // Detent strength is folded into every locally-applied config, so the list
    // the reset returns to has to be re-sent to pick up the default feel.
    reapplyCurrentConfig();
    motor_task_.playHaptic(true, settings_.clickForceScale());
    log("Settings reset to defaults");
}

void InterfaceTask::startStrainCalibration() {
    #if SK_STRAIN
        if (!configuration_loaded_) {
            log("Configuration not loaded yet; try again in a moment");
            return;
        }
        if (strain_calibration_step_ != 0) {
            // A second trigger cancels, rather than leaving it stuck mid-flow.
            log("Strain calibration cancelled");
            strain_calibration_step_ = 0;
            ui_state_.calibration_step = 0;
            publishUiState();
            return;
        }
        strain_calibration_step_ = 1;
        calib_phase_ms_ = millis();
        ui_state_.calibration_step = 1;
        publishUiState();
        log("Strain calibration: let go of the knob...");
    #else
        log("No strain sensor on this build");
    #endif
}

void InterfaceTask::updateStrainCalibration() {
    #if SK_STRAIN
        if (strain_calibration_step_ == 1) {
            if (millis() - calib_phase_ms_ < CALIB_SETTLE_MS) {
                return;
            }
            configuration_value_.strain.idle_value = strain_reading_;
            snprintf(buf_, sizeof(buf_), "  idle_value=%d", configuration_value_.strain.idle_value);
            log(buf_);
            log("Now tap the knob down firmly, then let go");
            strain_calibration_step_ = 2;
            calib_phase_ms_ = millis();
            calib_peak_ = strain_reading_;
            ui_state_.calibration_step = 2;
            publishUiState();
            return;
        }

        if (strain_calibration_step_ != 2) {
            return;
        }

        if (strain_reading_ > calib_peak_) {
            calib_peak_ = strain_reading_;
        }

        int32_t idle = configuration_value_.strain.idle_value;
        int32_t peak_delta = calib_peak_ - idle;
        bool pushed = peak_delta >= CALIB_MIN_PRESS_DELTA;
        // Released once the reading has fallen back to the bottom quarter of the push.
        bool released = pushed && (strain_reading_ - idle) < peak_delta / 4;

        if (pushed && released) {
            configuration_value_.strain.press_delta = peak_delta;
            configuration_value_.has_strain = true;
            snprintf(buf_, sizeof(buf_), "  press_delta=%d", configuration_value_.strain.press_delta);
            log(buf_);
            strain_calibration_step_ = 0;
            ui_state_.calibration_step = 0;
            publishUiState();
            if (configuration_->setStrainCalibrationAndSave(configuration_value_.strain)) {
                log("Strain calibration saved");
            } else {
                log("FAILED to save strain calibration!");
            }
            motor_task_.playHaptic(true, settings_.clickForceScale());
        } else if (millis() - calib_phase_ms_ > CALIB_PUSH_TIMEOUT_MS) {
            log("Strain calibration timed out; nothing saved");
            strain_calibration_step_ = 0;
            ui_state_.calibration_step = 0;
            publishUiState();
        }
    #endif
}

bool InterfaceTask::timerActive() const {
    return timer_deadline_ms_ != 0 || ui_state_.timer_elapsed;
}

void InterfaceTask::clearTimer() {
    // Dropping the countdown always silences its alarm: acknowledging, dialling a
    // new duration and re-arming all come through here.
    stopPattern();
    timer_deadline_ms_ = 0;
    timer_paused_s_ = 0;
    timer_last_shown_s_ = 0;
    ui_state_.timer_running = false;
    ui_state_.timer_remaining_s = 0;
    ui_state_.timer_total_s = 0;
    ui_state_.timer_elapsed = false;
}

void InterfaceTask::toggleTimer() {
    if (ui_state_.timer_elapsed) {
        // Acknowledge a finished countdown instead of immediately re-arming.
        clearTimer();
        // The dial was locked while it ran, so the knob may be nowhere near the
        // duration still shown. Put the motor back on it before rotation counts.
        reapplyCurrentConfig();
        return;
    }

    if (timer_deadline_ms_ != 0) {
        // Running -> paused: keep whatever is left.
        uint32_t now = millis();
        timer_paused_s_ = timer_deadline_ms_ > now ? (timer_deadline_ms_ - now + 999) / 1000 : 0;
        timer_deadline_ms_ = 0;
        ui_state_.timer_running = false;
        ui_state_.timer_remaining_s = timer_paused_s_;
        // Pausing unlocks the dial, so re-seat it for the same reason.
        reapplyCurrentConfig();
        return;
    }

    uint32_t seconds = timer_paused_s_;
    if (seconds == 0) {
        // Fresh start: the dial reads in minutes.
        int32_t minutes = app_position_[ui_state_.app_index];
        seconds = (uint32_t)CLAMP(minutes, (int32_t)0, (int32_t)600) * 60;
        ui_state_.timer_total_s = seconds;
    }
    if (seconds == 0) {
        return;
    }
    timer_paused_s_ = 0;
    timer_deadline_ms_ = millis() + seconds * 1000;
    ui_state_.timer_running = true;
    ui_state_.timer_remaining_s = seconds;
    timer_last_shown_s_ = seconds;
}

void InterfaceTask::startPattern(uint8_t beats, uint32_t kick_ms, uint32_t gap_ms, float strength, bool cancel_on_rotation) {
    if (beats == 0) {
        return;
    }
    // Whatever was playing is dropped rather than queued behind this one: there
    // is one motor, and the newest feedback is the one the user is waiting for.
    stopPattern();
    pattern_beats_left_ = beats;
    pattern_settled_ = true;
    pattern_kick_ms_ = kick_ms;
    pattern_gap_ms_ = gap_ms;
    pattern_strength_ = strength > 0 ? strength : 1;
    pattern_cancel_on_rotation_ = cancel_on_rotation;
    pattern_next_ms_ = millis();
}

void InterfaceTask::startAlert() {
    startPattern(ALERT_RINGS, ALERT_KICK_MS, ALERT_GAP_MS, 1, true);
}

void InterfaceTask::stopPattern() {
    if (pattern_beats_left_ == 0 && pattern_settled_) {
        return;
    }
    pattern_beats_left_ = 0;
    if (!pattern_settled_) {
        // A kick is still out. Release it, or the motor holds that torque until
        // something else happens to send a haptic.
        motor_task_.playHaptic(false, pattern_strength_ * settings_.clickForceScale());
        pattern_settled_ = true;
    }
}

void InterfaceTask::tickPattern() {
    if (pattern_beats_left_ == 0 || (int32_t)(millis() - pattern_next_ms_) < 0) {
        return;
    }

    float scale = pattern_strength_ * settings_.clickForceScale();

    if (pattern_settled_) {
        // Kick: the beat itself.
        motor_task_.playHaptic(true, scale);
        pattern_settled_ = false;
        pattern_next_ms_ = millis() + pattern_kick_ms_;
        return;
    }

    // Release, then wait out the gap. The gap is what makes a run of beats read
    // as separate beats instead of one long buzz.
    motor_task_.playHaptic(false, scale);
    pattern_settled_ = true;
    pattern_beats_left_--;
    pattern_next_ms_ = millis() + pattern_gap_ms_;
}

void InterfaceTask::tickTimer() {

    if (timer_deadline_ms_ == 0) {
        return;
    }

    uint32_t now = millis();
    uint32_t remaining_s = 0;
    if ((int32_t)(timer_deadline_ms_ - now) > 0) {
        remaining_s = (timer_deadline_ms_ - now + 999) / 1000;
    }

    if (remaining_s == 0) {
        timer_deadline_ms_ = 0;
        timer_paused_s_ = 0;
        ui_state_.timer_running = false;
        ui_state_.timer_remaining_s = 0;
        ui_state_.timer_elapsed = true;
        publishUiState();
        log("Timer finished");
        startAlert();
        return;
    }

    if (remaining_s != timer_last_shown_s_) {
        timer_last_shown_s_ = remaining_s;
        ui_state_.timer_remaining_s = remaining_s;
        publishUiState();
    }
}

void InterfaceTask::notePetStroke(int32_t detents) {
    if (detents <= 0) {
        return;
    }

    // A hand back on the knob cuts off the previous reaction: the pet is being
    // touched again, so its answer to the last pat is stale. This is also why the
    // reaction pattern does not cancel itself on rotation - its own beats move
    // the knob, and only a real pat should stop it.
    stopPattern();

    uint32_t now = millis();
    const PetTextureDescriptor& t = petTextureFor(pet_mood_);
    // Detents are the mood's own grain, so they are converted to degrees before
    // anything is banked. Otherwise a coarse coat would earn nine times as much
    // affection per turn as a fine one, purely because its steps are wider.
    float degrees_turned = detents * t.detent_width_degrees;

    if (!pet_pat_open_) {
        pet_pat_open_ = true;
        pet_pat_degrees_ = 0;
        pet_grain_deg_ = 0;
        pet_speed_ = 0;
        pet_tremble_ms_ = now;
        ui_state_.pet_petting = true;
    } else {
        // Speed over the gap since the last report, smoothed hard: the motor
        // publishes every 5ms, so a raw figure is far too jumpy to judge with.
        uint32_t gap_ms = now - pet_last_stroke_ms_;
        if (gap_ms > 0 && gap_ms < 400) {
            float instant = degrees_turned * 1000.0f / gap_ms;
            pet_speed_ = pet_speed_ + (instant - pet_speed_) * 0.25f;
        }
    }

    pet_last_stroke_ms_ = now;
    pet_tick_ms_ = now;

    // Capped rather than summed without bound: past the over-pet mark the exact
    // total stops mattering.
    pet_pat_degrees_ = min(pet_pat_degrees_ + degrees_turned, PET_OVERPET_DEG + 1);
    pet_grain_deg_ += degrees_turned;

    // Gentle patting banks affection; frantic patting banks agitation instead of
    // it. The crossfade is what makes how you pat matter as much as how much:
    // the same rotation is a caress or a shove depending on how fast it went.
    float roughness = CLAMP((pet_speed_ - PET_ROUGH_DEG_PER_S) / PET_ROUGH_DEG_PER_S,
                            (float)0, (float)1);
    pet_charge_ = CLAMP(pet_charge_ + PET_CHARGE_PER_DEG * degrees_turned * (1 - roughness),
                        (float)0, (float)1);
    pet_agitation_ = CLAMP(pet_agitation_ + PET_AGITATION_PER_DEG * degrees_turned * roughness,
                           (float)0, (float)1);
    ui_state_.pet_charge = pet_charge_;
    ui_state_.pet_agitation = pet_agitation_;
    ui_state_.pet_speed = pet_speed_;
}

void InterfaceTask::playPetPulse(float strength) {
    uint32_t now = millis();
    if (now - pet_pulse_last_ms_ < PET_PULSE_MIN_MS) {
        return;
    }
    pet_pulse_last_ms_ = now;
    // A bump, not a click: press=false is the lighter of the two haptics, and the
    // texture should sit under the detents rather than compete with them.
    motor_task_.playHaptic(false, strength * settings_.clickForceScale());
}

void InterfaceTask::tickPetTexture() {
    const PetTextureDescriptor& t = petTextureFor(pet_mood_);

    // Grain rides distance covered, so it feels like something the hand is
    // passing over rather than something happening to it.
    if (t.grain_every_degrees > 0 && pet_grain_deg_ >= t.grain_every_degrees) {
        pet_grain_deg_ = fmodf(pet_grain_deg_, t.grain_every_degrees);
        playPetPulse(t.grain_strength);
        return;
    }

    // Tremble rides the clock, so it keeps its own rhythm no matter how the knob
    // is moved - which is exactly what tells a purr apart from a texture.
    if (t.tremble_ms > 0 && millis() - pet_tremble_ms_ >= t.tremble_ms) {
        pet_tremble_ms_ = millis();
        playPetPulse(t.tremble_strength);
    }
}

void InterfaceTask::endPetPat() {
    pet_pat_open_ = false;
    ui_state_.pet_petting = false;

    // Feedback 2: the mood's own rhythm, played once the hand is still.
    const PetMoodDescriptor& mood = petMoodAt(pet_mood_);
    startPattern(mood.beats, mood.beat_kick_ms, mood.beat_gap_ms, mood.beat_strength, false);
    ui_state_.pet_reaction_nonce++;

    PetMood next = nextPetMood(pet_mood_, pet_charge_, pet_agitation_, pet_pat_degrees_);
    if (next != pet_mood_) {
        pet_pending_mood_ = next;
        pet_mood_pending_ = true;
    }
    pet_pat_degrees_ = 0;
    pet_speed_ = 0;
    ui_state_.pet_speed = 0;
    pet_tick_ms_ = millis();
    publishUiState();
}

void InterfaceTask::tickPet() {
    if (ui_state_.mode != UiMode::APP || currentApp().kind != AppKind::PET) {
        if (pet_pat_open_) {
            // Left the page with a hand still on the knob. The pat is dropped
            // rather than finished: its reaction belongs to a page that is no
            // longer showing, and the mood it would have earned was not seen out.
            pet_pat_open_ = false;
            pet_pat_degrees_ = 0;
            pet_speed_ = 0;
            ui_state_.pet_petting = false;
            ui_state_.pet_speed = 0;
            publishUiState();
        }
        return;
    }

    uint32_t now = millis();

    if (pet_pat_open_) {
        if (now - pet_last_stroke_ms_ >= PET_SETTLE_MS) {
            endPetPat();
            return;
        }
        // Mid-stroke: the coat's own grain and tremble, which is what the hand
        // feels between the detents.
        tickPetTexture();
        return;
    }

    if (pet_mood_pending_ && pattern_beats_left_ == 0) {
        setPetMood(pet_pending_mood_);
        return;
    }

    // The rest is idle upkeep, run in tenths of a second: the decay is slow, and
    // every step of it pushes state across to the display task.
    if (now - pet_tick_ms_ < 100) {
        return;
    }
    uint32_t dt_ms = now - pet_tick_ms_;
    pet_tick_ms_ = now;

    if (pet_charge_ > 0 || pet_agitation_ > 0) {
        // Both bleed away while the pet is left alone, so a mood has to be earned
        // in one sitting rather than over a whole evening. Agitation goes faster
        // than affection: the pet calms down sooner than it forgets kindness.
        float dt_s = dt_ms / 1000.0f;
        pet_charge_ = CLAMP(pet_charge_ - PET_CHARGE_DECAY_PER_S * dt_s, (float)0, (float)1);
        pet_agitation_ = CLAMP(pet_agitation_ - PET_AGITATION_DECAY_PER_S * dt_s, (float)0, (float)1);
        ui_state_.pet_charge = pet_charge_;
        ui_state_.pet_agitation = pet_agitation_;
        publishUiState();
    }

    if (pet_mood_ != PetMood::SLEEPY && now - pet_last_stroke_ms_ >= PET_IDLE_SLEEP_MS) {
        setPetMood(PetMood::SLEEPY);
    }
}

void InterfaceTask::setPetMood(PetMood mood) {
    pet_mood_ = mood;
    pet_mood_pending_ = false;
    pet_charge_ = petChargeAfterMood(mood);
    pet_agitation_ = petAgitationAfterMood(mood);
    // The new mood brings a new coat, so the texture's counters start fresh
    // rather than firing a grain pulse left over from the old one.
    pet_grain_deg_ = 0;
    pet_tremble_ms_ = millis();

    ui_state_.pet_mood = (uint8_t)mood;
    ui_state_.pet_charge = pet_charge_;
    ui_state_.pet_agitation = pet_agitation_;
    ui_state_.pet_mood_nonce++;
    publishUiState();

    // Feedback 1 changes with the mood, so re-apply the page's config: the knob
    // starts feeling like the new mood before it is patted again.
    applyLocalConfig(currentAppConfig());

    snprintf(buf_, sizeof(buf_), "Pet mood: %s", petMoodAt(mood).name);
    log(buf_);
}

const char* InterfaceTask::channelName(HostChannel channel) {
    switch (channel) {
        case HostChannel::VOLUME:
            return "volume";
        case HostChannel::BRIGHTNESS:
            return "brightness";
        case HostChannel::NONE:
        default:
            return nullptr;
    }
}

void InterfaceTask::noteHostValue() {
    if (ui_state_.mode != UiMode::APP) {
        return;
    }
    if (currentApp().host_channel == HostChannel::NONE) {
        return;
    }
    host_send_pending_ = true;
}

void InterfaceTask::flushHostValue() {
    if (!host_send_pending_ || ui_state_.mode != UiMode::APP) {
        return;
    }
    HostChannel channel = currentApp().host_channel;
    const char* name = channelName(channel);
    if (name == nullptr) {
        host_send_pending_ = false;
        return;
    }
    if (millis() - host_send_last_ms_ < HOST_SEND_INTERVAL_MS) {
        return;
    }

    int32_t value = app_position_[ui_state_.app_index];
    host_send_pending_ = false;
    host_send_last_ms_ = millis();
    if (channel == host_sent_channel_ && value == host_sent_value_) {
        return;
    }
    host_sent_channel_ = channel;
    host_sent_value_ = value;
    plaintext_protocol_.sendHostSet(name, value);
}

void InterfaceTask::requestHostValue(HostChannel channel) {
    const char* name = channelName(channel);
    if (name == nullptr) {
        return;
    }
    // Forget the last sent value so the reply is not mistaken for an echo.
    host_sent_channel_ = HostChannel::NONE;
    host_sent_value_ = INT32_MIN;
    host_send_pending_ = false;
    plaintext_protocol_.sendHostGet(name);
}

void InterfaceTask::handleHostValue(const char* channel, int32_t value) {
    for (uint8_t i = 0; i < APP_COUNT; i++) {
        const char* name = channelName(APPS[i].host_channel);
        if (name == nullptr || strcmp(name, channel) != 0) {
            continue;
        }

        int32_t clamped = CLAMP(value, APPS[i].config.min_position, APPS[i].config.max_position);
        if (app_position_[i] == clamped) {
            return;
        }
        app_position_[i] = clamped;

        // Only re-seat the motor if that app is the one on screen; otherwise the
        // value is just remembered for the next time it is opened.
        if (ui_state_.mode == UiMode::APP && ui_state_.app_index == i) {
            host_sent_channel_ = APPS[i].host_channel;
            host_sent_value_ = clamped;
            host_send_pending_ = false;
            reapplyCurrentConfig();
            publishUiState();
        }
        return;
    }
}

void InterfaceTask::updateHardware() {
    // How far button is pressed, in range [0, 1]
    float press_value_unit = 0;

    #if SK_ALS
        const float LUX_ALPHA = 0.005;
        static float lux_avg;
        float lux = veml.readLux();
        lux_avg = lux * LUX_ALPHA + lux_avg * (1 - LUX_ALPHA);
        static uint32_t last_als;
        if (millis() - last_als > 1000 && strain_calibration_step_ == 0) {
            snprintf(buf_, sizeof(buf_), "millilux: %.2f", lux*1000);
            log(buf_);
            last_als = millis();
        }
    #endif

    #if SK_STRAIN
        if (scale.wait_ready_timeout(100)) {
            strain_reading_ = scale.read();

            static uint32_t last_reading_display;
            if (millis() - last_reading_display > 1000 && strain_calibration_step_ == 0) {
                snprintf(buf_, sizeof(buf_), "HX711 reading: %d", strain_reading_);
                log(buf_);
                last_reading_display = millis();
            }
            if (strain_calibration_step_ != 0) {
                updateStrainCalibration();
            } else if (configuration_loaded_ && configuration_value_.has_strain) {
                // The threshold is a fraction of the calibrated span, so a press
                // registers well before the force the calibration itself recorded.
                float span = configuration_value_.strain.press_delta * PRESS_THRESHOLD;
                // TODO: calibrate and track (long term moving average) idle point (lower)
                press_value_unit = lerp(strain_reading_, configuration_value_.strain.idle_value, configuration_value_.strain.idle_value + span, 0, 1);

                // Ignore readings that are way out of expected bounds
                if (-1 < press_value_unit && press_value_unit < 2) {
                    static uint8_t press_readings;
                    if (!knob_pressed_ && press_value_unit > 1) {
                        press_readings++;
                        if (press_readings > 2) {
                            motor_task_.playHaptic(true, settings_.clickForceScale());
                            knob_pressed_ = true;
                            press_started_ms_ = millis();
                            hold_consumed_ = false;
                            press_count_++;
                            publishState();
                        }
                    } else if (knob_pressed_ && press_value_unit < 0.5) {
                        press_readings++;
                        if (press_readings > 2) {
                            motor_task_.playHaptic(false, settings_.clickForceScale());
                            knob_pressed_ = false;
                            // A hold already acted, so the release must not.
                            if (!hold_consumed_) {
                                handlePress();
                            }
                            ui_state_.hold_progress = 0;
                            publishUiState();
                        }
                    } else {
                        press_readings = 0;
                    }

                    if (knob_pressed_ && !hold_consumed_) {
                        float progress = (float)(millis() - press_started_ms_) / HOLD_TO_EXIT_MS;
                        bool can_exit = ui_state_.mode != UiMode::MENU;
                        ui_state_.hold_progress = can_exit ? CLAMP(progress, (float)0, (float)1) : 0;
                        if (can_exit && progress >= 1) {
                            hold_consumed_ = true;
                            ui_state_.hold_progress = 0;
                            handleBack();
                        } else {
                            publishUiState();
                        }
                    }
                }
            }
        } else {
            log("HX711 not found.");

            #if SK_LEDS
                for (uint8_t i = 0; i < NUM_LEDS; i++) {
                    leds[i] = CRGB::Red;
                }
                FastLED.show();
            #endif
        }
    #endif

    uint16_t brightness = UINT16_MAX;
    // The floor is user-configurable: how dim the screen may go in a dark room.
    uint16_t min_backlight = settings_.minBacklight();
    #if SK_ALS
        brightness = (uint16_t)CLAMP(lux_avg * 13000, (float)min_backlight, (float)UINT16_MAX);
    #else
        brightness = max(min_backlight, brightness);
    #endif

    #if SK_DISPLAY
        display_task_->setBrightness(brightness); // TODO: apply gamma correction
    #endif

    #if SK_LEDS
        // The ring follows the ambient level, capped by the user's LED setting.
        uint8_t led_value = (uint8_t)((uint32_t)settings_.ledBrightness() * (brightness >> 8) / 255);
        for (uint8_t i = 0; i < NUM_LEDS; i++) {
            leds[i].setHSV(latest_config_.led_hue, 255 - 180*CLAMP(press_value_unit, (float)0, (float)1) - 75*knob_pressed_, led_value);

            // Gamma adjustment
            leds[i].r = dim8_video(leds[i].r);
            leds[i].g = dim8_video(leds[i].g);
            leds[i].b = dim8_video(leds[i].b);
        }
        FastLED.show();
    #endif
}

void InterfaceTask::setConfiguration(Configuration* configuration) {
    SemaphoreGuard lock(mutex_);
    configuration_ = configuration;
}

void InterfaceTask::publishState() {
    // Apply local state before publishing to serial
    latest_state_.press_nonce = press_count_;
    current_protocol_->handleState(latest_state_);
}

void InterfaceTask::applyLocalConfig(PB_SmartKnobConfig config) {
    // The knob-strength setting scales every page's detents together.
    config.detent_strength_unit *= settings_.knobStrengthScale();
    config.endstop_strength_unit *= settings_.knobStrengthScale();
    config.position_nonce = local_nonce_++;
    applyConfig(config, false);
}

void InterfaceTask::applyConfig(PB_SmartKnobConfig& config, bool from_remote) {
    if (from_remote && ui_state_.mode != UiMode::REMOTE) {
        // A host took over: give it its own page instead of leaving the menu
        // carousel to follow a position that no longer selects an app.
        ui_state_.mode = UiMode::REMOTE;
        ui_state_.hold_progress = 0;
        ui_state_.mode_nonce++;
        publishUiState();
    }
    remote_controlled_ = from_remote;
    latest_config_ = config;
    // A config move is our own doing, not the user's. Re-seat the rotation
    // baseline with it so the jump it causes is not read as a knob turn - that
    // would silence the alarm the moment the timer page re-seats the dial.
    last_state_position_ = config.position;
    motor_task_.setConfig(config);
}

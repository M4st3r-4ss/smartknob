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

            if (ui_state_.mode == UiMode::MENU) {
                menu_selection_ = (uint8_t)CLAMP(latest_state_.current_position, (int32_t)0, (int32_t)(MENU_ITEM_COUNT - 1));
                ui_state_.app_index = MENU_ITEMS[menu_selection_];
                publishUiState();
            } else if (ui_state_.mode == UiMode::APP) {
                int32_t previous = app_position_[ui_state_.app_index];
                app_position_[ui_state_.app_index] = latest_state_.current_position;
                if (latest_state_.current_position != previous) {
                    // Turning the knob cancels a countdown rather than fighting it.
                    if (currentApp().kind == AppKind::TIMER) {
                        timer_deadline_ms_ = 0;
                        timer_paused_s_ = 0;
                        ui_state_.timer_running = false;
                        ui_state_.timer_remaining_s = 0;
                        ui_state_.timer_total_s = 0;
                        ui_state_.timer_elapsed = false;
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
                }
            }
        }

        tickTimer();
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
    publishUiState();

    PB_SmartKnobConfig config = app.config;
    config.position = app_position_[index];
    config.sub_position_unit = 0;
    applyLocalConfig(config);

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

void InterfaceTask::publishSettingValues() {
    for (uint8_t i = 0; i < SETTING_COUNT; i++) {
        ui_state_.setting_values[i] = settings_.get((SettingId)i);
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
        case UiMode::APP: {
            PB_SmartKnobConfig config = currentApp().config;
            config.position = app_position_[ui_state_.app_index];
            config.sub_position_unit = 0;
            applyLocalConfig(config);
            break;
        }
        case UiMode::REMOTE:
            break;
    }
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
            log("Now push the knob down firmly, then let go");
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

void InterfaceTask::toggleTimer() {
    if (ui_state_.timer_elapsed) {
        // Acknowledge a finished countdown instead of immediately re-arming.
        ui_state_.timer_elapsed = false;
        ui_state_.timer_remaining_s = 0;
        ui_state_.timer_total_s = 0;
        return;
    }

    if (timer_deadline_ms_ != 0) {
        // Running -> paused: keep whatever is left.
        uint32_t now = millis();
        timer_paused_s_ = timer_deadline_ms_ > now ? (timer_deadline_ms_ - now + 999) / 1000 : 0;
        timer_deadline_ms_ = 0;
        ui_state_.timer_running = false;
        ui_state_.timer_remaining_s = timer_paused_s_;
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

void InterfaceTask::tickAlert() {
    if (alert_pulses_left_ == 0 || (int32_t)(millis() - alert_next_ms_) < 0) {
        return;
    }
    // Alternating kick and release reads as a distinct double-buzz.
    motor_task_.playHaptic(alert_pulses_left_ % 2 == 0, settings_.clickForceScale());
    alert_pulses_left_--;
    alert_next_ms_ = millis() + 100;
}

void InterfaceTask::tickTimer() {
    tickAlert();

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
        // Pulsed rather than blocking, so serial and press detection keep running.
        alert_pulses_left_ = 6;
        alert_next_ms_ = now;
        return;
    }

    if (remaining_s != timer_last_shown_s_) {
        timer_last_shown_s_ = remaining_s;
        ui_state_.timer_remaining_s = remaining_s;
        publishUiState();
    }
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
                // The press-force setting scales the calibrated span, so 1.0 lands
                // at a lighter or firmer push than the calibration itself used.
                float span = configuration_value_.strain.press_delta * settings_.pressThreshold();
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
    motor_task_.setConfig(config);
}

#pragma once

#include "../proto_gen/smartknob.pb.h"

#include "interface_callbacks.h"
#include "motor_task.h"
#include "serial_protocol.h"
#include "uart_stream.h"

typedef std::function<void(void)> PressCallback;
typedef std::function<void(void)> BackCallback;
typedef std::function<void(void)> StrainCalibrationCallback;
/** An "@VAL <channel> <value>" line from the host agent. */
typedef std::function<void(const char*, int32_t)> HostValueCallback;

class SerialProtocolPlaintext : public SerialProtocol {
    public:
        SerialProtocolPlaintext(Stream& stream, MotorCalibrationCallback motor_calibration_callback) : SerialProtocol(), stream_(stream), motor_calibration_callback_(motor_calibration_callback) {}
        ~SerialProtocolPlaintext(){}
        void log(const char* msg) override;
        void loop() override;
        void handleState(const PB_SmartKnobState& state) override;

        void init(PressCallback press_callback, BackCallback back_callback, StrainCalibrationCallback strain_calibration_callback);

        void setHostValueCallback(HostValueCallback cb) {
            host_value_callback_ = cb;
        }

        /**
         * Emits a host-agent line, e.g. "@SET volume 42" or "@GET volume".
         * Passing no value sends the bare verb and channel.
         */
        void sendHostCommand(const char* verb, const char* channel);
        void sendHostCommand(const char* verb, const char* channel, int32_t value);

        void sendHostSet(const char* channel, int32_t value) {
            sendHostCommand("SET", channel, value);
        }
        void sendHostGet(const char* channel) {
            sendHostCommand("GET", channel);
        }

    private:
        Stream& stream_;
        MotorCalibrationCallback motor_calibration_callback_;
        PB_SmartKnobState latest_state_ = {};
        PressCallback press_callback_;
        BackCallback back_callback_;
        StrainCalibrationCallback strain_calibration_callback_;
        HostValueCallback host_value_callback_;

        /** Line buffer, filled only while an "@" command is being read. */
        char line_[48] = {};
        uint8_t line_len_ = 0;
        bool in_line_ = false;

        void handleLine();
};

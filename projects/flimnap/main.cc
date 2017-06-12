/*
    ChibiOS - Copyright (C) 2006..2015 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include "ch.h"
#include "hal.h"

#include "analog.h"
#include "encoder.h"
#include "encoderfoaw.h"
#include "cobs.h"
#include "packet/serialize.h"
#include "simulation.pb.h"
#include "messageutil.h"
#include "filter/movingaverage.h"

#include "blink.h"
#include "usbconfig.h"
#include "saconfig.h"
#include "utility.h"

#include "parameters.h"

#include <array>
#include <type_traits>

#include "bicycle/kinematic.h" // simplified bicycle model
#include "bicycle/whipple.h" // whipple bicycle model
#include "oracle.h" // oracle observer
#include "kalman.h" // kalman filter observer
#include "haptic.h" // handlebar feedback
#include "simbicycle.h"

namespace {
#if defined(USE_BICYCLE_KINEMATIC_MODEL)
    using model_t = model::BicycleKinematic;
    using observer_t = observer::Oracle<model_t>;
    using haptic_t = haptic::HandlebarStatic;
#else // defined(USE_BICYCLE_KINEMATIC_MODEL)
    using model_t = model::BicycleWhipple;
    using observer_t = observer::Kalman<model_t>;
    using haptic_t = haptic::HandlebarDynamic;
#endif // defined(USE_BICYCLE_KINEMATIC_MODEL)
    using bicycle_t = sim::Bicycle<model_t, observer_t, haptic_t>;

    // sensors
    Analog analog;
    Encoder encoder_steer(sa::RLS_ROLIN_ENC, sa::RLS_ROLIN_ENC_INDEX_CFG);
    EncoderFoaw<float, 32> encoder_rear_wheel(sa::RLS_GTS35_ENC,
                                              sa::RLS_GTS35_ENC_CFG,
                                              MS2ST(1), 3.0f);
    filter::MovingAverage<float, 5> velocity_filter;

    constexpr float fixed_velocity = 5.0f;

    // transmission
    constexpr std::size_t VARINT_MAX_SIZE = 10;
    std::array<uint8_t, SimulationMessage_size + VARINT_MAX_SIZE> encode_buffer;
    std::array<uint8_t, cobs::max_encoded_length(SimulationMessage_size + VARINT_MAX_SIZE)> packet_buffer;
    SimulationMessage msg;

    // pose calculation loop
    constexpr systime_t pose_loop_time = US2ST(8333); // update pose at 120 Hz

    // dynamics loop
    constexpr systime_t looptime = MS2ST(1); // 1 ms -> 1 kHz
    time_measurement_t computation_time_measurement;
    time_measurement_t transmission_time_measurement;

    dacsample_t set_handlebar_velocity(float velocity) {
        // limit velocity to a maximum magnitude of 100 deg/s
        // input is in units of rad/s
        const float saturated_velocity = util::clamp(velocity, -sa::MAX_KOLLMORGEN_VELOCITY, sa::MAX_KOLLMORGEN_VELOCITY);
        const dacsample_t aout = saturated_velocity/sa::MAX_KOLLMORGEN_VELOCITY*sa::DAC_HALF_RANGE + sa::DAC_HALF_RANGE;
        dacPutChannelX(sa::KOLLM_DAC, 0, aout); // TODO: don't hardcode zero but find the DAC channel constant
        return aout;
    }

    constexpr float adc_to_nm(adcsample_t value, adcsample_t adc_zero, float magnitude) {
        // Convert torque from ADC samples to Nm.
        // ADC samples are 12 bits.
        // It's not clear when scaling should be applied as data was never saved after the scale
        // factors were determined.
        const int16_t shifted_value = static_cast<int16_t>(value) - static_cast<int16_t>(adc_zero);
        return static_cast<float>(shifted_value)*magnitude/static_cast<float>(sa::ADC_HALF_RANGE);
    }

    void update_and_transmit_pose(bicycle_t& bicycle) {
        bicycle.update_kinematics();
        //
        //TODO: Pose message should be transmitted asynchronously.
        //After starting transmission, the simulation should start to calculate pose for the next timestep.
        //This is currently not necessary given the observed timings.
        //
        //size_t bytes_encoded = packet::serialize::encode_delimited(bicycle.pose(),
        //        encode_buffer.data(), encode_buffer.size());
        //TODO: clear message
        //message::set_simulation_loop_values(&msg, bicycle);
        //size_t bytes_encoded = packet::serialize::encode_delimited(
        //        msg, encode_buffer.data(), encode_buffer.size());
        //size_t bytes_written = packet::frame::stuff(
        //        encode_buffer.data(), packet_buffer.data(), bytes_encoded);
        //if ((SDU1.config->usbp->state == USB_ACTIVE) && (SDU1.state == SDU_READY)) {
        //    usbTransmit(SDU1.config->usbp, SDU1.config->bulk_in, packet_buffer.data(), bytes_written);
        //}
    }

    template <typename T>
    struct observer_initializer{
        template <typename S = T>
        typename std::enable_if<std::is_same<typename S::observer_t, observer::Kalman<model_t>>::value, void>::type
        initialize(S& bicycle) {
            typename S::observer_t& observer = bicycle.observer();
            observer.set_Q(parameters::defaultvalue::kalman::Q(observer.dt()));
            // Reduce steer measurement noise covariance
            observer.set_R(parameters::defaultvalue::kalman::R/1000);

            // prime the Kalman gain matrix
            bicycle.prime_observer();

            // We start with steer angle equal to the measurement and all other state elements at zero.
            model_t::state_t x0 = model_t::state_t::Zero();
            model_t::set_state_element(x0, model_t::state_index_t::steer_angle,
                    util::encoder_count<float>(encoder_steer));
            observer.set_x(x0);
        }
        template <typename S = T>
        typename std::enable_if<!std::is_same<typename S::observer_t, observer::Kalman<model_t>>::value, void>::type
        initialize(S& bicycle) {
            // no-op
            (void)bicycle;
        }
    };

    size_t write_message_to_encode_buffer(const SimulationMessage& m) {
        const size_t encode_buffer_len = packet::serialize::encode_delimited(
            m,
            encode_buffer.data(),
            encode_buffer.size()
        );

        const cobs::EncodeResult encode_result = cobs::encode(
            encode_buffer.data(),
            encode_buffer_len,
            packet_buffer.data(),
            packet_buffer.size()
        );

        // Encoding only fails when the destination buffer is too small, this
        // should not happen as long as we only encode SimulationMessage objects
        // because we allocated packet_buffer based on its size.
        chDbgAssert(encode_result.status == cobs::EncodeResult::Status::OK, "Expected encoding to succeed.");

        return encode_result.produced;
    }

    THD_WORKING_AREA(wa_pose_thread, 2048);
    THD_FUNCTION(pose_thread, arg) {
        bicycle_t& bicycle = *static_cast<bicycle_t*>(arg);

        chRegSetThreadName("pose");
        while (true) {
            systime_t starttime = chVTGetSystemTime();
            update_and_transmit_pose(bicycle);
            systime_t sleeptime = pose_loop_time + starttime - chVTGetSystemTime();
            if (sleeptime >= pose_loop_time) {
                continue;
            } else {
                chThdSleep(sleeptime);
            }
        }
    }

    THD_WORKING_AREA(wa_transmission_thread, 2048);
    THD_FUNCTION(transmission_thread, arg) {
        (void)arg;

        chRegSetThreadName("transmission");
        while (true) {
            // sleep for 1 ms to allow processing of other threads
            chThdSleep(MS2ST(1));
        }
    }
} // namespace

/*
 * Application entry point.
 */
int main(void) {

    //System initializations.
    //- HAL initialization, this also initializes the configured device drivers
    //  and performs the board-specific initializations.
    //- Kernel initialization, the main() function becomes a thread and the
    //  RTOS is active.
    halInit();
    chSysInit();

    // Initialize a serial-over-USB CDC driver.
    sduObjectInit(&SDU1);
    sduStart(&SDU1, &serusbcfg);

    //Activate the USB driver and then the USB bus pull-up on D+.
    //Note, a delay is inserted in order to not have to disconnect the cable
    //after a reset.
    board_usb_lld_disconnect_bus();   //usbDisconnectBus(serusbcfg.usbp);
    chThdSleepMilliseconds(1500);
    usbStart(serusbcfg.usbp, &usbcfg);
    board_usb_lld_connect_bus();      //usbConnectBus(serusbcfg.usbp);

    // create the blink thread and print state monitor
    chBlinkThreadCreateStatic();

    // Start sensors.
    // Encoder:
    //   Initialize encoder driver 5 on pins PA0, PA1 (EXT2-4, EXT2-8).
    //   Pins for encoder driver 3 are already set in board.h.
    palSetLineMode(LINE_TIM5_CH1, PAL_MODE_ALTERNATE(2) | PAL_STM32_PUPDR_FLOATING);
    palSetLineMode(LINE_TIM5_CH2, PAL_MODE_ALTERNATE(2) | PAL_STM32_PUPDR_FLOATING);
    encoder_steer.start();
    encoder_rear_wheel.start();
    analog.start(10000); // trigger ADC conversion at 10 kHz

    //Set torque measurement enable line low.
    //The output of the Kistler torque sensor is not valid until after a falling edge
    //on the measurement line and it is held low. The 'LINE_TORQUE_MEAS_EN' line is
    //reversed due to NPN switch Q1.
    palClearLine(LINE_TORQUE_MEAS_EN);
    chThdSleepMilliseconds(1);
    palSetLine(LINE_TORQUE_MEAS_EN);

    // Start DAC1 driver and set output pin as analog as suggested in Reference Manual.
    // The default line configuration is OUTPUT_OPENDRAIN_PULLUP  for SPI1_ENC1_NSS
    // and must be changed to use as analog output.
    palSetLineMode(LINE_KOLLM_ACTL_TORQUE, PAL_MODE_INPUT_ANALOG);
    dacStart(sa::KOLLM_DAC, sa::KOLLM_DAC_CFG);
    set_handlebar_velocity(0.0f);

    // Initialize bicycle. The initial velocity is important as we use it to prime
    // the Kalman gain matrix.
    bicycle_t bicycle(fixed_velocity, static_cast<model::real_t>(looptime)/CH_CFG_ST_FREQUENCY);

    // Initialize HandlebarDynamic object to estimate torque due to handlebar inertia.
    // TODO: naming here is poor
#if !defined(FLIMNAP_ZERO_INPUT)
    haptic::HandlebarDynamic handlebar_model(bicycle.model(), sa::UPPER_ASSEMBLY_INERTIA_VIRTUAL);
#endif  // !defined(FLIMNAP_ZERO_INPUT)

    observer_initializer<observer_t> oi;
    oi.initialize(bicycle);

    chTMObjectInit(&computation_time_measurement);
    chTMObjectInit(&transmission_time_measurement);

    // Transmit initial message containing gitsha1, model, and observer data.
    // This initial transmission is blocking.
    msg = SimulationMessage_init_zero;
    msg.timestamp = chVTGetSystemTime();
    message::set_simulation_full_model_observer(&msg, bicycle);
    size_t bytes_written = write_message_to_encode_buffer(msg);

    // block until ready
    while ((SDU1.config->usbp->state != USB_ACTIVE) || (SDU1.state != SDU_READY)) {
        chThdSleepMilliseconds(10);
    }

    // transmit data
    if ((SDU1.config->usbp->state == USB_ACTIVE) && (SDU1.state == SDU_READY)) {
        chTMStartMeasurementX(&transmission_time_measurement);
        usbTransmit(SDU1.config->usbp, SDU1.config->bulk_in, packet_buffer.data(), bytes_written);
        chTMStopMeasurementX(&transmission_time_measurement);
    }

    // Normal main() thread activity, in this project it simulates the bicycle
    // dynamics in real-time (roughly).
    chThdCreateStatic(wa_pose_thread, sizeof(wa_pose_thread),
            NORMALPRIO - 1, pose_thread, static_cast<void*>(&bicycle));
    chThdCreateStatic(wa_transmission_thread, sizeof(wa_transmission_thread),
            NORMALPRIO - 2, transmission_thread, nullptr);
    systime_t deadline = chVTGetSystemTime();
    while (true) {
        systime_t starttime = chVTGetSystemTime();
        chTMStartMeasurementX(&computation_time_measurement);
        constexpr float roll_torque = 0.0f;

        // get sensor measurements
        const float kistler_torque = adc_to_nm(analog.get_adc12(),
                sa::KISTLER_ADC_ZERO_OFFSET, sa::MAX_KISTLER_TORQUE);
        const float motor_torque = adc_to_nm(analog.get_adc13(),
                sa::KOLLMORGEN_ADC_ZERO_OFFSET, sa::MAX_KOLLMORGEN_TORQUE);
        const float steer_angle = util::encoder_count<float>(encoder_steer);
        const float rear_wheel_angle = -util::encoder_count<float>(encoder_rear_wheel);
        const float v = velocity_filter.output(
                -sa::REAR_WHEEL_RADIUS*(util::encoder_rate(encoder_rear_wheel)));
        (void)motor_torque; // not currently used

        // yaw angle, just use previous state value
        const float yaw_angle = util::wrap(bicycle.pose().yaw);

        // calculate rider applied torque
#if defined(FLIMNAP_ZERO_INPUT)
        const float steer_torque = 0.0f;
        (void)kistler_torque;
#else // defined(FLIMNAP_ZERO_INPUT)
        const float inertia_torque = -handlebar_model.torque(bicycle.observer().state());
        const float steer_torque = kistler_torque - inertia_torque;
#endif // defined(FLIMNAP_ZERO_INPUT)

        // simulate bicycle
        bicycle.set_v(fixed_velocity);
        bicycle.update_dynamics(roll_torque, steer_torque, yaw_angle, steer_angle, rear_wheel_angle);

        // generate handlebar torque output
        const float desired_velocity = model_t::get_state_element(bicycle.observer().state(),
                model_t::state_index_t::steer_rate);
        const dacsample_t handlebar_velocity_dac = set_handlebar_velocity(desired_velocity);
        chTMStopMeasurementX(&computation_time_measurement);

        // prepare message for transmission
        msg = SimulationMessage_init_zero;
        msg.timestamp = starttime;
        message::set_bicycle_input(&msg.input,
                (model_t::input_t() << roll_torque, steer_torque).finished());
        msg.has_input = true;
        message::set_simulation_state(&msg, bicycle);
        message::set_simulation_auxiliary_state(&msg, bicycle);
        if (std::is_same<observer_t, typename observer::Kalman<model_t>>::value) {
            message::set_kalman_gain(&msg.kalman, bicycle.observer());
            msg.has_kalman = true;
        }
        message::set_simulation_actuators(&msg, handlebar_velocity_dac);
        message::set_simulation_sensors(&msg,
                analog.get_adc12(), analog.get_adc13(),
                encoder_steer.count(), encoder_rear_wheel.count());
        message::set_simulation_timing(&msg,
                computation_time_measurement.last, transmission_time_measurement.last);
        message::set_simulation_pose(&msg, bicycle);
        size_t bytes_written = write_message_to_encode_buffer(msg);

        // transmit message
        if ((SDU1.config->usbp->state == USB_ACTIVE) && (SDU1.state == SDU_READY)) {
            chTMStartMeasurementX(&transmission_time_measurement);
            usbTransmit(SDU1.config->usbp, SDU1.config->bulk_in, packet_buffer.data(), bytes_written);
            chTMStopMeasurementX(&transmission_time_measurement);
        }

        deadline = chThdSleepUntilWindowed(deadline, deadline + looptime);
    }
}

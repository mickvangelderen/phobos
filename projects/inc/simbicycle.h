#pragma once
#include "ch.h"
#include "pose.pb.h"
#include "saconfig.h"
#include "haptic.h"
// bicycle submodule imports
#include "bicycle/bicycle.h"
#include "observer.h"
#include "constants.h" // rad/deg constants, real_t
#include "parameters.h"

#include <cstddef>
#include <type_traits>

namespace sim {

#define OBSERVER_FUNCTION_DECL(return_type) \
    template <typename T = Observer> \
    typename std::enable_if<std::is_base_of<observer::ObserverBase, T>::value, return_type>::type
#define NULL_OBSERVER_FUNCTION_DECL(return_type) \
    template <typename T = Observer> \
    typename std::enable_if<!std::is_base_of<observer::ObserverBase, T>::value, return_type>::type

/*
 * This template class simulates a bicycle model (template argument Model)
 * with an observer type (template argument Observer).
 *  - incorporates fields necessary for visualization, such as wheel angle
 *  - provides a single interface for using different bicycle models
 *  - allows simulation of dynamics and kinematics separately
 */
template <typename Model, typename Observer>
class Bicycle {
    static_assert(std::is_base_of<model::Bicycle, Model>::value,
            "Invalid template parameter type for sim::Bicycle");
    static_assert((std::is_base_of<observer::ObserverBase, Observer>::value) ||
                  (std::is_same<std::nullptr_t, Observer>::value),
            "Invalid template parameter type for sim::Bicycle. "
            "Observer type must be derived from observer::ObserverBase or std::nullptr_t.");

    public:
        using model_t = Model;
        using observer_t = Observer;
        using real_t = model::real_t;
        using second_order_matrix_t = typename model_t::second_order_matrix_t;
        using state_t = typename model_t::state_t;
        using auxiliary_state_t = typename model_t::auxiliary_state_t;
        using full_state_t = typename model_t::full_state_t;
        using input_t = typename model_t::input_t;
        using measurement_t = typename model_t::output_t;
        using full_state_index_t = typename model_t::full_state_index_t;

        // default bicycle model parameters
        static constexpr real_t default_fs = 200.0; // sample rate, Hz
        static constexpr real_t default_dt = 1.0/default_fs; // sample period, s
        static constexpr real_t default_v = 5.0; // forward speed, m/s
        static constexpr real_t v_quantization_resolution = 0.1; // m/s

        template <typename T = Observer>
            Bicycle(typename std::enable_if<std::is_base_of<observer::ObserverBase, T>::value, real_t>::type
                v = default_v, real_t dt = default_dt);
        template <typename T = Observer>
            Bicycle(typename std::enable_if<!std::is_base_of<observer::ObserverBase, T>::value, real_t>::type
                v = default_v, real_t dt = default_dt);

        void set_v(real_t v);
        void set_dt(real_t dt);
        OBSERVER_FUNCTION_DECL(void) reset();
        NULL_OBSERVER_FUNCTION_DECL(void) reset();
        void update_dynamics(real_t roll_torque_input, // update bicycle internal state
                real_t steer_torque_input,             // and handlebar feedback torque
                real_t yaw_angle_measurement,
                real_t steer_angle_measurement,
                real_t rear_wheel_angle_measurement);
        void update_kinematics(); // update bicycle pose
        OBSERVER_FUNCTION_DECL(void) prime_observer(); // perform observer specific initialization routine
        NULL_OBSERVER_FUNCTION_DECL(void) prime_observer(); // perform observer specific initialization routine

        const BicyclePoseMessage& pose() const; // get most recently computed pose
        real_t handlebar_feedback_torque() const; // get most recently computed feedback torque
        const input_t& input() const; // get most recently computed input
        const full_state_t& full_state() const; // get most recently computed full state

        // common bicycle model member variables
        model_t& model();
        const model_t& model() const;
        observer_t& observer();
        const observer_t& observer() const;
        const second_order_matrix_t& M() const;
        const second_order_matrix_t& C1() const;
        const second_order_matrix_t& K0() const;
        const second_order_matrix_t& K2() const;
        real_t wheelbase() const;
        real_t trail() const;
        real_t steer_axis_tilt() const;
        real_t rear_wheel_radius() const;
        real_t front_wheel_radius() const;
        real_t v() const;
        real_t dt() const;

    private:
        model_t m_model; // bicycle model object
        observer_t m_observer; // observer object
        full_state_t m_full_state; // auxiliary + dynamic state
        BicyclePoseMessage m_pose; // Unity visualization message
        input_t m_input; // bicycle model input vector
        measurement_t m_measurement; // bicycle model measurement vector
        binary_semaphore_t m_state_sem; // bsem for synchronizing kinematics update

        OBSERVER_FUNCTION_DECL(full_state_t) do_full_state_update(const full_state_t& full_state);
        NULL_OBSERVER_FUNCTION_DECL(full_state_t) do_full_state_update(const full_state_t& full_state);
};

} // namespace sim

#include "simbicycle.hh"

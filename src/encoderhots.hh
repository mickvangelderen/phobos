#include "extconfig.h"
#include <utility>
#include <Eigen/Cholesky>
#include <cmath>

/*
 * Member function definitions of EncoderHots template class.
 * See tsencoder.h for template class declaration.
 */

template <size_t M, size_t N, size_t O>
EncoderHots<M, N, O>::EncoderHots(const EncoderHotsConfig& config) :
m_skip_order_counter(0),
m_A(decltype(m_A)::Ones()), /* elements in last column of time stamp matrix are always 1 */
m_P(decltype(m_P)::Zero()),
m_B(decltype(m_B)::Zero()),
m_T(decltype(m_T)::Ones()), /* last element of time vector is always 1 */
m_t0(0),
m_alpha(0.0),
m_config(config),
m_count(0),
m_tc(0.0),
m_state(state_t::STOP),
m_index(index_t::NONE),
m_iqhandler(nullptr) {
    /*
     * EXT interrupts will trigger too often for high resolution encoders.
     * TODO: consider using ICU to record timestamps
     * TODO: determine  better count/rev limit
     */
    if (config.counts_per_rev) {
        chSysHalt("Encoder counts per revolution too high");
    }
}

template <size_t M, size_t N, size_t O>
void EncoderHots<M, N, O>::start() {
    chSysLock();
    osalDbgAssert((extp->state == EXT_STOP) || (extp->state == EXT_ACTIVE),
            "invalid state");
    if (extp->state == EXT_STOP) {
        extStartI(extp);
    }
    extChannelEnableSetModeI(extp, m_config.a, EXT_CH_MODE_BOTH_EDGES,
                             ab_callback, this);
    extChannelEnableSetModeI(extp, m_config.b, EXT_CH_MODE_BOTH_EDGES,
                             ab_callback, this);
    if (m_config.z == PAL_NOLINE) {
        m_index = index_t::NONE;
    } else {
        extChannelEnableSetModeI(extp, m_config.z, EXT_CH_MODE_RISING_EDGE,
                                 index_callback, this);
        m_index = index_t::NOTFOUND;
    }
    m_state = state_t::READY;
    chSysUnlock();
    m_iqhandler.start();
    chVTSet(&m_event_deadline_timer, m_event_deadline,
            add_event_callback, static_cast<void*>(this));
}

template <size_t M, size_t N, size_t O>
void EncoderHots<M, N, O>::stop() {
    chVTReset(&m_event_deadline_timer);
    m_iqhandler.stop();

    chSysLock();
    m_state = state_t::STOP;
    m_index = index_t::NONE;

    extChannelDisableClearModeI(extp, PAL_PAD(m_config.a));
    extChannelDisableClearModeI(extp, PAL_PAD(m_config.b));
    if (m_config.z != PAL_NOLINE) {
        extChannelDisableClearModeI(extp, PAL_PAD(m_config.z));
    }
    extStopIfChannelsDisabledI(extp);
    chSysUnlock();
}

template <size_t M, size_t N, size_t O>
typename EncoderHots<M, N, O>::state_t EncoderHots<M, N, O>::state() const {
    return m_state;
}

template <size_t M, size_t N, size_t O>
typename EncoderHots<M, N, O>::index_t EncoderHots<M, N, O>::index() const volatile {
    return m_index;
}

template <size_t M, size_t N, size_t O>
void EncoderHots<M, N, O>::update_polynomial_fit() {
    /*
     * Redefine oldest event to be at time zero. Scale time so that difference
     * in time from oldest to newest event is equal to 1.
     *
     * If the difference A(N - 1, 1) - A(0, 1) is too large, the value will be
     * nonsensical due to uint rolling back to zero. The difference must also
     * be less than int32_t_max as these values will be cast from uint32_t to
     * int32_t.
     */
    m_iqhandler.wait();
    auto events = m_iqhandler.circular_buffer();
    size_t event_index = m_iqhandler.index();
    size_t newest_index = event_index - 1;
    if (event_index == 0) {
        newest_index = N - 1;
    }
    m_t0 = events[event_index].first;
    m_alpha = 1.0f / (events[newest_index].first - m_t0);

    for (unsigned int i = 0; i < N; ++i) {
        m_A(i, M - 1) = m_alpha * (events[(event_index + i) % N].first - m_t0);
        m_B(i) = events[(event_index + i) % N].second;
    }
    m_iqhandler.signal();

    for (unsigned int i = 0; i < M - 1; ++i) {
        m_A.col(M - 2 - i) = m_A.col(M - 1 - i).cwiseProduct(m_A.col(M - 1));
    }
    m_P.noalias() = (m_A.transpose() * m_A).ldlt().solve(
            (m_A.transpose() * m_B.template cast<polycoeff_t>()));
}

template <size_t M, size_t N, size_t O>
void EncoderHots<M, N, O>::update_estimate_time(rtcnt_t tc) {
    m_tc = tc;
    polycoeff_t tcf = m_alpha * static_cast<polycoeff_t>(tc - m_t0);
    chDbgAssert(tcf > 1.0f, "tcf should always be ahead of last prediction");
    m_T(M - 1) = tcf;
    for (unsigned int i = 0; i < M - 1; ++i) {
        m_T(M - 2 - i) = m_T(M - 1 - i) * tcf;
    }
}

template <size_t M, size_t N, size_t O>
polycoeff_t EncoderHots<M, N, O>::position() const {
    polycoeff_t x = m_P.transpose() * m_T;
    polycoeff_t count = static_cast<polycoeff_t>(m_count);
    if (std::abs(x - count) > 1.0f) {
        return count;
    }
    return x;
}

// TODO: Validity of velocity and acceleration for large error in estimated
// position (i.e. position is adjusted if a large amount of time passes without
// an encoder event).
template <size_t M, size_t N, size_t O>
polycoeff_t EncoderHots<M, N, O>::velocity() const {
    Eigen::Matrix<hotscnt_t, M, 1> velocity_coefficients;
    for (unsigned int i = 0; i < M; ++i) {
        velocity_coefficients(i) = M - i;
    }
    return velocity_coefficients.template cast<polycoeff_t>().cwiseProduct(
            m_P.template head<M>()).transpose() * m_T.template tail<M>();
}

template <size_t M, size_t N, size_t O>
polycoeff_t EncoderHots<M, N, O>::acceleration() const {
    Eigen::Matrix<hotscnt_t, M - 1 , 1> acceleration_coefficients;
    for (unsigned int i = 0; i < M - 1; ++i) {
        acceleration_coefficients(i) = (M - i) * (M - i - 1);
    }
    return acceleration_coefficients.template cast<polycoeff_t>().cwiseProduct(
            m_P.template head<M - 1>()).transpose() * m_T.template tail<M - 1>();
}

template <size_t M, size_t N, size_t O>
void EncoderHots<M, N, O>::ab_callback(EXTDriver* extp, expchannel_t channel) {
    (void)extp;
    chSysLockFromISR();
    auto enc = static_cast<EncoderHots<M, N, O>*>(extGetChannelCallbackObject(channel));
    if (((channel == PAL_PAD(enc->m_config.a)) &&
                (palReadLine(enc->m_config.a) != palReadLine(enc->m_config.b))) ||
        ((channel == PAL_PAD(enc->m_config.b)) &&
                (palReadLine(enc->m_config.a) == palReadLine(enc->m_config.b)))) {
        ++enc->m_count;
    } else {
        --enc->m_count;
    }
    chVTSetI(&enc->m_event_deadline_timer, enc->m_event_deadline,
            add_event_callback, static_cast<void*>(enc));
    event_t event = std::make_pair(chSysGetRealtimeCounterX(), enc->m_count);
    auto iqh = enc->m_iqhandler;
    if (enc->m_skip_order_counter++ != 0) {
        /* need to overwrite previous sample */
        if (iqh.m_buffer_index-- == 0) {
            iqh.m_buffer_index = iqh.m_circular_buffer.size() - 1;
        }
    }
    iqh.insertI(&event);
    if (enc->m_skip_order_counter > O) {
        enc->m_skip_order_counter = 0;
    }
    chSysUnlockFromISR();
}

template <size_t M, size_t N, size_t O>
void EncoderHots<M, N, O>::index_callback(EXTDriver* extp, expchannel_t channel) {
    chSysLockFromISR();
    auto enc = static_cast<EncoderHots<M, N, O>*>(extGetChannelCallbackObject(channel));
    enc->m_count = enc->m_config.z_count;
    enc->m_index = index_t::FOUND;
    extChannelDisableClearModeI(extp, PAL_PAD(enc->m_config.z));
    chSysUnlockFromISR();
}

template <size_t M, size_t N, size_t O>
void EncoderHots<M, N, O>::add_event_callback(void* p) {
    chSysLockFromISR();
    auto enc = static_cast<EncoderHots<M, N, O>*>(p);
    chVTSetI(&enc->m_event_deadline_timer, enc->m_event_deadline,
            add_event_callback, p);
    event_t event = std::make_pair(chSysGetRealtimeCounterX(), enc->m_count);
    enc->m_iqhandler.insertI(&event);
    enc->m_skip_order_counter = 0; /* don't overwrite on next edge */
    chSysUnlockFromISR();
}

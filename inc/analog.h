#pragma once
#include "hal.h"
#include <array>

class Analog {
    public:
        Analog();
        void start(gptcnt_t sample_rate, bool use_events=false);
        void stop();
        adcsample_t get_adc10() const;
        adcsample_t get_adc11() const;
        adcsample_t get_adc12() const;
        static adc_channels_num_t buffer_size();

    private:
        enum sensor_t {ADC10=0, ADC11, ADC12};
        static constexpr adc_channels_num_t m_adc_buffer_size = 3;
        std::array<adcsample_t, m_adc_buffer_size> m_adc_buffer;
};

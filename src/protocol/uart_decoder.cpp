#include "uart_decoder.h"

#include <cstdio>

namespace jtag::protocol {

std::vector<DecodedFrame> decodeUart(const std::vector<jtag::SampleFrame>& samples,
                                     const UartConfig& cfg) {
    std::vector<DecodedFrame> frames;
    if (samples.empty() || cfg.rx_pin.empty() || cfg.baud_rate == 0) return frames;

    // Duration of one bit in microseconds
    const double bit_us = 1e6 / static_cast<double>(cfg.baud_rate);

    const auto& t0 = samples.front();
    const int n = static_cast<int>(samples.size());

    int i = 0;
    while (i < n) {
        // Wait for start bit (falling edge: HIGH -> LOW on idle line)
        if (samplePinValue(samples[i], cfg.rx_pin)) {
            i++;
            continue;
        }

        // Potential start bit at samples[i]
        double start_time_us = sampleTimeUs(samples[i], t0);

        // Sample the middle of the start bit to confirm it is LOW
        double sample_point = start_time_us + bit_us * 0.5;

        // Advance i to the sample closest to sample_point
        while (i + 1 < n &&
               sampleTimeUs(samples[i + 1], t0) <= sample_point) {
            i++;
        }
        if (i >= n) break;

        if (samplePinValue(samples[i], cfg.rx_pin)) {
            // Not a real start bit (noise), skip and keep searching
            i++;
            continue;
        }

        // Decode data bits
        uint32_t data_val = 0;
        bool frame_error = false;

        for (int bit = 0; bit < cfg.data_bits; bit++) {
            // Sample middle of data bit
            double bit_center_us = start_time_us + bit_us * (1.5 + bit);
            while (i + 1 < n &&
                   sampleTimeUs(samples[i + 1], t0) <= bit_center_us) {
                i++;
            }
            if (i >= n) { frame_error = true; break; }
            if (samplePinValue(samples[i], cfg.rx_pin)) {
                data_val |= (1u << bit);  // LSB first
            }
        }

        // Parity bit (if enabled)
        if (!frame_error && cfg.parity_enable) {
            double parity_center_us = start_time_us + bit_us * (1.5 + cfg.data_bits);
            while (i + 1 < n &&
                   sampleTimeUs(samples[i + 1], t0) <= parity_center_us) {
                i++;
            }
            if (i < n) {
                int parity_val = samplePinValue(samples[i], cfg.rx_pin) ? 1 : 0;
                // Count set bits in data_val
                int ones = 0;
                for (int b = 0; b < cfg.data_bits; b++) ones += (data_val >> b) & 1;
                bool expected_parity = cfg.parity_odd ? ((ones % 2) == 0) : ((ones % 2) == 1);
                if (parity_val != (expected_parity ? 1 : 0)) frame_error = true;
            }
        }

        // Stop bit check
        int total_data_parity_bits = cfg.data_bits + (cfg.parity_enable ? 1 : 0);
        double stop_center_us = start_time_us + bit_us * (1.5 + total_data_parity_bits);
        while (i + 1 < n &&
               sampleTimeUs(samples[i + 1], t0) <= stop_center_us) {
            i++;
        }
        bool stop_ok = (i >= n) || samplePinValue(samples[i], cfg.rx_pin);
        if (!stop_ok) frame_error = true;

        // Record frame end time (after stop bit)
        double end_time_us = start_time_us +
                             bit_us * (1.0 + total_data_parity_bits + cfg.stop_bits);

        // Emit DecodedFrame
        DecodedFrame f;
        f.start_us  = start_time_us;
        f.end_us    = end_time_us;
        f.kind      = ProtocolKind::UART;
        f.error_flag = frame_error;

        char label_buf[32];
        char data_buf[16];
        snprintf(data_buf, sizeof(data_buf), "%02X", data_val & 0xFF);
        f.data = data_buf;

        if (!frame_error) {
            char ch = static_cast<char>(data_val & 0xFF);
            if (ch >= 0x20 && ch < 0x7F)
                snprintf(label_buf, sizeof(label_buf), "0x%02X '%c'", data_val & 0xFF, ch);
            else
                snprintf(label_buf, sizeof(label_buf), "0x%02X", data_val & 0xFF);
        } else {
            snprintf(label_buf, sizeof(label_buf), "ERR");
        }
        f.label = label_buf;

        frames.push_back(std::move(f));

        // Move past the stop bit
        i++;
    }

    return frames;
}

}  // namespace jtag::protocol

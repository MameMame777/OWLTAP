#include "spi_decoder.h"

#include <cstdio>

namespace jtag::protocol {

std::vector<DecodedFrame> decodeSpi(const std::vector<jtag::SampleFrame>& samples,
                                    const SpiConfig& cfg) {
    std::vector<DecodedFrame> frames;
    if (samples.empty() || cfg.clk_pin.empty()) return frames;

    const auto& t0_frame = samples.front();
    const int n = static_cast<int>(samples.size());

    // Idle clock level determined by cpol
    const bool idle_clk = cfg.cpol;  // false=LOW, true=HIGH

    // We decode on clock edges:
    // CPHA=0: sample on first (leading) edge
    // CPHA=1: sample on second (trailing) edge
    // Leading edge:  idle->active  (idle_clk -> !idle_clk)
    // Trailing edge: active->idle  (!idle_clk -> idle_clk)

    bool in_transfer = false;
    uint64_t mosi_word = 0, miso_word = 0;
    int bit_count = 0;
    double frame_start_us = 0.0;
    bool prev_clk = idle_clk;

    for (int i = 1; i < n; i++) {
        // Check chip-select (active LOW, unless cs_pin is empty)
        if (!cfg.cs_pin.empty()) {
            bool cs_active = !samplePinValue(samples[i], cfg.cs_pin);
            if (!cs_active) {
                // CS deasserted — flush any partial frame
                if (in_transfer && bit_count > 0) {
                    // Emit partial frame
                    double end_us = sampleTimeUs(samples[i], t0_frame);
                    char label[64];
                    if (!cfg.mosi_pin.empty() && !cfg.miso_pin.empty())
                        snprintf(label, sizeof(label), "MOSI:%0*llX MISO:%0*llX",
                                 (cfg.bits_per_word + 3) / 4,
                                 static_cast<unsigned long long>(mosi_word),
                                 (cfg.bits_per_word + 3) / 4,
                                 static_cast<unsigned long long>(miso_word));
                    else if (!cfg.mosi_pin.empty())
                        snprintf(label, sizeof(label), "0x%0*llX",
                                 (cfg.bits_per_word + 3) / 4,
                                 static_cast<unsigned long long>(mosi_word));
                    else
                        snprintf(label, sizeof(label), "0x%0*llX",
                                 (cfg.bits_per_word + 3) / 4,
                                 static_cast<unsigned long long>(miso_word));

                    DecodedFrame f;
                    f.start_us   = frame_start_us;
                    f.end_us     = end_us;
                    f.kind       = ProtocolKind::SPI;
                    f.label      = label;
                    f.error_flag = (bit_count != cfg.bits_per_word);
                    frames.push_back(std::move(f));
                }
                in_transfer = false;
                bit_count = 0;
                mosi_word = miso_word = 0;
                prev_clk = idle_clk;
                continue;
            }
        }

        bool curr_clk = samplePinValue(samples[i], cfg.clk_pin);
        bool clk_rose = (!prev_clk && curr_clk);
        bool clk_fell = (prev_clk && !curr_clk);
        prev_clk = curr_clk;

        // Determine if this edge is the sampling edge
        bool leading_edge  = (!idle_clk && clk_rose) || (idle_clk && clk_fell);
        bool trailing_edge = (!idle_clk && clk_fell) || (idle_clk && clk_rose);
        bool sample_edge   = (!cfg.cpha) ? leading_edge : trailing_edge;

        if (!sample_edge) continue;

        if (!in_transfer) {
            in_transfer = true;
            frame_start_us = sampleTimeUs(samples[i], t0_frame);
            mosi_word = miso_word = 0;
            bit_count = 0;
        }

        // Sample data bits
        bool mosi_bit = cfg.mosi_pin.empty() ? false : samplePinValue(samples[i], cfg.mosi_pin);
        bool miso_bit = cfg.miso_pin.empty() ? false : samplePinValue(samples[i], cfg.miso_pin);

        if (cfg.lsb_first) {
            if (mosi_bit) mosi_word |= (1ULL << bit_count);
            if (miso_bit) miso_word |= (1ULL << bit_count);
        } else {
            mosi_word = (mosi_word << 1) | (mosi_bit ? 1 : 0);
            miso_word = (miso_word << 1) | (miso_bit ? 1 : 0);
        }
        bit_count++;

        if (bit_count == cfg.bits_per_word) {
            double end_us = sampleTimeUs(samples[i], t0_frame);
            char label[64];
            if (!cfg.mosi_pin.empty() && !cfg.miso_pin.empty())
                snprintf(label, sizeof(label), "MOSI:%0*llX MISO:%0*llX",
                         (cfg.bits_per_word + 3) / 4,
                         static_cast<unsigned long long>(mosi_word),
                         (cfg.bits_per_word + 3) / 4,
                         static_cast<unsigned long long>(miso_word));
            else if (!cfg.mosi_pin.empty())
                snprintf(label, sizeof(label), "0x%0*llX",
                         (cfg.bits_per_word + 3) / 4,
                         static_cast<unsigned long long>(mosi_word));
            else
                snprintf(label, sizeof(label), "0x%0*llX",
                         (cfg.bits_per_word + 3) / 4,
                         static_cast<unsigned long long>(miso_word));

            DecodedFrame f;
            f.start_us   = frame_start_us;
            f.end_us     = end_us;
            f.kind       = ProtocolKind::SPI;
            f.label      = label;
            f.error_flag = false;
            frames.push_back(std::move(f));

            // Reset for next word
            mosi_word = miso_word = 0;
            bit_count = 0;
            frame_start_us = end_us;
        }
    }

    return frames;
}

}  // namespace jtag::protocol

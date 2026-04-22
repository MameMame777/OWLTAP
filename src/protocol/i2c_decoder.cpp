#include "i2c_decoder.h"

#include <cstdio>

namespace jtag::protocol {

// I2C state machine states
enum class I2cState {
    IDLE,
    ADDR,       // reading 7-bit address + R/W bit (8 bits)
    ADDR_ACK,   // ACK/NAK after address
    DATA,       // reading 8 data bits
    DATA_ACK,   // ACK/NAK after data byte
};

std::vector<DecodedFrame> decodeI2c(const std::vector<jtag::SampleFrame>& samples,
                                    const I2cConfig& cfg) {
    std::vector<DecodedFrame> frames;
    if (samples.empty() || cfg.scl_pin.empty() || cfg.sda_pin.empty()) return frames;

    const auto& t0_frame = samples.front();
    const int n = static_cast<int>(samples.size());

    I2cState state = I2cState::IDLE;
    bool prev_scl = true;  // I2C lines idle HIGH
    bool prev_sda = true;
    int bit_count = 0;
    uint8_t shift_reg = 0;
    double event_start_us = 0.0;

    auto emit = [&](double s, double e, const char* label, bool err) {
        DecodedFrame f;
        f.start_us  = s;
        f.end_us    = e;
        f.kind      = ProtocolKind::I2C;
        f.label     = label;
        f.error_flag = err;
        frames.push_back(std::move(f));
    };

    for (int i = 1; i < n; i++) {
        bool scl = samplePinValue(samples[i], cfg.scl_pin);
        bool sda = samplePinValue(samples[i], cfg.sda_pin);
        double t_us = sampleTimeUs(samples[i], t0_frame);

        bool scl_rose = (!prev_scl && scl);
        bool scl_fell = (prev_scl && !scl);
        bool sda_changed_while_scl_high = (prev_scl && scl && sda != prev_sda);

        // Detect START condition: SDA falls while SCL is HIGH
        if (sda_changed_while_scl_high && !sda && prev_sda) {
            emit(t_us, t_us, "START", false);
            state = I2cState::ADDR;
            bit_count = 0;
            shift_reg = 0;
            event_start_us = t_us;
            prev_scl = scl; prev_sda = sda;
            continue;
        }

        // Detect STOP condition: SDA rises while SCL is HIGH
        if (sda_changed_while_scl_high && sda && !prev_sda) {
            emit(t_us, t_us, "STOP", false);
            state = I2cState::IDLE;
            prev_scl = scl; prev_sda = sda;
            continue;
        }

        // Sample data on rising SCL edge
        if (scl_rose) {
            switch (state) {
                case I2cState::IDLE:
                    break;

                case I2cState::ADDR:
                case I2cState::DATA: {
                    shift_reg = static_cast<uint8_t>((shift_reg << 1) | (sda ? 1 : 0));
                    bit_count++;
                    if (bit_count == 8) {
                        // Emit address or data frame
                        char label[32];
                        if (state == I2cState::ADDR) {
                            uint8_t addr = (shift_reg >> 1) & 0x7F;
                            bool read = (shift_reg & 0x01) != 0;
                            snprintf(label, sizeof(label), "ADDR 0x%02X %s",
                                     addr, read ? "R" : "W");
                        } else {
                            snprintf(label, sizeof(label), "DATA 0x%02X", shift_reg);
                        }
                        emit(event_start_us, t_us, label, false);
                        // Next: ACK
                        state = (state == I2cState::ADDR) ? I2cState::ADDR_ACK
                                                          : I2cState::DATA_ACK;
                        bit_count = 0;
                        shift_reg = 0;
                        event_start_us = t_us;
                    }
                    break;
                }

                case I2cState::ADDR_ACK:
                case I2cState::DATA_ACK: {
                    // SDA LOW = ACK, HIGH = NAK
                    bool nak = sda;
                    emit(event_start_us, t_us, nak ? "NAK" : "ACK", nak);
                    state = I2cState::DATA;
                    bit_count = 0;
                    shift_reg = 0;
                    event_start_us = t_us;
                    break;
                }
            }
        }

        (void)scl_fell;  // not used directly; START/STOP are on SCL-high SDA changes

        prev_scl = scl;
        prev_sda = sda;
    }

    return frames;
}

}  // namespace jtag::protocol

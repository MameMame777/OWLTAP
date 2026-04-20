#include "mpsse.h"

namespace jtag {

MpsseCommandBuffer::MpsseCommandBuffer() {
    buffer_.reserve(4096);
}

void MpsseCommandBuffer::clear() {
    buffer_.clear();
    expected_read_bytes_ = 0;
}

void MpsseCommandBuffer::appendByte(uint8_t b) {
    buffer_.push_back(b);
}

void MpsseCommandBuffer::appendBytes(const uint8_t* data, size_t len) {
    buffer_.insert(buffer_.end(), data, data + len);
}

void MpsseCommandBuffer::clockTms(uint8_t tms_bits, int bit_count,
                                   bool tdi_value, bool read_tdo) {
    if (bit_count < 1 || bit_count > 7) return;

    uint8_t cmd = read_tdo ? mpsse_cmd::MPSSE_TMS_RDWR : mpsse_cmd::MPSSE_TMS_OUT;
    appendByte(cmd);
    appendByte(static_cast<uint8_t>(bit_count - 1));  // Length - 1
    // Bit 7 = TDI value, bits 0..6 = TMS pattern
    uint8_t data_byte = (tms_bits & 0x7F) | (tdi_value ? 0x80 : 0x00);
    appendByte(data_byte);

    if (read_tdo) {
        expected_read_bytes_ += 1;  // 1 byte for bit-mode read
    }
}

void MpsseCommandBuffer::shiftOut(const uint8_t* data, int bit_count) {
    if (bit_count <= 0) return;

    int full_bytes = bit_count / 8;
    int remaining_bits = bit_count % 8;

    // Shift full bytes in chunks of up to 65536 bytes (uint16_t limit)
    int offset = 0;
    while (full_bytes > 0) {
        int chunk = (full_bytes > 65536) ? 65536 : full_bytes;
        appendByte(mpsse_cmd::MPSSE_WRITE_NEG_LSB);
        uint16_t len = static_cast<uint16_t>(chunk - 1);
        appendByte(static_cast<uint8_t>(len & 0xFF));
        appendByte(static_cast<uint8_t>((len >> 8) & 0xFF));
        appendBytes(data + offset, chunk);
        offset += chunk;
        full_bytes -= chunk;
    }

    // Shift remaining bits
    if (remaining_bits > 0) {
        appendByte(mpsse_cmd::MPSSE_WRITE_BITS_NEG_LSB);
        appendByte(static_cast<uint8_t>(remaining_bits - 1));
        appendByte(data[bit_count / 8]);
    }
}

void MpsseCommandBuffer::shiftIn(int bit_count) {
    if (bit_count <= 0) return;

    int full_bytes = bit_count / 8;
    int remaining_bits = bit_count % 8;

    // Read full bytes in chunks of up to 65536
    while (full_bytes > 0) {
        int chunk = (full_bytes > 65536) ? 65536 : full_bytes;
        appendByte(mpsse_cmd::MPSSE_READ_POS_LSB);
        uint16_t len = static_cast<uint16_t>(chunk - 1);
        appendByte(static_cast<uint8_t>(len & 0xFF));
        appendByte(static_cast<uint8_t>((len >> 8) & 0xFF));
        expected_read_bytes_ += chunk;
        full_bytes -= chunk;
    }

    if (remaining_bits > 0) {
        appendByte(mpsse_cmd::MPSSE_READ_BITS_POS_LSB);
        appendByte(static_cast<uint8_t>(remaining_bits - 1));
        expected_read_bytes_ += 1;
    }
}

void MpsseCommandBuffer::shiftInOut(const uint8_t* tdi_data, int bit_count) {
    if (bit_count <= 0) return;

    int full_bytes = bit_count / 8;
    int remaining_bits = bit_count % 8;

    // Shift full bytes in chunks of up to 65536
    int offset = 0;
    while (full_bytes > 0) {
        int chunk = (full_bytes > 65536) ? 65536 : full_bytes;
        appendByte(mpsse_cmd::MPSSE_RDWR_LSB);
        uint16_t len = static_cast<uint16_t>(chunk - 1);
        appendByte(static_cast<uint8_t>(len & 0xFF));
        appendByte(static_cast<uint8_t>((len >> 8) & 0xFF));
        appendBytes(tdi_data + offset, chunk);
        expected_read_bytes_ += chunk;
        offset += chunk;
        full_bytes -= chunk;
    }

    if (remaining_bits > 0) {
        appendByte(mpsse_cmd::MPSSE_RDWR_BITS_LSB);
        appendByte(static_cast<uint8_t>(remaining_bits - 1));
        appendByte(tdi_data[bit_count / 8]);
        expected_read_bytes_ += 1;
    }
}

void MpsseCommandBuffer::setLowBits(uint8_t value, uint8_t direction) {
    appendByte(mpsse_cmd::SET_BITS_LOW);
    appendByte(value);
    appendByte(direction);
}

void MpsseCommandBuffer::getLowBits() {
    appendByte(mpsse_cmd::GET_BITS_LOW);
    expected_read_bytes_ += 1;
}

void MpsseCommandBuffer::setClockDivisor(uint16_t divisor) {
    appendByte(mpsse_cmd::TCK_DIVISOR);
    appendByte(static_cast<uint8_t>(divisor & 0xFF));
    appendByte(static_cast<uint8_t>((divisor >> 8) & 0xFF));
}

void MpsseCommandBuffer::disableClockDivide5() {
    appendByte(mpsse_cmd::DIS_DIV_5);
}

void MpsseCommandBuffer::enableClockDivide5() {
    appendByte(mpsse_cmd::EN_DIV_5);
}

void MpsseCommandBuffer::enable3PhaseClocking() {
    appendByte(mpsse_cmd::EN_3_PHASE);
}

void MpsseCommandBuffer::disable3PhaseClocking() {
    appendByte(mpsse_cmd::DIS_3_PHASE);
}

void MpsseCommandBuffer::disableLoopback() {
    appendByte(mpsse_cmd::LOOPBACK_END);
}

void MpsseCommandBuffer::sendImmediate() {
    appendByte(mpsse_cmd::SEND_IMMEDIATE);
}

} // namespace jtag

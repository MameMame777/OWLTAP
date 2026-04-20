#include "tap_controller.h"

#include <cstring>

namespace jtag {

// TAP state transition table: next_state[current_state][tms_value]
static const TapState kTapTransitions[16][2] = {
    // TMS=0                    TMS=1
    {TapState::RUN_TEST_IDLE,   TapState::TEST_LOGIC_RESET},  // TEST_LOGIC_RESET
    {TapState::RUN_TEST_IDLE,   TapState::SELECT_DR_SCAN},    // RUN_TEST_IDLE
    {TapState::CAPTURE_DR,      TapState::SELECT_IR_SCAN},    // SELECT_DR_SCAN
    {TapState::SHIFT_DR,        TapState::EXIT1_DR},          // CAPTURE_DR
    {TapState::SHIFT_DR,        TapState::EXIT1_DR},          // SHIFT_DR
    {TapState::PAUSE_DR,        TapState::UPDATE_DR},         // EXIT1_DR
    {TapState::PAUSE_DR,        TapState::EXIT2_DR},          // PAUSE_DR
    {TapState::SHIFT_DR,        TapState::UPDATE_DR},         // EXIT2_DR
    {TapState::RUN_TEST_IDLE,   TapState::SELECT_DR_SCAN},    // UPDATE_DR
    {TapState::CAPTURE_IR,      TapState::TEST_LOGIC_RESET},  // SELECT_IR_SCAN
    {TapState::SHIFT_IR,        TapState::EXIT1_IR},          // CAPTURE_IR
    {TapState::SHIFT_IR,        TapState::EXIT1_IR},          // SHIFT_IR
    {TapState::PAUSE_IR,        TapState::UPDATE_IR},         // EXIT1_IR
    {TapState::PAUSE_IR,        TapState::EXIT2_IR},          // PAUSE_IR
    {TapState::SHIFT_IR,        TapState::UPDATE_IR},         // EXIT2_IR
    {TapState::RUN_TEST_IDLE,   TapState::SELECT_DR_SCAN},    // UPDATE_IR
};

static const char* kTapStateNames[16] = {
    "TEST-LOGIC-RESET", "RUN-TEST/IDLE",
    "SELECT-DR-SCAN",   "CAPTURE-DR",
    "SHIFT-DR",         "EXIT1-DR",
    "PAUSE-DR",         "EXIT2-DR",
    "UPDATE-DR",        "SELECT-IR-SCAN",
    "CAPTURE-IR",       "SHIFT-IR",
    "EXIT1-IR",         "PAUSE-IR",
    "EXIT2-IR",         "UPDATE-IR",
};

const char* tapStateName(TapState state) {
    int idx = static_cast<int>(state);
    if (idx >= 0 && idx < 16) return kTapStateNames[idx];
    return "UNKNOWN";
}

int computeTmsPath(TapState from, TapState to, uint8_t& tms_bits) {
    if (from == to) {
        // Stay in current state
        // For SHIFT states, TMS=0 stays; for others, depends
        tms_bits = 0;
        return 0;
    }

    // BFS to find shortest TMS path (max 7 transitions)
    struct PathEntry {
        TapState state;
        uint8_t tms;
        int length;
    };

    bool visited[16] = {};
    PathEntry queue[16];
    int head = 0, tail = 0;

    queue[tail++] = {from, 0, 0};
    visited[static_cast<int>(from)] = true;

    while (head < tail) {
        PathEntry& cur = queue[head++];

        for (int tms_val = 0; tms_val <= 1; tms_val++) {
            TapState next = kTapTransitions[static_cast<int>(cur.state)][tms_val];
            uint8_t new_tms = cur.tms | (static_cast<uint8_t>(tms_val) << cur.length);
            int new_len = cur.length + 1;

            if (next == to) {
                tms_bits = new_tms;
                return new_len;
            }

            if (!visited[static_cast<int>(next)] && new_len < 7) {
                visited[static_cast<int>(next)] = true;
                queue[tail++] = {next, new_tms, new_len};
            }
        }
    }

    // Fallback: go through TEST-LOGIC-RESET (5x TMS=1)
    tms_bits = 0x1F;
    return 5;
}

TapController::TapController(FtdiDevice& device)
    : device_(device), state_(TapState::TEST_LOGIC_RESET) {}

bool TapController::reset() {
    // Clock TMS=1 five times to force TEST-LOGIC-RESET from any state
    MpsseCommandBuffer cmd;
    cmd.clockTms(0x1F, 5);  // 5 bits of TMS=1
    // Move to RUN-TEST-IDLE
    cmd.clockTms(0x00, 1);  // TMS=0

    if (!device_.write(cmd)) {
        last_error_ = device_.lastError();
        return false;
    }

    state_ = TapState::RUN_TEST_IDLE;
    return true;
}

bool TapController::clkIdle(int cycles) {
    if (!gotoState(TapState::RUN_TEST_IDLE)) return false;
    if (cycles <= 0) return true;
    // Clock TMS=0 to stay in RTI
    while (cycles > 0) {
        int n = (cycles > 7) ? 7 : cycles;
        MpsseCommandBuffer cmd;
        cmd.clockTms(0x00, n);  // TMS=0 × n
        if (!device_.write(cmd)) {
            last_error_ = device_.lastError();
            return false;
        }
        cycles -= n;
    }
    return true;
}

bool TapController::gotoState(TapState target) {
    if (state_ == target) return true;

    uint8_t tms_bits;
    int path_len = computeTmsPath(state_, target, tms_bits);

    if (path_len == 0) return true;

    MpsseCommandBuffer cmd;
    // MPSSE TMS command handles up to 7 bits at a time
    cmd.clockTms(tms_bits, path_len);

    if (!device_.write(cmd)) {
        last_error_ = device_.lastError();
        return false;
    }

    state_ = target;
    return true;
}

bool TapController::shiftIR(const uint8_t* ir_data, int ir_bits) {
    if (ir_bits <= 0) return false;

    // Move to SHIFT-IR
    if (!gotoState(TapState::SHIFT_IR)) return false;

    // Perform shift (no readback for IR)
    if (!doShift(ir_data, nullptr, ir_bits, false)) return false;

    // After doShift, we're in EXIT1-IR
    state_ = TapState::EXIT1_IR;

    // Move to RUN-TEST-IDLE
    return gotoState(TapState::RUN_TEST_IDLE);
}

bool TapController::shiftDR(const uint8_t* tdi_data,
                              std::vector<uint8_t>& tdo_data, int dr_bits) {
    if (dr_bits <= 0) return false;

    // Move to SHIFT-DR
    if (!gotoState(TapState::SHIFT_DR)) return false;

    // Perform shift with readback
    if (!doShift(tdi_data, &tdo_data, dr_bits, true)) return false;

    // After doShift, we're in EXIT1-DR
    state_ = TapState::EXIT1_DR;

    // Move to RUN-TEST-IDLE
    return gotoState(TapState::RUN_TEST_IDLE);
}

bool TapController::shiftDR(const uint8_t* tdi_data, int dr_bits) {
    if (dr_bits <= 0) return false;

    if (!gotoState(TapState::SHIFT_DR)) return false;
    if (!doShift(tdi_data, nullptr, dr_bits, false)) return false;

    state_ = TapState::EXIT1_DR;
    return gotoState(TapState::RUN_TEST_IDLE);
}

bool TapController::readDR(std::vector<uint8_t>& tdo_data, int dr_bits) {
    // Shift in zeros while reading
    std::vector<uint8_t> zeros((dr_bits + 7) / 8, 0);
    return shiftDR(zeros.data(), tdo_data, dr_bits);
}

bool TapController::doShift(const uint8_t* tdi_data,
                              std::vector<uint8_t>* tdo_data,
                              int bit_count, bool read_back) {
    if (bit_count <= 0) return true;

    MpsseCommandBuffer cmd;

    // For the shift operation, we need to handle the last bit specially:
    // The last bit is clocked with TMS=1 to exit the SHIFT state.

    int bulk_bits = bit_count - 1;  // All bits except the last
    int bulk_bytes = bulk_bits / 8;
    int bulk_remaining = bulk_bits % 8;

    // Shift bulk bytes (all except last bit)
    if (bulk_bytes > 0) {
        if (read_back) {
            cmd.shiftInOut(tdi_data, bulk_bytes * 8);
        } else {
            cmd.shiftOut(tdi_data, bulk_bytes * 8);
        }
    }

    // Shift remaining bits (not including the very last bit)
    if (bulk_remaining > 0) {
        uint8_t partial = tdi_data ? tdi_data[bulk_bytes] : 0;
        if (read_back) {
            // Bit-mode shift in/out
            cmd.shiftInOut(&partial, bulk_remaining);
        } else {
            cmd.shiftOut(&partial, bulk_remaining);
        }
    }

    // Last bit: clock with TMS=1 to exit SHIFT state
    {
        int last_bit_byte = (bit_count - 1) / 8;
        int last_bit_pos = (bit_count - 1) % 8;
        bool last_tdi = tdi_data ? ((tdi_data[last_bit_byte] >> last_bit_pos) & 1) : false;

        // TMS=1, clock one bit with TDI=last_tdi
        cmd.clockTms(0x01, 1, last_tdi, read_back);
    }

    cmd.sendImmediate();

    if (read_back) {
        std::vector<uint8_t> raw;
        if (!device_.transfer(cmd, raw)) {
            last_error_ = device_.lastError();
            return false;
        }

        // Reassemble TDO data from raw bytes
        int out_bytes = (bit_count + 7) / 8;
        tdo_data->resize(out_bytes, 0);

        size_t raw_idx = 0;

        // Copy bulk byte data
        if (bulk_bytes > 0 && raw_idx + bulk_bytes <= raw.size()) {
            std::memcpy(tdo_data->data(), raw.data() + raw_idx, bulk_bytes);
            raw_idx += bulk_bytes;
        }

        // Handle remaining bits (bit-mode reads are right-justified in byte)
        if (bulk_remaining > 0 && raw_idx < raw.size()) {
            uint8_t partial = raw[raw_idx];
            raw_idx++;
            // Bit-mode data is shifted in MSB first, so right-align it
            partial >>= (8 - bulk_remaining);
            // Place at the correct position
            int byte_offset = bulk_bytes;
            for (int i = 0; i < bulk_remaining; i++) {
                if (partial & (1 << i)) {
                    (*tdo_data)[byte_offset] |= (1 << i);
                }
            }
        }

        // Last bit from TMS read (bit 7 of the response byte)
        if (raw_idx < raw.size()) {
            bool last_tdo = (raw[raw_idx] >> 7) & 1;
            int last_byte = (bit_count - 1) / 8;
            int last_bit = (bit_count - 1) % 8;
            if (last_tdo) {
                (*tdo_data)[last_byte] |= (1 << last_bit);
            }
        }
    } else {
        if (!device_.write(cmd)) {
            last_error_ = device_.lastError();
            return false;
        }
    }

    return true;
}

} // namespace jtag

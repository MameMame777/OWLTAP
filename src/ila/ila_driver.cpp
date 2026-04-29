// SPDX-License-Identifier: Apache-2.0
#include "src/ila/ila_driver.h"

namespace jtag::ila {

namespace {

void packBits(uint64_t val, int bits, std::vector<uint8_t>& out) {
    out.assign((bits + 7) / 8, 0);
    for (int i = 0; i < bits; i++) {
        if ((val >> i) & 1ULL) out[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    }
}

uint32_t unpackBits(const std::vector<uint8_t>& buf, int bits) {
    uint32_t v = 0;
    for (int i = 0; i < bits; i++) {
        if ((buf[i / 8] >> (i % 8)) & 1u) v |= (1u << i);
    }
    return v;
}

// CTRL DR bit layout: {FORCE_TRIG, RESET, STOP, ARM}
constexpr uint32_t kCtrlArm   = 0x1;
constexpr uint32_t kCtrlStop  = 0x2;
constexpr uint32_t kCtrlReset = 0x4;
constexpr uint32_t kCtrlForce = 0x8;

} // namespace

IlaDriver::IlaDriver(IlaTapBackend& backend) : backend_(backend) {}

bool IlaDriver::shiftDrInt(int dr_bits, uint32_t tdi_val, uint32_t& tdo_val) {
    std::vector<uint8_t> tdi_buf, tdo_buf;
    packBits(tdi_val, dr_bits, tdi_buf);
    if (!backend_.shiftDr(dr_bits, tdi_buf.data(), tdo_buf)) {
        last_error_ = backend_.lastError();
        return false;
    }
    tdo_val = unpackBits(tdo_buf, dr_bits);
    return true;
}

bool IlaDriver::probe(IlaCaps& out) {
    // BSCANE2 warm-up: the first DR scan through the BSCANE2 USER1 chain
    // after JTAG reset returns all-zeros regardless of the captured register.
    // Reading IDCODE (sub-opcode 0x01) serves as a reliable single-scan
    // warm-up because stored_ir resets to 0x01, so no prime scan is needed.
    // This makes the subsequent CONFIG scan (2 scans: prime + real) reliable.
    uint32_t _warmup_idcode = 0;
    (void)readIdcode(_warmup_idcode);  // warm-up scan; result may be unreliable

    if (!backend_.selectIr(kIrConfig)) {
        last_error_ = backend_.lastError();
        return false;
    }
    uint32_t raw = 0;
    if (!shiftDrInt(32, 0, raw)) return false;

    IlaCaps c{};
    c.raw       = raw;
    c.version   = static_cast<uint8_t>((raw >> 24) & 0xFF);
    c.num_ch    = static_cast<uint8_t>((raw >> 20) & 0x0F);
    c.sig_count = static_cast<uint8_t>((raw >> 16) & 0x0F);
    c.data_w    = static_cast<uint8_t>(((raw >> 10) & 0x3F) + 1);
    c.addr_w    = static_cast<uint8_t>(raw & 0xFF);
    c.depth   = (c.addr_w == 0 || c.addr_w > 20)
                    ? static_cast<uint32_t>(kDepth)
                    : (1u << c.addr_w);
    c.probed  = (c.version != 0);

    if (!c.probed) {
        // Unsupported / absent CONFIG register — keep compile-time defaults.
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "ILA CONFIG not supported (raw=0x%08X)", raw);
        last_error_ = buf;
        out = caps_;  // unchanged
        return false;
    }

    // Read IDCODE after the hardware is fully warmed up (reliable at this point).
    uint32_t idcode = 0;
    (void)readIdcode(idcode);
    c.idcode = idcode;

    caps_ = c;
    out   = c;
    return true;
}

bool IlaDriver::readIdcode(uint32_t& idcode) {
    if (!backend_.selectIr(kIrIdcode)) { last_error_ = backend_.lastError(); return false; }
    return shiftDrInt(32, 0, idcode);
}

bool IlaDriver::readSignalDefs(std::vector<IlaSignalEntry>& out) {
    if (caps_.sig_count == 0) {
        last_error_ = "sig_count is 0; no SIG_DEF entries available";
        return false;
    }
    if (!backend_.selectIr(kIrSigDef)) {
        last_error_ = backend_.lastError();
        return false;
    }
    out.clear();
    out.reserve(caps_.sig_count);
    for (uint8_t i = 0; i < caps_.sig_count; ++i) {
        uint32_t word = 0;
        if (!shiftDrInt(32, 0, word)) return false;
        IlaSignalEntry e;
        e.fmt      = static_cast<uint8_t>((word >> 28) & 0x0F);
        e.hi       = static_cast<uint8_t>((word >> 16) & 0xFF);
        e.lo       = static_cast<uint8_t>((word >>  8) & 0xFF);
        e.name_idx = static_cast<uint8_t>( word        & 0xFF);
        out.push_back(e);
    }
    return true;
}

bool IlaDriver::configureTrigger(uint32_t mask, uint32_t value,
                                   uint32_t rise_mask, uint32_t fall_mask,
                                   uint32_t mask2, uint32_t val2, bool or_mode,
                                   uint16_t pre_samples) {
    if (static_cast<uint32_t>(pre_samples) >= depth()) {
        last_error_ = "pre_samples must be < DEPTH";
        return false;
    }
    // Legacy bitstream: edge registers not present.
    if (caps_.version < 2 && (rise_mask != 0 || fall_mask != 0)) {
        last_error_ = "edge trigger requires ILA IP version >= 2";
        return false;
    }
    // OR-mode registers not present before version 3.
    if (caps_.version < 3 && (mask2 != 0 || val2 != 0 || or_mode)) {
        last_error_ = "OR-mode trigger requires ILA IP version >= 3";
        return false;
    }
    uint32_t dummy;
    if (!backend_.selectIr(kIrTrigMask))   { last_error_ = backend_.lastError(); return false; }
    if (!shiftDrInt(dataWidth(), mask,  dummy))  return false;
    if (!backend_.selectIr(kIrTrigVal))    { last_error_ = backend_.lastError(); return false; }
    if (!shiftDrInt(dataWidth(), value, dummy))  return false;
    if (!backend_.selectIr(kIrPreSamples)) { last_error_ = backend_.lastError(); return false; }
    if (!shiftDrInt(addrWidth(), pre_samples, dummy)) return false;
    if (caps_.version >= 2) {
        if (!backend_.selectIr(kIrTrigRise)) { last_error_ = backend_.lastError(); return false; }
        if (!shiftDrInt(dataWidth(), rise_mask, dummy)) return false;
        if (!backend_.selectIr(kIrTrigFall)) { last_error_ = backend_.lastError(); return false; }
        if (!shiftDrInt(dataWidth(), fall_mask, dummy)) return false;
    }
    if (caps_.version >= 3) {
        if (!backend_.selectIr(kIrTrigMask2)) { last_error_ = backend_.lastError(); return false; }
        if (!shiftDrInt(dataWidth(), mask2, dummy)) return false;
        if (!backend_.selectIr(kIrTrigVal2))  { last_error_ = backend_.lastError(); return false; }
        if (!shiftDrInt(dataWidth(), val2,  dummy)) return false;
        if (!backend_.selectIr(kIrTrigCtrl)) { last_error_ = backend_.lastError(); return false; }
        if (!shiftDrInt(dataWidth(), or_mode ? 1u : 0u, dummy)) return false;
    }
    return true;
}

bool IlaDriver::configureTrigger(uint32_t mask, uint32_t value,
                                   uint32_t rise_mask, uint32_t fall_mask,
                                   uint16_t pre_samples) {
    return configureTrigger(mask, value, rise_mask, fall_mask,
                            0u, 0u, false, pre_samples);
}

bool IlaDriver::configureTrigger(uint32_t mask, uint32_t value,
                                   uint16_t pre_samples) {
    return configureTrigger(mask, value, 0u, 0u, 0u, 0u, false, pre_samples);
}

bool IlaDriver::arm() {
    uint32_t d;
    if (!backend_.selectIr(kIrCtrl)) { last_error_ = backend_.lastError(); return false; }
    return shiftDrInt(4, kCtrlArm, d);
}

bool IlaDriver::stop() {
    uint32_t d;
    if (!backend_.selectIr(kIrCtrl)) { last_error_ = backend_.lastError(); return false; }
    return shiftDrInt(4, kCtrlStop, d);
}

bool IlaDriver::resetCapture() {
    uint32_t d;
    if (!backend_.selectIr(kIrCtrl)) { last_error_ = backend_.lastError(); return false; }
    return shiftDrInt(4, kCtrlReset, d);
}

bool IlaDriver::forceTrigger() {
    uint32_t d;
    if (!backend_.selectIr(kIrCtrl)) { last_error_ = backend_.lastError(); return false; }
    return shiftDrInt(4, kCtrlForce, d);
}

bool IlaDriver::readStatus(IlaStatus& out) {
    if (!backend_.selectIr(kIrStatus)) { last_error_ = backend_.lastError(); return false; }
    uint32_t v;
    if (!shiftDrInt(8, 0, v)) return false;
    out.armed     = (v & 0x1) != 0;
    out.triggered = (v & 0x2) != 0;
    out.full      = (v & 0x4) != 0;
    return true;
}

bool IlaDriver::setReadAddr(uint16_t addr) {
    if (addr >= depth()) {
        last_error_ = "addr >= DEPTH";
        return false;
    }
    if (!backend_.selectIr(kIrReadAddr)) { last_error_ = backend_.lastError(); return false; }
    uint32_t d;
    return shiftDrInt(addrWidth(), addr, d);
}

bool IlaDriver::readSamples(std::vector<uint32_t>& out) {
    return readSamples(out, static_cast<uint32_t>(depth()));
}

bool IlaDriver::readSamples(std::vector<uint32_t>& out, uint32_t count) {
    if (!backend_.selectIr(kIrReadData)) { last_error_ = backend_.lastError(); return false; }
    // Use batch path: eliminates per-sample USB round-trips.
    // BscaneIlaTapBackend overrides shiftDrBatch to use TapController::shiftDRRepeat,
    // sending all scans in chunks of 512 per USB transfer (~3ms/chunk vs ~1ms/sample).
    static const uint8_t zeros[4] = {};
    std::vector<uint8_t> flat_tdo;
    if (!backend_.shiftDrBatch(static_cast<int>(count), dataWidth(), zeros, flat_tdo)) {
        last_error_ = backend_.lastError();
        return false;
    }
    const int bytes_per = (dataWidth() + 7) / 8;
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::vector<uint8_t> slice(flat_tdo.begin() + i * bytes_per,
                                    flat_tdo.begin() + i * bytes_per + bytes_per);
        out.push_back(unpackBits(slice, dataWidth()));
    }
    return true;
}

} // namespace jtag::ila

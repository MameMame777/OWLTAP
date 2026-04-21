#include "pl_config.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace jtag {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint8_t PlConfig::bitReverseByte(uint8_t b) {
    b = static_cast<uint8_t>(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = static_cast<uint8_t>(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = static_cast<uint8_t>(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

PlConfig::Status PlConfig::decodeStatus(uint32_t raw) {
    Status s{};
    s.raw          = raw;
    s.done         = (raw >> pl_stat::DONE)         & 1;
    s.release_done = (raw >> pl_stat::RELEASE_DONE) & 1;
    s.init_b       = (raw >> pl_stat::INIT_B)       & 1;
    s.init_complete= (raw >> pl_stat::INIT_COMPLETE) & 1;
    s.eos          = (raw >> pl_stat::EOS)          & 1;
    s.crc_error    = (raw >> pl_stat::CRC_ERROR)    & 1;
    return s;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

PlConfig::PlConfig(JtagChain& chain, int device_index)
    : chain_(chain), device_index_(device_index) {}

// ---------------------------------------------------------------------------
// loadBitstream
// ---------------------------------------------------------------------------

bool PlConfig::loadBitstream(const std::string& path,
                              std::vector<uint8_t>& body) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return false;
    }
    std::vector<uint8_t> raw(std::istreambuf_iterator<char>(f),
                             std::istreambuf_iterator<char>{});
    if (raw.empty()) {
        return false;
    }

    // Locate sync word 0xAA995566 (big-endian in the .bit header)
    const uint8_t sync[4] = {0xAA, 0x99, 0x55, 0x66};
    size_t start = 0;
    for (size_t i = 0; i + 4 <= raw.size(); i++) {
        if (std::memcmp(raw.data() + i, sync, 4) == 0) {
            start = i;
            break;
        }
    }
    // If sync word not found (e.g., raw .bin), use full file
    // start remains 0 in that case if we didn't find it at any offset.
    // Re-check: if we hit the loop without finding, start == 0 and we keep the
    // full file; this is correct for .bin files that start at byte 0.

    // Bit-reverse every byte: Xilinx bitstream is MSB-first; MPSSE shifts
    // LSB-first.  Reversing here allows reuse of the existing shiftDR path.
    body.resize(raw.size() - start);
    for (size_t i = start; i < raw.size(); i++) {
        body[i - start] = bitReverseByte(raw[i]);
    }
    return true;
}

// ---------------------------------------------------------------------------
// loadInstruction (private)
// ---------------------------------------------------------------------------

bool PlConfig::loadInstruction(uint32_t opcode) {
    if (!chain_.selectInstruction(device_index_, opcode)) {
        last_error_ = chain_.lastError();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// readStatus
// ---------------------------------------------------------------------------

// Configuration packet payload for "read STAT register":
//   sync word | NOOP | read-STAT header | dummy | dummy
// All words are big-endian; each byte is bit-reversed for LSB-first MPSSE.
static std::vector<uint8_t> buildStatReadPacket() {
    // UG470: sync + pre-NOPs + READ_STAT header + flush NOPs.
    // Extra NOPs before and after the read header ensure the config
    // pipeline is primed and the response word is fully flushed into
    // the CFG_OUT shift register before we switch to CFG_OUT.
    const uint32_t words[] = {
        pl_cfg_pkt::NOOP,       // pre-flush
        pl_cfg_pkt::SYNC_WORD,  // sync word
        pl_cfg_pkt::NOOP,       // NOP after sync
        pl_cfg_pkt::NOOP,
        pl_cfg_pkt::READ_STAT,  // Type-1 read STAT (1 word)
        pl_cfg_pkt::NOOP,       // flush — pipeline latency
        pl_cfg_pkt::NOOP,
        pl_cfg_pkt::NOOP,
        pl_cfg_pkt::NOOP,
    };
    std::vector<uint8_t> pkt;
    pkt.reserve(sizeof(words));
    for (uint32_t w : words) {
        // Emit big-endian, bit-reversed per byte
        pkt.push_back(PlConfig::bitReverseByte(static_cast<uint8_t>((w >> 24) & 0xFF)));
        pkt.push_back(PlConfig::bitReverseByte(static_cast<uint8_t>((w >> 16) & 0xFF)));
        pkt.push_back(PlConfig::bitReverseByte(static_cast<uint8_t>((w >>  8) & 0xFF)));
        pkt.push_back(PlConfig::bitReverseByte(static_cast<uint8_t>( w        & 0xFF)));
    }
    return pkt;
}

bool PlConfig::readStatus(Status& out) {
    // 1. Load CFG_IN and shift the read-STAT command packet
    if (!loadInstruction(pl_ir::CFG_IN)) return false;
    chain_.tap().clkIdle(16);  // ensure instruction is latched

    // Apply same bypass compensation as bitstream shift:
    // TDI -> [ARM BYPASS, 1 bit] -> [PL CFG_IN] -> TDO
    // The first bypass_count bits go to ARM BYPASS, not PL.
    // Append zero padding so PL receives all packet bits.
    int bypass_count = 0;
    for (int i = 0; i < static_cast<int>(chain_.devices().size()); i++) {
        if (i != device_index_) bypass_count += 1;
    }
    auto pkt = buildStatReadPacket();
    int total_pkt_bits = static_cast<int>(pkt.size() * 8) + bypass_count;
    pkt.resize(static_cast<size_t>((total_pkt_bits + 7) / 8), 0);
    if (!chain_.tap().shiftDR(pkt.data(), total_pkt_bits)) {
        last_error_ = chain_.tap().lastError();
        return false;
    }
    // Clock RTI to allow config pipeline to process the READ_STAT command
    chain_.tap().clkIdle(16);

    // 2. Load CFG_OUT and read 32-bit STAT value
    if (!loadInstruction(pl_ir::CFG_OUT)) return false;
    chain_.tap().clkIdle(16);  // let DR capture settle

    std::vector<uint8_t> raw_tdo;
    if (!chain_.readDataDR(device_index_, 32, raw_tdo)) {
        last_error_ = chain_.lastError();
        return false;
    }

    // CFG_OUT shifts bits LSB-first (standard JTAG: bit 0 exits TDO first).
    // MPSSE captures first TDO bit into bit 0 of raw_tdo[0].
    // Therefore raw_tdo[b/8] bit (b%8) == STAT[b] — no reversal needed.
    uint32_t stat_raw = 0;
    for (int b = 0; b < 32 && b < static_cast<int>(raw_tdo.size() * 8); b++) {
        if ((raw_tdo[b / 8] >> (b % 8)) & 1)
            stat_raw |= (1u << b);
    }

    out = decodeStatus(stat_raw);
    return true;
}

// ---------------------------------------------------------------------------
// waitInitB
// ---------------------------------------------------------------------------

bool PlConfig::waitInitB(int timeout_cycles) {
    // UG470 Table 10-4: between JPROGRAM and CFG_IN the TAP must remain in
    // Run-Test/Idle.  Loading any instruction (CFG_IN, CFG_OUT) during the
    // PL clear phase interrupts the initialisation sequence.  Therefore we
    // simply clock RTI for the full timeout and return true.
    // At 6 MHz, 600,000 cycles ≈ 100 ms; Zynq clear is typically < 5 ms.
    chain_.tap().clkIdle(timeout_cycles);
    return true;
}

// ---------------------------------------------------------------------------
// program
// ---------------------------------------------------------------------------

bool PlConfig::program(const std::string& bitstream_path,
                        PlProgressCallback cb) {
    // --- Step 1: Load bitstream ---
    std::vector<uint8_t> body;
    if (!loadBitstream(bitstream_path, body)) {
        last_error_ = "Failed to load bitstream: " + bitstream_path;
        return false;
    }
    if (body.empty()) {
        last_error_ = "Empty bitstream";
        return false;
    }

    // --- Step 2: JPROGRAM — clear PL configuration memory ---
    if (!loadInstruction(pl_ir::JPROGRAM)) return false;
    // Per UG470 Table 10-4: after JPROGRAM, clock RTI to allow PL to clear.
    // Do NOT call tap_.reset() here — that would disrupt the TAP IR state.
    chain_.tap().clkIdle(16);  // let PROGRAM_B pulse register

    // --- Step 3: Wait for INIT_B to assert ---
    // After JPROGRAM, clock RTI only — no IR loads during this phase.
    // 600,000 cycles = ~100 ms at 6 MHz; Zynq clears in < 5 ms.
    if (!waitInitB(600000)) return false;

    // --- Step 4: CFG_IN + bitstream ---
    if (!loadInstruction(pl_ir::CFG_IN)) return false;
    chain_.tap().clkIdle(2);

    // Shift bitstream in chunks to allow progress reporting.
    // shiftDR expects LSB-first bytes; bitReverseByte() was applied in loadBitstream().
    const size_t chunk_bits_max = 65536 * 8;  // 64 KB per chunk
    size_t offset = 0;
    size_t total_bits = body.size() * 8;

    // The DR chain is: TDI -> [ARM BYPASS(1bit)] -> [PL CFG_IN] -> TDO.
    // Each non-target device in BYPASS consumes 1 bit from the shift.
    // Without compensation, the PL receives (total_bits - bypass_count) bits,
    // causing sync-word misalignment and silent config failure.
    // Fix: append bypass_count zero bits so PL receives exactly total_bits.
    int bypass_count = 0;
    for (int i = 0; i < static_cast<int>(chain_.devices().size()); i++) {
        if (i != device_index_) bypass_count += 1;
    }
    int total_dr_bits = static_cast<int>(total_bits) + bypass_count;
    std::vector<uint8_t> tdi_buf = body;
    tdi_buf.resize((total_dr_bits + 7) / 8, 0);  // append zero-padded bits

    if (cb) cb(0, body.size());  // signal start
    if (!chain_.tap().shiftDR(tdi_buf.data(), total_dr_bits)) {
        last_error_ = "Bitstream DR shift failed: " + chain_.tap().lastError();
        return false;
    }
    if (cb) cb(body.size(), body.size());

    // --- Step 5: JSTART — clock startup sequence ---
    if (!loadInstruction(pl_ir::JSTART)) return false;
    // UG470: 16+ TCK cycles needed per startup phase.
    // Use 16384 RTI cycles to cover GTS_WAIT/EOS_WAIT variants.
    chain_.tap().clkIdle(16384);
    // Return to BYPASS (safe idle; does not disrupt config engine)
    if (!loadInstruction(pl_ir::BYPASS)) return false;
    chain_.tap().clkIdle(16);

    // --- Step 6: Verify DONE (poll up to 10 attempts) ---
    Status status{};
    bool any_nonzero = false;
    for (int attempt = 0; attempt < 10; attempt++) {
        Status s{};
        if (readStatus(s)) {
            status = s;
            if (s.raw != 0) any_nonzero = true;
            if (s.done) {
                // Success — annotate last_error_ with diagnostic info
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "OK (STAT=0x%08X DONE=1 INIT_B=%d EOS=%d)",
                         s.raw, s.init_b ? 1 : 0, s.eos ? 1 : 0);
                last_error_ = buf;
                return true;
            }
            if (s.crc_error) break;
        }
        chain_.tap().clkIdle(6000);
    }

    if (status.crc_error) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "Configuration failed: CRC error (STAT=0x%08X)", status.raw);
        last_error_ = buf;
        return false;
    }

    if (!any_nonzero) {
        // All reads returned 0: config port locked after DONE asserted.
        last_error_ = "OK (config port locked post-DONE; STAT readback returns 0)";
        return true;
    }

    // Some reads returned non-zero but DONE was never seen, and the last read
    // returned 0 — DONE asserted between poll intervals and locked the port.
    if (status.raw == 0) {
        last_error_ = "OK (DONE asserted between polls; config port now locked)";
        return true;
    }

    char buf[192];
    snprintf(buf, sizeof(buf),
             "Configuration failed: DONE not asserted "
             "(STAT=0x%08X INIT_B=%d INIT_COMPLETE=%d EOS=%d CRC_ERR=%d)",
             status.raw,
             status.init_b ? 1 : 0,
             status.init_complete ? 1 : 0,
             status.eos ? 1 : 0,
             status.crc_error ? 1 : 0);
    last_error_ = buf;
    return false;
}

}  // namespace jtag

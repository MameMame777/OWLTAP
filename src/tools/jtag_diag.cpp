/// jtag_diag - CLI JTAG diagnostic tool (no GUI required)
///
/// Usage: jtag_diag [options]
///   --bsdl <path>    BSDL file path (default: from cfg.json)
///   --dev <N>        Device index in chain (default: from cfg.json)
///   --sample <N>     Number of SAMPLE reads (default: 1)
///   --pin <name>     Pin name to watch (can be specified multiple times)
///   --freq <hz>      JTAG clock frequency override
///   --verbose, -v    Show all decoded pin states
///   --help, -h       Show this help

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "src/boundary_scan/scanner.h"
#include "src/ftdi/ftdi_device.h"
#include "src/gui/app_config.h"
#include "src/jtag/jtag_chain.h"
#include "src/jtag/tap_controller.h"

int main(int argc, char* argv[]) {
    // Load defaults from cfg.json in CWD
    auto cfg = jtag::gui::AppConfig::load("cfg.json");

    std::string bsdl_path = cfg.bsdl_path;
    int dev_idx           = cfg.bsdl_device_index;
    int sample_count      = 1;
    bool verbose          = false;
    std::vector<std::string> watch_pins;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bsdl") == 0 && i + 1 < argc) {
            bsdl_path = argv[++i];
        } else if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc) {
            dev_idx = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sample") == 0 && i + 1 < argc) {
            sample_count = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
            watch_pins.push_back(argv[++i]);
        } else if (strcmp(argv[i], "--freq") == 0 && i + 1 < argc) {
            cfg.clock_freq_hz = static_cast<uint32_t>(atol(argv[++i]));
        } else if (strcmp(argv[i], "--verbose") == 0 ||
                   strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            printf("Usage: jtag_diag [options]\n"
                   "  --bsdl <path>    BSDL file (default: from cfg.json)\n"
                   "  --dev <N>        Device index in chain (default: %d)\n"
                   "  --sample <N>     Number of SAMPLE reads (default: 1)\n"
                   "  --pin <name>     Pin to watch (repeatable)\n"
                   "  --freq <hz>      JTAG clock Hz override\n"
                   "  --verbose, -v    Show all decoded pin states\n",
                   dev_idx);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    // ── Print config ──────────────────────────────────────────────────────────
    printf("[cfg] VID=0x%04X PID=0x%04X serial='%s' ch=%d freq=%u Hz\n",
           cfg.vendor_id, cfg.product_id, cfg.serial.c_str(),
           cfg.interface_channel, cfg.clock_freq_hz);
    printf("[cfg] bsdl='%s' dev_idx=%d\n", bsdl_path.c_str(), dev_idx);

    // ── Open FTDI ─────────────────────────────────────────────────────────────
    jtag::FtdiDevice ftdi;
    if (!ftdi.open(cfg.vendor_id, cfg.product_id, cfg.serial,
                   static_cast<jtag::FtdiInterface>(cfg.interface_channel))) {
        fprintf(stderr, "[ERROR] FTDI open: %s\n", ftdi.lastError().c_str());
        return 1;
    }
    printf("[OK] FTDI opened\n");

    if (!ftdi.initMpsse(cfg.clock_freq_hz)) {
        fprintf(stderr, "[ERROR] MPSSE init: %s\n", ftdi.lastError().c_str());
        ftdi.close();
        return 1;
    }
    printf("[OK] MPSSE initialized at %u Hz\n", cfg.clock_freq_hz);

    // ── Detect chain ─────────────────────────────────────────────────────────
    jtag::TapController tap(ftdi);
    jtag::JtagChain chain(tap);

    int count = chain.detectDevices();
    // last_error_ contains "Raw TDO: XX XX ..." diagnostic even on success
    printf("[chain] detectDevices: %d device(s)  [%s]\n",
           count, chain.lastError().c_str());

    if (count <= 0) {
        fprintf(stderr, "[ERROR] No JTAG devices found\n");
        ftdi.close();
        return 1;
    }

    for (const auto& dev : chain.devices()) {
        printf("  device[%d]: IDCODE=0x%08X ir_length=%d\n",
               dev.position, dev.idcode, dev.ir_length);
    }

    // ── Raw DR test #1: re-read IDCODE without any IR shift (after reset)
    // detectDevices already did a TAP reset, so IDCODE is still the default DR.
    {
        std::vector<uint8_t> raw_dr;
        if (tap.readDR(raw_dr, 33)) {
            uint32_t idcode2 = 0;
            for (int b = 0; b < 32; b++) {
                if ((raw_dr[b / 8] >> (b % 8)) & 1) idcode2 |= (1u << b);
            }
            printf("[raw_dr1] readDR(33) after detectDevices (no IR shift): "
                   "IDCODE=0x%08X  raw:", idcode2);
            for (size_t i = 0; i < raw_dr.size(); i++)
                printf(" %02X", raw_dr[i]);
            printf("\n");
        } else {
            printf("[raw_dr1] FAILED: %s\n", tap.lastError().c_str());
        }
    }

    // ── Raw IR test: manually shift in IDCODE instruction and read DR
    {
        // After raw_dr1, TAP is in RTI. Now TAP-reset to get IDCODE back.
        tap.reset();
        tap.clkIdle(5);

        // Build 10-bit IR: PL_TAP(6) at low bits, ARM_DAP(4) at high bits.
        // Bits shifted in first (low positions) travel through ARM DAP
        // and end up in PL TAP (TDO-side). Last bits stay in ARM DAP.
        // PL TAP IDCODE=0x09 → bits[0:5], ARM DAP BYPASS=0xF → bits[6:9]
        // byte0: bits0-5=001001(0x09), bits6-7=11 → 11_001001 = 0xC9
        // byte1: bits8-9=11                       → 00000011  = 0x03
        uint8_t ir_data[2] = {0xC9, 0x03};  // bit0..7=0x9F, bit8=0, bit9=0
        if (tap.shiftIR(ir_data, 10)) {
            tap.clkIdle(2);
            std::vector<uint8_t> raw_dr;
            if (tap.readDR(raw_dr, 33)) {
                uint32_t idcode3 = 0;
                for (int b = 0; b < 32; b++) {
                    if ((raw_dr[b / 8] >> (b % 8)) & 1) idcode3 |= (1u << b);
                }
                printf("[raw_dr2] readDR(33) after manual IR=IDCODE: "
                       "IDCODE=0x%08X  raw:", idcode3);
                for (size_t i = 0; i < raw_dr.size(); i++)
                    printf(" %02X", raw_dr[i]);
                printf("\n");
            } else {
                printf("[raw_dr2] readDR FAILED: %s\n", tap.lastError().c_str());
            }
        } else {
            printf("[raw_ir] shiftIR FAILED: %s\n", tap.lastError().c_str());
        }
    }

    // ── Raw DR test #3: re-detect to restore state ────────────────────────
    chain.detectDevices();


    if (bsdl_path.empty()) {
        fprintf(stderr,
                "[ERROR] No BSDL path. Use --bsdl <path> or set bsdl_path in "
                "cfg.json\n");
        ftdi.close();
        return 1;
    }
    if (dev_idx < 0 || dev_idx >= count) {
        fprintf(stderr, "[ERROR] dev_idx %d out of range (chain has %d device(s))\n",
                dev_idx, count);
        ftdi.close();
        return 1;
    }

    if (!chain.loadBsdl(dev_idx, bsdl_path)) {
        fprintf(stderr, "[ERROR] loadBsdl: %s\n", chain.lastError().c_str());
        ftdi.close();
        return 1;
    }

    const auto& bdev = chain.devices()[dev_idx];
    {
        int total_ir = 0;
        for (const auto& d : chain.devices()) total_ir += d.ir_length;
        auto idcode_op = bdev.bsdl->idcodeOpcode();
        auto sample_op = bdev.bsdl->sampleOpcode();
        printf("[bsdl] boundary_length=%d  pins=%zu  ir_length=%d\n",
               bdev.bsdl->boundary_length,
               bdev.bsdl->boundary_cells.size(),
               bdev.bsdl->instruction_length);
        printf("[bsdl] SAMPLE_op=0x%02X  IDCODE_op=0x%02X  total_IR=%d  ir[0]=%d ir[1]=%d\n",
               (int)sample_op.value_or(0xFF),
               (int)idcode_op.value_or(0xFF),
               total_ir,
               count > 0 ? chain.devices()[0].ir_length : -1,
               count > 1 ? chain.devices()[1].ir_length : -1);
    }

    // ── Create scanner ────────────────────────────────────────────────────────
    jtag::Scanner scanner(chain, dev_idx);

    // Read IDCODE via scanner (IDCODE instruction + 32-bit DR)
    uint32_t idcode = 0;
    if (scanner.readIdCode(idcode)) {
        printf("[idcode] IDCODE=0x%08X\n", idcode);
    } else {
        printf("[idcode] readIdCode failed: %s\n", scanner.lastError().c_str());
    }

    // ── SAMPLE loop ───────────────────────────────────────────────────────────
    for (int s = 0; s < sample_count; s++) {
        auto result = scanner.sample();

        if (result.raw_bsr.empty()) {
            fprintf(stderr, "[sample %d] FAILED: %s\n", s,
                    scanner.lastError().c_str());
            continue;
        }

        int nz = 0;
        for (auto b : result.raw_bsr) if (b) nz++;

        printf("[sample %d] BSR: %d/%zu non-zero bytes  first16:",
               s, nz, result.raw_bsr.size());
        for (size_t i = 0; i < 16 && i < result.raw_bsr.size(); i++)
            printf(" %02X", result.raw_bsr[i]);
        printf("\n");

        // Watch specific pins
        for (const auto& pin : watch_pins) {
            auto state = result.getPin(pin);
            printf("  %-20s = %s\n", pin.c_str(),
                   state == jtag::PinState::HIGH    ? "HIGH"
                   : state == jtag::PinState::LOW   ? "LOW"
                                                    : "UNKNOWN");
        }

        // Verbose: print all non-UNKNOWN pins
        if (verbose && watch_pins.empty()) {
            for (const auto& [name, state] : result.pin_states) {
                printf("  %-20s = %s\n", name.c_str(),
                       state == jtag::PinState::HIGH ? "HIGH" : "LOW");
            }
        }
    }

    printf("[done]\n");
    ftdi.close();
    return 0;
}

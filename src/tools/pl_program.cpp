/// pl_program - CLI tool for programming Zynq/7-series PL via JTAG
///
/// Usage: pl_program [options] --bit <file.bit>
///   --bit  <path>   Bitstream file (.bit or .bin) — required
///   --dev  <N>      PL TAP device index in chain (default: from cfg.json)
///   --freq <hz>     JTAG clock frequency override
///   --verbose, -v   Show STAT register fields after programming
///   --help, -h      Show this help
///
/// Reference: Xilinx UG470, Table 10-4 (JTAG configuration sequence)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "src/config/pl_config.h"
#include "src/ftdi/ftdi_device.h"
#include "src/gui/app_config.h"
#include "src/jtag/jtag_chain.h"
#include "src/jtag/tap_controller.h"

int main(int argc, char* argv[]) {
    auto cfg = jtag::gui::AppConfig::load("cfg.json");

    std::string bit_path;
    int dev_idx  = cfg.bsdl_device_index;
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bit") == 0 && i + 1 < argc) {
            bit_path = argv[++i];
        } else if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc) {
            dev_idx = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--freq") == 0 && i + 1 < argc) {
            cfg.clock_freq_hz = static_cast<uint32_t>(atol(argv[++i]));
        } else if (strcmp(argv[i], "--verbose") == 0 ||
                   strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            printf("Usage: pl_program [options] --bit <file.bit>\n"
                   "  --bit  <path>   Bitstream file (.bit or .bin)\n"
                   "  --dev  <N>      PL TAP device index (default: %d)\n"
                   "  --freq <hz>     JTAG clock frequency\n"
                   "  --verbose, -v   Show STAT register fields\n",
                   dev_idx);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (bit_path.empty()) {
        fprintf(stderr, "Error: --bit <file> is required\n");
        return 1;
    }

    printf("[cfg] VID=0x%04X PID=0x%04X serial='%s' ch=%d freq=%u Hz\n",
           cfg.vendor_id, cfg.product_id, cfg.serial.c_str(),
           cfg.interface_channel, cfg.clock_freq_hz);
    printf("[cfg] bitstream='%s'  pl_dev=%d\n", bit_path.c_str(), dev_idx);

    // Open FTDI
    jtag::FtdiDevice ftdi;
    if (!ftdi.open(cfg.vendor_id, cfg.product_id, cfg.serial,
                   static_cast<jtag::FtdiInterface>(cfg.interface_channel))) {
        fprintf(stderr, "[ERROR] FTDI open: %s\n", ftdi.lastError().c_str());
        return 1;
    }
    printf("[OK] FTDI opened\n");

    if (!ftdi.initMpsse(cfg.clock_freq_hz)) {
        fprintf(stderr, "[ERROR] MPSSE init: %s\n", ftdi.lastError().c_str());
        return 1;
    }
    printf("[OK] MPSSE initialised  freq=%u Hz\n", cfg.clock_freq_hz);

    // Detect chain
    jtag::TapController tap(ftdi);
    jtag::JtagChain chain(tap);
    int ndev = chain.detectDevices();
    if (ndev == 0) {
        fprintf(stderr, "[ERROR] No JTAG devices found: %s\n",
                chain.lastError().c_str());
        return 1;
    }
    printf("[OK] %d device(s) detected:\n", ndev);
    for (const auto& d : chain.devices()) {
        printf("       [%d] IDCODE=0x%08X  IR=%d bit\n",
               d.position, d.idcode, d.ir_length);
    }

    if (dev_idx < 0 || dev_idx >= ndev) {
        fprintf(stderr, "[ERROR] Device index %d out of range (0-%d)\n",
                dev_idx, ndev - 1);
        return 1;
    }

    // Program PL
    jtag::PlConfig pl(chain, dev_idx);

    printf("[..] Programming PL from '%s'...\n", bit_path.c_str());
    bool ok = pl.program(bit_path, [](size_t sent, size_t total) {
        static size_t last_pct = 0;
        size_t pct = total ? (sent * 100 / total) : 100;
        if (pct / 10 != last_pct / 10) {
            printf("     %3zu%%\n", pct);
            last_pct = pct;
        }
    });

    if (!ok) {
        fprintf(stderr, "[ERROR] Programming failed: %s\n",
                pl.lastError().c_str());
    } else {
        printf("[OK] PL configured successfully\n");
    }

    if (verbose) {
        jtag::PlConfig::Status s{};
        if (pl.readStatus(s)) {
            printf("[STAT] raw=0x%08X\n", s.raw);
            printf("       DONE=%d  RELEASE_DONE=%d  INIT_B=%d\n",
                   s.done, s.release_done, s.init_b);
            printf("       INIT_COMPLETE=%d  EOS=%d  CRC_ERROR=%d\n",
                   s.init_complete, s.eos, s.crc_error);
        } else {
            fprintf(stderr, "[WARN] Could not read STAT: %s\n",
                    pl.lastError().c_str());
        }
    }

    return ok ? 0 : 1;
}

/// flash_program - CLI tool for programming an MT25QL128 SPI config ROM
///                 through a Zynq/7-series PL JTAG BSCAN SPI bridge.
///
/// Usage: flash_program [options] --bin <file.bin> --bridge <bscan_spi.bit>
///   --bin    <path>  Raw .bin or Intel HEX .mcs/.hex image — required
///   --bridge <path>  BSCAN SPI bridge bitstream (quartiq bscan_spi_xc7z020.bit)
///   --dev    <N>     PL TAP device index in chain (default: from cfg.json)
///   --freq   <hz>    JTAG clock frequency override
///   --verbose, -v    Verbose progress output
///   --help, -h       Show this help
///
/// References: Xilinx UG470; Micron MT25QL128ABA datasheet.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "src/flash/flash_programmer.h"
#include "src/ftdi/ftdi_device.h"
#include "src/gui/app_config.h"
#include "src/jtag/jtag_chain.h"
#include "src/jtag/tap_controller.h"

namespace {

const char* phaseName(jtag::flash::FlashPhase p) {
    switch (p) {
        case jtag::flash::FlashPhase::BRIDGE_LOAD: return "BRIDGE";
        case jtag::flash::FlashPhase::ERASE:       return "ERASE";
        case jtag::flash::FlashPhase::PROGRAM:     return "PROGRAM";
        case jtag::flash::FlashPhase::VERIFY:      return "VERIFY";
    }
    return "?";
}

}  // namespace

int main(int argc, char* argv[]) {
    auto cfg = jtag::gui::AppConfig::load("cfg.json");

    std::string bin_path;
    std::string bridge_path;
    int  dev_idx = cfg.bsdl_device_index;
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--bin") == 0 && i + 1 < argc) {
            bin_path = argv[++i];
        } else if (std::strcmp(argv[i], "--bridge") == 0 && i + 1 < argc) {
            bridge_path = argv[++i];
        } else if (std::strcmp(argv[i], "--dev") == 0 && i + 1 < argc) {
            dev_idx = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--freq") == 0 && i + 1 < argc) {
            cfg.clock_freq_hz = static_cast<uint32_t>(std::atol(argv[++i]));
        } else if (std::strcmp(argv[i], "--verbose") == 0 ||
                   std::strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "--help") == 0 ||
                   std::strcmp(argv[i], "-h") == 0) {
            std::printf("Usage: flash_program --bin <file.bin|.mcs> --bridge <bscan.bit>\n"
                        "  --bin    <path>  Raw .bin or Intel HEX .mcs/.hex image\n"
                        "  --bridge <path>  BSCAN SPI bridge bitstream\n"
                        "  --dev    <N>     PL TAP device index (default: %d)\n"
                        "  --freq   <hz>    JTAG clock frequency\n"
                        "  --verbose, -v    Verbose progress\n",
                        dev_idx);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (bin_path.empty() || bridge_path.empty()) {
        std::fprintf(stderr,
                     "Error: --bin <file> and --bridge <file> are required\n");
        return 1;
    }

    std::printf("[cfg] VID=0x%04X PID=0x%04X serial='%s' ch=%d freq=%u Hz\n",
                cfg.vendor_id, cfg.product_id, cfg.serial.c_str(),
                cfg.interface_channel, cfg.clock_freq_hz);
    std::printf("[cfg] bin='%s'  bridge='%s'  pl_dev=%d\n",
                bin_path.c_str(), bridge_path.c_str(), dev_idx);

    jtag::FtdiDevice ftdi;
    if (!ftdi.open(cfg.vendor_id, cfg.product_id, cfg.serial,
                   static_cast<jtag::FtdiInterface>(cfg.interface_channel))) {
        std::fprintf(stderr, "[ERROR] FTDI open: %s\n", ftdi.lastError().c_str());
        return 1;
    }
    if (!ftdi.initMpsse(cfg.clock_freq_hz)) {
        std::fprintf(stderr, "[ERROR] MPSSE init: %s\n", ftdi.lastError().c_str());
        return 1;
    }
    std::printf("[OK] MPSSE initialised  freq=%u Hz\n", cfg.clock_freq_hz);

    jtag::TapController tap(ftdi);
    jtag::JtagChain     chain(tap);
    int ndev = chain.detectDevices();
    if (ndev <= 0) {
        std::fprintf(stderr, "[ERROR] No JTAG devices: %s\n",
                     chain.lastError().c_str());
        return 1;
    }
    std::printf("[OK] %d device(s) detected\n", ndev);
    for (const auto& d : chain.devices()) {
        std::printf("       [%d] IDCODE=0x%08X  IR=%d\n",
                    d.position, d.idcode, d.ir_length);
    }
    if (dev_idx < 0 || dev_idx >= ndev) {
        std::fprintf(stderr, "[ERROR] --dev %d out of range\n", dev_idx);
        return 1;
    }

    jtag::flash::FlashProgrammer programmer(chain, dev_idx, bridge_path);

    bool ok = programmer.program(
        bin_path,
        [verbose](jtag::flash::FlashPhase phase,
                  std::size_t done, std::size_t total) {
            if (phase == jtag::flash::FlashPhase::ERASE) {
                std::printf("[..] %s\n", phaseName(phase));
                return;
            }
            if (total == 0) {
                std::printf("[..] %s\n", phaseName(phase));
                return;
            }
            static int last_pct_progress = -1;
            static int last_pct_verify   = -1;
            int pct = static_cast<int>(done * 100 / total);
            int& last = (phase == jtag::flash::FlashPhase::VERIFY)
                            ? last_pct_verify : last_pct_progress;
            if (verbose) {
                if (pct != last) {
                    std::printf("[..] %s %3d%% (%zu/%zu)\n",
                                phaseName(phase), pct, done, total);
                    last = pct;
                }
            } else if (pct / 10 != last / 10) {
                std::printf("[..] %s %3d%%\n", phaseName(phase), pct);
                last = pct;
            }
        });

    if (!ok) {
        std::fprintf(stderr, "[ERROR] Programming failed: %s\n",
                     programmer.lastError().c_str());
        return 1;
    }
    std::printf("[OK] Flash programmed and verified\n");
    return 0;
}

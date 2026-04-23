#include "flash_programmer.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include "flash_jtag_bridge.h"
#include "mcs_parser.h"
#include "spi_flash.h"
#include "src/config/pl_config.h"

namespace jtag::flash {

namespace {

constexpr auto kBulkEraseTimeout  = std::chrono::milliseconds(400000);
constexpr auto kPageProgramTimeout = std::chrono::milliseconds(500);

bool loadBinFile(const std::string& path, std::vector<uint8_t>& out,
                 std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        err = "Cannot open .bin file: " + path;
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>{});
    if (out.empty()) {
        err = "Empty .bin file: " + path;
        return false;
    }
    return true;
}

// Dispatch to the appropriate loader based on file extension.
bool loadImageFile(const std::string& path, std::vector<uint8_t>& out,
                   std::string& err) {
    const auto ext_pos = path.rfind('.');
    if (ext_pos != std::string::npos) {
        std::string ext = path.substr(ext_pos);
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".mcs" || ext == ".hex")
            return internal::parseMcsToImage(path, out, err);
    }
    return loadBinFile(path, out, err);
}

}  // namespace

FlashProgrammer::FlashProgrammer(JtagChain& chain,
                                   int pl_device_index,
                                   std::string bridge_bit_path)
    : chain_(chain),
      pl_device_index_(pl_device_index),
      bridge_bit_path_(std::move(bridge_bit_path)) {}

bool FlashProgrammer::program(const std::string& image_path,
                                FlashProgressCallback cb) {
    // (a) Load the BSCAN SPI bridge bitstream into the PL.
    if (cb) cb(FlashPhase::BRIDGE_LOAD, 0, 0);
    {
        PlConfig pl(chain_, pl_device_index_);
        if (!pl.program(bridge_bit_path_, nullptr)) {
            last_error_ = "Bridge bitstream load failed: " + pl.lastError();
            return false;
        }
    }

    // (b) Reset the TAP so the bridge starts from a clean state; a short idle
    // settles the startup logic inside the freshly configured PL.
    if (!chain_.tap().reset() || !chain_.tap().clkIdle(16)) {
        last_error_ = "TAP reset after bridge load: " + chain_.tap().lastError();
        return false;
    }

    // (c) Open the SPI bridge + flash driver.
    FlashJtagBridge bridge(chain_, pl_device_index_);
    Mt25qFlash      flash(bridge);

    // (d) Verify flash identity before touching it.
    uint32_t jedec = 0;
    if (!flash.readId(jedec)) {
        last_error_ = "Flash ID read failed: " + flash.lastError();
        return false;
    }
    if (jedec != Mt25qFlash::kJedecMt25ql128) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "Unsupported flash: JEDEC=0x%06X (expected 0x%06X)",
                      jedec, Mt25qFlash::kJedecMt25ql128);
        last_error_ = buf;
        return false;
    }

    // (e) Load the image (.bin raw or .mcs/.hex Intel HEX).
    std::vector<uint8_t> image;
    if (!loadImageFile(image_path, image, last_error_)) return false;
    if (image.size() > Mt25qFlash::kCapacityBytes) {
        last_error_ = "Image exceeds flash capacity";
        return false;
    }
    const std::size_t total = image.size();

    // (f) Bulk erase.
    if (cb) cb(FlashPhase::ERASE, 0, 0);
    if (!flash.bulkErase(kBulkEraseTimeout)) {
        last_error_ = "Bulk erase failed: " + flash.lastError();
        return false;
    }

    // (g) Page-program loop.
    if (cb) cb(FlashPhase::PROGRAM, 0, total);
    for (std::size_t off = 0; off < total; off += Mt25qFlash::kPageSize) {
        const std::size_t n =
            std::min(Mt25qFlash::kPageSize, total - off);
        if (!flash.pageProgram(static_cast<uint32_t>(off),
                                image.data() + off,
                                n,
                                kPageProgramTimeout)) {
            last_error_ = "Page program failed at 0x" +
                          std::to_string(off) + ": " + flash.lastError();
            return false;
        }
        if (cb) cb(FlashPhase::PROGRAM, off + n, total);
    }

    // (h) Verify.
    if (cb) cb(FlashPhase::VERIFY, 0, total);
    // Read back in chunks to keep DR shift size manageable.
    constexpr std::size_t kVerifyChunk = 4096;
    std::vector<uint8_t> readback(kVerifyChunk);
    for (std::size_t off = 0; off < total; off += kVerifyChunk) {
        const std::size_t n = std::min(kVerifyChunk, total - off);
        if (!flash.read(static_cast<uint32_t>(off), readback.data(), n)) {
            last_error_ = "Verify read failed at 0x" +
                          std::to_string(off) + ": " + flash.lastError();
            return false;
        }
        for (std::size_t i = 0; i < n; i++) {
            if (readback[i] != image[off + i]) {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "Verify mismatch at 0x%zX: wrote 0x%02X, read 0x%02X",
                              off + i, image[off + i], readback[i]);
                last_error_ = buf;
                return false;
            }
        }
        if (cb) cb(FlashPhase::VERIFY, off + n, total);
    }
    return true;
}

}  // namespace jtag::flash

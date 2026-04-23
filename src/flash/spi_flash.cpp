#include "spi_flash.h"

#include <algorithm>
#include <cstring>
#include <thread>

namespace jtag::flash {

Mt25qFlash::Mt25qFlash(IFlashBridge& bridge) : bridge_(bridge) {}

bool Mt25qFlash::readId(uint32_t& jedec) {
    const std::vector<uint8_t> tx = {kCmdReadId, 0, 0, 0};
    std::vector<uint8_t> rx;
    if (!bridge_.transfer(tx, rx)) {
        last_error_ = "readId: " + bridge_.lastError();
        return false;
    }
    // rx[0] is the byte received while kCmdReadId was shifting out → discarded.
    jedec = (static_cast<uint32_t>(rx[1]) << 16) |
            (static_cast<uint32_t>(rx[2]) << 8) |
            (static_cast<uint32_t>(rx[3]));
    return true;
}

bool Mt25qFlash::readStatus(uint8_t& sr) {
    const std::vector<uint8_t> tx = {kCmdReadStatus, 0};
    std::vector<uint8_t> rx;
    if (!bridge_.transfer(tx, rx)) {
        last_error_ = "readStatus: " + bridge_.lastError();
        return false;
    }
    sr = rx[1];
    return true;
}

bool Mt25qFlash::writeEnable() {
    const std::vector<uint8_t> tx = {kCmdWriteEnable};
    std::vector<uint8_t> rx;
    if (!bridge_.transfer(tx, rx)) {
        last_error_ = "writeEnable: " + bridge_.lastError();
        return false;
    }
    return true;
}

bool Mt25qFlash::waitWipClear(std::chrono::milliseconds timeout) {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        uint8_t sr = 0;
        if (!readStatus(sr)) return false;
        if ((sr & kStatusWip) == 0) return true;
        if (std::chrono::steady_clock::now() - start > timeout) {
            last_error_ = "waitWipClear: timeout";
            return false;
        }
        // Flash is busy; yield briefly to avoid hammering JTAG for no reason.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool Mt25qFlash::bulkErase(std::chrono::milliseconds timeout) {
    if (!writeEnable()) return false;
    const std::vector<uint8_t> tx = {kCmdBulkErase};
    std::vector<uint8_t> rx;
    if (!bridge_.transfer(tx, rx)) {
        last_error_ = "bulkErase: " + bridge_.lastError();
        return false;
    }
    return waitWipClear(timeout);
}

bool Mt25qFlash::pageProgram(uint32_t addr, const uint8_t* data, size_t n,
                              std::chrono::milliseconds timeout) {
    if (n == 0 || n > kPageSize) {
        last_error_ = "pageProgram: byte count out of range";
        return false;
    }
    if (addr + n > kCapacityBytes) {
        last_error_ = "pageProgram: address out of range";
        return false;
    }
    if (!writeEnable()) return false;

    std::vector<uint8_t> tx;
    tx.reserve(4 + n);
    tx.push_back(kCmdPageProgram);
    tx.push_back(static_cast<uint8_t>((addr >> 16) & 0xFF));
    tx.push_back(static_cast<uint8_t>((addr >>  8) & 0xFF));
    tx.push_back(static_cast<uint8_t>( addr        & 0xFF));
    tx.insert(tx.end(), data, data + n);
    std::vector<uint8_t> rx;
    if (!bridge_.transfer(tx, rx)) {
        last_error_ = "pageProgram: " + bridge_.lastError();
        return false;
    }
    return waitWipClear(timeout);
}

bool Mt25qFlash::read(uint32_t addr, uint8_t* out, size_t n) {
    if (n == 0) return true;
    if (addr + n > kCapacityBytes) {
        last_error_ = "read: address out of range";
        return false;
    }

    std::vector<uint8_t> tx(4 + n, 0);
    tx[0] = kCmdRead;
    tx[1] = static_cast<uint8_t>((addr >> 16) & 0xFF);
    tx[2] = static_cast<uint8_t>((addr >>  8) & 0xFF);
    tx[3] = static_cast<uint8_t>( addr        & 0xFF);
    std::vector<uint8_t> rx;
    if (!bridge_.transfer(tx, rx)) {
        last_error_ = "read: " + bridge_.lastError();
        return false;
    }
    std::memcpy(out, rx.data() + 4, n);
    return true;
}

}  // namespace jtag::flash

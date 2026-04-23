#include "mcs_parser.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "spi_flash.h"  // for kCapacityBytes

namespace jtag::flash::internal {

namespace {

// Parse one record line.  Returns false and sets err on malformed input.
bool parseMcsRecord(const std::string& line, int line_num,
                    uint8_t& rec_len, uint16_t& rec_addr, uint8_t& rec_type,
                    std::vector<uint8_t>& data, std::string& err) {
    auto hexDigit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    auto hexByte = [&](size_t pos) -> int {
        if (pos + 2 > line.size()) return -1;
        int hi = hexDigit(line[pos]);
        int lo = hexDigit(line[pos + 1]);
        if (hi < 0 || lo < 0) return -1;
        return (hi << 4) | lo;
    };

    // Minimum: ':' + 2(len) + 4(addr) + 2(type) + 2(ck) = 11 chars
    if (line.size() < 11 || line[0] != ':') {
        err = "Invalid MCS record at line " + std::to_string(line_num);
        return false;
    }

    int iLen  = hexByte(1);
    int addrH = hexByte(3);
    int addrL = hexByte(5);
    int iType = hexByte(7);
    if (iLen < 0 || addrH < 0 || addrL < 0 || iType < 0) {
        err = "Bad hex digits in MCS record at line " + std::to_string(line_num);
        return false;
    }

    // Validate line length: ':' + (4 + iLen + 1) field bytes * 2 hex chars each.
    if (line.size() < static_cast<size_t>(1 + (4 + iLen + 1) * 2)) {
        err = "MCS record too short at line " + std::to_string(line_num);
        return false;
    }

    rec_len  = static_cast<uint8_t>(iLen);
    rec_addr = static_cast<uint16_t>((addrH << 8) | addrL);
    rec_type = static_cast<uint8_t>(iType);

    // Read data bytes and accumulate checksum.
    uint8_t cksum = static_cast<uint8_t>(iLen + addrH + addrL + iType);
    data.resize(iLen);
    for (int i = 0; i < iLen; i++) {
        int b = hexByte(9 + i * 2);
        if (b < 0) {
            err = "Bad data byte in MCS record at line " + std::to_string(line_num);
            return false;
        }
        data[i] = static_cast<uint8_t>(b);
        cksum += static_cast<uint8_t>(b);
    }
    int ck = hexByte(9 + iLen * 2);
    if (ck < 0) {
        err = "Missing checksum in MCS record at line " + std::to_string(line_num);
        return false;
    }
    cksum += static_cast<uint8_t>(ck);
    if (cksum != 0) {
        err = "Checksum mismatch in MCS record at line " + std::to_string(line_num);
        return false;
    }
    return true;
}

}  // namespace

bool parseMcsToImage(const std::string& path, std::vector<uint8_t>& out,
                     std::string& err) {
    std::ifstream f(path);
    if (!f.is_open()) {
        err = "Cannot open MCS file: " + path;
        return false;
    }

    uint32_t upper_addr = 0;
    std::string line;
    int line_num = 0;

    while (std::getline(f, line)) {
        ++line_num;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] != ':') continue;

        uint8_t rec_len = 0;
        uint16_t rec_addr = 0;
        uint8_t rec_type = 0;
        std::vector<uint8_t> data;
        if (!parseMcsRecord(line, line_num, rec_len, rec_addr, rec_type, data, err))
            return false;

        if (rec_type == 0x01) break;  // EOF record

        if (rec_type == 0x04) {
            // Extended linear address (upper 16 bits).
            if (rec_len != 2) {
                err = "Bad ELA record at line " + std::to_string(line_num);
                return false;
            }
            upper_addr = static_cast<uint32_t>((data[0] << 8) | data[1]) << 16;
        } else if (rec_type == 0x02) {
            // Extended segment address (segment base << 4).
            if (rec_len != 2) {
                err = "Bad ESA record at line " + std::to_string(line_num);
                return false;
            }
            upper_addr = static_cast<uint32_t>((data[0] << 8) | data[1]) << 4;
        } else if (rec_type == 0x00) {
            // Data record.
            const uint32_t base = upper_addr | static_cast<uint32_t>(rec_addr);
            const uint32_t end  = base + rec_len;
            if (end > Mt25qFlash::kCapacityBytes) {
                err = "MCS address 0x" + std::to_string(end) + " exceeds flash capacity";
                return false;
            }
            if (end > out.size()) {
                out.resize(end, 0xFF);  // fill gap with erased-state 0xFF
            }
            std::memcpy(out.data() + base, data.data(), rec_len);
        }
        // Record types 0x03 and 0x05 (start address hints) are not needed for writing.
    }

    if (out.empty()) {
        err = "No data records found in MCS file: " + path;
        return false;
    }
    return true;
}

}  // namespace jtag::flash::internal

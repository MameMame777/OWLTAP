#include "ftdi_device.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

// ── Windows: D2XX dynamic backend ──────────────────────────────────────────
// Uses FTD2XX.DLL which ships with every FTDI driver install (including
// Digilent's). No WinUSB / Zadig required.
// ───────────────────────────────────────────────────────────────────────────
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef PVOID  FT_HANDLE;
typedef ULONG  FT_STATUS;

#define FT_OK                   0UL
#define FT_BITMODE_RESET        0x00
#define FT_BITMODE_MPSSE        0x02
#define FT_PURGE_RX             1UL
#define FT_PURGE_TX             2UL
#define FT_OPEN_BY_DESCRIPTION  2UL

typedef struct {
    ULONG     Flags;
    ULONG     Type;
    ULONG     ID;            // (VID << 16) | PID
    DWORD     LocId;
    char      SerialNumber[16];
    char      Description[64];
    FT_HANDLE ftHandle;
} FT_DEVICE_LIST_INFO_NODE;

typedef FT_STATUS (WINAPI *PFN_FT_CreateDeviceInfoList)(LPDWORD);
typedef FT_STATUS (WINAPI *PFN_FT_GetDeviceInfoList)(FT_DEVICE_LIST_INFO_NODE*, LPDWORD);
typedef FT_STATUS (WINAPI *PFN_FT_OpenEx)(PVOID, DWORD, FT_HANDLE*);
typedef FT_STATUS (WINAPI *PFN_FT_Close)(FT_HANDLE);
typedef FT_STATUS (WINAPI *PFN_FT_Read)(FT_HANDLE, PVOID, DWORD, LPDWORD);
typedef FT_STATUS (WINAPI *PFN_FT_Write)(FT_HANDLE, PVOID, DWORD, LPDWORD);
typedef FT_STATUS (WINAPI *PFN_FT_SetBitMode)(FT_HANDLE, UCHAR, UCHAR);
typedef FT_STATUS (WINAPI *PFN_FT_SetLatencyTimer)(FT_HANDLE, UCHAR);
typedef FT_STATUS (WINAPI *PFN_FT_SetUSBParameters)(FT_HANDLE, ULONG, ULONG);
typedef FT_STATUS (WINAPI *PFN_FT_Purge)(FT_HANDLE, ULONG);
typedef FT_STATUS (WINAPI *PFN_FT_SetTimeouts)(FT_HANDLE, ULONG, ULONG);

struct D2xxLib {
    HMODULE hDll = nullptr;
    bool    loaded = false;

    PFN_FT_CreateDeviceInfoList FT_CreateDeviceInfoList = nullptr;
    PFN_FT_GetDeviceInfoList    FT_GetDeviceInfoList    = nullptr;
    PFN_FT_OpenEx               FT_OpenEx               = nullptr;
    PFN_FT_Close                FT_Close                = nullptr;
    PFN_FT_Read                 FT_Read                 = nullptr;
    PFN_FT_Write                FT_Write                = nullptr;
    PFN_FT_SetBitMode           FT_SetBitMode           = nullptr;
    PFN_FT_SetLatencyTimer      FT_SetLatencyTimer      = nullptr;
    PFN_FT_SetUSBParameters     FT_SetUSBParameters     = nullptr;
    PFN_FT_Purge                FT_Purge                = nullptr;
    PFN_FT_SetTimeouts          FT_SetTimeouts          = nullptr;

    bool load() {
        if (loaded) return true;
        hDll = LoadLibraryA("FTD2XX.DLL");
        if (!hDll) return false;
#define LOAD(fn) \
    fn = reinterpret_cast<decltype(fn)>(GetProcAddress(hDll, #fn)); \
    if (!fn) { FreeLibrary(hDll); hDll = nullptr; return false; }
        LOAD(FT_CreateDeviceInfoList)
        LOAD(FT_GetDeviceInfoList)
        LOAD(FT_OpenEx)
        LOAD(FT_Close)
        LOAD(FT_Read)
        LOAD(FT_Write)
        LOAD(FT_SetBitMode)
        LOAD(FT_SetLatencyTimer)
        LOAD(FT_SetUSBParameters)
        LOAD(FT_Purge)
        LOAD(FT_SetTimeouts)
#undef LOAD
        loaded = true;
        return true;
    }

    ~D2xxLib() {
        if (hDll) { FreeLibrary(hDll); hDll = nullptr; }
    }
};

static D2xxLib g_d2xx;

static const char* ifaceSuffix(jtag::FtdiInterface iface) {
    switch (iface) {
        case jtag::FtdiInterface::A: return " A";
        case jtag::FtdiInterface::B: return " B";
        case jtag::FtdiInterface::C: return " C";
        case jtag::FtdiInterface::D: return " D";
        default: return " A";
    }
}

static std::string statusStr(FT_STATUS st) {
    switch (st) {
        case 0:  return "FT_OK";
        case 1:  return "FT_INVALID_HANDLE";
        case 2:  return "FT_DEVICE_NOT_FOUND";
        case 3:  return "FT_DEVICE_NOT_OPENED";
        case 4:  return "FT_IO_ERROR";
        case 5:  return "FT_INSUFFICIENT_RESOURCES";
        case 6:  return "FT_INVALID_PARAMETER";
        case 10: return "FT_FAILED_TO_WRITE_DEVICE";
        default: {
            char buf[32];
            snprintf(buf, sizeof(buf), "FT_STATUS(%lu)", (unsigned long)st);
            return buf;
        }
    }
}

#else
// ── Non-Windows: libftdi backend ────────────────────────────────────────────
#include <ftdi.h>
#endif  // _WIN32

namespace jtag {

FtdiDevice::FtdiDevice() {
#ifndef _WIN32
    ftdi_ = ftdi_new();
#endif
}

FtdiDevice::~FtdiDevice() {
    close();
#ifndef _WIN32
    if (ftdi_) {
        ftdi_free(ftdi_);
        ftdi_ = nullptr;
    }
#endif
}

FtdiDevice::FtdiDevice(FtdiDevice&& other) noexcept
    : ftdi_(other.ftdi_),
      d2xx_handle_(other.d2xx_handle_),
      is_open_(other.is_open_),
      last_error_(std::move(other.last_error_)) {
    other.ftdi_        = nullptr;
    other.d2xx_handle_ = nullptr;
    other.is_open_     = false;
}

FtdiDevice& FtdiDevice::operator=(FtdiDevice&& other) noexcept {
    if (this != &other) {
        close();
#ifndef _WIN32
        if (ftdi_) ftdi_free(ftdi_);
#endif
        ftdi_              = other.ftdi_;
        d2xx_handle_       = other.d2xx_handle_;
        is_open_           = other.is_open_;
        last_error_        = std::move(other.last_error_);
        other.ftdi_        = nullptr;
        other.d2xx_handle_ = nullptr;
        other.is_open_     = false;
    }
    return *this;
}

std::vector<FtdiDeviceInfo> FtdiDevice::enumerate(
    uint16_t vendor_id, uint16_t product_id) {

    std::vector<FtdiDeviceInfo> result;

#ifdef _WIN32
    if (!g_d2xx.load()) return result;

    DWORD num_devs = 0;
    if (g_d2xx.FT_CreateDeviceInfoList(&num_devs) != FT_OK || num_devs == 0)
        return result;

    std::vector<FT_DEVICE_LIST_INFO_NODE> nodes(num_devs);
    if (g_d2xx.FT_GetDeviceInfoList(nodes.data(), &num_devs) != FT_OK)
        return result;

    ULONG target_id = (static_cast<ULONG>(vendor_id) << 16) |
                       static_cast<ULONG>(product_id);

    for (DWORD i = 0; i < num_devs; i++) {
        if (product_id != 0 && nodes[i].ID != target_id) continue;
        if (product_id == 0 && (nodes[i].ID >> 16) != vendor_id) continue;

        FtdiDeviceInfo info;
        info.vendor_id   = static_cast<uint16_t>(nodes[i].ID >> 16);
        info.product_id  = static_cast<uint16_t>(nodes[i].ID & 0xFFFF);
        info.description = nodes[i].Description;
        info.serial      = nodes[i].SerialNumber;
        result.push_back(std::move(info));
    }

#else
    ftdi_context* ftdi = ftdi_new();
    if (!ftdi) return result;

    const uint16_t pids[] = { 0x6001, 0x6010, 0x6011, 0x6014, 0x6015 };

    auto scan_pid = [&](uint16_t vid, uint16_t pid) {
        struct ftdi_device_list* devlist = nullptr;
        int count = ftdi_usb_find_all(ftdi, &devlist, vid, pid);
        if (count <= 0) {
            if (devlist) ftdi_list_free(&devlist);
            return;
        }
        for (auto* cur = devlist; cur; cur = cur->next) {
            FtdiDeviceInfo info;
            info.vendor_id  = vid;
            info.product_id = pid;
            char mfr[128] = {}, desc[128] = {}, ser[128] = {};
            if (ftdi_usb_get_strings(ftdi, cur->dev,
                    mfr, sizeof(mfr), desc, sizeof(desc), ser, sizeof(ser)) >= 0) {
                info.manufacturer = mfr;
                info.description  = desc;
                info.serial       = ser;
            }
            result.push_back(std::move(info));
        }
        ftdi_list_free(&devlist);
    };

    if (product_id != 0) {
        scan_pid(vendor_id, product_id);
    } else {
        for (auto pid : pids) scan_pid(vendor_id, pid);
    }
    ftdi_free(ftdi);
#endif

    return result;
}

bool FtdiDevice::open(uint16_t vendor_id, uint16_t product_id,
                       const std::string& serial, FtdiInterface iface) {
    close();

#ifdef _WIN32
    if (!g_d2xx.load()) {
        last_error_ = "FTD2XX.DLL not found. Install FTDI drivers from "
                      "https://ftdichip.com/drivers/d2xx-drivers/";
        return false;
    }

    DWORD num_devs = 0;
    if (g_d2xx.FT_CreateDeviceInfoList(&num_devs) != FT_OK || num_devs == 0) {
        last_error_ = "No FTDI devices found";
        return false;
    }
    std::vector<FT_DEVICE_LIST_INFO_NODE> nodes(num_devs);
    if (g_d2xx.FT_GetDeviceInfoList(nodes.data(), &num_devs) != FT_OK) {
        last_error_ = "Failed to enumerate FTDI devices";
        return false;
    }

    ULONG target_id = (static_cast<ULONG>(vendor_id) << 16) |
                       static_cast<ULONG>(product_id);
    const char* suffix = ifaceSuffix(iface);
    size_t suffix_len  = strlen(suffix);

    std::string open_desc;
    bool found = false;
    for (DWORD i = 0; i < num_devs; i++) {
        if (nodes[i].ID != target_id) continue;
        if (!serial.empty() &&
            std::string(nodes[i].SerialNumber).find(serial) == std::string::npos)
            continue;

        std::string desc = nodes[i].Description;
        size_t dlen = desc.size();
        bool has_suffix = (dlen >= suffix_len) &&
                          (desc.substr(dlen - suffix_len) == suffix);
        bool no_channel = (desc.find(" A") == std::string::npos &&
                           desc.find(" B") == std::string::npos);
        if (has_suffix || (no_channel && iface == FtdiInterface::A)) {
            open_desc = desc;
            found = true;
            break;
        }
    }
    if (!found) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "Device not found (VID=0x%04X PID=0x%04X channel%s)",
                 vendor_id, product_id, suffix);
        last_error_ = buf;
        return false;
    }

    FT_HANDLE handle = nullptr;
    FT_STATUS st = g_d2xx.FT_OpenEx(
        const_cast<char*>(open_desc.c_str()),
        FT_OPEN_BY_DESCRIPTION, &handle);
    if (st != FT_OK) {
        last_error_ = "FT_OpenEx(\"" + open_desc + "\") failed: " + statusStr(st);
        return false;
    }
    d2xx_handle_ = handle;
    is_open_     = true;
    return true;

#else
    if (!ftdi_) {
        last_error_ = "FTDI context not initialized";
        return false;
    }
    int iface_val;
    switch (iface) {
        case FtdiInterface::A: iface_val = INTERFACE_A; break;
        case FtdiInterface::B: iface_val = INTERFACE_B; break;
        case FtdiInterface::C: iface_val = INTERFACE_C; break;
        case FtdiInterface::D: iface_val = INTERFACE_D; break;
        default:               iface_val = INTERFACE_A; break;
    }
    if (ftdi_set_interface(ftdi_, static_cast<ftdi_interface>(iface_val)) < 0) {
        last_error_ = std::string("Failed to set interface: ") +
                      ftdi_get_error_string(ftdi_);
        return false;
    }
    int ret = serial.empty()
        ? ftdi_usb_open(ftdi_, vendor_id, product_id)
        : ftdi_usb_open_desc(ftdi_, vendor_id, product_id, nullptr, serial.c_str());
    if (ret < 0) {
        last_error_ = std::string("Failed to open device: ") +
                      ftdi_get_error_string(ftdi_);
        return false;
    }
    is_open_ = true;
    return true;
#endif
}

void FtdiDevice::close() {
    if (!is_open_) return;
#ifdef _WIN32
    if (d2xx_handle_ && g_d2xx.loaded) {
        g_d2xx.FT_SetBitMode(static_cast<FT_HANDLE>(d2xx_handle_),
                             0, FT_BITMODE_RESET);
        g_d2xx.FT_Close(static_cast<FT_HANDLE>(d2xx_handle_));
        d2xx_handle_ = nullptr;
    }
#else
    if (ftdi_) {
        ftdi_set_bitmode(ftdi_, 0, 0);
        ftdi_usb_close(ftdi_);
    }
#endif
    is_open_ = false;
}

bool FtdiDevice::purgeBuffers() {
#ifdef _WIN32
    FT_STATUS st = g_d2xx.FT_Purge(static_cast<FT_HANDLE>(d2xx_handle_),
                                    FT_PURGE_RX | FT_PURGE_TX);
    if (st != FT_OK) {
        last_error_ = "FT_Purge failed: " + statusStr(st);
        return false;
    }
    return true;
#else
    if (ftdi_tcioflush(ftdi_) < 0) {
        last_error_ = std::string("Purge failed: ") + ftdi_get_error_string(ftdi_);
        return false;
    }
    return true;
#endif
}

bool FtdiDevice::syncMpsse() {
    // Send an invalid MPSSE opcode; the engine echoes back 0xFA <cmd>
    uint8_t bad_cmd = mpsse_cmd::BAD_COMMAND;

#ifdef _WIN32
    FT_HANDLE h = static_cast<FT_HANDLE>(d2xx_handle_);
    DWORD written = 0;
    if (g_d2xx.FT_Write(h, &bad_cmd, 1, &written) != FT_OK || written != 1) {
        last_error_ = "MPSSE sync: write failed";
        return false;
    }
    uint8_t response[2] = {};
    int retries = 50;
    DWORD total_read = 0;
    while (total_read < 2 && retries-- > 0) {
        DWORD n = 0;
        g_d2xx.FT_Read(h, response + total_read, 2 - total_read, &n);
        total_read += n;
        if (total_read < 2)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (total_read < 2 || response[0] != 0xFA) {
        last_error_ = "MPSSE sync failed (no 0xFA response)";
        return false;
    }
    return true;

#else
    if (ftdi_write_data(ftdi_, &bad_cmd, 1) < 0) {
        last_error_ = "MPSSE sync: write failed";
        return false;
    }
    uint8_t response[2] = {};
    int retries = 50;
    int total_read = 0;
    while (total_read < 2 && retries-- > 0) {
        int n = ftdi_read_data(ftdi_, response + total_read, 2 - total_read);
        if (n < 0) { last_error_ = "MPSSE sync: read failed"; return false; }
        total_read += n;
        if (total_read < 2)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (total_read < 2 || response[0] != 0xFA) {
        last_error_ = "MPSSE sync failed";
        return false;
    }
    return true;
#endif
}

bool FtdiDevice::initMpsse(uint32_t clock_freq_hz) {
    if (!is_open_) {
        last_error_ = "Device not open";
        return false;
    }

#ifdef _WIN32
    FT_HANDLE h = static_cast<FT_HANDLE>(d2xx_handle_);
    g_d2xx.FT_SetUSBParameters(h, 65536, 65536);
    g_d2xx.FT_SetTimeouts(h, 5000, 5000);
    if (g_d2xx.FT_SetLatencyTimer(h, 2) != FT_OK) {
        last_error_ = "FT_SetLatencyTimer failed";
        return false;
    }
    if (!purgeBuffers()) return false;
    if (g_d2xx.FT_SetBitMode(h, 0, FT_BITMODE_RESET) != FT_OK) {
        last_error_ = "FT_SetBitMode(RESET) failed";
        return false;
    }
    if (g_d2xx.FT_SetBitMode(h, 0, FT_BITMODE_MPSSE) != FT_OK) {
        last_error_ = "FT_SetBitMode(MPSSE) failed";
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!purgeBuffers()) return false;
    if (!syncMpsse()) return false;

#else
    if (ftdi_set_bitmode(ftdi_, 0, 0) < 0) {
        last_error_ = std::string("Reset bitmode failed: ") +
                      ftdi_get_error_string(ftdi_);
        return false;
    }
    if (ftdi_set_latency_timer(ftdi_, 2) < 0) {
        last_error_ = std::string("Set latency failed: ") +
                      ftdi_get_error_string(ftdi_);
        return false;
    }
    if (!purgeBuffers()) return false;
    if (ftdi_set_bitmode(ftdi_, 0, BITMODE_MPSSE) < 0) {
        last_error_ = std::string("Enable MPSSE failed: ") +
                      ftdi_get_error_string(ftdi_);
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!purgeBuffers()) return false;
    if (!syncMpsse()) return false;
#endif

    // Configure MPSSE for JTAG (common to both backends)
    MpsseCommandBuffer cmd;
    cmd.disableClockDivide5();
    cmd.disable3PhaseClocking();
    cmd.disableLoopback();

    // TCK = 60MHz / ((1 + divisor) * 2)
    uint16_t divisor = 0;
    if (clock_freq_hz > 0 && clock_freq_hz < 30000000) {
        uint32_t raw = (30000000 / clock_freq_hz) - 1;
        divisor = (raw > 0xFFFF) ? 0xFFFF : static_cast<uint16_t>(raw);
    }
    cmd.setClockDivisor(divisor);
    cmd.setLowBits(jtag_pins::INIT_VALUE, jtag_pins::OUTPUT_MASK);
    return write(cmd);
}

bool FtdiDevice::transfer(const MpsseCommandBuffer& cmd_buf,
                           std::vector<uint8_t>& read_buf) {
    if (!is_open_) {
        last_error_ = "Device not open";
        return false;
    }
    const auto& data = cmd_buf.data();
    if (data.empty()) return true;

#ifdef _WIN32
    FT_HANDLE h = static_cast<FT_HANDLE>(d2xx_handle_);

    // D2XX FT_Write can silently truncate writes larger than the internal
    // USB buffer (~64 KB).  Loop in 65536-byte chunks to handle large
    // transfers (e.g. 4 MB FPGA bitstreams) reliably.
    {
        const uint8_t* wp = data.data();
        DWORD remaining = static_cast<DWORD>(data.size());
        while (remaining > 0) {
            DWORD chunk = (remaining > 65536u) ? 65536u : remaining;
            DWORD written = 0;
            FT_STATUS st = g_d2xx.FT_Write(
                h, const_cast<uint8_t*>(wp), chunk, &written);
            if (st != FT_OK || written != chunk) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "FT_Write failed: wrote %lu of %lu bytes (status %lu)",
                         (unsigned long)written, (unsigned long)chunk,
                         (unsigned long)st);
                last_error_ = buf;
                return false;
            }
            wp        += written;
            remaining -= written;
        }
    }

    size_t expected = cmd_buf.expectedReadBytes();
    if (expected == 0) { read_buf.clear(); return true; }

    read_buf.resize(expected);
    DWORD total_read = 0;
    int retries = 200;
    while (total_read < static_cast<DWORD>(expected) && retries-- > 0) {
        DWORD n = 0;
        g_d2xx.FT_Read(h, read_buf.data() + total_read,
                       static_cast<DWORD>(expected) - total_read, &n);
        total_read += n;
        if (total_read < static_cast<DWORD>(expected))
            std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (total_read < static_cast<DWORD>(expected)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Read timeout: got %lu of %zu bytes",
                 (unsigned long)total_read, expected);
        last_error_ = buf;
        return false;
    }
    return true;

#else
    const uint8_t* wp = data.data();
    size_t rem = data.size();
    while (rem > 0) {
        int n = ftdi_write_data(ftdi_,
                                const_cast<unsigned char*>(wp),
                                static_cast<int>(rem));
        if (n < 0) {
            last_error_ = std::string("Write failed: ") + ftdi_get_error_string(ftdi_);
            return false;
        }
        if (n == 0) { last_error_ = "Write stalled"; return false; }
        wp  += n;
        rem -= static_cast<size_t>(n);
    }

    size_t expected = cmd_buf.expectedReadBytes();
    if (expected == 0) { read_buf.clear(); return true; }

    read_buf.resize(expected);
    size_t total_read = 0;
    int retries = 200;
    while (total_read < expected && retries-- > 0) {
        int n = ftdi_read_data(ftdi_,
                               read_buf.data() + total_read,
                               static_cast<int>(expected - total_read));
        if (n < 0) {
            last_error_ = std::string("Read failed: ") + ftdi_get_error_string(ftdi_);
            return false;
        }
        total_read += static_cast<size_t>(n);
        if (total_read < expected)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (total_read < expected) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Read timeout: got %zu of %zu bytes",
                 total_read, expected);
        last_error_ = buf;
        return false;
    }
    return true;
#endif
}

bool FtdiDevice::write(const MpsseCommandBuffer& cmd_buf) {
    std::vector<uint8_t> dummy;
    return transfer(cmd_buf, dummy);
}

} // namespace jtag

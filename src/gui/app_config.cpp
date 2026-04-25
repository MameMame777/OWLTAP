#include "app_config.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace jtag::gui {

// ── Minimal JSON helpers ─────────────────────────────────────────────────────

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"')  { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\r') { out += "\\r"; }
        else { out += c; }
    }
    return out;
}

// Find the string value of a JSON key in a flat JSON object.
// Returns empty string if not found.
static std::string jsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t p = json.find(search);
    if (p == std::string::npos) return {};
    p = json.find(':', p + search.size());
    if (p == std::string::npos) return {};
    p = json.find('"', p + 1);
    if (p == std::string::npos) return {};
    p++; // skip opening quote
    std::string result;
    while (p < json.size()) {
        char c = json[p++];
        if (c == '"') break;
        if (c == '\\' && p < json.size()) {
            char e = json[p++];
            if (e == 'n') result += '\n';
            else if (e == 'r') result += '\r';
            else result += e;
        } else {
            result += c;
        }
    }
    return result;
}

// Find the integer/number value of a JSON key.
static long long jsonInt(const std::string& json, const std::string& key,
                         long long def = 0) {
    std::string search = "\"" + key + "\"";
    size_t p = json.find(search);
    if (p == std::string::npos) return def;
    p = json.find(':', p + search.size());
    if (p == std::string::npos) return def;
    // Skip whitespace
    while (p < json.size() && (json[p] == ':' || json[p] == ' ' || json[p] == '\t')) p++;
    if (p >= json.size()) return def;
    // Handle hex (0x...) or decimal
    if (json[p] == '"') {
        // String-encoded number
        size_t start = p + 1;
        size_t end = json.find('"', start);
        if (end == std::string::npos) return def;
        std::string num = json.substr(start, end - start);
        try { return std::stoll(num, nullptr, 0); } catch (...) { return def; }
    }
    std::string num;
    while (p < json.size() && (std::isdigit(json[p]) || json[p] == 'x' ||
                                json[p] == 'X' || (json[p] >= 'a' && json[p] <= 'f') ||
                                (json[p] >= 'A' && json[p] <= 'F') || json[p] == '-')) {
        num += json[p++];
    }
    if (num.empty()) return def;
    try { return std::stoll(num, nullptr, 0); } catch (...) { return def; }
}

// Extract a JSON array of strings for a given key.
static std::vector<std::string> jsonStringArray(const std::string& json,
                                                 const std::string& key) {
    std::vector<std::string> result;
    std::string search = "\"" + key + "\"";
    size_t p = json.find(search);
    if (p == std::string::npos) return result;
    p = json.find('[', p + search.size());
    if (p == std::string::npos) return result;
    p++; // skip '['
    while (p < json.size()) {
        // Skip whitespace and commas
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t' ||
               json[p] == '\n' || json[p] == '\r' || json[p] == ',')) p++;
        if (p >= json.size() || json[p] == ']') break;
        if (json[p] != '"') { p++; continue; }
        p++; // skip opening quote
        std::string s;
        while (p < json.size()) {
            char c = json[p++];
            if (c == '"') break;
            if (c == '\\' && p < json.size()) {
                char e = json[p++];
                if (e == 'n') s += '\n';
                else if (e == 'r') s += '\r';
                else s += e;
            } else {
                s += c;
            }
        }
        result.push_back(std::move(s));
    }
    return result;
}

// Parse an array of bus-definition objects from a JSON string.
// Expected format: [{"name":"B","signals":["A","B"],"format":"HEX"}, ...]
static std::vector<BusDefinition> jsonBusArray(const std::string& json,
                                                const std::string& key) {
    std::vector<BusDefinition> result;
    std::string search = "\"" + key + "\"";
    size_t p = json.find(search);
    if (p == std::string::npos) return result;
    p = json.find('[', p + search.size());
    if (p == std::string::npos) return result;
    p++;  // skip '['

    while (p < json.size()) {
        // Skip whitespace and commas
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t' ||
               json[p] == '\n' || json[p] == '\r' || json[p] == ',')) p++;
        if (p >= json.size() || json[p] == ']') break;
        if (json[p] != '{') { p++; continue; }

        // Find matching '}'
        size_t obj_start = p;
        size_t obj_end = json.find('}', obj_start + 1);
        if (obj_end == std::string::npos) break;
        std::string obj = json.substr(obj_start, obj_end - obj_start + 1);
        p = obj_end + 1;

        BusDefinition bus;
        bus.name = jsonString(obj, "name");
        if (bus.name.empty()) continue;
        bus.signals = jsonStringArray(obj, "signals");

        std::string fmt_str = jsonString(obj, "format");
        if (fmt_str == "DEC") bus.format = BusFormat::DEC;
        else if (fmt_str == "BIN") bus.format = BusFormat::BIN;
        else bus.format = BusFormat::HEX;

        result.push_back(std::move(bus));
    }
    return result;
}

// Parse an array of ILA signal config objects from a JSON string.
// Expected format: [{"name":"data[31:16]","hi":31,"lo":16,"fmt":"HEX"}, ...]
static std::vector<IlaSignalConfig> jsonIlaSignalArray(const std::string& json,
                                                       const std::string& key) {
    std::vector<IlaSignalConfig> result;
    std::string search = "\"" + key + "\"";
    size_t p = json.find(search);
    if (p == std::string::npos) return result;
    p = json.find('[', p + search.size());
    if (p == std::string::npos) return result;
    p++;  // skip '['

    while (p < json.size()) {
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t' ||
               json[p] == '\n' || json[p] == '\r' || json[p] == ',')) p++;
        if (p >= json.size() || json[p] == ']') break;
        if (json[p] != '{') { p++; continue; }

        size_t obj_start = p;
        size_t obj_end = json.find('}', obj_start + 1);
        if (obj_end == std::string::npos) break;
        std::string obj = json.substr(obj_start, obj_end - obj_start + 1);
        p = obj_end + 1;

        IlaSignalConfig s;
        s.name = jsonString(obj, "name");
        if (s.name.empty()) continue;
        s.hi = static_cast<int>(jsonInt(obj, "hi", 0));
        s.lo = static_cast<int>(jsonInt(obj, "lo", 0));

        std::string fmt_str = jsonString(obj, "fmt");
        if (fmt_str == "DEC") s.fmt = BusFormat::DEC;
        else if (fmt_str == "BIN") s.fmt = BusFormat::BIN;
        else s.fmt = BusFormat::HEX;

        s.trig_cond   = static_cast<int>(jsonInt(obj, "trig_cond",   0));
        s.trig_value  = static_cast<uint32_t>(jsonInt(obj, "trig_value",  0));
        s.trig_cond_b  = static_cast<int>(jsonInt(obj, "trig_cond_b",  0));
        s.trig_value_b = static_cast<uint32_t>(jsonInt(obj, "trig_value_b", 0));

        result.push_back(std::move(s));
    }
    return result;
}

// ── AppConfig implementation ─────────────────────────────────────────────────

bool AppConfig::save(const std::string& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << "{\n";
    f << "  \"vendor_id\": " << vendor_id << ",\n";
    f << "  \"product_id\": " << product_id << ",\n";
    f << "  \"serial\": \"" << jsonEscape(serial) << "\",\n";
    f << "  \"interface_channel\": " << interface_channel << ",\n";
    f << "  \"clock_freq_hz\": " << clock_freq_hz << ",\n";
    f << "  \"bsdl_path\": \"" << jsonEscape(bsdl_path) << "\",\n";
    f << "  \"bsdl_device_index\": " << bsdl_device_index << ",\n";
    f << "  \"selected_pins\": [";
    for (size_t i = 0; i < selected_pins.size(); i++) {
        if (i == 0) f << "\n";
        f << "    \"" << jsonEscape(selected_pins[i]) << "\"";
        if (i + 1 < selected_pins.size()) f << ",";
        f << "\n";
    }
    if (!selected_pins.empty()) f << "  ";
    f << "],\n";

    // Buses
    f << "  \"buses\": [";
    for (size_t i = 0; i < buses.size(); i++) {
        const auto& bus = buses[i];
        if (i == 0) f << "\n";
        f << "    {\"name\":\"" << jsonEscape(bus.name) << "\","
          << "\"signals\":[";
        for (size_t j = 0; j < bus.signals.size(); j++) {
            f << "\"" << jsonEscape(bus.signals[j]) << "\"";
            if (j + 1 < bus.signals.size()) f << ",";
        }
        const char* fmt_str =
            (bus.format == BusFormat::DEC) ? "DEC" :
            (bus.format == BusFormat::BIN) ? "BIN" : "HEX";
        f << "],\"format\":\"" << fmt_str << "\"}";
        if (i + 1 < buses.size()) f << ",";
        f << "\n";
    }
    if (!buses.empty()) f << "  ";
    f << "],\n";

    // ILA signal lane definitions
    f << "  \"ila_signals\": [";
    for (size_t i = 0; i < ila_signals.size(); i++) {
        const auto& s = ila_signals[i];
        if (i == 0) f << "\n";
        const char* fmt_str =
            (s.fmt == BusFormat::DEC) ? "DEC" :
            (s.fmt == BusFormat::BIN) ? "BIN" : "HEX";
        f << "    {\"name\":\"" << jsonEscape(s.name) << "\","
          << "\"hi\":" << s.hi << ","
          << "\"lo\":" << s.lo << ","
          << "\"fmt\":\"" << fmt_str << "\","
          << "\"trig_cond\":" << s.trig_cond << ","
          << "\"trig_value\":" << s.trig_value << ","
          << "\"trig_cond_b\":" << s.trig_cond_b << ","
          << "\"trig_value_b\":" << s.trig_value_b << "}";
        if (i + 1 < ila_signals.size()) f << ",";
        f << "\n";
    }
    if (!ila_signals.empty()) f << "  ";
    f << "],\n";

    // XDC pin alias file
    f << "  \"xdc_path\": \"" << jsonEscape(xdc_path) << "\",\n";
    f << "  \"ila_or_mode\": " << (ila_or_mode ? 1 : 0) << "\n";
    f << "}\n";
    return f.good();
}

AppConfig AppConfig::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};

    std::stringstream ss;
    ss << f.rdbuf();
    std::string json = ss.str();

    AppConfig cfg;
    cfg.vendor_id         = static_cast<uint16_t>(jsonInt(json, "vendor_id",         0x0403));
    cfg.product_id        = static_cast<uint16_t>(jsonInt(json, "product_id",        0x6010));
    cfg.serial            = jsonString(json, "serial");
    cfg.interface_channel = static_cast<int>(jsonInt(json, "interface_channel",      0));
    cfg.clock_freq_hz     = static_cast<uint32_t>(jsonInt(json, "clock_freq_hz",     1000000));
    cfg.bsdl_path         = jsonString(json, "bsdl_path");
    cfg.bsdl_device_index = static_cast<int>(jsonInt(json, "bsdl_device_index",      0));
    cfg.selected_pins     = jsonStringArray(json, "selected_pins");
    cfg.buses             = jsonBusArray(json, "buses");
    cfg.ila_signals       = jsonIlaSignalArray(json, "ila_signals");
    cfg.xdc_path          = jsonString(json, "xdc_path");
    cfg.ila_or_mode       = (jsonInt(json, "ila_or_mode", 0) != 0);
    return cfg;
}

} // namespace jtag::gui

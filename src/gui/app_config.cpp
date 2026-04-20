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
    f << "]\n";
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
    return cfg;
}

} // namespace jtag::gui

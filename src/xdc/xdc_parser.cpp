#include "xdc_parser.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

namespace jtag::xdc {

namespace {

// Strip leading/trailing whitespace from a string in-place.
static void trim(std::string& s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
}

// Remove surrounding braces or brackets: "{foo}" -> "foo", "[foo]" -> "foo".
// Handles nested-free single-wrap only.
static void stripWrap(std::string& s, char open, char close) {
    if (s.size() >= 2 && s.front() == open && s.back() == close) {
        s = s.substr(1, s.size() - 2);
        trim(s);
    }
}

// Extract the value associated with a keyword token inside a Tcl dict string.
// dict_body: content between the outer braces, e.g.
//   "PACKAGE_PIN M14 IOSTANDARD LVCMOS33"
// keyword: e.g. "PACKAGE_PIN"
// Returns the next whitespace-delimited token after keyword, or empty string.
static std::string dictValue(const std::string& dict_body,
                              const std::string& keyword) {
    // Search for the keyword as a whole word (preceded by start or whitespace).
    size_t pos = 0;
    while (pos < dict_body.size()) {
        size_t found = dict_body.find(keyword, pos);
        if (found == std::string::npos) break;

        // Verify it is a word boundary (not a substring of another token).
        bool left_ok  = (found == 0) || std::isspace((unsigned char)dict_body[found - 1]);
        size_t after  = found + keyword.size();
        bool right_ok = (after >= dict_body.size()) ||
                        std::isspace((unsigned char)dict_body[after]);

        if (left_ok && right_ok) {
            // Skip whitespace after keyword.
            while (after < dict_body.size() && std::isspace((unsigned char)dict_body[after]))
                ++after;
            // Collect the next token.
            size_t end = after;
            while (end < dict_body.size() && !std::isspace((unsigned char)dict_body[end]))
                ++end;
            return dict_body.substr(after, end - after);
        }
        pos = found + 1;
    }
    return {};
}

}  // namespace

PinAliasMap parseXdc(const std::string& path) {
    PinAliasMap result;

    std::ifstream file(path);
    if (!file.is_open()) return result;

    std::string line;
    while (std::getline(file, line)) {
        trim(line);

        // Skip empty lines and comments.
        if (line.empty() || line[0] == '#') continue;

        // We only process set_property lines.
        if (line.rfind("set_property", 0) != 0) continue;

        // Must contain -dict and PACKAGE_PIN and get_ports.
        if (line.find("PACKAGE_PIN") == std::string::npos) continue;
        if (line.find("get_ports")   == std::string::npos) continue;

        // Extract dict body: content between the first '{' and its matching '}'.
        size_t dict_open = line.find('{');
        if (dict_open == std::string::npos) continue;
        size_t dict_close = line.find('}', dict_open + 1);
        if (dict_close == std::string::npos) continue;
        std::string dict_body = line.substr(dict_open + 1, dict_close - dict_open - 1);

        // Extract PACKAGE_PIN value and uppercase it.
        std::string pkg_pin = dictValue(dict_body, "PACKAGE_PIN");
        if (pkg_pin.empty()) continue;
        for (char& c : pkg_pin) c = static_cast<char>(std::toupper((unsigned char)c));

        // Extract port name: content inside [get_ports ...].
        // Find the '[' that introduces get_ports.
        size_t gp_pos = line.find("get_ports", dict_close);
        if (gp_pos == std::string::npos) continue;
        size_t bracket_open = line.rfind('[', gp_pos);
        if (bracket_open == std::string::npos) continue;
        // Use rfind to get the *outermost* closing bracket — port names like
        // "led_out[0]" contain their own '[' ']', so the first ']' would be
        // the one inside the port name, not the one closing [get_ports ...].
        size_t bracket_close = line.rfind(']');
        if (bracket_close == std::string::npos || bracket_close < gp_pos) continue;

        // Inside [get_ports ...], skip the "get_ports" keyword and grab the rest.
        size_t port_start = gp_pos + std::string("get_ports").size();
        std::string port_expr = line.substr(port_start, bracket_close - port_start);
        trim(port_expr);
        stripWrap(port_expr, '{', '}');
        trim(port_expr);

        if (pkg_pin.empty() || port_expr.empty()) continue;

        result.emplace(pkg_pin, port_expr);
    }

    return result;
}

}  // namespace jtag::xdc

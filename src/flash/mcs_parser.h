#pragma once
// Internal MCS (Intel HEX) parser — header is for unit tests only.
// Not part of the public API.
#include <cstdint>
#include <string>
#include <vector>

namespace jtag::flash::internal {

/// Parse an Intel HEX (.mcs / .hex) file into a flat binary image.
/// Gaps between records are filled with 0xFF.
/// @param path  File to read.
/// @param out   Output buffer.
/// @param err   Error message on failure.
bool parseMcsToImage(const std::string& path, std::vector<uint8_t>& out,
                     std::string& err);

}  // namespace jtag::flash::internal

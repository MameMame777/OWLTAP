#include "gtest/gtest.h"

#include "src/boundary_scan/scanner.h"

namespace jtag {
namespace {

TEST(ScannerTest, DecodeBoundaryScanPrefersInputCells) {
    bsdl::BSDLDevice device;
    device.boundary_length = 8;
    device.boundary_cells.resize(8);

    device.boundary_cells[0] = {
        0, "BC_1", "*", bsdl::CellFunction::CONTROL, 1, -1, -1,
        bsdl::DisableResult::NONE};
    device.boundary_cells[1] = {
        1, "BC_1", "IO0", bsdl::CellFunction::BIDIR, -1, 0, 1,
        bsdl::DisableResult::HIGHZ};
    device.boundary_cells[2] = {
        2, "BC_1", "IO1", bsdl::CellFunction::INPUT, -1, -1, -1,
        bsdl::DisableResult::NONE};
    device.boundary_cells[3] = {
        3, "BC_1", "*", bsdl::CellFunction::CONTROL, 1, -1, -1,
        bsdl::DisableResult::NONE};
    device.boundary_cells[4] = {
        4, "BC_1", "IO1", bsdl::CellFunction::OUTPUT3, -1, 3, 1,
        bsdl::DisableResult::HIGHZ};
    device.boundary_cells[5] = {
        5, "BC_1", "CLK_IN", bsdl::CellFunction::CLOCK, -1, -1, -1,
        bsdl::DisableResult::NONE};
    device.boundary_cells[6] = {
        6, "BC_1", "DONE_T12", bsdl::CellFunction::OUTPUT3, -1, 7, 1,
        bsdl::DisableResult::HIGHZ};
    device.boundary_cells[7] = {
        7, "BC_1", "*", bsdl::CellFunction::CONTROL, 1, -1, -1,
        bsdl::DisableResult::NONE};

    std::vector<uint8_t> raw_bsr(1, 0);
    raw_bsr[0] |= (1u << 1);  // IO0 bidir -> HIGH
    raw_bsr[0] |= (1u << 4);  // IO1 output -> HIGH
    raw_bsr[0] |= (1u << 5);  // CLK_IN -> HIGH
    raw_bsr[0] |= (1u << 6);  // DONE_T12 output-only -> HIGH

    const ScanResult result = decodeBoundaryScan(device, std::move(raw_bsr));

    EXPECT_EQ(result.getPin("IO0"), PinState::HIGH);
    EXPECT_EQ(result.getPin("IO1"), PinState::LOW);
    EXPECT_EQ(result.getPin("CLK_IN"), PinState::HIGH);
    EXPECT_EQ(result.getPin("DONE_T12"), PinState::HIGH);
    EXPECT_EQ(result.getPin("MISSING_PIN"), PinState::UNKNOWN);
}

TEST(ScannerTest, DecodeBoundaryScanHandlesEmptySnapshot) {
    bsdl::BSDLDevice device;
    device.boundary_length = 2;
    device.boundary_cells.resize(2);
    device.boundary_cells[0] = {
        0, "BC_1", "IO0", bsdl::CellFunction::INPUT, -1, -1, -1,
        bsdl::DisableResult::NONE};
    device.boundary_cells[1] = {
        1, "BC_1", "IO1", bsdl::CellFunction::OUTPUT2, -1, -1, -1,
        bsdl::DisableResult::NONE};

    const ScanResult result = decodeBoundaryScan(device, {});

    EXPECT_TRUE(result.raw_bsr.empty());
    EXPECT_TRUE(result.pin_states.empty());
    EXPECT_EQ(result.getPin("IO0"), PinState::UNKNOWN);
    EXPECT_EQ(result.getPin("IO1"), PinState::UNKNOWN);
}

// Verify that decodeBoundaryScan works correctly when given a partial BSR
// (fewer bytes than boundary_length would normally require).
// This models the behaviour when Scanner::sample() uses partial_bits read:
// cells beyond the partial length return 0 from getBit().
TEST(ScannerTest, DecodeBoundaryScanHandlesPartialBsr) {
    bsdl::BSDLDevice device;
    device.boundary_length = 16;  // full BSR is 16 bits (2 bytes)
    device.boundary_cells.resize(16);

    // Pin "LOW_PIN" lives at cell 2 — within the partial read range.
    device.boundary_cells[2] = {
        2, "BC_1", "LOW_PIN", bsdl::CellFunction::INPUT, -1, -1, -1,
        bsdl::DisableResult::NONE};
    // Pin "HIGH_PIN" lives at cell 3 — within the partial read range.
    device.boundary_cells[3] = {
        3, "BC_1", "HIGH_PIN", bsdl::CellFunction::INPUT, -1, -1, -1,
        bsdl::DisableResult::NONE};
    // Pin "FAR_PIN" lives at cell 12 — beyond the partial read boundary.
    device.boundary_cells[12] = {
        12, "BC_1", "FAR_PIN", bsdl::CellFunction::INPUT, -1, -1, -1,
        bsdl::DisableResult::NONE};
    // Fill remaining cells with no-pin entries so iterators are safe.
    for (int i = 0; i < 16; ++i) {
        if (i == 2 || i == 3 || i == 12) continue;
        device.boundary_cells[i] = {
            i, "BC_1", "*", bsdl::CellFunction::CONTROL, 1, -1, -1,
            bsdl::DisableResult::NONE};
    }

    // Partial BSR: only 1 byte (8 bits) instead of 2.
    // Bit 3 is set -> HIGH_PIN = HIGH; bit 2 is 0 -> LOW_PIN = LOW.
    // Bit 12 is not present (out of range) -> FAR_PIN returns 0 (LOW).
    std::vector<uint8_t> partial_bsr = {0b00001000};  // bit 3 set

    const ScanResult result = decodeBoundaryScan(device, std::move(partial_bsr));

    EXPECT_EQ(result.getPin("LOW_PIN"),  PinState::LOW);
    EXPECT_EQ(result.getPin("HIGH_PIN"), PinState::HIGH);
    // Cell 12 is beyond the 8-bit partial boundary -> getBit returns false -> LOW.
    EXPECT_EQ(result.getPin("FAR_PIN"),  PinState::LOW);
}

} // namespace
} // namespace jtag
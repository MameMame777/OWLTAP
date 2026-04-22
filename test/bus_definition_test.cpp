#include <gtest/gtest.h>

#include "src/gui/app_config.h"
#include "src/gui/bus_definition.h"
#include "src/boundary_scan/scanner.h"

namespace {

// Helper: build a ScanResult with given pin HIGH values
jtag::ScanResult makeScanResult(
        const std::vector<std::pair<std::string, bool>>& pins) {
    jtag::ScanResult result;
    for (const auto& [name, high] : pins) {
        result.pin_states[name] = high ? jtag::PinState::HIGH : jtag::PinState::LOW;
    }
    return result;
}

TEST(BusDefinitionTest, ComputeValue_MSBFirst) {
    jtag::gui::BusDefinition bus;
    bus.name    = "DATA";
    bus.signals = {"D3", "D2", "D1", "D0"};  // MSB first

    jtag::ScanResult r = makeScanResult({{"D3", true}, {"D2", false}, {"D1", true}, {"D0", false}});
    // binary: 1010 = 0xA
    EXPECT_EQ(jtag::gui::computeBusValue(bus, r), 0xAu);
}

TEST(BusDefinitionTest, ComputeValue_AllHigh) {
    jtag::gui::BusDefinition bus;
    bus.signals = {"B3", "B2", "B1", "B0"};
    jtag::ScanResult r = makeScanResult({{"B3", true}, {"B2", true}, {"B1", true}, {"B0", true}});
    EXPECT_EQ(jtag::gui::computeBusValue(bus, r), 0xFu);
}

TEST(BusDefinitionTest, ComputeValue_AllLow) {
    jtag::gui::BusDefinition bus;
    bus.signals = {"B3", "B2", "B1", "B0"};
    jtag::ScanResult r = makeScanResult({{"B3", false}, {"B2", false}, {"B1", false}, {"B0", false}});
    EXPECT_EQ(jtag::gui::computeBusValue(bus, r), 0u);
}

TEST(BusDefinitionTest, ComputeValue_UnknownPinTreatedAsZero) {
    jtag::gui::BusDefinition bus;
    bus.signals = {"A", "MISSING", "B"};  // "MISSING" not in ScanResult
    jtag::ScanResult r = makeScanResult({{"A", true}, {"B", true}});
    // A=1, MISSING=0, B=1 → 101 = 5
    EXPECT_EQ(jtag::gui::computeBusValue(bus, r), 5u);
}

TEST(BusDefinitionTest, ComputeValue_SingleBit) {
    jtag::gui::BusDefinition bus;
    bus.signals = {"CLK"};
    jtag::ScanResult r = makeScanResult({{"CLK", true}});
    EXPECT_EQ(jtag::gui::computeBusValue(bus, r), 1u);
}

TEST(BusDefinitionTest, ComputeValue_8bit) {
    jtag::gui::BusDefinition bus;
    for (int i = 7; i >= 0; i--)
        bus.signals.push_back("D" + std::to_string(i));
    // 0b10100101 = 0xA5
    jtag::ScanResult r = makeScanResult({
        {"D7", true}, {"D6", false}, {"D5", true}, {"D4", false},
        {"D3", false}, {"D2", true}, {"D1", false}, {"D0", true}
    });
    EXPECT_EQ(jtag::gui::computeBusValue(bus, r), 0xA5u);
}

TEST(BusDefinitionTest, AppConfigSerialization) {
    jtag::gui::AppConfig cfg;
    cfg.buses.push_back({"BUS_A", {"P3", "P2", "P1", "P0"}, jtag::gui::BusFormat::HEX});
    cfg.buses.push_back({"BUS_B", {"Q1", "Q0"}, jtag::gui::BusFormat::BIN});

    const std::string tmp = "test_bus_cfg.json";
    ASSERT_TRUE(cfg.save(tmp));

    auto loaded = jtag::gui::AppConfig::load(tmp);
    ASSERT_EQ(loaded.buses.size(), 2u);
    EXPECT_EQ(loaded.buses[0].name, "BUS_A");
    EXPECT_EQ(loaded.buses[0].signals.size(), 4u);
    EXPECT_EQ(loaded.buses[0].signals[0], "P3");
    EXPECT_EQ(loaded.buses[0].format, jtag::gui::BusFormat::HEX);
    EXPECT_EQ(loaded.buses[1].name, "BUS_B");
    EXPECT_EQ(loaded.buses[1].format, jtag::gui::BusFormat::BIN);
    std::remove(tmp.c_str());
}

}  // namespace

#include <gtest/gtest.h>

#include <string>

#include "src/boundary_scan/pin_driver.h"

namespace jtag {
namespace {

TEST(PinDriverTest, BlocksLiveExtestOnZynqProcessingSystemPins) {
    bsdl::BSDLDevice device;
    device.pins["PS_DDR_A0"].name = "PS_DDR_A0";
    device.pins["PS_MIO0"].name = "PS_MIO0";

    std::string reason;
    EXPECT_FALSE(deviceAllowsLiveExtest(device, &reason));
    EXPECT_NE(reason.find("PS/DDR/MIO"), std::string::npos);
}

TEST(PinDriverTest, AllowsLiveExtestWhenPsPinsAreAbsent) {
    bsdl::BSDLDevice device;
    device.pins["LED0"].name = "LED0";
    device.pins["DONE_T12"].name = "DONE_T12";

    std::string reason;
    EXPECT_TRUE(deviceAllowsLiveExtest(device, &reason));
    EXPECT_TRUE(reason.empty());
}

} // namespace
} // namespace jtag
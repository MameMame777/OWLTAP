#include "gtest/gtest.h"

#include "src/capture/trigger.h"

namespace jtag {
namespace {

ScanResult makeSample(const std::map<std::string, PinState>& states) {
    ScanResult result;
    result.pin_states = states;
    return result;
}

TEST(TriggerEngineTest, FreeRunAlwaysTriggered) {
    TriggerEngine engine;
    engine.setMode(TriggerMode::FREE_RUN);

    auto prev = makeSample({{"CLK", PinState::LOW}});
    auto curr = makeSample({{"CLK", PinState::HIGH}});

    EXPECT_TRUE(engine.evaluate(prev, curr));
}

TEST(TriggerEngineTest, ArmRequired) {
    TriggerEngine engine;
    engine.setMode(TriggerMode::SINGLE);

    TriggerCondition cond;
    cond.pin_name = "CLK";
    cond.type = TriggerType::EDGE;
    cond.edge = TriggerEdge::RISING;
    engine.setConditions({cond});

    auto prev = makeSample({{"CLK", PinState::LOW}});
    auto curr = makeSample({{"CLK", PinState::HIGH}});

    // Not armed -> should not trigger
    EXPECT_FALSE(engine.evaluate(prev, curr));

    // Arm and try again
    engine.arm();
    EXPECT_TRUE(engine.evaluate(prev, curr));
}

TEST(TriggerEngineTest, RisingEdge) {
    TriggerEngine engine;
    engine.setMode(TriggerMode::SINGLE);

    TriggerCondition cond;
    cond.pin_name = "DATA";
    cond.type = TriggerType::EDGE;
    cond.edge = TriggerEdge::RISING;
    engine.setConditions({cond});
    engine.arm();

    auto low = makeSample({{"DATA", PinState::LOW}});
    auto high = makeSample({{"DATA", PinState::HIGH}});

    // LOW -> HIGH = rising edge
    EXPECT_TRUE(engine.evaluate(low, high));
}

TEST(TriggerEngineTest, FallingEdge) {
    TriggerEngine engine;
    engine.setMode(TriggerMode::SINGLE);

    TriggerCondition cond;
    cond.pin_name = "DATA";
    cond.type = TriggerType::EDGE;
    cond.edge = TriggerEdge::FALLING;
    engine.setConditions({cond});
    engine.arm();

    auto high = makeSample({{"DATA", PinState::HIGH}});
    auto low = makeSample({{"DATA", PinState::LOW}});

    // HIGH -> LOW = falling edge
    EXPECT_TRUE(engine.evaluate(high, low));

    // LOW -> HIGH = not falling
    engine.arm();
    EXPECT_FALSE(engine.evaluate(low, high));
}

TEST(TriggerEngineTest, EitherEdge) {
    TriggerEngine engine;
    engine.setMode(TriggerMode::SINGLE);

    TriggerCondition cond;
    cond.pin_name = "CLK";
    cond.type = TriggerType::EDGE;
    cond.edge = TriggerEdge::EITHER;
    engine.setConditions({cond});

    auto low = makeSample({{"CLK", PinState::LOW}});
    auto high = makeSample({{"CLK", PinState::HIGH}});

    engine.arm();
    EXPECT_TRUE(engine.evaluate(low, high));  // Rising

    engine.arm();
    EXPECT_TRUE(engine.evaluate(high, low));  // Falling
}

TEST(TriggerEngineTest, LevelTrigger) {
    TriggerEngine engine;
    engine.setMode(TriggerMode::SINGLE);

    TriggerCondition cond;
    cond.pin_name = "EN";
    cond.type = TriggerType::LEVEL;
    cond.level = PinState::HIGH;
    engine.setConditions({cond});
    engine.arm();

    auto low = makeSample({{"EN", PinState::LOW}});
    auto high = makeSample({{"EN", PinState::HIGH}});

    EXPECT_FALSE(engine.evaluate(low, low));  // Level not met

    engine.arm();
    EXPECT_TRUE(engine.evaluate(low, high));  // Level met
}

TEST(TriggerEngineTest, MultipleConditionsAND) {
    TriggerEngine engine;
    engine.setMode(TriggerMode::SINGLE);

    TriggerCondition cond1;
    cond1.pin_name = "CLK";
    cond1.type = TriggerType::EDGE;
    cond1.edge = TriggerEdge::RISING;

    TriggerCondition cond2;
    cond2.pin_name = "EN";
    cond2.type = TriggerType::LEVEL;
    cond2.level = PinState::HIGH;

    engine.setConditions({cond1, cond2});
    engine.arm();

    auto prev = makeSample({{"CLK", PinState::LOW}, {"EN", PinState::LOW}});
    auto curr = makeSample({{"CLK", PinState::HIGH}, {"EN", PinState::LOW}});

    // CLK rises but EN is LOW -> not triggered
    EXPECT_FALSE(engine.evaluate(prev, curr));

    engine.arm();
    auto curr2 = makeSample({{"CLK", PinState::HIGH}, {"EN", PinState::HIGH}});
    // CLK rises AND EN is HIGH -> triggered
    EXPECT_TRUE(engine.evaluate(prev, curr2));
}

TEST(TriggerEngineTest, SingleModePreventsRetrigger) {
    TriggerEngine engine;
    engine.setMode(TriggerMode::SINGLE);

    TriggerCondition cond;
    cond.pin_name = "CLK";
    cond.type = TriggerType::EDGE;
    cond.edge = TriggerEdge::RISING;
    engine.setConditions({cond});
    engine.arm();

    auto low = makeSample({{"CLK", PinState::LOW}});
    auto high = makeSample({{"CLK", PinState::HIGH}});

    EXPECT_TRUE(engine.evaluate(low, high));
    // Second trigger should fail (single mode)
    EXPECT_FALSE(engine.evaluate(low, high));
}

TEST(TriggerEngineTest, UnknownPinDoesNotTrigger) {
    TriggerEngine engine;
    engine.setMode(TriggerMode::SINGLE);

    TriggerCondition cond;
    cond.pin_name = "MISSING_PIN";
    cond.type = TriggerType::EDGE;
    cond.edge = TriggerEdge::RISING;
    engine.setConditions({cond});
    engine.arm();

    auto prev = makeSample({});
    auto curr = makeSample({});

    EXPECT_FALSE(engine.evaluate(prev, curr));
}

TEST(TriggerEngineTest, EmptyConditionsAlwaysTrigger) {
    TriggerEngine engine;
    engine.setMode(TriggerMode::SINGLE);
    engine.arm();

    auto s = makeSample({});
    EXPECT_TRUE(engine.evaluate(s, s));
}

TEST(TriggerEngineTest, PreTriggerRatioClamped) {
    TriggerEngine engine;

    engine.setPreTriggerRatio(-0.5f);
    EXPECT_FLOAT_EQ(engine.preTriggerRatio(), 0.0f);

    engine.setPreTriggerRatio(1.5f);
    EXPECT_FLOAT_EQ(engine.preTriggerRatio(), 1.0f);

    engine.setPreTriggerRatio(0.3f);
    EXPECT_FLOAT_EQ(engine.preTriggerRatio(), 0.3f);
}

TEST(TriggerEngineTest, ClearConditions) {
    TriggerEngine engine;
    TriggerCondition cond;
    cond.pin_name = "A";
    engine.setConditions({cond});
    EXPECT_EQ(engine.conditions().size(), 1u);
    engine.clearConditions();
    EXPECT_TRUE(engine.conditions().empty());
    EXPECT_FALSE(engine.hasTriggered());
}

} // namespace
} // namespace jtag

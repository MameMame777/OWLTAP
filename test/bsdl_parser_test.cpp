#include "gtest/gtest.h"

#include "src/bsdl/bsdl_parser.h"

namespace jtag::bsdl {
namespace {

// Minimal BSDL snippet for testing
const char* kMinimalBsdl = R"BSDL(
entity TEST_DEVICE is
  generic (PHYSICAL_PIN_MAP : string := "TEST_PACKAGE");

  port (
    TDI : in bit;
    TDO : out bit;
    TCK : in bit;
    TMS : in bit;
    IO0 : inout bit;
    IO1 : inout bit;
    CLK_IN : in bit
  );

  use STD_1149_1_2001.all;

  attribute INSTRUCTION_LENGTH of TEST_DEVICE : entity is 6;

  attribute INSTRUCTION_OPCODE of TEST_DEVICE : entity is
    "BYPASS  (111111)," &
    "EXTEST  (000000)," &
    "SAMPLE  (000010)," &
    "IDCODE  (000001)";

  attribute IDCODE_REGISTER of TEST_DEVICE : entity is
    "0001" &       -- version
    "0000000000000001" &  -- part number
    "00000001001" & -- manufacturer
    "1";           -- required 1

  attribute BOUNDARY_LENGTH of TEST_DEVICE : entity is 5;

  attribute BOUNDARY_REGISTER of TEST_DEVICE : entity is
    "4 (BC_1, IO1, output3, X, 3, 1, Z)," &
    "3 (BC_1, *, control, 1)," &
    "2 (BC_1, IO1, input, X)," &
    "1 (BC_1, IO0, bidir, X, 0, 1, Z)," &
    "0 (BC_1, *, control, 1)";

  attribute PIN_MAP of TEST_DEVICE : entity is PHYSICAL_PIN_MAP;

  constant TEST_PACKAGE : PIN_MAP_STRING :=
    "TDI : 1," &
    "TDO : 2," &
    "TCK : 3," &
    "TMS : 4," &
    "IO0 : 5," &
    "IO1 : 6," &
    "CLK_IN : 7";

end TEST_DEVICE;
)BSDL";

TEST(BSDLParserTest, ParseMinimalBsdl) {
    BSDLParser parser;
    auto dev = parser.parseString(kMinimalBsdl);
    ASSERT_NE(dev, nullptr) << "Parse error: " << parser.lastError();

    EXPECT_EQ(dev->entity_name, "TEST_DEVICE");
    EXPECT_EQ(dev->instruction_length, 6);
}

TEST(BSDLParserTest, InstructionOpcodes) {
    BSDLParser parser;
    auto dev = parser.parseString(kMinimalBsdl);
    ASSERT_NE(dev, nullptr);

    // Check opcodes
    auto bypass = dev->getInstruction("BYPASS");
    ASSERT_TRUE(bypass.has_value());
    EXPECT_EQ(*bypass, 0x3Fu);  // 111111 = 63

    auto extest = dev->getInstruction("EXTEST");
    ASSERT_TRUE(extest.has_value());
    EXPECT_EQ(*extest, 0x00u);

    auto sample = dev->getInstruction("SAMPLE");
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(*sample, 0x02u);  // 000010 = 2

    auto idcode = dev->getInstruction("IDCODE");
    ASSERT_TRUE(idcode.has_value());
    EXPECT_EQ(*idcode, 0x01u);
}

TEST(BSDLParserTest, BoundaryScanRegister) {
    BSDLParser parser;
    auto dev = parser.parseString(kMinimalBsdl);
    ASSERT_NE(dev, nullptr);

    EXPECT_EQ(dev->boundary_length, 5);
    ASSERT_EQ(dev->boundary_cells.size(), 5u);

    // Cell 4: IO1 output
    EXPECT_EQ(dev->boundary_cells[4].position, 4);
    EXPECT_EQ(dev->boundary_cells[4].pin_name, "IO1");
    EXPECT_EQ(dev->boundary_cells[4].function, CellFunction::OUTPUT3);

    // Cell 3: control
    EXPECT_EQ(dev->boundary_cells[3].position, 3);
    EXPECT_EQ(dev->boundary_cells[3].function, CellFunction::CONTROL);

    // Cell 2: IO1 input
    EXPECT_EQ(dev->boundary_cells[2].position, 2);
    EXPECT_EQ(dev->boundary_cells[2].pin_name, "IO1");
    EXPECT_EQ(dev->boundary_cells[2].function, CellFunction::INPUT);

    // Cell 1: IO0 bidir
    EXPECT_EQ(dev->boundary_cells[1].position, 1);
    EXPECT_EQ(dev->boundary_cells[1].pin_name, "IO0");
    EXPECT_EQ(dev->boundary_cells[1].function, CellFunction::BIDIR);

    // Cell 0: control
    EXPECT_EQ(dev->boundary_cells[0].position, 0);
    EXPECT_EQ(dev->boundary_cells[0].function, CellFunction::CONTROL);
}

TEST(BSDLParserTest, IdCode) {
    BSDLParser parser;
    auto dev = parser.parseString(kMinimalBsdl);
    ASSERT_NE(dev, nullptr);

    EXPECT_NE(dev->idcode.raw, 0u);
    // version=0001, part=0000000000000001, mfr=00000001001, 1
    // = 0001 0000000000000001 00000001001 1
    // = 0x10001013
    EXPECT_EQ(dev->idcode.raw, 0x10001013u);
}

TEST(BSDLParserTest, GetInputCells) {
    BSDLParser parser;
    auto dev = parser.parseString(kMinimalBsdl);
    ASSERT_NE(dev, nullptr);

    auto input_cells = dev->getInputCells();
    // Should include: cell 2 (IO1 input), cell 1 (IO0 bidir)
    EXPECT_GE(input_cells.size(), 2u);
}

TEST(BSDLParserTest, GetOutputCells) {
    BSDLParser parser;
    auto dev = parser.parseString(kMinimalBsdl);
    ASSERT_NE(dev, nullptr);

    auto output_cells = dev->getOutputCells();
    // Should include: cell 4 (IO1 output3), cell 1 (IO0 bidir)
    EXPECT_GE(output_cells.size(), 1u);
}

TEST(BSDLParserTest, EmptyString) {
    BSDLParser parser;
    auto dev = parser.parseString("");
    // Parser is lenient: returns an empty device, not nullptr
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(dev->instruction_length, 0);
    EXPECT_TRUE(dev->pins.empty());
}

TEST(BSDLParserTest, InvalidSyntax) {
    BSDLParser parser;
    auto dev = parser.parseString("this is not valid BSDL");
    // Parser is lenient: unrecognized identifiers are skipped
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(dev->instruction_length, 0);
}

} // namespace
} // namespace jtag::bsdl

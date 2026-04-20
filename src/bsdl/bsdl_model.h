#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace jtag::bsdl {

/// Boundary cell function types
enum class CellFunction {
    INPUT,    // Captures external pin state
    OUTPUT2,  // 2-state output (H/L only)
    OUTPUT3,  // 3-state output (H/L/Z)
    CONTROL,  // Output enable control
    BIDIR,    // Bidirectional
    CLOCK,    // Clock input
    INTERNAL, // Internal cell (no pin connection)
};

/// Result when a cell is disabled
enum class DisableResult {
    NONE,   // Not applicable
    PULL0,  // Pull-down
    PULL1,  // Pull-up
    HIGHZ,  // High impedance
    KEEPER, // Bus keeper
    UNKNOWN,
};

/// A single boundary scan register cell
struct BoundaryCell {
    int position = -1;           // Bit position in BSR (0 = closest to TDO)
    std::string cell_type;       // e.g., "BC_1", "BC_7"
    std::string pin_name;        // Port name ("*" = no pin connection)
    CellFunction function = CellFunction::INPUT;
    int safe_value = -1;         // 0, 1, or -1 (X/don't care)
    int disabled_by = -1;        // Position of control cell (-1 = none)
    int disable_value = -1;      // Value that disables (0 or 1)
    DisableResult disable_result = DisableResult::NONE;

    /// Is this cell connected to a physical pin?
    bool hasPin() const { return pin_name != "*" && !pin_name.empty(); }

    /// Is this an input capture cell?
    bool isInput() const {
        return function == CellFunction::INPUT ||
               function == CellFunction::BIDIR ||
               function == CellFunction::CLOCK;
    }

    /// Is this an output drive cell?
    bool isOutput() const {
        return function == CellFunction::OUTPUT2 ||
               function == CellFunction::OUTPUT3 ||
               function == CellFunction::BIDIR;
    }

    /// Is this a control (output enable) cell?
    bool isControl() const { return function == CellFunction::CONTROL; }
};

/// Pin direction (from BSDL port declaration)
enum class PinDirection {
    IN,
    OUT,
    INOUT,
    BUFFER,
    LINKAGE,  // Power, ground, etc.
};

/// Pin information from BSDL port declaration
struct PinInfo {
    std::string name;
    PinDirection direction = PinDirection::IN;
    int physical_pin = -1;  // From PIN_MAP attribute

    // Associated BSR cell positions
    int input_cell = -1;    // INPUT cell position
    int output_cell = -1;   // OUTPUT cell position
    int control_cell = -1;  // CONTROL cell position
};

/// IDCODE register fields
struct IdCode {
    uint32_t raw = 0;
    uint32_t version = 0;       // Bits 28-31
    uint32_t part_number = 0;   // Bits 12-27
    uint32_t manufacturer = 0;  // Bits 1-11

    /// Check if IDCODE matches a raw value
    bool matches(uint32_t other_raw) const {
        // Mask version bits for comparison (may differ between revisions)
        return (raw & 0x0FFFFFFF) == (other_raw & 0x0FFFFFFF);
    }
};

/// Complete parsed BSDL device description
struct BSDLDevice {
    std::string entity_name;            // Entity name from BSDL
    std::string package_name;           // Physical package variant

    // Instruction register
    int instruction_length = 0;         // IR bit count
    std::map<std::string, uint32_t> instructions;  // name -> opcode

    // Boundary scan register
    int boundary_length = 0;            // BSR bit count
    std::vector<BoundaryCell> boundary_cells;  // Indexed by position

    // Pin information
    std::map<std::string, PinInfo> pins;  // name -> PinInfo

    // Device identification
    IdCode idcode;

    /// Get a specific instruction opcode.
    /// @return opcode value, or std::nullopt if not found
    std::optional<uint32_t> getInstruction(const std::string& name) const {
        auto it = instructions.find(name);
        if (it != instructions.end()) return it->second;
        return std::nullopt;
    }

    /// Get SAMPLE instruction opcode.
    /// Checks both "SAMPLE" and "SAMPLE/PRELOAD" (IEEE 1149.1 name).
    std::optional<uint32_t> sampleOpcode() const {
        auto op = getInstruction("SAMPLE");
        if (!op) op = getInstruction("SAMPLE/PRELOAD");
        return op;
    }

    /// Get EXTEST instruction opcode.
    std::optional<uint32_t> extestOpcode() const { return getInstruction("EXTEST"); }

    /// Get IDCODE instruction opcode.
    std::optional<uint32_t> idcodeOpcode() const { return getInstruction("IDCODE"); }

    /// Get BYPASS instruction opcode.
    std::optional<uint32_t> bypassOpcode() const { return getInstruction("BYPASS"); }

    /// Get boundary cell by position.
    const BoundaryCell* getCellAt(int position) const {
        if (position >= 0 && position < static_cast<int>(boundary_cells.size())) {
            return &boundary_cells[position];
        }
        return nullptr;
    }

    /// Get all input cells (for observation).
    std::vector<const BoundaryCell*> getInputCells() const {
        std::vector<const BoundaryCell*> result;
        for (const auto& cell : boundary_cells) {
            if (cell.isInput() && cell.hasPin()) {
                result.push_back(&cell);
            }
        }
        return result;
    }

    /// Get all output cells (for driving).
    std::vector<const BoundaryCell*> getOutputCells() const {
        std::vector<const BoundaryCell*> result;
        for (const auto& cell : boundary_cells) {
            if (cell.isOutput() && cell.hasPin()) {
                result.push_back(&cell);
            }
        }
        return result;
    }

    /// Find the input cell for a given pin name.
    const BoundaryCell* getInputCellForPin(const std::string& pin_name) const {
        for (const auto& cell : boundary_cells) {
            if (cell.pin_name == pin_name && cell.isInput()) {
                return &cell;
            }
        }
        return nullptr;
    }

    /// Find the output cell for a given pin name.
    const BoundaryCell* getOutputCellForPin(const std::string& pin_name) const {
        for (const auto& cell : boundary_cells) {
            if (cell.pin_name == pin_name && cell.isOutput()) {
                return &cell;
            }
        }
        return nullptr;
    }

    /// Find the control cell for a given output cell.
    const BoundaryCell* getControlCellFor(const BoundaryCell& output_cell) const {
        if (output_cell.disabled_by >= 0) {
            return getCellAt(output_cell.disabled_by);
        }
        return nullptr;
    }

    /// Cross-reference boundary cells with pin information.
    /// Call after parsing is complete to link cells to pins.
    void crossReference();
};

} // namespace jtag::bsdl

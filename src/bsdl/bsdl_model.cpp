#include "bsdl_model.h"

namespace jtag::bsdl {

void BSDLDevice::crossReference() {
    // Link boundary cells to pin info
    for (const auto& cell : boundary_cells) {
        if (!cell.hasPin()) continue;

        auto it = pins.find(cell.pin_name);
        if (it == pins.end()) continue;

        PinInfo& pin = it->second;
        if (cell.isInput()) {
            pin.input_cell = cell.position;
        }
        if (cell.isOutput()) {
            pin.output_cell = cell.position;
        }
        if (cell.isControl()) {
            pin.control_cell = cell.position;
        }
    }
}

} // namespace jtag::bsdl

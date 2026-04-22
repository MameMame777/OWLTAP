#include "bus_definition.h"

namespace jtag::gui {

uint64_t computeBusValue(const BusDefinition& bus,
                         const jtag::ScanResult& result) {
    uint64_t value = 0;
    const int bits = static_cast<int>(bus.signals.size());
    for (int i = 0; i < bits; i++) {
        if (result.getPin(bus.signals[i]) == jtag::PinState::HIGH) {
            value |= (1ULL << (bits - 1 - i));
        }
    }
    return value;
}

}  // namespace jtag::gui

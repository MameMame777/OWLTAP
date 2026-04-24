// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jtag { class JtagChain; }

namespace jtag::ila {

/// Abstract backend used by `IlaDriver` to talk to the ILA's TAP.
///
/// Implementations must present a chain-aware view where only the ILA's
/// instruction/data register bits are exposed (other devices handled via
/// BYPASS internally). All DRs are LSB-first.
class IlaTapBackend {
public:
    virtual ~IlaTapBackend() = default;

    /// Load a 5-bit instruction opcode into the ILA IR.
    virtual bool selectIr(uint32_t opcode) = 0;

    /// Shift `dr_bits` through the ILA DR. `tdi` is (bits+7)/8 bytes LSB-first.
    /// On success, `tdo` is resized and filled with TDO captured during shift.
    virtual bool shiftDr(int dr_bits, const uint8_t* tdi,
                          std::vector<uint8_t>& tdo) = 0;

    virtual const std::string& lastError() const = 0;
};

/// Concrete backend that routes through an existing `JtagChain` + device index.
class ChainIlaTapBackend : public IlaTapBackend {
public:
    ChainIlaTapBackend(JtagChain& chain, int device_index);

    bool selectIr(uint32_t opcode) override;
    bool shiftDr(int dr_bits, const uint8_t* tdi,
                  std::vector<uint8_t>& tdo) override;
    const std::string& lastError() const override { return last_error_; }

private:
    JtagChain& chain_;
    int        device_index_;
    std::string last_error_;
};

} // namespace jtag::ila

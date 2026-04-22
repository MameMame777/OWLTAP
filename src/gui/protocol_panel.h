#pragma once

#include <string>
#include <vector>

#include "src/protocol/protocol.h"

namespace jtag::gui {

/// Panel for configuring and running offline protocol decoders (UART/SPI/I2C).
class ProtocolPanel {
public:
    /// Draw the protocol panel (call each frame).
    static void draw(const std::vector<std::string>& available_signals,
                     const std::vector<jtag::SampleFrame>& samples);

    /// Retrieve the last decoded frames (set after user clicks "Decode").
    static const std::vector<jtag::protocol::DecodedFrame>& decodedFrames();

    /// Returns true if new decoded frames are available (one-shot consume).
    static bool consumeNewFrames();

    /// Show / hide the panel.
    static void setVisible(bool visible);
    static bool isVisible();

private:
    static bool visible_;
    static bool new_frames_ready_;
    static std::vector<jtag::protocol::DecodedFrame> frames_;

    // UART config UI state
    static int uart_rx_idx_;
    static int uart_baud_idx_;
    static int uart_data_bits_;
    static bool uart_parity_en_;
    static bool uart_parity_odd_;
    static int uart_stop_bits_;

    // SPI config UI state
    static int spi_clk_idx_;
    static int spi_mosi_idx_;
    static int spi_miso_idx_;
    static int spi_cs_idx_;
    static bool spi_cpol_;
    static bool spi_cpha_;
    static bool spi_lsb_first_;
    static int spi_bits_per_word_;

    // I2C config UI state
    static int i2c_scl_idx_;
    static int i2c_sda_idx_;

    // Common UI state
    static int selected_protocol_;   // 0=UART, 1=SPI, 2=I2C
    static int selected_frame_idx_;  // highlighted row in results table
};

} // namespace jtag::gui

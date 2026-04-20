#pragma once

#include <memory>
#include <string>

#include "bsdl_lexer.h"
#include "bsdl_model.h"

namespace jtag::bsdl {

/// Parser for BSDL (Boundary Scan Description Language) files.
/// Extracts device information needed for JTAG boundary scan operations:
///   - Instruction register length and opcodes
///   - Boundary scan register structure
///   - Pin-to-BSR mapping
///   - IDCODE register value
class BSDLParser {
public:
    BSDLParser() = default;

    /// Parse a BSDL file from disk.
    /// @param filepath  Path to the BSDL file
    /// @return Parsed device description, or nullptr on error
    std::unique_ptr<BSDLDevice> parseFile(const std::string& filepath);

    /// Parse BSDL from a string.
    /// @param source  BSDL source text
    /// @return Parsed device description, or nullptr on error
    std::unique_ptr<BSDLDevice> parseString(const std::string& source);

    /// Get last error message.
    const std::string& lastError() const { return last_error_; }

private:
    /// Main parse routine.
    std::unique_ptr<BSDLDevice> parse(BSDLLexer& lexer);

    // Section parsers
    void parseEntity(BSDLLexer& lexer, BSDLDevice& dev);
    void parsePorts(BSDLLexer& lexer, BSDLDevice& dev);
    void parseAttribute(BSDLLexer& lexer, BSDLDevice& dev);

    // Attribute-specific parsers
    void parseInstructionLength(BSDLLexer& lexer, BSDLDevice& dev);
    void parseInstructionOpcode(BSDLLexer& lexer, BSDLDevice& dev);
    void parseBoundaryLength(BSDLLexer& lexer, BSDLDevice& dev);
    void parseBoundaryRegister(BSDLLexer& lexer, BSDLDevice& dev);
    void parseIdcodeRegister(BSDLLexer& lexer, BSDLDevice& dev);
    void parsePinMap(BSDLLexer& lexer, BSDLDevice& dev);

    // Helpers
    std::string readConcatenatedString(BSDLLexer& lexer);
    void skipToSemicolon(BSDLLexer& lexer);
    void expectToken(BSDLLexer& lexer, TokenType type);
    Token expectIdentifier(BSDLLexer& lexer);
    CellFunction parseCellFunction(const std::string& name);
    DisableResult parseDisableResult(const std::string& name);

    std::string last_error_;
    bool has_error_ = false;

    void setError(const std::string& msg, int line = 0);
};

} // namespace jtag::bsdl

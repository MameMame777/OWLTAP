#include "bsdl_parser.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace jtag::bsdl {

// Upper bound on BSR bit count accepted from untrusted BSDL files.
// Real FPGAs top out well below this (Zynq UltraScale+ ~4096 bits).
static constexpr int kMaxBsrBits = 65536;

void BSDLParser::setError(const std::string& msg, int line) {
    has_error_ = true;
    if (line > 0) {
        last_error_ = "Line " + std::to_string(line) + ": " + msg;
    } else {
        last_error_ = msg;
    }
}

std::unique_ptr<BSDLDevice> BSDLParser::parseFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        setError("Cannot open file: " + filepath);
        return nullptr;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    return parseString(ss.str());
}

std::unique_ptr<BSDLDevice> BSDLParser::parseString(const std::string& source) {
    BSDLLexer lexer(source);
    return parse(lexer);
}

std::unique_ptr<BSDLDevice> BSDLParser::parse(BSDLLexer& lexer) {
    has_error_ = false;
    auto dev = std::make_unique<BSDLDevice>();

    while (true) {
        Token tok = lexer.nextToken();
        if (tok.type == TokenType::END_OF_FILE || has_error_) break;

        if (tok.type == TokenType::IDENTIFIER) {
            if (tok.value == "ENTITY") {
                parseEntity(lexer, *dev);
            } else if (tok.value == "PORT") {
                parsePorts(lexer, *dev);
            } else if (tok.value == "ATTRIBUTE") {
                parseAttribute(lexer, *dev);
            }
            // Skip other VHDL constructs (use, architecture, etc.)
        }
    }

    if (has_error_) return nullptr;

    // Resize boundary_cells vector and populate by position
    if (dev->boundary_length > 0) {
        // Already populated during parsing
    }

    dev->crossReference();
    return dev;
}

void BSDLParser::parseEntity(BSDLLexer& lexer, BSDLDevice& dev) {
    Token name = lexer.nextToken();
    if (name.type == TokenType::IDENTIFIER) {
        dev.entity_name = name.value;
    }
    // Skip to end of entity header (looking for "is")
    skipToSemicolon(lexer);
}

void BSDLParser::parsePorts(BSDLLexer& lexer, BSDLDevice& dev) {
    // Parse port list: port ( name : direction type ; ... )
    Token tok = lexer.nextToken();
    if (tok.type != TokenType::LPAREN) return;

    while (true) {
        tok = lexer.nextToken();
        if (tok.type == TokenType::RPAREN || tok.type == TokenType::END_OF_FILE)
            break;

        if (tok.type == TokenType::IDENTIFIER) {
            std::string pin_name = tok.value;

            // Handle indexed names like MIO(0), DDR_A(3), etc.
            // Format: NAME(index) : direction type ;
            if (lexer.peekToken().type == TokenType::LPAREN) {
                lexer.nextToken(); // consume '('
                std::string index_str;
                while (lexer.peekToken().type != TokenType::RPAREN &&
                       lexer.peekToken().type != TokenType::END_OF_FILE) {
                    index_str += lexer.nextToken().value;
                }
                if (lexer.peekToken().type == TokenType::RPAREN) {
                    lexer.nextToken(); // consume ')'
                }
                pin_name = pin_name + "(" + index_str + ")";
            }

            // Could be comma-separated list of pin names
            std::vector<std::string> names = {pin_name};
            while (lexer.peekToken().type == TokenType::COMMA) {
                lexer.nextToken(); // consume comma
                tok = lexer.nextToken();
                if (tok.type == TokenType::IDENTIFIER) {
                    std::string extra = tok.value;
                    // Handle indexed names in list too
                    if (lexer.peekToken().type == TokenType::LPAREN) {
                        lexer.nextToken(); // consume '('
                        std::string idx;
                        while (lexer.peekToken().type != TokenType::RPAREN &&
                               lexer.peekToken().type != TokenType::END_OF_FILE) {
                            idx += lexer.nextToken().value;
                        }
                        if (lexer.peekToken().type == TokenType::RPAREN) lexer.nextToken();
                        extra = extra + "(" + idx + ")";
                    }
                    names.push_back(extra);
                }
            }

            // Expect colon
            tok = lexer.nextToken();
            if (tok.type != TokenType::COLON) continue;

            // Read direction
            tok = lexer.nextToken();
            PinDirection dir = PinDirection::IN;
            if (tok.type == TokenType::IDENTIFIER) {
                if (tok.value == "IN") dir = PinDirection::IN;
                else if (tok.value == "OUT") dir = PinDirection::OUT;
                else if (tok.value == "INOUT") dir = PinDirection::INOUT;
                else if (tok.value == "BUFFER") dir = PinDirection::BUFFER;
                else if (tok.value == "LINKAGE") dir = PinDirection::LINKAGE;
            }

            // Create pin entries
            for (const auto& n : names) {
                PinInfo pin;
                pin.name = n;
                pin.direction = dir;
                dev.pins[n] = pin;
            }

            // Skip type declaration to the terminating ';' or port-list ')'
            // Track nesting depth so BIT_VECTOR(0 TO 53) doesn't fool us.
            {
                int depth = 0;
                while (true) {
                    tok = lexer.nextToken();
                    if (tok.type == TokenType::END_OF_FILE) break;
                    if (tok.type == TokenType::LPAREN)  { depth++; continue; }
                    if (tok.type == TokenType::RPAREN)  {
                        if (depth > 0) { depth--; continue; }
                        break; // depth==0: this ')' closes the port list
                    }
                    if (tok.type == TokenType::SEMICOLON && depth == 0) break;
                }
            }
            // If we hit the closing paren, we're done with ports
            if (tok.type == TokenType::RPAREN) break;
        }
    }

    // Consume the trailing semicolon after the closing paren: );
    if (lexer.peekToken().type == TokenType::SEMICOLON) {
        lexer.nextToken();
    }
}

void BSDLParser::parseAttribute(BSDLLexer& lexer, BSDLDevice& dev) {
    Token attr_name = lexer.nextToken();
    if (attr_name.type != TokenType::IDENTIFIER) return;

    std::string name = attr_name.value;

    // Check for attribute declaration vs. attribute specification
    Token next = lexer.nextToken();
    if (next.type == TokenType::IDENTIFIER && next.value == "OF") {
        // Attribute specification: attribute NAME of ENTITY: entity is VALUE;
        // Skip entity name and "entity is"
        while (true) {
            Token t = lexer.nextToken();
            if (t.type == TokenType::END_OF_FILE) return;
            if (t.type == TokenType::IDENTIFIER && t.value == "IS") break;
        }

        if (name == "INSTRUCTION_LENGTH") {
            parseInstructionLength(lexer, dev);
        } else if (name == "INSTRUCTION_OPCODE") {
            parseInstructionOpcode(lexer, dev);
        } else if (name == "BOUNDARY_LENGTH") {
            parseBoundaryLength(lexer, dev);
        } else if (name == "BOUNDARY_REGISTER") {
            parseBoundaryRegister(lexer, dev);
        } else if (name == "IDCODE_REGISTER") {
            parseIdcodeRegister(lexer, dev);
        } else if (name == "PIN_MAP" || name == "PIN_MAP_STRING") {
            parsePinMap(lexer, dev);
        } else {
            skipToSemicolon(lexer);
        }
    } else {
        // Attribute declaration (just a type definition) — skip
        skipToSemicolon(lexer);
    }
}

void BSDLParser::parseInstructionLength(BSDLLexer& lexer, BSDLDevice& dev) {
    Token tok = lexer.nextToken();
    if (tok.type == TokenType::INTEGER) {
        dev.instruction_length = std::stoi(tok.value);
    }
    skipToSemicolon(lexer);
}

void BSDLParser::parseInstructionOpcode(BSDLLexer& lexer, BSDLDevice& dev) {
    // Format: "INSTR_NAME (binary_code), ..." & "..."
    std::string content = readConcatenatedString(lexer);

    // Parse instruction entries from the concatenated string
    // Format: INSTR_NAME (binary), INSTR_NAME (binary), ...
    size_t pos = 0;
    while (pos < content.size()) {
        // Skip whitespace and commas
        while (pos < content.size() &&
               (content[pos] == ' ' || content[pos] == '\t' ||
                content[pos] == '\n' || content[pos] == '\r' ||
                content[pos] == ',')) {
            pos++;
        }
        if (pos >= content.size()) break;

        // Read instruction name
        std::string instr_name;
        while (pos < content.size() && content[pos] != ' ' &&
               content[pos] != '\t' && content[pos] != '(') {
            instr_name += static_cast<char>(std::toupper(content[pos]));
            pos++;
        }
        if (instr_name.empty()) break;

        // Find opening paren
        while (pos < content.size() && content[pos] != '(') pos++;
        if (pos >= content.size()) break;
        pos++; // skip (

        // Read binary code
        std::string binary;
        while (pos < content.size() && content[pos] != ')') {
            if (content[pos] == '0' || content[pos] == '1') {
                binary += content[pos];
            }
            pos++;
        }
        if (pos < content.size()) pos++; // skip )

        // Convert binary to uint32_t
        // BSDL binary strings are MSB-first, but JTAG shifts LSB-first.
        // Store with bit 0 = LSB (rightmost bit of binary string).
        if (!binary.empty()) {
            uint32_t opcode = 0;
            for (size_t i = 0; i < binary.size(); i++) {
                if (binary[i] == '1') {
                    opcode |= (1u << (binary.size() - 1 - i));
                }
            }
            dev.instructions[instr_name] = opcode;
        }
    }

    skipToSemicolon(lexer);
}

void BSDLParser::parseBoundaryLength(BSDLLexer& lexer, BSDLDevice& dev) {
    Token tok = lexer.nextToken();
    if (tok.type == TokenType::INTEGER) {
        int len = 0;
        try { len = std::stoi(tok.value); } catch (...) {}
        if (len > 0 && len <= kMaxBsrBits) {
            dev.boundary_length = len;
            dev.boundary_cells.resize(static_cast<size_t>(len));
        }
    }
    skipToSemicolon(lexer);
}

void BSDLParser::parseBoundaryRegister(BSDLLexer& lexer, BSDLDevice& dev) {
    // Format (concatenated strings):
    // "pos (cell_type, pin_name, function, safe [, disabled_by, disable_val, result]),"
    std::string content = readConcatenatedString(lexer);

    // Ensure boundary_cells is sized
    if (dev.boundary_cells.empty() && dev.boundary_length > 0) {
        dev.boundary_cells.resize(dev.boundary_length);
    }

    // Parse each cell entry
    size_t pos = 0;
    while (pos < content.size()) {
        // Skip whitespace and commas between entries
        while (pos < content.size() &&
               (content[pos] == ' ' || content[pos] == '\t' ||
                content[pos] == '\n' || content[pos] == '\r' ||
                content[pos] == ',')) {
            pos++;
        }
        if (pos >= content.size()) break;

        // Read cell position number
        std::string pos_str;
        while (pos < content.size() && std::isdigit(content[pos])) {
            pos_str += content[pos++];
        }
        if (pos_str.empty()) {
            pos++;
            continue;
        }

        int cell_pos = std::stoi(pos_str);

        // Find opening paren
        while (pos < content.size() && content[pos] != '(') pos++;
        if (pos >= content.size()) break;
        pos++; // skip (

        // Parse cell fields separated by commas, ending at )
        auto readField = [&]() -> std::string {
            std::string field;
            while (pos < content.size() && content[pos] != ',' &&
                   content[pos] != ')') {
                if (content[pos] != ' ' && content[pos] != '\t' &&
                    content[pos] != '\n' && content[pos] != '\r') {
                    field += static_cast<char>(std::toupper(content[pos]));
                }
                pos++;
            }
            if (pos < content.size() && content[pos] == ',') pos++; // skip comma
            return field;
        };

        std::string cell_type = readField();
        std::string pin_name = readField();
        std::string function_str = readField();
        std::string safe_str = readField();

        // Optional: disabled_by, disable_value, disable_result
        std::string disabled_by_str;
        std::string disable_val_str;
        std::string disable_result_str;

        if (pos < content.size() && content[pos] != ')') {
            disabled_by_str = readField();
        }
        if (pos < content.size() && content[pos] != ')') {
            disable_val_str = readField();
        }
        if (pos < content.size() && content[pos] != ')') {
            disable_result_str = readField();
        }

        // Skip to closing paren
        while (pos < content.size() && content[pos] != ')') pos++;
        if (pos < content.size()) pos++; // skip )

        // Build BoundaryCell
        BoundaryCell cell;
        cell.position = cell_pos;
        cell.cell_type = cell_type;
        cell.pin_name = pin_name;
        cell.function = parseCellFunction(function_str);

        if (safe_str == "0") cell.safe_value = 0;
        else if (safe_str == "1") cell.safe_value = 1;
        else cell.safe_value = -1;  // X

        if (!disabled_by_str.empty() && disabled_by_str != "X") {
            try { cell.disabled_by = std::stoi(disabled_by_str); }
            catch (...) { cell.disabled_by = -1; }
        }

        if (disable_val_str == "0") cell.disable_value = 0;
        else if (disable_val_str == "1") cell.disable_value = 1;

        cell.disable_result = parseDisableResult(disable_result_str);

        // Store in vector by position (guard against malicious BSDL)
        if (cell_pos >= 0 && cell_pos < static_cast<int>(dev.boundary_cells.size())) {
            dev.boundary_cells[cell_pos] = cell;
        } else if (cell_pos >= static_cast<int>(dev.boundary_cells.size()) &&
                   cell_pos < kMaxBsrBits) {
            dev.boundary_cells.resize(static_cast<size_t>(cell_pos) + 1);
            dev.boundary_cells[cell_pos] = cell;
            dev.boundary_length = static_cast<int>(dev.boundary_cells.size());
        }
    }

    skipToSemicolon(lexer);
}

void BSDLParser::parseIdcodeRegister(BSDLLexer& lexer, BSDLDevice& dev) {
    std::string content = readConcatenatedString(lexer);

    // Content is a 32-bit binary string: "VVVV PPPPPPPPPPPPPPPP MMMMMMMMMMM 1"
    // Remove whitespace
    std::string binary;
    for (char c : content) {
        if (c == '0' || c == '1') binary += c;
    }

    if (binary.size() == 32) {
        uint32_t raw = 0;
        for (int i = 0; i < 32; i++) {
            raw <<= 1;
            if (binary[i] == '1') raw |= 1;
        }
        dev.idcode.raw = raw;
        dev.idcode.version = (raw >> 28) & 0xF;
        dev.idcode.part_number = (raw >> 12) & 0xFFFF;
        dev.idcode.manufacturer = (raw >> 1) & 0x7FF;
    }

    skipToSemicolon(lexer);
}

void BSDLParser::parsePinMap(BSDLLexer& lexer, BSDLDevice& dev) {
    std::string content = readConcatenatedString(lexer);

    // Format: "PIN_NAME:number, PIN_NAME:number, ..."
    // or "PIN_NAME:(num1, num2, ...)" for multi-pin signals
    size_t pos = 0;
    while (pos < content.size()) {
        // Skip whitespace and commas
        while (pos < content.size() &&
               (content[pos] == ' ' || content[pos] == '\t' ||
                content[pos] == '\n' || content[pos] == '\r' ||
                content[pos] == ',')) {
            pos++;
        }
        if (pos >= content.size()) break;

        // Read pin name
        std::string pin_name;
        while (pos < content.size() && content[pos] != ':' &&
               content[pos] != ' ' && content[pos] != '\t') {
            pin_name += static_cast<char>(std::toupper(content[pos]));
            pos++;
        }

        // Find colon
        while (pos < content.size() && content[pos] != ':') pos++;
        if (pos >= content.size()) break;
        pos++; // skip :

        // Skip whitespace
        while (pos < content.size() &&
               (content[pos] == ' ' || content[pos] == '\t')) {
            pos++;
        }

        // Read pin number(s)
        if (pos < content.size() && content[pos] == '(') {
            // Multi-pin: skip for now (vector pins)
            while (pos < content.size() && content[pos] != ')') pos++;
            if (pos < content.size()) pos++;
        } else {
            // Single pin number
            std::string num_str;
            while (pos < content.size() && (std::isdigit(content[pos]) ||
                   std::isalpha(content[pos]))) {
                num_str += content[pos++];
            }

            if (!pin_name.empty() && !num_str.empty()) {
                // Store signal_name -> package_pin_designator for XDC alias composition.
                // Uppercase the package pin to match XDC PACKAGE_PIN convention.
                std::string pkg_pin_upper = num_str;
                for (char& c : pkg_pin_upper)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                dev.package_pin_map[pin_name] = pkg_pin_upper;

                // Also try to store numeric physical_pin for backward compat.
                auto it = dev.pins.find(pin_name);
                if (it != dev.pins.end()) {
                    try { it->second.physical_pin = std::stoi(num_str); }
                    catch (...) {}
                }
            }
        }
    }

    skipToSemicolon(lexer);
}

std::string BSDLParser::readConcatenatedString(BSDLLexer& lexer) {
    // Read a value that may be a string literal or concatenation of strings
    // "string1" & "string2" & ...
    // or just a simple value

    std::string result;

    Token tok = lexer.peekToken();
    if (tok.type == TokenType::STRING_LITERAL) {
        // Concatenated string(s)
        while (true) {
            tok = lexer.peekToken();
            if (tok.type == TokenType::STRING_LITERAL) {
                lexer.nextToken(); // consume
                result += tok.value;
            } else if (tok.type == TokenType::AMPERSAND) {
                lexer.nextToken(); // consume &
                continue;
            } else {
                break; // end of concatenation (don't consume terminator)
            }
        }
    }

    return result;
}

void BSDLParser::skipToSemicolon(BSDLLexer& lexer) {
    while (true) {
        Token tok = lexer.nextToken();
        if (tok.type == TokenType::SEMICOLON ||
            tok.type == TokenType::END_OF_FILE) {
            break;
        }
    }
}

void BSDLParser::expectToken(BSDLLexer& lexer, TokenType type) {
    Token tok = lexer.nextToken();
    if (tok.type != type) {
        setError("Expected token type, got: " + tok.value, tok.line);
    }
}

Token BSDLParser::expectIdentifier(BSDLLexer& lexer) {
    Token tok = lexer.nextToken();
    if (tok.type != TokenType::IDENTIFIER) {
        setError("Expected identifier, got: " + tok.value, tok.line);
    }
    return tok;
}

CellFunction BSDLParser::parseCellFunction(const std::string& name) {
    if (name == "INPUT") return CellFunction::INPUT;
    if (name == "OUTPUT2") return CellFunction::OUTPUT2;
    if (name == "OUTPUT3") return CellFunction::OUTPUT3;
    if (name == "CONTROL") return CellFunction::CONTROL;
    if (name == "BIDIR") return CellFunction::BIDIR;
    if (name == "CLOCK") return CellFunction::CLOCK;
    if (name == "INTERNAL") return CellFunction::INTERNAL;
    return CellFunction::INPUT;  // Default
}

DisableResult BSDLParser::parseDisableResult(const std::string& name) {
    if (name == "PULL0") return DisableResult::PULL0;
    if (name == "PULL1") return DisableResult::PULL1;
    if (name == "HIGHZ" || name == "Z") return DisableResult::HIGHZ;
    if (name == "KEEPER") return DisableResult::KEEPER;
    return DisableResult::NONE;
}

} // namespace jtag::bsdl

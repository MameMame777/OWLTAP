#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jtag::bsdl {

/// Token types for BSDL lexer
enum class TokenType {
    // Literals
    IDENTIFIER,     // keywords and identifiers
    STRING_LITERAL, // "..."
    INTEGER,        // decimal integer
    BIT_STRING,     // binary string like "001101"

    // Punctuation
    LPAREN,         // (
    RPAREN,         // )
    COMMA,          // ,
    SEMICOLON,      // ;
    COLON,          // :
    AMPERSAND,      // &
    DOT,            // .
    ASSIGN,         // :=

    // Special
    END_OF_FILE,
    ERROR,
};

/// A single lexical token
struct Token {
    TokenType type = TokenType::ERROR;
    std::string value;
    int line = 0;
    int column = 0;
};

/// Tokenizer for BSDL files (VHDL subset).
/// Handles:
///   - VHDL-style comments: -- to end of line
///   - String literals: "..."
///   - Identifiers (case-insensitive)
///   - Integer literals
///   - Punctuation: ( ) , ; : & .
///   - Assignment: :=
class BSDLLexer {
public:
    /// Initialize lexer with BSDL source text.
    explicit BSDLLexer(const std::string& source);

    /// Get the next token.
    Token nextToken();

    /// Peek at the next token without consuming it.
    Token peekToken();

    /// Get current line number (1-based).
    int currentLine() const { return line_; }

    /// Get current column (1-based).
    int currentColumn() const { return column_; }

private:
    void skipWhitespace();
    void skipComment();
    Token readIdentifier();
    Token readStringLiteral();
    Token readInteger();
    Token makeToken(TokenType type, const std::string& value);

    char peek() const;
    char advance();
    bool isAtEnd() const;

    std::string source_;
    size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;

    bool has_peeked_ = false;
    Token peeked_token_;
};

} // namespace jtag::bsdl

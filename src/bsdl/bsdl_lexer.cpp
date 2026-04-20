#include "bsdl_lexer.h"

#include <algorithm>
#include <cctype>

namespace jtag::bsdl {

BSDLLexer::BSDLLexer(const std::string& source) : source_(source) {}

char BSDLLexer::peek() const {
    if (isAtEnd()) return '\0';
    return source_[pos_];
}

char BSDLLexer::advance() {
    char c = source_[pos_++];
    if (c == '\n') {
        line_++;
        column_ = 1;
    } else {
        column_++;
    }
    return c;
}

bool BSDLLexer::isAtEnd() const {
    return pos_ >= source_.size();
}

Token BSDLLexer::makeToken(TokenType type, const std::string& value) {
    Token t;
    t.type = type;
    t.value = value;
    t.line = line_;
    t.column = column_;
    return t;
}

void BSDLLexer::skipWhitespace() {
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '-' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '-') {
            skipComment();
        } else {
            break;
        }
    }
}

void BSDLLexer::skipComment() {
    // Skip -- to end of line
    while (!isAtEnd() && peek() != '\n') {
        advance();
    }
}

Token BSDLLexer::readIdentifier() {
    int start_line = line_;
    int start_col = column_;
    std::string value;

    while (!isAtEnd()) {
        char c = peek();
        if (std::isalnum(c) || c == '_') {
            value += advance();
        } else {
            break;
        }
    }

    // BSDL identifiers are case-insensitive; normalize to uppercase
    std::transform(value.begin(), value.end(), value.begin(), ::toupper);

    Token t;
    t.type = TokenType::IDENTIFIER;
    t.value = value;
    t.line = start_line;
    t.column = start_col;
    return t;
}

Token BSDLLexer::readStringLiteral() {
    int start_line = line_;
    int start_col = column_;

    advance(); // consume opening "
    std::string value;

    while (!isAtEnd()) {
        char c = peek();
        if (c == '"') {
            advance(); // consume closing "
            // Check for escaped quote ("")
            if (!isAtEnd() && peek() == '"') {
                value += '"';
                advance();
                continue;
            }
            Token t;
            t.type = TokenType::STRING_LITERAL;
            t.value = value;
            t.line = start_line;
            t.column = start_col;
            return t;
        }
        value += advance();
    }

    // Unterminated string literal
    Token t;
    t.type = TokenType::ERROR;
    t.value = "Unterminated string literal";
    t.line = start_line;
    t.column = start_col;
    return t;
}

Token BSDLLexer::readInteger() {
    int start_line = line_;
    int start_col = column_;
    std::string value;

    while (!isAtEnd() && std::isdigit(peek())) {
        value += advance();
    }

    Token t;
    t.type = TokenType::INTEGER;
    t.value = value;
    t.line = start_line;
    t.column = start_col;
    return t;
}

Token BSDLLexer::nextToken() {
    if (has_peeked_) {
        has_peeked_ = false;
        return peeked_token_;
    }

    skipWhitespace();

    if (isAtEnd()) {
        return makeToken(TokenType::END_OF_FILE, "");
    }

    char c = peek();

    // Identifiers and keywords
    if (std::isalpha(c) || c == '_') {
        return readIdentifier();
    }

    // String literals
    if (c == '"') {
        return readStringLiteral();
    }

    // Integer literals
    if (std::isdigit(c)) {
        return readInteger();
    }

    // Punctuation
    advance();
    switch (c) {
        case '(': return makeToken(TokenType::LPAREN, "(");
        case ')': return makeToken(TokenType::RPAREN, ")");
        case ',': return makeToken(TokenType::COMMA, ",");
        case ';': return makeToken(TokenType::SEMICOLON, ";");
        case '&': return makeToken(TokenType::AMPERSAND, "&");
        case '.': return makeToken(TokenType::DOT, ".");
        case ':':
            if (!isAtEnd() && peek() == '=') {
                advance();
                return makeToken(TokenType::ASSIGN, ":=");
            }
            return makeToken(TokenType::COLON, ":");
        case '*':
            // Asterisk used as pin name placeholder
            return makeToken(TokenType::IDENTIFIER, "*");
        default:
            return makeToken(TokenType::ERROR,
                             std::string("Unexpected character: ") + c);
    }
}

Token BSDLLexer::peekToken() {
    if (!has_peeked_) {
        peeked_token_ = nextToken();
        has_peeked_ = true;
    }
    return peeked_token_;
}

} // namespace jtag::bsdl

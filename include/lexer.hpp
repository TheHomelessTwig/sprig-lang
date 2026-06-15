#pragma once
#include <string>
#include <vector>

// Every distinct token kind the lexer can produce.
enum class TokenType {
    // Literals
    NUMBER,
    STRING,
    IDENTIFIER,

    // Declaration keywords
    LET,
    DEFINE,
    SHAPE,
    MUTABLE,
    BORROW,
    OWN,
    INCLUDE,
    UNSAFE,

    // Control flow keywords
    WHEN,
    ELSE,
    WHILE,   // "as long as"
    FOR,
    EACH,
    IN,
    STOP,    // break
    SKIP,    // continue
    RETURN,  // "give back"

    // Value keywords
    TRUE,
    FALSE,
    NOTHING,

    // Logical operators
    AND,
    OR,
    NOT,
    IS,

    // Built-in type names (used in shape field declarations)
    TYPE_TEXT,
    TYPE_NUMBER,
    TYPE_DECIMAL,
    TYPE_FLAG,

    // Arithmetic / comparison operators
    PLUS,
    MINUS,
    STAR,
    SLASH,
    ASSIGN,   // =
    EQ,       // ==
    NEQ,      // !=
    LT,       // <
    GT,       // >
    LTE,      // <=
    GTE,      // >=

    // Delimiters
    LPAREN,
    RPAREN,
    LBRACE,
    RBRACE,
    LBRACKET,
    RBRACKET,
    COMMA,
    COLON,
    DOT,

    // Structural tokens (indent-sensitive grammar)
    NEWLINE,
    INDENT,
    DEDENT,

    // Meta
    EOF_TOKEN,
    ILLEGAL,
};

struct Token {
    TokenType   type;
    std::string lexeme;
    int         line;
    int         col;
    Token(TokenType token_type, std::string lexeme_str, int line_number, int column = 0)
        : type(token_type), lexeme(std::move(lexeme_str)), line(line_number), col(column) {}
};

class Lexer {
    std::string source;
    int start     = 0;
    int current   = 0;
    int line      = 1;
    int col       = 0;
    int start_col = 0;

public:
    Lexer(std::string src);
    std::vector<Token> tokenize();

private:
    // Character navigation
    bool at_end();
    char advance();
    char peek();
    char peek_next();
    bool match(char expected);

    // Token emission
    void add(std::vector<Token>& tokens, TokenType type);

    // Scanners
    void scan_token(std::vector<Token>& tokens);
    void scan_string(std::vector<Token>& tokens);
    void scan_number(std::vector<Token>& tokens);
    void scan_identifier(std::vector<Token>& tokens);
};

inline std::string token_type_name(TokenType type) {
    switch (type) {
        // Literals
        case TokenType::NUMBER:     return "NUMBER";
        case TokenType::STRING:     return "STRING";
        case TokenType::IDENTIFIER: return "IDENTIFIER";

        // Declaration keywords
        case TokenType::LET:     return "LET";
        case TokenType::DEFINE:  return "DEFINE";
        case TokenType::SHAPE:   return "SHAPE";
        case TokenType::MUTABLE: return "MUTABLE";
        case TokenType::BORROW:  return "BORROW";
        case TokenType::OWN:     return "OWN";
        case TokenType::INCLUDE: return "INCLUDE";
        case TokenType::UNSAFE:  return "UNSAFE";

        // Control flow keywords
        case TokenType::WHEN:   return "WHEN";
        case TokenType::ELSE:   return "ELSE";
        case TokenType::WHILE:  return "WHILE";
        case TokenType::FOR:    return "FOR";
        case TokenType::EACH:   return "EACH";
        case TokenType::IN:     return "IN";
        case TokenType::STOP:   return "STOP";
        case TokenType::SKIP:   return "SKIP";
        case TokenType::RETURN: return "RETURN";

        // Value keywords
        case TokenType::TRUE:    return "TRUE";
        case TokenType::FALSE:   return "FALSE";
        case TokenType::NOTHING: return "NOTHING";

        // Logical operators
        case TokenType::AND: return "AND";
        case TokenType::OR:  return "OR";
        case TokenType::NOT: return "NOT";
        case TokenType::IS:  return "IS";

        // Type names
        case TokenType::TYPE_TEXT:    return "TYPE_TEXT";
        case TokenType::TYPE_NUMBER:  return "TYPE_NUMBER";
        case TokenType::TYPE_DECIMAL: return "TYPE_DECIMAL";
        case TokenType::TYPE_FLAG:    return "TYPE_FLAG";

        // Operators
        case TokenType::PLUS:   return "PLUS";
        case TokenType::MINUS:  return "MINUS";
        case TokenType::STAR:   return "STAR";
        case TokenType::SLASH:  return "SLASH";
        case TokenType::ASSIGN: return "ASSIGN";
        case TokenType::EQ:     return "EQ";
        case TokenType::NEQ:    return "NEQ";
        case TokenType::LT:     return "LT";
        case TokenType::GT:     return "GT";
        case TokenType::LTE:    return "LTE";
        case TokenType::GTE:    return "GTE";

        // Delimiters
        case TokenType::LPAREN:   return "LPAREN";
        case TokenType::RPAREN:   return "RPAREN";
        case TokenType::LBRACE:   return "LBRACE";
        case TokenType::RBRACE:   return "RBRACE";
        case TokenType::LBRACKET: return "LBRACKET";
        case TokenType::RBRACKET: return "RBRACKET";
        case TokenType::COMMA:    return "COMMA";
        case TokenType::COLON:    return "COLON";
        case TokenType::DOT:      return "DOT";

        // Structural
        case TokenType::NEWLINE: return "NEWLINE";
        case TokenType::INDENT:  return "INDENT";
        case TokenType::DEDENT:  return "DEDENT";

        // Meta
        case TokenType::EOF_TOKEN: return "EOF";
        case TokenType::ILLEGAL:   return "ILLEGAL";
    }
    return "UNKNOWN";
}

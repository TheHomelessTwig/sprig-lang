#pragma once
#include <string>
#include <vector>

enum class TokenType {
    NUMBER,
    STRING,
    IDENTIFIER,
    LET,
    DEFINE,
    WHEN,
    ELSE,
    RETURN,
    TRUE,
    FALSE,
    WHILE,
    NOTHING,
    PLUS,
    MINUS,
    STAR,
    SLASH,
    ASSIGN,
    EQ,
    NEQ,
    LT,
    GT,
    LPAREN,
    RPAREN,
    LBRACE,
    RBRACE,
    COMMA,
    COLON,
    NEWLINE,
    INDENT,
    DEDENT,
    EOF_TOKEN,
    ILLEGAL,
    AND,
    OR,
    NOT,
    MUTABLE,
    EACH,
    FOR,
    IN,
    STOP,
    SKIP,
    SHAPE,
    INCLUDE,
    IS,
    TYPE_TEXT,
    TYPE_NUMBER,
    TYPE_DECIMAL,
    TYPE_FLAG,
    LBRACKET,
    RBRACKET,
};

struct Token {
    TokenType type;
    std::string lexeme;
    int line;
    int col;
    Token(TokenType t, std::string lex, int ln, int c = 0)
        : type(t), lexeme(std::move(lex)), line(ln), col(c) {}
};

class Lexer {
    std::string source;
    int start = 0;
    int current = 0;
    int line = 1;
    int col = 0;
    int start_col = 0;

public:
    Lexer(std::string src);
    std::vector<Token> tokenize();

private:
    bool at_end();
    char advance();
    char peek();
    char peek_next();
    bool match(char expected);
    void add(std::vector<Token>& tokens, TokenType type);
    void scan_token(std::vector<Token>& tokens);
    void scan_string(std::vector<Token>& tokens);
    void scan_number(std::vector<Token>& tokens);
    void scan_identifier(std::vector<Token>& tokens);
};

inline std::string token_type_name(TokenType t) {
    switch (t) {
        case TokenType::NUMBER:
            return "NUMBER";
        case TokenType::STRING:
            return "STRING";
        case TokenType::IDENTIFIER:
            return "IDENTIFIER";
        case TokenType::LET:
            return "LET";
        case TokenType::DEFINE:
            return "DEFINE";
        case TokenType::WHEN:
            return "WHEN";
        case TokenType::ELSE:
            return "ELSE";
        case TokenType::RETURN:
            return "RETURN";
        case TokenType::TRUE:
            return "TRUE";
        case TokenType::FALSE:
            return "FALSE";
        case TokenType::WHILE:
            return "WHILE";
        case TokenType::NOTHING:
            return "NOTHING";
        case TokenType::PLUS:
            return "PLUS";
        case TokenType::MINUS:
            return "MINUS";
        case TokenType::STAR:
            return "STAR";
        case TokenType::SLASH:
            return "SLASH";
        case TokenType::ASSIGN:
            return "ASSIGN";
        case TokenType::EQ:
            return "EQ";
        case TokenType::NEQ:
            return "NEQ";
        case TokenType::LT:
            return "LT";
        case TokenType::GT:
            return "GT";
        case TokenType::LPAREN:
            return "LPAREN";
        case TokenType::RPAREN:
            return "RPAREN";
        case TokenType::LBRACE:
            return "LBRACE";
        case TokenType::RBRACE:
            return "RBRACE";
        case TokenType::COMMA:
            return "COMMA";
        case TokenType::COLON:
            return "COLON";
        case TokenType::NEWLINE:
            return "NEWLINE";
        case TokenType::INDENT:
            return "INDENT";
        case TokenType::DEDENT:
            return "DEDENT";
        case TokenType::EOF_TOKEN:
            return "EOF";
        case TokenType::ILLEGAL:
            return "ILLEGAL";
        case TokenType::AND:
            return "AND";
        case TokenType::OR:
            return "OR";
        case TokenType::NOT:
            return "NOT";
        case TokenType::MUTABLE:
            return "MUTABLE";
        case TokenType::EACH:
            return "EACH";
        case TokenType::FOR:
            return "FOR";
        case TokenType::IN:
            return "IN";
        case TokenType::STOP:
            return "STOP";
        case TokenType::SKIP:
            return "SKIP";
        case TokenType::SHAPE:
            return "SHAPE";
        case TokenType::INCLUDE:
            return "INCLUDE";
        case TokenType::IS:
            return "IS";
        case TokenType::TYPE_TEXT:
            return "TYPE_TEXT";
        case TokenType::TYPE_NUMBER:
            return "TYPE_NUMBER";
        case TokenType::TYPE_DECIMAL:
            return "TYPE_DECIMAL";
        case TokenType::TYPE_FLAG:
            return "TYPE_FLAG";
        case TokenType::LBRACKET:
            return "LBRACKET";
        case TokenType::RBRACKET:
            return "RBRACKET";
    }
    return "UNKNOWN";
}

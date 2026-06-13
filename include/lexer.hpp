#pragma once
#include <string>
#include <vector>

enum class TokenType {
  // Literals
  NUMBER,     // 42, 3.14
  STRING,     // "hello"
  IDENTIFIER, // x, foo, myVar

  // Keywords
  VARIABLE, // allow
  FUNCTION, // module
  IF,       // given
  ELSE,     // otherwise
  RETURN,   // output
  TRUE,     // true
  FALSE,    // FALSE
  WHILE,    // as long as

  // Operators
  PLUS,   // +
  MINUS,  // -
  STAR,   // *
  SLASH,  // /
  ASSIGN, // =
  EQ,     // ==
  NEQ,    // !=
  LT,     //
  GT,     // >

  // Delimiters
  LPAREN, // (
  RPAREN, // )
  LBRACE, // {
  RBRACE, // }
  COMMA,  // ,

  // Special
  NEWLINE,   // \n (statement separator)
  EOF_TOKEN, // end of input
  ILLEGAL,   // unrecognised character
};

struct Token {
  TokenType type;
  std::string lexeme; // the raw text: "42", "let", "foo"
  int line;           // for error messages later

  Token(TokenType t, std::string lex, int ln)
      : type(t), lexeme(std::move(lex)), line(ln) {}
};

// Declare class
class Lexer {
  std::string source;
  int start = 0;
  int current = 0;
  int line = 0;

public:
  Lexer(std::string src);
  std::vector<Token> tokenize();

private:
  bool at_end();
  char advance();
  char peek();
  char peek_next();
  bool match(char expected);
  void add(std::vector<Token> &tokens, TokenType type);
  void scan_token(std::vector<Token> &tokens);
  void scan_string(std::vector<Token> &tokens);
  void scan_number(std::vector<Token> &tokens);
  void scan_identifier(std::vector<Token> &tokens);
};

inline std::string token_type_name(TokenType t) {
  switch (t) {
  case TokenType::NUMBER:
    return "NUMBER";
  case TokenType::STRING:
    return "STRING";
  case TokenType::IDENTIFIER:
    return "IDENTIFIER";
  case TokenType::VARIABLE:
    return "VARIABLE";
  case TokenType::FUNCTION:
    return "FUNCTION";
  case TokenType::IF:
    return "IF";
  case TokenType::ELSE:
    return "ELSE";
  case TokenType::RETURN:
    return "RETURN";
  case TokenType::TRUE:
    return "TRUE";
  case TokenType::FALSE:
    return "FALSE";
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
  case TokenType::NEWLINE:
    return "NEWLINE";
  case TokenType::EOF_TOKEN:
    return "EOF";
  case TokenType::ILLEGAL:
    return "ILLEGAL";
  case TokenType::WHILE:
    return "WHILE";
  }
  return "UNKNOWN";
}

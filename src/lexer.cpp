#include "lexer.hpp"
#include <cctype>
#include <stdexcept>
#include <unordered_map>

// Keyword lookup table — maps "let" -> VARIABLE, etc.
static const std::unordered_map<std::string, TokenType> KEYWORDS = {
    {"allow", TokenType::VARIABLE}, {"module", TokenType::FUNCTION},
    {"given", TokenType::IF},       {"otherwise", TokenType::ELSE},
    {"output", TokenType::RETURN},  {"true", TokenType::TRUE},
    {"false", TokenType::FALSE},
};

// Each entry: the word sequence, and what token to collapse it into
static const std::vector<std::pair<std::vector<std::string>, TokenType>>
    MULTIWORD = {
        {{"as", "long", "as"}, TokenType::WHILE},
        // add more here later, e.g:
        // {{"give", "back"}, TokenType::RETURN},
        // {{"otherwise"},    TokenType::ELSE},
};

static std::vector<Token> resolve_multiword(std::vector<Token> tokens) {
  std::vector<Token> out;
  int i = 0;
  while (i < (int)tokens.size()) {
    bool matched = false;
    for (auto &[words, type] : MULTIWORD) {
      // Check if the words starting at i match this pattern
      if (i + (int)words.size() > (int)tokens.size())
        continue;
      bool ok = true;
      for (int j = 0; j < (int)words.size(); j++) {
        if (tokens[i + j].type != TokenType::IDENTIFIER ||
            tokens[i + j].lexeme != words[j]) {
          ok = false;
          break;
        }
      }
      if (ok) {
        // Merge the matched tokens into one
        out.emplace_back(type, tokens[i].lexeme, tokens[i].line);
        i += words.size();
        matched = true;
        break;
      }
    }
    if (!matched)
      out.push_back(tokens[i++]);
  }
  return out;
}

Lexer::Lexer(std::string src) : source(std::move(src)) {}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> tokens;
  while (!at_end()) {
    start = current;
    scan_token(tokens);
  }
  tokens.emplace_back(TokenType::EOF_TOKEN, "", line);
  return resolve_multiword(tokens);
}

bool Lexer::at_end() { return current >= (int)source.size(); }
char Lexer::advance() { return source[current++]; }
char Lexer::peek() { return at_end() ? '\0' : source[current]; }
char Lexer::peek_next() {
  return (current + 1 >= (int)source.size()) ? '\0' : source[current + 1];
}

bool Lexer::match(char expected) {
  if (at_end() || source[current] != expected)
    return false;
  current++;
  return true;
}

void Lexer::add(std::vector<Token> &tokens, TokenType type) {
  tokens.emplace_back(type, source.substr(start, current - start), line);
}

void Lexer::scan_token(std::vector<Token> &tokens) {
  char c = advance();
  switch (c) {
  case '(':
    add(tokens, TokenType::LPAREN);
    break;
  case ')':
    add(tokens, TokenType::RPAREN);
    break;
  case '{':
    add(tokens, TokenType::LBRACE);
    break;
  case '}':
    add(tokens, TokenType::RBRACE);
    break;
  case ',':
    add(tokens, TokenType::COMMA);
    break;
  case '+':
    add(tokens, TokenType::PLUS);
    break;
  case '-':
    add(tokens, TokenType::MINUS);
    break;
  case '*':
    add(tokens, TokenType::STAR);
    break;
  case '/':
    add(tokens, TokenType::SLASH);
    break;
  case '=':
    add(tokens, match('=') ? TokenType::EQ : TokenType::ASSIGN);
    break;
  case '!':
    add(tokens, match('=') ? TokenType::NEQ : TokenType::ILLEGAL);
    break;
  case '<':
    add(tokens, TokenType::LT);
    break;
  case '>':
    add(tokens, TokenType::GT);
    break;
  case ' ':
  case '\r':
  case '\t':
    break;
  case '\n':
    add(tokens, TokenType::NEWLINE);
    line++;
    break;
  case '"':
    scan_string(tokens);
    break;
  default:
    if (std::isdigit(c))
      scan_number(tokens);
    else if (std::isalpha(c) || c == '_')
      scan_identifier(tokens);
    else
      add(tokens, TokenType::ILLEGAL);
  }
}

void Lexer::scan_string(std::vector<Token> &tokens) {
  while (peek() != '"' && !at_end()) {
    if (peek() == '\n')
      line++;
    advance();
  }
  if (at_end())
    throw std::runtime_error("Unterminated string at line " +
                             std::to_string(line));
  advance();
  add(tokens, TokenType::STRING);
}

void Lexer::scan_number(std::vector<Token> &tokens) {
  while (std::isdigit(peek()))
    advance();
  if (peek() == '.' && std::isdigit(peek_next())) {
    advance();
    while (std::isdigit(peek()))
      advance();
  }
  add(tokens, TokenType::NUMBER);
}

void Lexer::scan_identifier(std::vector<Token> &tokens) {
  while (std::isalnum(peek()) || peek() == '_')
    advance();
  std::string word = source.substr(start, current - start);
  auto it = KEYWORDS.find(word);
  TokenType type = (it != KEYWORDS.end()) ? it->second : TokenType::IDENTIFIER;
  add(tokens, type);
}

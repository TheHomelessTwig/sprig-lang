#include "parser.hpp"
#include "ast.hpp"
#include <stdexcept>

Parser::Parser(std::vector<Token> toks) : tokens(std::move(toks)) {}

// ── Navigation helpers
// ────────────────────────────────────────────────────────

Token &Parser::peek() { return tokens[current]; }
Token &Parser::previous() { return tokens[current - 1]; }
bool Parser::at_end() { return peek().type == TokenType::EOF_TOKEN; }

Token &Parser::advance() {
  if (!at_end())
    current++;
  return previous();
}
bool Parser::check(TokenType t) { return !at_end() && peek().type == t; }
bool Parser::match(TokenType t) {
  if (check(t)) {
    advance();
    return true;
  }
  return false;
}
Token Parser::expect(TokenType t, const std::string &msg) {
  if (check(t))
    return advance();
  throw std::runtime_error(msg + " (got '" + peek().lexeme + "' at line " +
                           std::to_string(peek().line) + ")");
}
void Parser::skip_newlines() {
  while (match(TokenType::NEWLINE)) {
  }
}

// ── Program
// ───────────────────────────────────────────────────────────────────

Program Parser::parse() {
  Program program;
  skip_newlines();
  while (!at_end()) {
    program.stmts.push_back(statement());
    skip_newlines();
  }
  return program;
}

// ── Statements
// ────────────────────────────────────────────────────────────────

StatementPointer Parser::statement() {
  if (match(TokenType::VARIABLE))
    return variable_statement();
  if (match(TokenType::FUNCTION))
    return function_statement();
  if (match(TokenType::IF))
    return if_statement();
  if (match(TokenType::WHILE))
    return while_statement();
  if (match(TokenType::RETURN))
    return return_statement();
  return expression_statement();
}

StatementPointer Parser::variable_statement() {
  Token name = expect(TokenType::IDENTIFIER, "Expected name after 'variable'");
  expect(TokenType::ASSIGN, "Expected '='");
  ExpressionPointer val = expression();
  match(TokenType::NEWLINE);
  return std::make_unique<VariableStatement>(name.lexeme, std::move(val));
}

StatementPointer Parser::function_statement() {
  Token name = expect(TokenType::IDENTIFIER, "Expected function name");
  expect(TokenType::LPAREN, "Expected '('");
  std::vector<std::string> params;
  if (!check(TokenType::RPAREN)) {
    do {
      params.push_back(expect(TokenType::IDENTIFIER, "Expected param").lexeme);
    } while (match(TokenType::COMMA));
  }
  expect(TokenType::RPAREN, "Expected ')'");
  return std::make_unique<FunctionStatement>(name.lexeme, std::move(params),
                                             block());
}

StatementPointer Parser::if_statement() {
  ExpressionPointer cond = expression();
  Block then_b = block();
  std::optional<Block> else_b;
  skip_newlines();
  if (match(TokenType::ELSE))
    else_b = block();
  return std::make_unique<IfStatement>(std::move(cond), std::move(then_b),
                                       std::move(else_b));
}

StatementPointer Parser::while_statement() {
  ExpressionPointer cond = expression();
  Block body = block();
  return std::make_unique<WhileStatement>(std::move(cond), std::move(body));
}

StatementPointer Parser::return_statement() {
  ExpressionPointer val = expression();
  match(TokenType::NEWLINE);
  return std::make_unique<ReturnStatement>(std::move(val));
}

StatementPointer Parser::expression_statement() {
  ExpressionPointer e = expression();
  match(TokenType::NEWLINE);
  return std::make_unique<ExpressionStatement>(std::move(e));
}

Block Parser::block() {
  skip_newlines();
  expect(TokenType::LBRACE, "Expected '{'");
  skip_newlines();
  Block b;
  while (!check(TokenType::RBRACE) && !at_end()) {
    b.stmts.push_back(statement());
    skip_newlines();
  }
  expect(TokenType::RBRACE, "Expected '}'");
  return b;
}

// ── Expressions
// ─────────────────────────────────────────────────────────────── Each level
// calls the NEXT level first, then handles its own operators. This is what
// gives lower-precedence operators lower binding power.

ExpressionPointer Parser::expression() { return equality(); }

ExpressionPointer Parser::equality() {
  ExpressionPointer left = comparison();
  while (check(TokenType::EQ) || check(TokenType::NEQ)) {
    std::string op = advance().lexeme;
    left =
        std::make_unique<BinaryExpression>(std::move(left), op, comparison());
  }
  return left;
}

ExpressionPointer Parser::comparison() {
  ExpressionPointer left = term();
  while (check(TokenType::LT) || check(TokenType::GT)) {
    std::string op = advance().lexeme;
    left = std::make_unique<BinaryExpression>(std::move(left), op, term());
  }
  return left;
}

ExpressionPointer Parser::term() {
  ExpressionPointer left = factor();
  while (check(TokenType::PLUS) || check(TokenType::MINUS)) {
    std::string op = advance().lexeme;
    left = std::make_unique<BinaryExpression>(std::move(left), op, factor());
  }
  return left;
}

ExpressionPointer Parser::factor() {
  ExpressionPointer left = call();
  while (check(TokenType::STAR) || check(TokenType::SLASH)) {
    std::string op = advance().lexeme;
    left = std::make_unique<BinaryExpression>(std::move(left), op, call());
  }
  return left;
}

ExpressionPointer Parser::call() {
  ExpressionPointer expr = primary();
  if (check(TokenType::LPAREN)) {
    auto *ident = dynamic_cast<IdentExpression *>(expr.get());
    if (!ident)
      throw std::runtime_error("Can only call named functions");
    std::string callee = ident->name;
    advance(); // consume '('
    std::vector<ExpressionPointer> args;
    if (!check(TokenType::RPAREN)) {
      do {
        args.push_back(expression());
      } while (match(TokenType::COMMA));
    }
    expect(TokenType::RPAREN, "Expected ')'");
    return std::make_unique<CallExpression>(callee, std::move(args));
  }
  return expr;
}

ExpressionPointer Parser::primary() {
  if (match(TokenType::NUMBER))
    return std::make_unique<NumberExpression>(std::stod(previous().lexeme));
  if (match(TokenType::STRING)) {
    std::string raw = previous().lexeme;
    return std::make_unique<StringExpression>(raw.substr(1, raw.size() - 2));
  }
  if (match(TokenType::TRUE))
    return std::make_unique<BoolExpression>(true);
  if (match(TokenType::FALSE))
    return std::make_unique<BoolExpression>(false);
  if (match(TokenType::IDENTIFIER))
    return std::make_unique<IdentExpression>(previous().lexeme);
  if (match(TokenType::LPAREN)) {
    ExpressionPointer e = expression();
    expect(TokenType::RPAREN, "Expected ')'");
    return e;
  }
  throw std::runtime_error("Unexpected token: '" + peek().lexeme +
                           "' at line " + std::to_string(peek().line));
}

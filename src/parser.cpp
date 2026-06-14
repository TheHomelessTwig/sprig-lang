#include "parser.hpp"

#include <stdexcept>

#include "ast.hpp"

Parser::Parser(std::vector<Token> toks) : tokens(std::move(toks)) {}

Token &Parser::peek() { return tokens[current]; }
Token &Parser::previous() { return tokens[current - 1]; }
bool Parser::at_end() { return peek().type == TokenType::EOF_TOKEN; }

Token &Parser::advance() {
    if (!at_end()) current++;
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
    if (check(t)) return advance();
    throw std::runtime_error(msg + " (got '" + peek().lexeme + "' at line " +
                             std::to_string(peek().line) + ")");
}
void Parser::skip_newlines() {
    while (match(TokenType::NEWLINE)) {
    }
}

Program Parser::parse() {
    Program program;
    skip_newlines();
    while (!at_end()) {
        program.stmts.push_back(statement());
        skip_newlines();
    }
    return program;
}

StatementPointer Parser::statement() {
    if (match(TokenType::LET)) return variable_statement();
    if (match(TokenType::DEFINE)) return function_statement();
    if (match(TokenType::WHEN)) return if_statement();
    if (match(TokenType::WHILE)) return while_statement();
    if (match(TokenType::FOR)) {
        expect(TokenType::EACH, "Expected 'each' after 'for'");
        return for_each_statement();
    }
    if (match(TokenType::RETURN)) return return_statement();
    if (match(TokenType::STOP)) {
        match(TokenType::NEWLINE);
        return std::make_unique<StopStatement>();
    }
    if (match(TokenType::SKIP)) {
        match(TokenType::NEWLINE);
        return std::make_unique<SkipStatement>();
    }
    return expression_statement();
}

StatementPointer Parser::variable_statement() {
    Token name = expect(TokenType::IDENTIFIER, "Expected name after 'let'");
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
            params.push_back(
                expect(TokenType::IDENTIFIER, "Expected param").lexeme);
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
    if (match(TokenType::ELSE)) {
        skip_newlines();
        if (match(TokenType::WHEN)) {
            Block chained;
            chained.stmts.push_back(if_statement());
            else_b = std::move(chained);
        } else {
            else_b = block();
        }
    }
    return std::make_unique<IfStatement>(std::move(cond), std::move(then_b),
                                         std::move(else_b));
}

StatementPointer Parser::while_statement() {
    ExpressionPointer cond = expression();
    Block body = block();
    return std::make_unique<WhileStatement>(std::move(cond), std::move(body));
}

StatementPointer Parser::for_each_statement() {
    Token var =
        expect(TokenType::IDENTIFIER, "Expected variable after 'for each'");
    expect(TokenType::IN, "Expected 'in'");
    ExpressionPointer iterable = expression();
    Block body = block();
    return std::make_unique<ForEachStatement>(var.lexeme, std::move(iterable),
                                              std::move(body));
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
    expect(TokenType::COLON, "Expected ':' before block");
    expect(TokenType::NEWLINE, "Expected newline after ':'");
    expect(TokenType::INDENT, "Expected indented block");
    Block b;
    while (!check(TokenType::DEDENT) && !at_end()) {
        skip_newlines();
        if (check(TokenType::DEDENT) || at_end()) break;
        b.stmts.push_back(statement());
    }
    expect(TokenType::DEDENT, "Expected end of block");
    return b;
}

ExpressionPointer Parser::expression() { return logical_or(); }

ExpressionPointer Parser::logical_or() {
    ExpressionPointer left = logical_and();
    while (check(TokenType::OR)) {
        advance();
        left = std::make_unique<BinaryExpression>(std::move(left), "or",
                                                  logical_and());
    }
    return left;
}

ExpressionPointer Parser::logical_and() {
    ExpressionPointer left = logical_not();
    while (check(TokenType::AND)) {
        advance();
        left = std::make_unique<BinaryExpression>(std::move(left), "and",
                                                  logical_not());
    }
    return left;
}

ExpressionPointer Parser::logical_not() {
    if (match(TokenType::NOT)) {
        ExpressionPointer operand =
            logical_not();  // recursive: "not not x" works
        return std::make_unique<UnaryExpression>("not", std::move(operand));
    }
    return equality();
}

ExpressionPointer Parser::equality() {
    ExpressionPointer left = comparison();
    while (check(TokenType::EQ) || check(TokenType::NEQ) ||
           check(TokenType::IS)) {
        if (match(TokenType::IS)) {
            if (match(TokenType::NOT)) {
                // "is not" → !=
                left = std::make_unique<BinaryExpression>(std::move(left),
                                                          "!=", comparison());
            } else {
                // "is" → ==
                left = std::make_unique<BinaryExpression>(std::move(left),
                                                          "==", comparison());
            }
        } else {
            std::string op = advance().lexeme;
            left = std::make_unique<BinaryExpression>(std::move(left), op,
                                                      comparison());
        }
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
        left =
            std::make_unique<BinaryExpression>(std::move(left), op, factor());
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
    while (true) {
        if (check(TokenType::LPAREN)) {
            auto *ident = dynamic_cast<IdentExpression *>(expr.get());
            if (!ident)
                throw std::runtime_error("Can only call named functions");
            std::string callee = ident->name;
            advance();
            std::vector<ExpressionPointer> args;
            if (!check(TokenType::RPAREN)) {
                do {
                    args.push_back(expression());
                } while (match(TokenType::COMMA));
            }
            expect(TokenType::RPAREN, "Expected ')'");
            expr = std::make_unique<CallExpression>(callee, std::move(args));
        } else if (check(TokenType::LBRACKET)) {
            advance();
            ExpressionPointer index = expression();
            expect(TokenType::RBRACKET, "Expected ']'");
            expr = std::make_unique<IndexExpression>(std::move(expr),
                                                     std::move(index));
        } else {
            break;
        }
    }
    return expr;
}
ExpressionPointer Parser::primary() {
    if (match(TokenType::NUMBER))
        return std::make_unique<NumberExpression>(std::stod(previous().lexeme));
    if (match(TokenType::STRING)) {
        std::string raw = previous().lexeme;
        return std::make_unique<StringExpression>(
            raw.substr(1, raw.size() - 2));
    }
    if (match(TokenType::TRUE)) return std::make_unique<BoolExpression>(true);
    if (match(TokenType::FALSE)) return std::make_unique<BoolExpression>(false);
    if (match(TokenType::NOTHING)) return std::make_unique<NothingExpression>();
    if (match(TokenType::IDENTIFIER))
        return std::make_unique<IdentExpression>(previous().lexeme);
    if (match(TokenType::LPAREN)) {
        ExpressionPointer e = expression();
        expect(TokenType::RPAREN, "Expected ')'");
        return e;
    }
    if (match(TokenType::LBRACKET)) {
        std::vector<ExpressionPointer> elements;
        if (!check(TokenType::RBRACKET)) {
            do {
                elements.push_back(expression());
            } while (match(TokenType::COMMA));
        }
        expect(TokenType::RBRACKET, "Expected ']'");
        return std::make_unique<ListExpression>(std::move(elements));
    }
    throw std::runtime_error("Unexpected token: '" + peek().lexeme +
                             "' at line " + std::to_string(peek().line));
}

#include "parser.hpp"

#include <stdexcept>

#include "ast.hpp"

// ── Navigation helpers ────────────────────────────────────────────────────────

Parser::Parser(std::vector<Token> toks) : tokens(std::move(toks)) {}

Token& Parser::peek()     { return tokens[current]; }
Token& Parser::previous() { return tokens[current - 1]; }
bool   Parser::at_end()   { return peek().type == TokenType::EOF_TOKEN; }

Token& Parser::advance() {
    if (!at_end()) current++;
    return previous();
}

// Non-consuming lookahead — peek_at(1) is the token after the current one.
Token& Parser::peek_at(int offset) {
    int idx = current + offset;
    if (idx >= (int)tokens.size()) return tokens.back();
    return tokens[idx];
}

bool  Parser::check(TokenType t) { return !at_end() && peek().type == t; }
bool  Parser::match(TokenType t) { if (check(t)) { advance(); return true; } return false; }

Token Parser::expect(TokenType t, const std::string& msg) {
    if (check(t)) return advance();
    throw std::runtime_error(msg + " (got '" + peek().lexeme +
                             "' at line " + std::to_string(peek().line) + ")");
}

void Parser::skip_newlines() {
    while (match(TokenType::NEWLINE)) {}
}

// ── Top-level ─────────────────────────────────────────────────────────────────

Program Parser::parse() {
    Program program;
    skip_newlines();
    while (!at_end()) {
        program.stmts.push_back(statement());
        skip_newlines();
    }
    return program;
}

// Everything between a ':' + INDENT and the matching DEDENT.
Block Parser::block() {
    expect(TokenType::COLON,   "Expected ':' before block");
    expect(TokenType::NEWLINE, "Expected newline after ':'");
    expect(TokenType::INDENT,  "Expected indented block");
    Block b;
    while (!check(TokenType::DEDENT) && !at_end()) {
        skip_newlines();
        if (check(TokenType::DEDENT) || at_end()) break;
        b.stmts.push_back(statement());
    }
    expect(TokenType::DEDENT, "Expected end of block");
    return b;
}

// ── Statement parsers ─────────────────────────────────────────────────────────

StatementPointer Parser::statement() {
    // Field assignment must be detected by lookahead before consuming tokens:
    //   identifier '.' identifier '='  →  field_assign_statement
    if (check(TokenType::IDENTIFIER)             &&
        peek_at(1).type == TokenType::DOT        &&
        peek_at(2).type == TokenType::IDENTIFIER &&
        peek_at(3).type == TokenType::ASSIGN)
        return field_assign_statement();

    if (match(TokenType::LET))     return variable_statement();
    if (match(TokenType::DEFINE))  return function_statement();
    if (match(TokenType::SHAPE))   return shape_definition_statement();
    if (match(TokenType::INCLUDE)) return include_statement();
    if (match(TokenType::WHEN))    return if_statement();
    if (match(TokenType::WHILE))   return while_statement();
    if (match(TokenType::FOR)) {
        expect(TokenType::EACH, "Expected 'each' after 'for'");
        return for_each_statement();
    }
    if (match(TokenType::RETURN)) return return_statement();
    if (match(TokenType::STOP))   { match(TokenType::NEWLINE); return std::make_unique<StopStatement>(); }
    if (match(TokenType::SKIP))   { match(TokenType::NEWLINE); return std::make_unique<SkipStatement>(); }
    return expression_statement();
}

// let [mutable] x = expr  /  let x borrow [mutable] y
StatementPointer Parser::variable_statement() {
    // let x borrow [mutable] y
    if (check(TokenType::IDENTIFIER) &&
        peek_at(1).type == TokenType::BORROW)
        return borrow_statement();

    bool  is_mutable = match(TokenType::MUTABLE);
    Token name       = expect(TokenType::IDENTIFIER, "Expected name after 'let'");
    expect(TokenType::ASSIGN, "Expected '='");
    ExpressionPointer val = expression();
    match(TokenType::NEWLINE);
    return std::make_unique<VariableStatement>(
        name.lexeme, std::move(val), is_mutable, name.line);
}

// let x borrow [mutable] y
StatementPointer Parser::borrow_statement() {
    Token target = advance();                        // x
    expect(TokenType::BORROW, "Expected 'borrow'"); // borrow
    if (match(TokenType::MUTABLE)) {
        Token source = expect(TokenType::IDENTIFIER,
                              "Expected variable to borrow");
        match(TokenType::NEWLINE);
        return std::make_unique<MutableBorrowStatement>(
            target.lexeme, source.lexeme, target.line);
    }
    Token source = expect(TokenType::IDENTIFIER,
                          "Expected variable to borrow");
    match(TokenType::NEWLINE);
    return std::make_unique<BorrowStatement>(
        target.lexeme, source.lexeme, target.line);
}

// define name(params): body
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
    return std::make_unique<FunctionStatement>(
        name.lexeme, std::move(params), block());
}

// shape Person:
//     name: text
//     age:  number
StatementPointer Parser::shape_definition_statement() {
    Token name = expect(TokenType::IDENTIFIER, "Expected shape name");
    expect(TokenType::COLON,   "Expected ':' after shape name");
    expect(TokenType::NEWLINE, "Expected newline after ':'");
    expect(TokenType::INDENT,  "Expected indented fields");

    std::vector<ShapeField> fields;
    while (!check(TokenType::DEDENT) && !at_end()) {
        skip_newlines();
        if (check(TokenType::DEDENT)) break;
        Token fname = expect(TokenType::IDENTIFIER, "Expected field name");
        expect(TokenType::COLON, "Expected ':' after field name");

        std::string type_str;
        if      (match(TokenType::TYPE_TEXT))    type_str = "text";
        else if (match(TokenType::TYPE_NUMBER))  type_str = "number";
        else if (match(TokenType::TYPE_DECIMAL)) type_str = "decimal";
        else if (match(TokenType::TYPE_FLAG))    type_str = "flag";
        else throw std::runtime_error(
            "Expected type (text, number, decimal, flag) at line " +
            std::to_string(peek().line));

        fields.push_back({fname.lexeme, type_str});
        match(TokenType::NEWLINE);
    }
    expect(TokenType::DEDENT, "Expected end of shape body");
    return std::make_unique<ShapeDefinitionStatement>(
        name.lexeme, std::move(fields));
}

// sam.age = 21  (all four tokens already confirmed by lookahead in statement())
StatementPointer Parser::field_assign_statement() {
    Token var   = advance(); // identifier
    advance();               // '.'
    Token field = advance(); // field name
    advance();               // '='
    ExpressionPointer val = expression();
    match(TokenType::NEWLINE);
    return std::make_unique<FieldAssignStatement>(
        var.lexeme, field.lexeme, std::move(val), var.line);
}

// include "path/to/file.sprig"
StatementPointer Parser::include_statement() {
    Token path_tok = expect(TokenType::STRING, "Expected file path after 'include'");
    match(TokenType::NEWLINE);
    // Strip surrounding quotes
    std::string raw  = path_tok.lexeme;
    std::string path = raw.substr(1, raw.size() - 2);
    return std::make_unique<IncludeStatement>(path, path_tok.line);
}

// when cond: ... [otherwise: ...]
StatementPointer Parser::if_statement() {
    ExpressionPointer cond   = expression();
    Block             then_b = block();
    std::optional<Block> else_b;
    skip_newlines();
    if (match(TokenType::ELSE)) {
        skip_newlines();
        if (match(TokenType::WHEN)) {
            // "otherwise when" chains as a nested if
            Block chained;
            chained.stmts.push_back(if_statement());
            else_b = std::move(chained);
        } else {
            else_b = block();
        }
    }
    return std::make_unique<IfStatement>(
        std::move(cond), std::move(then_b), std::move(else_b));
}

// as long as cond: body
StatementPointer Parser::while_statement() {
    ExpressionPointer cond = expression();
    Block             body = block();
    return std::make_unique<WhileStatement>(std::move(cond), std::move(body));
}

// for each x in list: body
StatementPointer Parser::for_each_statement() {
    Token var = expect(TokenType::IDENTIFIER, "Expected variable after 'for each'");
    expect(TokenType::IN, "Expected 'in'");
    ExpressionPointer iterable = expression();
    Block             body     = block();
    return std::make_unique<ForEachStatement>(
        var.lexeme, std::move(iterable), std::move(body));
}

// give back expr
StatementPointer Parser::return_statement() {
    ExpressionPointer val = expression();
    match(TokenType::NEWLINE);
    return std::make_unique<ReturnStatement>(std::move(val));
}

// Bare expression used as a statement (typically a function call)
StatementPointer Parser::expression_statement() {
    ExpressionPointer e = expression();
    match(TokenType::NEWLINE);
    return std::make_unique<ExpressionStatement>(std::move(e));
}

// ── Expression parsers (descending precedence) ────────────────────────────────

ExpressionPointer Parser::expression() { return logical_or(); }

ExpressionPointer Parser::logical_or() {
    ExpressionPointer left = logical_and();
    while (check(TokenType::OR)) {
        advance();
        int op_line = previous().line;
        left = std::make_unique<BinaryExpression>(
            std::move(left), "or", logical_and(), op_line);
    }
    return left;
}

ExpressionPointer Parser::logical_and() {
    ExpressionPointer left = logical_not();
    while (check(TokenType::AND)) {
        advance();
        int op_line = previous().line;
        left = std::make_unique<BinaryExpression>(
            std::move(left), "and", logical_not(), op_line);
    }
    return left;
}

// Recursive to support "not not x"
ExpressionPointer Parser::logical_not() {
    if (match(TokenType::NOT))
        return std::make_unique<UnaryExpression>("not", logical_not());
    return equality();
}

// == / != / is / is not
ExpressionPointer Parser::equality() {
    ExpressionPointer left = comparison();
    while (check(TokenType::EQ) || check(TokenType::NEQ) ||
           check(TokenType::IS)) {
        if (match(TokenType::IS)) {
            int         op_line = previous().line;
            std::string op      = match(TokenType::NOT) ? "!=" : "==";
            left = std::make_unique<BinaryExpression>(
                std::move(left), op, comparison(), op_line);
        } else {
            std::string op      = advance().lexeme;
            int         op_line = previous().line;
            left = std::make_unique<BinaryExpression>(
                std::move(left), op, comparison(), op_line);
        }
    }
    return left;
}

// < / >
ExpressionPointer Parser::comparison() {
    ExpressionPointer left = term();
    while (check(TokenType::LT) || check(TokenType::GT)) {
        std::string op      = advance().lexeme;
        int         op_line = previous().line;
        left = std::make_unique<BinaryExpression>(
            std::move(left), op, term(), op_line);
    }
    return left;
}

// + / -
ExpressionPointer Parser::term() {
    ExpressionPointer left = factor();
    while (check(TokenType::PLUS) || check(TokenType::MINUS)) {
        std::string op      = advance().lexeme;
        int         op_line = previous().line;
        left = std::make_unique<BinaryExpression>(
            std::move(left), op, factor(), op_line);
    }
    return left;
}

// * / /
ExpressionPointer Parser::factor() {
    ExpressionPointer left = call();
    while (check(TokenType::STAR) || check(TokenType::SLASH)) {
        std::string op      = advance().lexeme;
        int         op_line = previous().line;
        left = std::make_unique<BinaryExpression>(
            std::move(left), op, call(), op_line);
    }
    return left;
}

// Postfix operators: function calls f(...), index access a[i], field access a.f
ExpressionPointer Parser::call() {
    ExpressionPointer expr = primary();
    while (true) {
        if (check(TokenType::LPAREN)) {
            auto* ident = dynamic_cast<IdentExpression*>(expr.get());
            if (!ident)
                throw std::runtime_error("Can only call named functions");
            std::string callee   = ident->name;
            int         fn_line  = ident->line;
            advance();
            std::vector<ExpressionPointer> args;
            if (!check(TokenType::RPAREN)) {
                do { args.push_back(expression()); } while (match(TokenType::COMMA));
            }
            expect(TokenType::RPAREN, "Expected ')'");
            expr = std::make_unique<CallExpression>(
                callee, std::move(args), fn_line);
        } else if (check(TokenType::LBRACKET)) {
            advance();
            ExpressionPointer index = expression();
            expect(TokenType::RBRACKET, "Expected ']'");
            expr = std::make_unique<IndexExpression>(
                std::move(expr), std::move(index));
        } else if (match(TokenType::DOT)) {
            Token field = expect(TokenType::IDENTIFIER, "Expected field name after '.'");
            expr = std::make_unique<FieldAccessExpression>(
                std::move(expr), field.lexeme, field.line);
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
        // Strip surrounding quotes
        return std::make_unique<StringExpression>(raw.substr(1, raw.size() - 2));
    }

    if (match(TokenType::TRUE))    return std::make_unique<BoolExpression>(true);
    if (match(TokenType::FALSE))   return std::make_unique<BoolExpression>(false);
    if (match(TokenType::NOTHING)) return std::make_unique<NothingExpression>();

    // Shape instantiation: Identifier '{' — detected via lookahead so a bare
    // identifier followed by something other than '{' falls through to IdentExpression.
    if (check(TokenType::IDENTIFIER) &&
        peek_at(1).type == TokenType::LBRACE) {
        Token name = advance(); // identifier
        advance();              // '{'
        std::vector<std::pair<std::string, ExpressionPointer>> fields;
        skip_newlines();
        while (!check(TokenType::RBRACE) && !at_end()) {
            Token fname = expect(TokenType::IDENTIFIER, "Expected field name");
            expect(TokenType::COLON, "Expected ':' after field name");
            auto val = expression();
            fields.push_back({fname.lexeme, std::move(val)});
            if (!match(TokenType::COMMA)) break;
            skip_newlines();
        }
        expect(TokenType::RBRACE, "Expected '}'");
        return std::make_unique<ShapeInstanceExpression>(
            name.lexeme, std::move(fields));
    }

    // borrow [mutable] x  — used in function arguments and expressions
    if (match(TokenType::BORROW)) {
        int borrow_line = previous().line;
        if (match(TokenType::MUTABLE)) {
            Token source = expect(TokenType::IDENTIFIER,
                                  "Expected variable name after 'borrow mutable'");
            return std::make_unique<MutableBorrowExpression>(
                source.lexeme, borrow_line);
        }
        Token source = expect(TokenType::IDENTIFIER,
                              "Expected variable name after 'borrow'");
        return std::make_unique<BorrowExpression>(source.lexeme, borrow_line);
    }

    // Regular identifier — must come AFTER shape instantiation check
    if (match(TokenType::IDENTIFIER))
        return std::make_unique<IdentExpression>(previous().lexeme, previous().line);

    if (match(TokenType::LPAREN)) {
        ExpressionPointer e = expression();
        expect(TokenType::RPAREN, "Expected ')'");
        return e;
    }

    if (match(TokenType::LBRACKET)) {
        std::vector<ExpressionPointer> elements;
        if (!check(TokenType::RBRACKET)) {
            do { elements.push_back(expression()); } while (match(TokenType::COMMA));
        }
        expect(TokenType::RBRACKET, "Expected ']'");
        return std::make_unique<ListExpression>(std::move(elements));
    }

    throw std::runtime_error("Unexpected token: '" + peek().lexeme +
                             "' at line " + std::to_string(peek().line));
}

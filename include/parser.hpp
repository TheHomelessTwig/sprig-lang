#pragma once
#include "ast.hpp"
#include "lexer.hpp"

class Parser {
    std::vector<Token> tokens;
    int current = 0;

public:
    Parser(std::vector<Token> toks);
    Program parse();

private:
    // ── Navigation helpers ────────────────────────────────────────────────────

    Token& peek();
    Token& previous();
    bool   at_end();
    Token& advance();
    Token& peek_at(int offset);   // non-consuming lookahead
    bool   check(TokenType token_type);
    bool   match(TokenType token_type);
    Token  expect(TokenType token_type, const std::string& msg);
    void   skip_newlines();
    void   skip_structure(); // also skips INDENT/DEDENT — for inside () and {}

    // ── Statements ────────────────────────────────────────────────────────────

    StatementPointer statement();
    StatementPointer variable_statement();
    StatementPointer borrow_statement();
    StatementPointer function_statement();
    StatementPointer shape_definition_statement();
    StatementPointer field_assign_statement();
    StatementPointer include_statement();
    StatementPointer if_statement();
    StatementPointer while_statement();
    StatementPointer for_each_statement();
    StatementPointer return_statement();
    StatementPointer expression_statement();
    StatementPointer unsafe_statement();
    Block            block();

    // ── Expressions (descending precedence) ───────────────────────────────────

    ExpressionPointer expression();
    ExpressionPointer logical_or();
    ExpressionPointer logical_and();
    ExpressionPointer logical_not();
    ExpressionPointer equality();
    ExpressionPointer comparison();
    ExpressionPointer term();
    ExpressionPointer factor();
    ExpressionPointer unary();   // prefix: -expr
    ExpressionPointer call();    // postfix: calls, indexing, field access
    ExpressionPointer primary();
};

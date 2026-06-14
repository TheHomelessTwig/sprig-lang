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
    Token &peek();
    Token &previous();
    bool at_end();
    Token &advance();
    bool check(TokenType t);
    bool match(TokenType t);
    Token expect(TokenType t, const std::string &msg);
    void skip_newlines();

    StatementPointer statement();
    StatementPointer variable_statement();
    StatementPointer function_statement();
    StatementPointer if_statement();
    StatementPointer return_statement();
    StatementPointer expression_statement();
    StatementPointer while_statement();
    StatementPointer for_each_statement();
    Block block();

    // One function per precedence level — mirrors the grammar exactly
    ExpressionPointer expression();
    ExpressionPointer equality();
    ExpressionPointer comparison();
    ExpressionPointer term();
    ExpressionPointer factor();
    ExpressionPointer call();
    ExpressionPointer primary();
    ExpressionPointer logical_or();
    ExpressionPointer logical_and();
    ExpressionPointer logical_not();
};

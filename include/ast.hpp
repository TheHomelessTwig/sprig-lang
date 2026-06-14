#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct Expression;
struct Statement;
using ExpressionPointer = std::unique_ptr<Expression>;
using StatementPointer = std::unique_ptr<Statement>;

struct Expression {
    virtual ~Expression() = default;
};
struct Statement {
    virtual ~Statement() = default;
};

struct NumberExpression : Expression {
    double value;
    NumberExpression(double v) : value(v) {}
};
struct StringExpression : Expression {
    std::string value;
    StringExpression(std::string v) : value(std::move(v)) {}
};
struct BoolExpression : Expression {
    bool value;
    BoolExpression(bool v) : value(v) {}
};
struct IdentExpression : Expression {
    std::string name;
    IdentExpression(std::string n) : name(std::move(n)) {}
};
struct BinaryExpression : Expression {
    ExpressionPointer left;
    std::string op;
    ExpressionPointer right;
    BinaryExpression(ExpressionPointer l, std::string op, ExpressionPointer r)
        : left(std::move(l)), op(std::move(op)), right(std::move(r)) {}
};
struct UnaryExpression : Expression {
    std::string op;
    ExpressionPointer operand;
    UnaryExpression(std::string op, ExpressionPointer operand)
        : op(std::move(op)), operand(std::move(operand)) {}
};
struct CallExpression : Expression {
    std::string callee;
    std::vector<ExpressionPointer> args;
    CallExpression(std::string c, std::vector<ExpressionPointer> a)
        : callee(std::move(c)), args(std::move(a)) {}
};
struct NothingExpression : Expression {};

struct Block {
    std::vector<StatementPointer> stmts;
};
struct VariableStatement : Statement {
    std::string name;
    ExpressionPointer value;
    VariableStatement(std::string n, ExpressionPointer v)
        : name(std::move(n)), value(std::move(v)) {}
};
struct ReturnStatement : Statement {
    ExpressionPointer value;
    ReturnStatement(ExpressionPointer v) : value(std::move(v)) {}
};
struct ExpressionStatement : Statement {
    ExpressionPointer expr;
    ExpressionStatement(ExpressionPointer e) : expr(std::move(e)) {}
};
struct IfStatement : Statement {
    ExpressionPointer condition;
    Block then_block;
    std::optional<Block> else_block;
    IfStatement(ExpressionPointer c, Block t, std::optional<Block> e)
        : condition(std::move(c)),
          then_block(std::move(t)),
          else_block(std::move(e)) {}
};
struct FunctionStatement : Statement {
    std::string name;
    std::vector<std::string> params;
    Block body;
    FunctionStatement(std::string n, std::vector<std::string> p, Block b)
        : name(std::move(n)), params(std::move(p)), body(std::move(b)) {}
};
struct WhileStatement : Statement {
    ExpressionPointer condition;
    Block body;
    WhileStatement(ExpressionPointer c, Block b)
        : condition(std::move(c)), body(std::move(b)) {}
};

struct ListExpression : Expression {
    std::vector<ExpressionPointer> elements;
    ListExpression(std::vector<ExpressionPointer> e) : elements(std::move(e)) {}
};

struct IndexExpression : Expression {
    ExpressionPointer object;
    ExpressionPointer index;
    IndexExpression(ExpressionPointer obj, ExpressionPointer idx)
        : object(std::move(obj)), index(std::move(idx)) {}
};

struct ForEachStatement : Statement {
    std::string variable;
    ExpressionPointer iterable;
    Block body;
    ForEachStatement(std::string var, ExpressionPointer iter, Block b)
        : variable(std::move(var)),
          iterable(std::move(iter)),
          body(std::move(b)) {}
};

struct StopStatement : Statement {};
struct SkipStatement : Statement {};

struct Program {
    std::vector<StatementPointer> stmts;
};

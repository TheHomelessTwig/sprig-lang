#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Type aliases - saves typing std::unique_ptr<Expression> everywhere
struct Expression;
struct Statement;
using ExpressionPointer = std::unique_ptr<Expression>;
using StatementPointer = std::unique_ptr<Statement>;

// ── Base classes ─────────────────────────────────────────────────────────────
struct Expression {
  virtual ~Expression() = default;
};
struct Statement {
  virtual ~Statement() = default;
};

// ── Expression nodes ─────────────────────────────────────────────────────────
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
struct CallExpression : Expression {
  std::string callee;
  std::vector<ExpressionPointer> args;
  CallExpression(std::string c, std::vector<ExpressionPointer> a)
      : callee(std::move(c)), args(std::move(a)) {}
};

// ── Statement nodes
// ───────────────────────────────────────────────────────────
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
  std::optional<Block> else_block; // optional: else is not required
  IfStatement(ExpressionPointer c, Block t, std::optional<Block> e)
      : condition(std::move(c)), then_block(std::move(t)),
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

// The root node — a whole program is just a list of statements
struct Program {
  std::vector<StatementPointer> stmts;
};

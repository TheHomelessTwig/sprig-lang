#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

// ── Forward declarations ──────────────────────────────────────────────────────

struct Expression;
struct Statement;
using ExpressionPointer = std::unique_ptr<Expression>;
using StatementPointer  = std::unique_ptr<Statement>;

struct Expression { virtual ~Expression() = default; };
struct Statement  { virtual ~Statement()  = default; };

// ── Expressions ───────────────────────────────────────────────────────────────

// Literals (no line needed — literal type errors are parse-time, not runtime)
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
struct NothingExpression : Expression {};

// Variable reference — line needed to report "undefined variable" with context
struct IdentExpression : Expression {
    std::string name;
    int         line;
    IdentExpression(std::string n, int ln) : name(std::move(n)), line(ln) {}
};

// Operations — line on binary for division-by-zero and operator errors
struct BinaryExpression : Expression {
    ExpressionPointer left;
    std::string       op;
    ExpressionPointer right;
    int               line;
    BinaryExpression(ExpressionPointer l, std::string op,
                     ExpressionPointer r, int ln)
        : left(std::move(l)), op(std::move(op)),
          right(std::move(r)), line(ln) {}
};
struct UnaryExpression : Expression {
    std::string       op;
    ExpressionPointer operand;
    UnaryExpression(std::string op, ExpressionPointer operand)
        : op(std::move(op)), operand(std::move(operand)) {}
};

// Function call — line used when function is undefined
struct CallExpression : Expression {
    std::string                    callee;
    std::vector<ExpressionPointer> args;
    int                            line;
    CallExpression(std::string c, std::vector<ExpressionPointer> a, int ln)
        : callee(std::move(c)), args(std::move(a)), line(ln) {}
};

// List literal: [a, b, c]
struct ListExpression : Expression {
    std::vector<ExpressionPointer> elements;
    ListExpression(std::vector<ExpressionPointer> e) : elements(std::move(e)) {}
};

// Index access: collection[i]
struct IndexExpression : Expression {
    ExpressionPointer object;
    ExpressionPointer index;
    IndexExpression(ExpressionPointer obj, ExpressionPointer idx)
        : object(std::move(obj)), index(std::move(idx)) {}
};

// Shape instantiation: Person { name: "sam", age: 20 }
struct ShapeInstanceExpression : Expression {
    std::string shape_name;
    std::vector<std::pair<std::string, ExpressionPointer>> fields;
    ShapeInstanceExpression(
        std::string n,
        std::vector<std::pair<std::string, ExpressionPointer>> f)
        : shape_name(std::move(n)), fields(std::move(f)) {}
};

// Field read: sam.name — line for "no such field" errors
struct FieldAccessExpression : Expression {
    ExpressionPointer object;
    std::string       field;
    int               line;
    FieldAccessExpression(ExpressionPointer obj, std::string f, int ln)
        : object(std::move(obj)), field(std::move(f)), line(ln) {}
};

// ── Statements ────────────────────────────────────────────────────────────────

struct Block {
    std::vector<StatementPointer> stmts;
};

// let [mutable] x = expr
struct VariableStatement : Statement {
    std::string       name;
    ExpressionPointer value;
    bool              is_mutable; // true when declared with 'let mutable'
    int               line;
    VariableStatement(std::string n, ExpressionPointer v, bool mut, int ln)
        : name(std::move(n)), value(std::move(v)),
          is_mutable(mut), line(ln) {}
};

// give back expr
struct ReturnStatement : Statement {
    ExpressionPointer value;
    ReturnStatement(ExpressionPointer v) : value(std::move(v)) {}
};

// Bare expression used as a statement (e.g. a print() call)
struct ExpressionStatement : Statement {
    ExpressionPointer expr;
    ExpressionStatement(ExpressionPointer e) : expr(std::move(e)) {}
};

// when cond: ... [otherwise: ...]
struct IfStatement : Statement {
    ExpressionPointer    condition;
    Block                then_block;
    std::optional<Block> else_block;
    IfStatement(ExpressionPointer c, Block t, std::optional<Block> e)
        : condition(std::move(c)),
          then_block(std::move(t)),
          else_block(std::move(e)) {}
};

// define name(params): body
struct FunctionStatement : Statement {
    std::string              name;
    std::vector<std::string> params;
    Block                    body;
    FunctionStatement(std::string n, std::vector<std::string> p, Block b)
        : name(std::move(n)), params(std::move(p)), body(std::move(b)) {}
};

// as long as cond: body
struct WhileStatement : Statement {
    ExpressionPointer condition;
    Block             body;
    WhileStatement(ExpressionPointer c, Block b)
        : condition(std::move(c)), body(std::move(b)) {}
};

// for each x in list: body
struct ForEachStatement : Statement {
    std::string       variable;
    ExpressionPointer iterable;
    Block             body;
    ForEachStatement(std::string var, ExpressionPointer iter, Block b)
        : variable(std::move(var)), iterable(std::move(iter)), body(std::move(b)) {}
};

// Loop control — unwind to the nearest enclosing loop via signal
struct StopStatement : Statement {};  // break
struct SkipStatement : Statement {};  // continue

// Field schema entry — name + declared type
struct ShapeField {
    std::string name;
    std::string type; // "text" | "number" | "decimal" | "flag"
};

// shape Person:
//     name: text
//     age:  number
struct ShapeDefinitionStatement : Statement {
    std::string              name;
    std::vector<ShapeField>  fields;
    ShapeDefinitionStatement(std::string n, std::vector<ShapeField> f)
        : name(std::move(n)), fields(std::move(f)) {}
};

// sam.age = 21  — mutates field through shared_ptr so all copies see the change
struct FieldAssignStatement : Statement {
    std::string       variable;
    std::string       field;
    ExpressionPointer value;
    int               line;
    FieldAssignStatement(std::string var, std::string f,
                         ExpressionPointer v, int ln)
        : variable(std::move(var)), field(std::move(f)),
          value(std::move(v)), line(ln) {}
};

// include "path/to/file.sprig"  — runs the file in the current interpreter context
struct IncludeStatement : Statement {
    std::string path;
    int         line;
    IncludeStatement(std::string p, int ln)
        : path(std::move(p)), line(ln) {}
};

// ── Program root ──────────────────────────────────────────────────────────────

struct Program {
    std::vector<StatementPointer> stmts;
};

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
    NumberExpression(double val) : value(val) {}
};
struct StringExpression : Expression {
    std::string value;
    StringExpression(std::string val) : value(std::move(val)) {}
};
struct BoolExpression : Expression {
    bool value;
    BoolExpression(bool val) : value(val) {}
};
struct NothingExpression : Expression {};

// Variable reference — line needed to report "undefined variable" with context
struct IdentExpression : Expression {
    std::string name;
    int         line;
    IdentExpression(std::string name_str, int line_number) : name(std::move(name_str)), line(line_number) {}
};

// Operations — line on binary for division-by-zero and operator errors
struct BinaryExpression : Expression {
    ExpressionPointer left;
    std::string       op;
    ExpressionPointer right;
    int               line;
    BinaryExpression(ExpressionPointer left_expr, std::string op,
                     ExpressionPointer right_expr, int line_number)
        : left(std::move(left_expr)), op(std::move(op)),
          right(std::move(right_expr)), line(line_number) {}
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
    CallExpression(std::string callee_name, std::vector<ExpressionPointer> arg_list, int line_number)
        : callee(std::move(callee_name)), args(std::move(arg_list)), line(line_number) {}
};

// List literal: [a, b, c]
struct ListExpression : Expression {
    std::vector<ExpressionPointer> elements;
    ListExpression(std::vector<ExpressionPointer> elems) : elements(std::move(elems)) {}
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
        std::string name,
        std::vector<std::pair<std::string, ExpressionPointer>> field_list)
        : shape_name(std::move(name)), fields(std::move(field_list)) {}
};

// Field read: sam.name — line for "no such field" errors
struct FieldAccessExpression : Expression {
    ExpressionPointer object;
    std::string       field;
    int               line;
    FieldAccessExpression(ExpressionPointer obj, std::string field_name, int line_number)
        : object(std::move(obj)), field(std::move(field_name)), line(line_number) {}
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
    VariableStatement(std::string name, ExpressionPointer val, bool is_mutable, int line_number)
        : name(std::move(name)), value(std::move(val)),
          is_mutable(is_mutable), line(line_number) {}
};

// give back expr
struct ReturnStatement : Statement {
    ExpressionPointer value;
    ReturnStatement(ExpressionPointer val) : value(std::move(val)) {}
};

// Bare expression used as a statement (e.g. a print() call)
struct ExpressionStatement : Statement {
    ExpressionPointer expr;
    ExpressionStatement(ExpressionPointer expression) : expr(std::move(expression)) {}
};

// when cond: ... [otherwise: ...]
struct IfStatement : Statement {
    ExpressionPointer    condition;
    Block                then_block;
    std::optional<Block> else_block;
    IfStatement(ExpressionPointer cond, Block then_blk, std::optional<Block> else_blk)
        : condition(std::move(cond)),
          then_block(std::move(then_blk)),
          else_block(std::move(else_blk)) {}
};

// define name(params): body
struct FunctionStatement : Statement {
    std::string              name;
    std::vector<std::string> params;
    Block                    body;
    FunctionStatement(std::string name, std::vector<std::string> params, Block body)
        : name(std::move(name)), params(std::move(params)), body(std::move(body)) {}
};

// as long as cond: body
struct WhileStatement : Statement {
    ExpressionPointer condition;
    Block             body;
    WhileStatement(ExpressionPointer cond, Block body)
        : condition(std::move(cond)), body(std::move(body)) {}
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
    ShapeDefinitionStatement(std::string name, std::vector<ShapeField> fields)
        : name(std::move(name)), fields(std::move(fields)) {}
};

// sam.age = 21  — mutates field through shared_ptr so all copies see the change
struct FieldAssignStatement : Statement {
    std::string       variable;
    std::string       field;
    ExpressionPointer value;
    int               line;
    FieldAssignStatement(std::string var, std::string field_name,
                         ExpressionPointer val, int line_number)
        : variable(std::move(var)), field(std::move(field_name)),
          value(std::move(val)), line(line_number) {}
};

// include "path/to/file.sprig"  — runs the file in the current interpreter context
struct IncludeStatement : Statement {
    std::string path;
    int         line;
    IncludeStatement(std::string path, int line_number)
        : path(std::move(path)), line(line_number) {}
};

// own expr — heap-allocates expr with unique ownership (Box<T>)
struct OwnExpression : Expression {
    ExpressionPointer inner;
    int               line;
    OwnExpression(ExpressionPointer inner, int ln)
        : inner(std::move(inner)), line(ln) {}
};

// unsafe: block — permits raw pointer operations inside
struct UnsafeStatement : Statement {
    Block body;
    UnsafeStatement(Block body) : body(std::move(body)) {}
};

// let x borrow y  — immutable borrow binding
struct BorrowStatement : Statement {
    std::string target;
    std::string source;
    int         line;
    BorrowStatement(std::string target, std::string source, int line_number)
        : target(std::move(target)), source(std::move(source)), line(line_number) {}
};

// let x borrow mutable y  — mutable borrow binding
struct MutableBorrowStatement : Statement {
    std::string target;
    std::string source;
    int         line;
    MutableBorrowStatement(std::string target, std::string source, int line_number)
        : target(std::move(target)), source(std::move(source)), line(line_number) {}
};

// borrow x  — immutable borrow expression (for function arguments)
struct BorrowExpression : Expression {
    std::string source;
    int         line;
    BorrowExpression(std::string source, int line_number)
        : source(std::move(source)), line(line_number) {}
};

// borrow mutable x  — mutable borrow expression
struct MutableBorrowExpression : Expression {
    std::string source;
    int         line;
    MutableBorrowExpression(std::string source, int line_number)
        : source(std::move(source)), line(line_number) {}
};

// ── Program root ──────────────────────────────────────────────────────────────

struct Program {
    std::vector<StatementPointer> stmts;
};

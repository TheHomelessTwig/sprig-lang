#include "interpreter.hpp"
#include <cmath>
#include <iostream>

// ── Entry point
// ───────────────────────────────────────────────────────────────

void Interpreter::run(const Program &program) {
  for (auto &stmt : program.stmts)
    eval_statement(stmt.get(), global);
}

// ── Block execution
// ─────────────────────────────────────────────────────────── Note: eval_block
// does NOT create a new scope — callers do that. This lets if/while/functions
// each control their own scoping rules.

void Interpreter::eval_block(const Block &b, Environment &env) {
  for (auto &stmt : b.stmts)
    eval_statement(stmt.get(), env);
}

// ── Statements
// ────────────────────────────────────────────────────────────────

void Interpreter::eval_statement(const Statement *s, Environment &env) {

  // allow x = <expr>  →  evaluate expr, store in current scope
  if (auto *vs = dynamic_cast<const VariableStatement *>(s)) {
    env.set(vs->name, eval_expression(vs->value.get(), env));
    return;
  }

  // module f(a,b) { ... }  →  register function, don't execute yet
  if (auto *fs = dynamic_cast<const FunctionStatement *>(s)) {
    functions[fs->name] = TwigFunction{fs->params, &fs->body};
    return;
  }

  // given <cond> { ... } otherwise { ... }
  if (auto *is = dynamic_cast<const IfStatement *>(s)) {
    Value cond = eval_expression(is->condition.get(), env);
    if (cond.is_truthy()) {
      Environment inner(&env); // new scope for the block
      eval_block(is->then_block, inner);
    } else if (is->else_block) {
      Environment inner(&env);
      eval_block(*is->else_block, inner);
    }
    return;
  }

  // as long as <cond> { ... }
  if (auto *ws = dynamic_cast<const WhileStatement *>(s)) {
    while (eval_expression(ws->condition.get(), env).is_truthy()) {
      Environment inner(&env);
      eval_block(ws->body, inner);
    }
    return;
  }

  // output <expr>  →  throw ReturnSignal to unwind to call site
  if (auto *rs = dynamic_cast<const ReturnStatement *>(s)) {
    throw ReturnSignal{eval_expression(rs->value.get(), env)};
  }

  // standalone expression (e.g. a function call used for its side effect)
  if (auto *es = dynamic_cast<const ExpressionStatement *>(s)) {
    eval_expression(es->expr.get(), env);
    return;
  }

  throw std::runtime_error("Unknown statement type");
}

// ── Expressions
// ───────────────────────────────────────────────────────────────

Value Interpreter::eval_expression(const Expression *e, Environment &env) {

  // Literals — base cases, no recursion needed
  if (auto *n = dynamic_cast<const NumberExpression *>(e))
    return Value::make_number(n->value);

  if (auto *s = dynamic_cast<const StringExpression *>(e))
    return Value::make_string(s->value);

  if (auto *b = dynamic_cast<const BoolExpression *>(e))
    return Value::make_bool(b->value);

  // Variable lookup — walk outward through scopes
  if (auto *i = dynamic_cast<const IdentExpression *>(e))
    return env.get(i->name);

  // Binary operations — evaluate both sides, then apply operator
  if (auto *bin = dynamic_cast<const BinaryExpression *>(e)) {
    Value left = eval_expression(bin->left.get(), env);
    Value right = eval_expression(bin->right.get(), env);

    // + supports both numbers and string concatenation
    if (bin->op == "+") {
      if (left.kind == Value::Kind::String || right.kind == Value::Kind::String)
        return Value::make_string(left.to_string() + right.to_string());
      return Value::make_number(left.number + right.number);
    }
    if (bin->op == "-")
      return Value::make_number(left.number - right.number);
    if (bin->op == "*")
      return Value::make_number(left.number * right.number);
    if (bin->op == "/") {
      if (right.number == 0)
        throw std::runtime_error("Division by zero");
      return Value::make_number(left.number / right.number);
    }
    if (bin->op == ">")
      return Value::make_bool(left.number > right.number);
    if (bin->op == "<")
      return Value::make_bool(left.number < right.number);
    if (bin->op == "==") {
      if (left.kind != right.kind)
        return Value::make_bool(false);
      switch (left.kind) {
      case Value::Kind::Number:
        return Value::make_bool(left.number == right.number);
      case Value::Kind::String:
        return Value::make_bool(left.str == right.str);
      case Value::Kind::Bool:
        return Value::make_bool(left.boolean == right.boolean);
      case Value::Kind::Nil:
        return Value::make_bool(true);
      }
    }
    if (bin->op == "!=") {
      if (left.kind != right.kind)
        return Value::make_bool(true);
      switch (left.kind) {
      case Value::Kind::Number:
        return Value::make_bool(left.number != right.number);
      case Value::Kind::String:
        return Value::make_bool(left.str != right.str);
      case Value::Kind::Bool:
        return Value::make_bool(left.boolean != right.boolean);
      case Value::Kind::Nil:
        return Value::make_bool(false);
      }
    }
    throw std::runtime_error("Unknown operator: " + bin->op);
  }

  // Function calls
  if (auto *c = dynamic_cast<const CallExpression *>(e)) {

    // Built-in: print
    if (c->callee == "print") {
      for (auto &arg : c->args)
        std::cout << eval_expression(arg.get(), env).to_string();
      std::cout << "\n";
      return Value::make_nil();
    }

    // User-defined: evaluate args, then call
    std::vector<Value> args;
    for (auto &arg : c->args)
      args.push_back(eval_expression(arg.get(), env));
    return call_function(c->callee, args);
  }

  throw std::runtime_error("Unknown expression type");
}

// ── Function calls
// ────────────────────────────────────────────────────────────

Value Interpreter::call_function(const std::string &name,
                                 const std::vector<Value> &args) {
  auto it = functions.find(name);
  if (it == functions.end())
    throw std::runtime_error("Undefined function '" + name + "'");

  TwigFunction &fn = it->second;
  if (args.size() != fn.params.size())
    throw std::runtime_error("'" + name + "' expects " +
                             std::to_string(fn.params.size()) + " args, got " +
                             std::to_string(args.size()));

  // Each function call gets its own scope, rooted at global
  Environment fn_env(&global);
  for (size_t i = 0; i < fn.params.size(); i++)
    fn_env.set(fn.params[i], args[i]);

  try {
    eval_block(*fn.body, fn_env);
  } catch (ReturnSignal &ret) {
    return ret.value; // 'output' was hit — hand the value back up
  }
  return Value::make_nil(); // function fell off the end without output
}

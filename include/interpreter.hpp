#pragma once
#include "ast.hpp"
#include <stdexcept>
#include <string>
#include <unordered_map>

// ── Runtime value
// ───────────────────────────────────────────────────────────── Everything in
// Twig evaluates to one of these at runtime
struct Value {
  enum class Kind { Number, String, Bool, Nil };
  Kind kind = Kind::Nil;
  double number = 0;
  std::string str;
  bool boolean = false;

  // Factory methods — cleaner than constructors
  static Value make_number(double n) {
    Value v;
    v.kind = Kind::Number;
    v.number = n;
    return v;
  }
  static Value make_string(std::string s) {
    Value v;
    v.kind = Kind::String;
    v.str = std::move(s);
    return v;
  }
  static Value make_bool(bool b) {
    Value v;
    v.kind = Kind::Bool;
    v.boolean = b;
    return v;
  }
  static Value make_nil() { return Value{}; }

  // Convert to printable string
  std::string to_string() const {
    switch (kind) {
    case Kind::Number: {
      char buf[64];
      snprintf(buf, sizeof(buf), "%g", number); // %g strips trailing zeros
      return buf;
    }
    case Kind::String:
      return str;
    case Kind::Bool:
      return boolean ? "true" : "false";
    case Kind::Nil:
      return "nil";
    }
    return "nil";
  }

  // Only false and nil are falsy — everything else is truthy
  bool is_truthy() const {
    if (kind == Kind::Bool)
      return boolean;
    if (kind == Kind::Nil)
      return false;
    return true;
  }
};

// ── Return signal
// ───────────────────────────────────────────────────────────── Thrown when
// 'output' is hit — unwinds the stack back to the call site. Using an exception
// here is intentional: it's the cleanest way to exit arbitrarily deep recursion
// inside a function body.
struct ReturnSignal {
  Value value;
};

// ── Environment (scope)
// ─────────────────────────────────────────────────────── Each scope holds its
// own variables and a pointer to its enclosing scope. Looking up a variable
// walks outward until found or throws.
class Environment {
  std::unordered_map<std::string, Value> values;
  Environment *outer; // raw pointer is safe — outer always outlives inner

public:
  Environment(Environment *outer = nullptr) : outer(outer) {}

  void set(const std::string &name, Value val) {
    // Walk up the chain — if variable exists anywhere, update it there
    Environment *env = this;
    while (env) {
      auto it = env->values.find(name);
      if (it != env->values.end()) {
        it->second = std::move(val);
        return;
      }
      env = env->outer;
    }
    // Doesn't exist anywhere yet — create in current scope
    values[name] = std::move(val);
  }

  Value get(const std::string &name) const {
    auto it = values.find(name);
    if (it != values.end())
      return it->second;
    if (outer)
      return outer->get(name);
    throw std::runtime_error("Undefined variable '" + name + "'");
  }
};

// ── Stored function definition
// ────────────────────────────────────────────────
struct TwigFunction {
  std::vector<std::string> params;
  const Block *body; // points into the AST — valid for program lifetime
};

// ── Interpreter
// ───────────────────────────────────────────────────────────────
class Interpreter {
  Environment global;
  std::unordered_map<std::string, TwigFunction> functions;

public:
  void run(const Program &program);

private:
  Value eval_expression(const Expression *e, Environment &env);
  void eval_statement(const Statement *s, Environment &env);
  void eval_block(const Block &b, Environment &env);
  Value call_function(const std::string &name, const std::vector<Value> &args);
};

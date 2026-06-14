#pragma once
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "ast.hpp"

// ── Runtime value ─────────────────────────────────────────────────────────────

struct Value {
    enum class Kind { Number, String, Bool, List, Shape, Nil };

    Kind        kind    = Kind::Nil;
    double      number  = 0;
    std::string str;
    bool        boolean = false;
    std::shared_ptr<std::vector<Value>> list;

    // Shape data — shared_ptr so field mutation is visible through all copies
    std::string shape_type;
    std::shared_ptr<std::unordered_map<std::string, Value>> shape;

    // ── Factory methods ───────────────────────────────────────────────────────

    static Value make_number(double n) {
        Value v; v.kind = Kind::Number; v.number = n; return v;
    }
    static Value make_string(std::string s) {
        Value v; v.kind = Kind::String; v.str = std::move(s); return v;
    }
    static Value make_bool(bool b) {
        Value v; v.kind = Kind::Bool; v.boolean = b; return v;
    }
    static Value make_nil() { return Value{}; }
    static Value make_list(std::vector<Value> items = {}) {
        Value v;
        v.kind = Kind::List;
        v.list = std::make_shared<std::vector<Value>>(std::move(items));
        return v;
    }
    static Value make_shape(std::string type_name,
                            std::unordered_map<std::string, Value> fields) {
        Value v;
        v.kind       = Kind::Shape;
        v.shape_type = std::move(type_name);
        v.shape      = std::make_shared<std::unordered_map<std::string, Value>>(
            std::move(fields));
        return v;
    }

    // ── Conversions ───────────────────────────────────────────────────────────

    std::string to_string() const {
        switch (kind) {
            case Kind::Number: {
                char buf[64];
                snprintf(buf, sizeof(buf), "%g", number);
                return buf;
            }
            case Kind::String: return str;
            case Kind::Bool:   return boolean ? "true" : "false";
            case Kind::Nil:    return "nil";
            case Kind::List: {
                std::string s = "[";
                for (size_t i = 0; i < list->size(); i++) {
                    if (i) s += ", ";
                    s += (*list)[i].to_string();
                }
                return s + "]";
            }
            case Kind::Shape: {
                std::string s = shape_type + " { ";
                bool first = true;
                for (auto& [k, v] : *shape) {
                    if (!first) s += ", ";
                    s += k + ": " + v.to_string();
                    first = false;
                }
                return s + " }";
            }
        }
        return "nil";
    }

    bool is_truthy() const {
        if (kind == Kind::Bool) return boolean;
        if (kind == Kind::Nil)  return false;
        if (kind == Kind::List) return !list->empty();
        return true; // numbers, strings, shapes always truthy
    }
};

// ── Control-flow signals ──────────────────────────────────────────────────────

struct ReturnSignal { Value value; };  // thrown by ReturnStatement
struct StopSignal   {};                // thrown by StopStatement (break)
struct SkipSignal   {};                // thrown by SkipStatement (continue)

// ── Environment ───────────────────────────────────────────────────────────────

// Lexically-scoped variable store with mutability tracking.
class Environment {
    std::unordered_map<std::string, Value> values;
    std::unordered_map<std::string, bool>  mutability;
    Environment* outer;

public:
    Environment(Environment* outer = nullptr) : outer(outer) {}

    // declare: if 'name' already exists in this scope, check mutability and update.
    // If not found in this scope, create a new binding here with the given mutability.
    // Does NOT walk outer scopes — use assign() for cross-scope mutation.
    void declare(const std::string& name, Value val, bool is_mutable) {
        auto it = values.find(name);
        if (it != values.end()) {
            if (!mutability[name])
                throw std::runtime_error(
                    "Cannot reassign immutable variable '" + name + "'");
            it->second = std::move(val);
            // Mutability is locked at first declaration — 'let x = new_val'
            // keeps the original mutable/immutable status.
            return;
        }
        values[name]     = std::move(val);
        mutability[name] = is_mutable;
    }

    // assign: walk outer scopes to update an existing binding.
    // Throws if the variable is immutable or does not exist anywhere.
    // Reserved for future explicit assignment syntax (e.g. `x = val` without 'let').
    void assign(const std::string& name, Value val) {
        Environment* env = this;
        while (env) {
            auto it = env->values.find(name);
            if (it != env->values.end()) {
                if (!env->mutability[name])
                    throw std::runtime_error(
                        "Cannot reassign immutable variable '" + name + "'");
                it->second = std::move(val);
                return;
            }
            env = env->outer;
        }
        throw std::runtime_error("Undefined variable '" + name + "'");
    }

    // bind: unconditionally create/overwrite a binding in this scope.
    // Used for function parameters and loop iteration variables where we always
    // want a fresh local binding regardless of what outer scopes contain.
    void bind(const std::string& name, Value val, bool is_mutable = true) {
        values[name]     = std::move(val);
        mutability[name] = is_mutable;
    }

    Value get(const std::string& name) const {
        auto it = values.find(name);
        if (it != values.end()) return it->second;
        if (outer) return outer->get(name);
        throw std::runtime_error("Undefined variable '" + name + "'");
    }
};

// ── Callable types ────────────────────────────────────────────────────────────

struct SprigFunction {
    std::vector<std::string> params;
    const Block*             body;
};

// Registered schema for a shape (field names and declared types).
struct SprigShapeDefinition {
    std::vector<ShapeField> fields;
};

// ── Interpreter ───────────────────────────────────────────────────────────────

class Interpreter {
    Environment global;
    std::unordered_map<std::string, SprigFunction>        functions;
    std::unordered_map<std::string, SprigShapeDefinition> shapes;

    std::vector<std::string>        source_lines;    // split from source for error display
    std::string                     base_path;       // directory of the main file
    std::unordered_set<std::string> included_files;  // canonical paths already run (dedup)
    std::vector<Program>            owned_programs;  // keeps included ASTs alive (SprigFunction holds raw ptrs into them)

public:
    void run(const Program& program, const std::string& source,
             const std::string& file_path = "");

private:
    // Formats a runtime error with source context:
    //   Error at line N:
    //     <source line>
    //   <message>
    std::string make_error(const std::string& msg, int line) const;

    void  eval_block(const Block& b, Environment& env);
    void  eval_statement(const Statement* s, Environment& env);
    Value eval_expression(const Expression* e, Environment& env);
    Value call_function(const std::string& name,
                        const std::vector<Value>& args, int line);
};

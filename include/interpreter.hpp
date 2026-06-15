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

    static Value make_number(double value) {
        Value result; result.kind = Kind::Number; result.number = value; return result;
    }
    static Value make_string(std::string text) {
        Value result; result.kind = Kind::String; result.str = std::move(text); return result;
    }
    static Value make_bool(bool value) {
        Value result; result.kind = Kind::Bool; result.boolean = value; return result;
    }
    static Value make_nil() { return Value{}; }
    static Value make_list(std::vector<Value> items = {}) {
        Value result;
        result.kind = Kind::List;
        result.list = std::make_shared<std::vector<Value>>(std::move(items));
        return result;
    }
    static Value make_shape(std::string type_name,
                            std::unordered_map<std::string, Value> fields) {
        Value result;
        result.kind       = Kind::Shape;
        result.shape_type = std::move(type_name);
        result.shape      = std::make_shared<std::unordered_map<std::string, Value>>(
            std::move(fields));
        return result;
    }

    // ── Conversions ───────────────────────────────────────────────────────────

    std::string to_string() const {
        switch (kind) {
            case Kind::Number: {
                char number_buf[64];
                snprintf(number_buf, sizeof(number_buf), "%g", number);
                return number_buf;
            }
            case Kind::String: return str;
            case Kind::Bool:   return boolean ? "true" : "false";
            case Kind::Nil:    return "nil";
            case Kind::List: {
                std::string result = "[";
                for (size_t i = 0; i < list->size(); i++) {
                    if (i) result += ", ";
                    result += (*list)[i].to_string();
                }
                return result + "]";
            }
            case Kind::Shape: {
                std::string result = shape_type + " { ";
                bool first = true;
                for (auto& [field_key, field_value] : *shape) {
                    if (!first) result += ", ";
                    result += field_key + ": " + field_value.to_string();
                    first = false;
                }
                return result + " }";
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
    void declare(const std::string& name, Value new_value, bool is_mutable) {
        auto existing = values.find(name);
        if (existing != values.end()) {
            if (!mutability[name])
                throw std::runtime_error(
                    "Cannot reassign immutable variable '" + name + "'");
            existing->second = std::move(new_value);
            // Mutability is locked at first declaration — 'let x = new_val'
            // keeps the original mutable/immutable status.
            return;
        }
        values[name]     = std::move(new_value);
        mutability[name] = is_mutable;
    }

    // assign: walk outer scopes to update an existing binding.
    // Throws if the variable is immutable or does not exist anywhere.
    // Reserved for future explicit assignment syntax (e.g. `x = val` without 'let').
    void assign(const std::string& name, Value new_value) {
        Environment* env = this;
        while (env) {
            auto existing = env->values.find(name);
            if (existing != env->values.end()) {
                if (!env->mutability[name])
                    throw std::runtime_error(
                        "Cannot reassign immutable variable '" + name + "'");
                existing->second = std::move(new_value);
                return;
            }
            env = env->outer;
        }
        throw std::runtime_error("Undefined variable '" + name + "'");
    }

    // bind: unconditionally create/overwrite a binding in this scope.
    // Used for function parameters and loop iteration variables where we always
    // want a fresh local binding regardless of what outer scopes contain.
    void bind(const std::string& name, Value new_value, bool is_mutable = true) {
        values[name]     = std::move(new_value);
        mutability[name] = is_mutable;
    }

    Value get(const std::string& name) const {
        auto entry = values.find(name);
        if (entry != values.end()) return entry->second;
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
    std::vector<std::string>        program_args;    // argv passed from main

public:
    void run(const Program& program, const std::string& source,
             const std::string& file_path = "",
             std::vector<std::string> args = {});

private:
    // Formats a runtime error with source context:
    //   Error at line N:
    //     <source line>
    //   <message>
    std::string make_error(const std::string& msg, int line) const;

    void  eval_block(const Block& block, Environment& env);
    void  eval_statement(const Statement* stmt, Environment& env);
    Value eval_expression(const Expression* expr, Environment& env);
    Value call_function(const std::string& name,
                        const std::vector<Value>& args, int line);
};

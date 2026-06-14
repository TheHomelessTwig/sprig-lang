#pragma once
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "ast.hpp"

struct Value {
    enum class Kind { Number, String, Bool, List, Nil };
    Kind kind = Kind::Nil;
    double number = 0;
    std::string str;
    bool boolean = false;
    std::shared_ptr<std::vector<Value>> list;

    static Value make_list(std::vector<Value> items = {}) {
        Value v;
        v.kind = Kind::List;
        v.list = std::make_shared<std::vector<Value>>(std::move(items));
        return v;
    }

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

    std::string to_string() const {
        switch (kind) {
            case Kind::Number: {
                char buf[64];
                snprintf(buf, sizeof(buf), "%g", number);
                return buf;
            }
            case Kind::String:
                return str;
            case Kind::Bool:
                return boolean ? "true" : "false";
            case Kind::Nil:
                return "nil";
            case Kind::List: {
                std::string s = "[";
                for (size_t i = 0; i < list->size(); i++) {
                    if (i) s += ", ";
                    s += (*list)[i].to_string();
                }
                return s + "]";
            }
        }
        return "nil";
    }

    bool is_truthy() const {
        if (kind == Kind::Bool) return boolean;
        if (kind == Kind::Nil) return false;
        if (kind == Kind::List) return !list->empty();
        return true;
    }
};

struct ReturnSignal {
    Value value;
};
struct StopSignal {};
struct SkipSignal {};

class Environment {
    std::unordered_map<std::string, Value> values;
    Environment *outer;

public:
    Environment(Environment *outer = nullptr) : outer(outer) {}

    void set(const std::string &name, Value val) {
        Environment *env = this;
        while (env) {
            auto it = env->values.find(name);
            if (it != env->values.end()) {
                it->second = std::move(val);
                return;
            }
            env = env->outer;
        }
        values[name] = std::move(val);
    }

    Value get(const std::string &name) const {
        auto it = values.find(name);
        if (it != values.end()) return it->second;
        if (outer) return outer->get(name);
        throw std::runtime_error("Undefined variable '" + name + "'");
    }
};

struct SprigFunction {
    std::vector<std::string> params;
    const Block *body;
};

class Interpreter {
    Environment global;
    std::unordered_map<std::string, SprigFunction> functions;

public:
    void run(const Program &program);

private:
    Value eval_expression(const Expression *e, Environment &env);
    void eval_statement(const Statement *s, Environment &env);
    void eval_block(const Block &b, Environment &env);
    Value call_function(const std::string &name,
                        const std::vector<Value> &args);
};

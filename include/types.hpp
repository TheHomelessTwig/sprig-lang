#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct Expression;

// ── Type representation ───────────────────────────────────────────────────────

struct Type;
using TypePtr = std::shared_ptr<Type>;

struct Type {
    enum class Kind {
        Number,    // number / decimal
        Text,      // text
        Flag,      // flag (bool)
        Nothing,   // nil / nothing
        List,      // List<T>
        Shape,     // user-defined shape
        Function,  // (T1, T2, ...) -> Tret
        Var,       // type variable — unknown, to be resolved by unification
    };

    Kind kind;

    TypePtr    element_type;              // for List
    std::string shape_name;              // for Shape
    std::vector<TypePtr> param_types;    // for Function
    TypePtr    return_type;              // for Function
    int        var_id = -1;             // for Var — unique ID

    // ── Factory methods ───────────────────────────────────────────────────────

    static TypePtr make_number() {
        auto t = std::make_shared<Type>(); t->kind = Kind::Number;  return t;
    }
    static TypePtr make_text() {
        auto t = std::make_shared<Type>(); t->kind = Kind::Text;    return t;
    }
    static TypePtr make_flag() {
        auto t = std::make_shared<Type>(); t->kind = Kind::Flag;    return t;
    }
    static TypePtr make_nothing() {
        auto t = std::make_shared<Type>(); t->kind = Kind::Nothing; return t;
    }
    static TypePtr make_list(TypePtr elem) {
        auto t = std::make_shared<Type>();
        t->kind         = Kind::List;
        t->element_type = std::move(elem);
        return t;
    }
    static TypePtr make_shape(std::string name) {
        auto t = std::make_shared<Type>();
        t->kind       = Kind::Shape;
        t->shape_name = std::move(name);
        return t;
    }
    static TypePtr make_function(std::vector<TypePtr> params, TypePtr ret) {
        auto t = std::make_shared<Type>();
        t->kind        = Kind::Function;
        t->param_types = std::move(params);
        t->return_type = std::move(ret);
        return t;
    }
    static TypePtr make_var(int id) {
        auto t = std::make_shared<Type>();
        t->kind   = Kind::Var;
        t->var_id = id;
        return t;
    }

    // ── Display ───────────────────────────────────────────────────────────────

    std::string to_string() const {
        switch (kind) {
            case Kind::Number:  return "number";
            case Kind::Text:    return "text";
            case Kind::Flag:    return "flag";
            case Kind::Nothing: return "nothing";
            case Kind::List:    return "list[" + element_type->to_string() + "]";
            case Kind::Shape:   return shape_name;
            case Kind::Var:     return "?" + std::to_string(var_id);
            case Kind::Function: {
                std::string s = "(";
                for (size_t i = 0; i < param_types.size(); i++) {
                    if (i) s += ", ";
                    s += param_types[i]->to_string();
                }
                return s + ") -> " + return_type->to_string();
            }
        }
        return "unknown";
    }
};

using ExprTypeMap = std::unordered_map<const Expression*, TypePtr>;

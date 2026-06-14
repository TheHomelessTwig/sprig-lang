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
        Own,       // Box<T> — owned heap pointer
        RawPtr,    // unsafe raw pointer (*mut void)
    };

    Kind kind;

    TypePtr    element_type;              // for List
    std::string shape_name;              // for Shape
    std::vector<TypePtr> param_types;    // for Function
    TypePtr    return_type;              // for Function
    int        var_id = -1;             // for Var — unique ID

    // ── Factory methods ───────────────────────────────────────────────────────

    static TypePtr make_number() {
        auto new_type = std::make_shared<Type>(); new_type->kind = Kind::Number;  return new_type;
    }
    static TypePtr make_text() {
        auto new_type = std::make_shared<Type>(); new_type->kind = Kind::Text;    return new_type;
    }
    static TypePtr make_flag() {
        auto new_type = std::make_shared<Type>(); new_type->kind = Kind::Flag;    return new_type;
    }
    static TypePtr make_nothing() {
        auto new_type = std::make_shared<Type>(); new_type->kind = Kind::Nothing; return new_type;
    }
    static TypePtr make_list(TypePtr elem) {
        auto new_type = std::make_shared<Type>();
        new_type->kind         = Kind::List;
        new_type->element_type = std::move(elem);
        return new_type;
    }
    static TypePtr make_shape(std::string name) {
        auto new_type = std::make_shared<Type>();
        new_type->kind       = Kind::Shape;
        new_type->shape_name = std::move(name);
        return new_type;
    }
    static TypePtr make_function(std::vector<TypePtr> params, TypePtr ret) {
        auto new_type = std::make_shared<Type>();
        new_type->kind        = Kind::Function;
        new_type->param_types = std::move(params);
        new_type->return_type = std::move(ret);
        return new_type;
    }
    static TypePtr make_var(int id) {
        auto new_type = std::make_shared<Type>();
        new_type->kind   = Kind::Var;
        new_type->var_id = id;
        return new_type;
    }
    static TypePtr make_own(TypePtr inner) {
        auto new_type = std::make_shared<Type>();
        new_type->kind         = Kind::Own;
        new_type->element_type = std::move(inner);
        return new_type;
    }
    static TypePtr make_raw_ptr() {
        auto new_type = std::make_shared<Type>();
        new_type->kind = Kind::RawPtr;
        return new_type;
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
            case Kind::Own:     return "own " + element_type->to_string();
            case Kind::RawPtr:  return "raw_ptr";
            case Kind::Function: {
                std::string result = "(";
                for (size_t i = 0; i < param_types.size(); i++) {
                    if (i) result += ", ";
                    result += param_types[i]->to_string();
                }
                return result + ") -> " + return_type->to_string();
            }
        }
        return "unknown";
    }
};

using ExprTypeMap = std::unordered_map<const Expression*, TypePtr>;

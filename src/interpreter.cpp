#include "interpreter.hpp"

#include <cmath>
#include <iostream>
#include <string>

void Interpreter::run(const Program &program) {
    try {
        for (auto &stmt : program.stmts) eval_statement(stmt.get(), global);
    } catch (ReturnSignal &) {
    }
}

void Interpreter::eval_block(const Block &b, Environment &env) {
    for (auto &stmt : b.stmts) eval_statement(stmt.get(), env);
}

void Interpreter::eval_statement(const Statement *s, Environment &env) {
    if (auto *vs = dynamic_cast<const VariableStatement *>(s)) {
        env.set(vs->name, eval_expression(vs->value.get(), env));
        return;
    }

    if (auto *fs = dynamic_cast<const FunctionStatement *>(s)) {
        functions[fs->name] = SprigFunction{fs->params, &fs->body};
        return;
    }

    if (auto *is = dynamic_cast<const IfStatement *>(s)) {
        Value cond = eval_expression(is->condition.get(), env);
        if (cond.is_truthy()) {
            Environment inner(&env);
            eval_block(is->then_block, inner);
        } else if (is->else_block) {
            Environment inner(&env);
            eval_block(*is->else_block, inner);
        }
        return;
    }

    if (auto *ws = dynamic_cast<const WhileStatement *>(s)) {
        while (eval_expression(ws->condition.get(), env).is_truthy()) {
            Environment inner(&env);
            try {
                eval_block(ws->body, inner);
            } catch (StopSignal &) {
                break;
            } catch (SkipSignal &) {
                continue;
            }
        }
        return;
    }

    if (auto *rs = dynamic_cast<const ReturnStatement *>(s)) {
        throw ReturnSignal{eval_expression(rs->value.get(), env)};
    }

    if (auto *es = dynamic_cast<const ExpressionStatement *>(s)) {
        eval_expression(es->expr.get(), env);
        return;
    }

    if (auto *fe = dynamic_cast<const ForEachStatement *>(s)) {
        Value iterable = eval_expression(fe->iterable.get(), env);
        if (iterable.kind != Value::Kind::List)
            throw std::runtime_error("'for each' requires a list");
        for (auto &item : *iterable.list) {
            Environment inner(&env);
            inner.set(fe->variable, item);
            try {
                eval_block(fe->body, inner);
            } catch (StopSignal &) {
                break;
            } catch (SkipSignal &) {
                continue;
            }
        }
        return;
    }

    if (dynamic_cast<const StopStatement *>(s)) throw StopSignal{};

    if (dynamic_cast<const SkipStatement *>(s)) throw SkipSignal{};

    throw std::runtime_error("Unknown statement type");
}

Value Interpreter::eval_expression(const Expression *e, Environment &env) {
    if (auto *n = dynamic_cast<const NumberExpression *>(e))
        return Value::make_number(n->value);

    if (auto *s = dynamic_cast<const StringExpression *>(e))
        return Value::make_string(s->value);

    if (auto *b = dynamic_cast<const BoolExpression *>(e))
        return Value::make_bool(b->value);

    if (auto *i = dynamic_cast<const IdentExpression *>(e))
        return env.get(i->name);

    if (auto *bin = dynamic_cast<const BinaryExpression *>(e)) {
        Value left = eval_expression(bin->left.get(), env);
        Value right = eval_expression(bin->right.get(), env);

        if (bin->op == "+") {
            if (left.kind == Value::Kind::String ||
                right.kind == Value::Kind::String)
                return Value::make_string(left.to_string() + right.to_string());
            return Value::make_number(left.number + right.number);
        }
        if (bin->op == "-")
            return Value::make_number(left.number - right.number);
        if (bin->op == "*")
            return Value::make_number(left.number * right.number);
        if (bin->op == "/") {
            if (right.number == 0) throw std::runtime_error("Division by zero");
            return Value::make_number(left.number / right.number);
        }
        if (bin->op == ">") return Value::make_bool(left.number > right.number);

        if (bin->op == "<") return Value::make_bool(left.number < right.number);

        if (bin->op == "==") {
            if (left.kind != right.kind) return Value::make_bool(false);
            switch (left.kind) {
                case Value::Kind::Number:
                    return Value::make_bool(left.number == right.number);
                case Value::Kind::String:
                    return Value::make_bool(left.str == right.str);
                case Value::Kind::Bool:
                    return Value::make_bool(left.boolean == right.boolean);
                case Value::Kind::Nil:
                    return Value::make_bool(true);
                case Value::Kind::List:
                    return Value::make_bool(left.list == right.list);
            }
        }

        if (bin->op == "!=") {
            if (left.kind != right.kind) return Value::make_bool(true);
            switch (left.kind) {
                case Value::Kind::Number:
                    return Value::make_bool(left.number != right.number);
                case Value::Kind::String:
                    return Value::make_bool(left.str != right.str);
                case Value::Kind::Bool:
                    return Value::make_bool(left.boolean != right.boolean);
                case Value::Kind::Nil:
                    return Value::make_bool(false);
                case Value::Kind::List:
                    return Value::make_bool(left.list != right.list);
            }
        }

        if (bin->op == "and") {
            Value left = eval_expression(bin->left.get(), env);
            if (!left.is_truthy()) return Value::make_bool(false);
            return Value::make_bool(
                eval_expression(bin->right.get(), env).is_truthy());
        }
        if (bin->op == "or") {
            Value left = eval_expression(bin->left.get(), env);
            if (left.is_truthy()) return Value::make_bool(true);
            return Value::make_bool(
                eval_expression(bin->right.get(), env).is_truthy());
        }
        throw std::runtime_error("Unknown operator: " + bin->op);
    }

    if (auto *c = dynamic_cast<const CallExpression *>(e)) {
        if (c->callee == "print") {
            for (auto &arg : c->args)
                std::cout << eval_expression(arg.get(), env).to_string();
            std::cout << "\n";
            return Value::make_nil();
        }
        if (c->callee == "length") {
            if (c->args.size() != 1)
                throw std::runtime_error("length() takes 1 argument");
            Value val = eval_expression(c->args[0].get(), env);
            if (val.kind == Value::Kind::List)
                return Value::make_number((double)val.list->size());
            if (val.kind == Value::Kind::String)
                return Value::make_number((double)val.str.size());
            throw std::runtime_error("length() requires a list or string");
        }
        if (c->callee == "append") {
            if (c->args.size() != 2)
                throw std::runtime_error("append() takes 2 arguments");
            Value lst = eval_expression(c->args[0].get(), env);
            Value item = eval_expression(c->args[1].get(), env);
            if (lst.kind != Value::Kind::List)
                throw std::runtime_error("append() requires a list");
            lst.list->push_back(std::move(item));
            return Value::make_nil();
        }
        if (c->callee == "first") {
            if (c->args.size() != 1)
                throw std::runtime_error("first() takes 1 argument");
            Value val = eval_expression(c->args[0].get(), env);
            if (val.kind != Value::Kind::List || val.list->empty())
                throw std::runtime_error("first() requires a non-empty list");
            return val.list->front();
        }
        if (c->callee == "last") {
            if (c->args.size() != 1)
                throw std::runtime_error("last() takes 1 argument");
            Value val = eval_expression(c->args[0].get(), env);
            if (val.kind != Value::Kind::List || val.list->empty())
                throw std::runtime_error("last() requires a non-empty list");
            return val.list->back();
        }
        if (c->callee == "input") {
            if (c->args.size() > 1)
                throw std::runtime_error("input() takes 0 or 1 arguments");
            if (c->args.size() == 1)
                std::cout << eval_expression(c->args[0].get(), env).to_string();
            std::string line;
            std::getline(std::cin, line);
            return Value::make_string(line);
        }

        if (c->callee == "to_number") {
            if (c->args.size() != 1)
                throw std::runtime_error("to_number() takes 1 argument");
            Value val = eval_expression(c->args[0].get(), env);
            if (val.kind == Value::Kind::Number) return val;
            if (val.kind == Value::Kind::String) {
                try {
                    return Value::make_number(std::stod(val.str));
                } catch (...) {
                    throw std::runtime_error("Cannot convert '" + val.str +
                                             "' to number");
                }
            }
            throw std::runtime_error("to_number() requires a string or number");
        }

        if (c->callee == "to_text") {
            if (c->args.size() != 1)
                throw std::runtime_error("to_text() takes 1 argument");
            return Value::make_string(
                eval_expression(c->args[0].get(), env).to_string());
        }
        std::vector<Value> args;
        for (auto &arg : c->args)
            args.push_back(eval_expression(arg.get(), env));
        return call_function(c->callee, args);
    }

    if (dynamic_cast<const NothingExpression *>(e)) return Value::make_nil();

    if (auto *u = dynamic_cast<const UnaryExpression *>(e)) {
        Value operand = eval_expression(u->operand.get(), env);
        if (u->op == "not") return Value::make_bool(!operand.is_truthy());
        throw std::runtime_error("Unknown unary operator: " + u->op);
    }
    if (auto *le = dynamic_cast<const ListExpression *>(e)) {
        std::vector<Value> items;
        for (auto &elem : le->elements)
            items.push_back(eval_expression(elem.get(), env));
        return Value::make_list(std::move(items));
    }

    if (auto *ie = dynamic_cast<const IndexExpression *>(e)) {
        Value obj = eval_expression(ie->object.get(), env);
        Value idx = eval_expression(ie->index.get(), env);
        if (idx.kind != Value::Kind::Number)
            throw std::runtime_error("Index must be a number");
        int i = (int)idx.number;
        if (obj.kind == Value::Kind::List) {
            if (i < 0 || i >= (int)obj.list->size())
                throw std::runtime_error("List index out of range");
            return (*obj.list)[i];
        }
        if (obj.kind == Value::Kind::String) {
            if (i < 0 || i >= (int)obj.str.size())
                throw std::runtime_error("String index out of range");
            return Value::make_string(std::string(1, obj.str[i]));
        }
        throw std::runtime_error("Cannot index into this type");
    }
    throw std::runtime_error("Unknown expression type");
}

Value Interpreter::call_function(const std::string &name,
                                 const std::vector<Value> &args) {
    auto it = functions.find(name);
    if (it == functions.end())
        throw std::runtime_error("Undefined function '" + name + "'");

    SprigFunction &fn = it->second;
    if (args.size() != fn.params.size())
        throw std::runtime_error("'" + name + "' expects " +
                                 std::to_string(fn.params.size()) +
                                 " args, got " + std::to_string(args.size()));

    Environment fn_env(&global);
    for (size_t i = 0; i < fn.params.size(); i++)
        fn_env.set(fn.params[i], args[i]);

    try {
        eval_block(*fn.body, fn_env);
    } catch (ReturnSignal &ret) {
        return ret.value;
    }
    return Value::make_nil();
}

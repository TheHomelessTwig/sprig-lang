#include "interpreter.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "lexer.hpp"
#include "parser.hpp"

// ── Type helpers ──────────────────────────────────────────────────────────────

// Returns true if 'v' satisfies the declared type string from a ShapeField.
static bool value_matches_type(const Value& v, const std::string& type) {
    if (type == "text")    return v.kind == Value::Kind::String;
    if (type == "number")  return v.kind == Value::Kind::Number;
    if (type == "decimal") return v.kind == Value::Kind::Number;
    if (type == "flag")    return v.kind == Value::Kind::Bool;
    return true; // unknown type — pass through
}

// Human-readable type name for error messages.
static std::string type_name_of(const Value& v) {
    switch (v.kind) {
        case Value::Kind::String: return "text";
        case Value::Kind::Number: return "number";
        case Value::Kind::Bool:   return "flag";
        case Value::Kind::List:   return "list";
        case Value::Kind::Shape:  return v.shape_type;
        case Value::Kind::Nil:    return "nothing";
    }
    return "unknown";
}

// ── Program entry ─────────────────────────────────────────────────────────────

void Interpreter::run(const Program& program, const std::string& source,
                      const std::string& file_path) {
    // Split source into lines so make_error() can show the offending line.
    std::istringstream stream(source);
    std::string        ln;
    while (std::getline(stream, ln))
        source_lines.push_back(ln);

    if (!file_path.empty()) {
        base_path = std::filesystem::path(file_path).parent_path().string();
        try {
            included_files.insert(
                std::filesystem::canonical(file_path).string());
        } catch (...) {
            included_files.insert(file_path);
        }
    }

    try {
        for (auto& stmt : program.stmts)
            eval_statement(stmt.get(), global);
    } catch (ReturnSignal&) {
        // top-level "give back" is silently swallowed
    }
}

// Formats a runtime error with a source-line excerpt:
//   Error at line N:
//     <source line text>
//   <message>
std::string Interpreter::make_error(const std::string& msg, int line) const {
    std::string out = "Error at line " + std::to_string(line) + ":\n";
    if (line >= 1 && line <= (int)source_lines.size())
        out += "  " + source_lines[line - 1] + "\n";
    out += msg;
    return out;
}

void Interpreter::eval_block(const Block& b, Environment& env) {
    for (auto& stmt : b.stmts)
        eval_statement(stmt.get(), env);
}

// ── Statement evaluation ──────────────────────────────────────────────────────

void Interpreter::eval_statement(const Statement* s, Environment& env) {

    // let [mutable] x = expr
    if (auto* vs = dynamic_cast<const VariableStatement*>(s)) {
        Value val = eval_expression(vs->value.get(), env);
        try {
            env.declare(vs->name, std::move(val), vs->is_mutable);
        } catch (std::runtime_error& err) {
            throw std::runtime_error(make_error(err.what(), vs->line));
        }
        return;
    }

    // let x borrow [mutable] y — alias: binds target to source's current value
    // Lists and shapes use shared_ptr so mutations are visible through the alias.
    if (auto* bs = dynamic_cast<const BorrowStatement*>(s)) {
        env.declare(bs->target, env.get(bs->source), false);
        return;
    }
    if (auto* mbs = dynamic_cast<const MutableBorrowStatement*>(s)) {
        env.declare(mbs->target, env.get(mbs->source), false);
        return;
    }

    // define name(params): body  — register without executing
    if (auto* fs = dynamic_cast<const FunctionStatement*>(s)) {
        functions[fs->name] = SprigFunction{fs->params, &fs->body};
        return;
    }

    // shape Person:  — register field schema
    if (auto* sd = dynamic_cast<const ShapeDefinitionStatement*>(s)) {
        shapes[sd->name] = SprigShapeDefinition{sd->fields};
        return;
    }

    // sam.age = 21  — mutate through shared_ptr + enforce declared type
    if (auto* fa = dynamic_cast<const FieldAssignStatement*>(s)) {
        Value obj = env.get(fa->variable);
        if (obj.kind != Value::Kind::Shape)
            throw std::runtime_error(make_error(
                "'" + fa->variable + "' is not a shape", fa->line));

        auto schema_it = shapes.find(obj.shape_type);
        if (schema_it == shapes.end())
            throw std::runtime_error(make_error(
                "Unknown shape type '" + obj.shape_type + "'", fa->line));

        // Find declared type for this field
        const ShapeField* field_def = nullptr;
        for (auto& f : schema_it->second.fields)
            if (f.name == fa->field) { field_def = &f; break; }
        if (!field_def)
            throw std::runtime_error(make_error(
                "Shape '" + obj.shape_type +
                "' has no field '" + fa->field + "'", fa->line));

        Value new_val = eval_expression(fa->value.get(), env);
        if (!value_matches_type(new_val, field_def->type))
            throw std::runtime_error(make_error(
                "Field '" + fa->field + "' expects " + field_def->type +
                " but got " + type_name_of(new_val), fa->line));

        (*obj.shape)[fa->field] = std::move(new_val);
        return;
    }

    // include "path/to/file.sprig"  — lex, parse, run in current context
    if (auto* inc = dynamic_cast<const IncludeStatement*>(s)) {
        std::string path = inc->path;

        // Resolve relative to the including file's directory
        if (!base_path.empty() && !path.empty() && path[0] != '/')
            path = base_path + "/" + path;

        // Deduplicate: skip if already included
        std::string canonical;
        try {
            canonical = std::filesystem::canonical(path).string();
        } catch (...) {
            canonical = path;
        }
        if (included_files.count(canonical)) return;
        included_files.insert(canonical);

        std::ifstream file(path);
        if (!file)
            throw std::runtime_error(make_error(
                "Cannot open include file '" + path + "'", inc->line));

        std::stringstream buf;
        buf << file.rdbuf();
        std::string src = buf.str();

        Lexer   lex(src);
        Parser  par(lex.tokenize());
        // Store the program in owned_programs before running — SprigFunction holds raw
        // pointers into the AST (&fs->body), so the Program must outlive the interpreter.
        owned_programs.push_back(par.parse());
        Program& prog = owned_programs.back();
        for (auto& stmt : prog.stmts)
            eval_statement(stmt.get(), global);
        return;
    }

    // when cond: ...
    if (auto* is = dynamic_cast<const IfStatement*>(s)) {
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

    // as long as cond: ...
    if (auto* ws = dynamic_cast<const WhileStatement*>(s)) {
        while (eval_expression(ws->condition.get(), env).is_truthy()) {
            Environment inner(&env);
            try {
                eval_block(ws->body, inner);
            } catch (StopSignal&) {
                break;
            } catch (SkipSignal&) {
                continue;
            }
        }
        return;
    }

    // for each x in list: ...
    if (auto* fe = dynamic_cast<const ForEachStatement*>(s)) {
        Value iterable = eval_expression(fe->iterable.get(), env);
        if (iterable.kind != Value::Kind::List)
            throw std::runtime_error("'for each' requires a list");
        for (auto& item : *iterable.list) {
            Environment inner(&env);
            // bind() ensures the loop variable is always local to this iteration
            inner.bind(fe->variable, item, true);
            try {
                eval_block(fe->body, inner);
            } catch (StopSignal&) {
                break;
            } catch (SkipSignal&) {
                continue;
            }
        }
        return;
    }

    // give back expr — unwind via signal to call_function()
    if (auto* rs = dynamic_cast<const ReturnStatement*>(s)) {
        throw ReturnSignal{eval_expression(rs->value.get(), env)};
    }

    // stop / skip — unwind to the nearest enclosing loop
    if (dynamic_cast<const StopStatement*>(s)) throw StopSignal{};
    if (dynamic_cast<const SkipStatement*>(s)) throw SkipSignal{};

    // Bare expression statement (e.g. a print() call)
    if (auto* es = dynamic_cast<const ExpressionStatement*>(s)) {
        eval_expression(es->expr.get(), env);
        return;
    }

    throw std::runtime_error("Unknown statement type");
}

// ── Expression evaluation ─────────────────────────────────────────────────────

Value Interpreter::eval_expression(const Expression* e, Environment& env) {

    // ── Literals ──────────────────────────────────────────────────────────────

    if (auto* n = dynamic_cast<const NumberExpression*>(e))
        return Value::make_number(n->value);

    if (auto* s = dynamic_cast<const StringExpression*>(e))
        return Value::make_string(s->value);

    if (auto* b = dynamic_cast<const BoolExpression*>(e))
        return Value::make_bool(b->value);

    if (dynamic_cast<const NothingExpression*>(e))
        return Value::make_nil();

    // ── Variable lookup / borrow expressions ─────────────────────────────────

    // borrow [mutable] x — at runtime just produces the value of x
    if (auto* be = dynamic_cast<const BorrowExpression*>(e))
        return env.get(be->source);
    if (auto* mbe = dynamic_cast<const MutableBorrowExpression*>(e))
        return env.get(mbe->source);

    if (auto* i = dynamic_cast<const IdentExpression*>(e)) {
        try {
            return env.get(i->name);
        } catch (std::runtime_error&) {
            throw std::runtime_error(make_error(
                "Undefined variable '" + i->name + "'", i->line));
        }
    }

    // ── Unary ─────────────────────────────────────────────────────────────────

    if (auto* u = dynamic_cast<const UnaryExpression*>(e)) {
        Value operand = eval_expression(u->operand.get(), env);
        if (u->op == "not") return Value::make_bool(!operand.is_truthy());
        throw std::runtime_error("Unknown unary operator: " + u->op);
    }

    // ── Binary ────────────────────────────────────────────────────────────────

    if (auto* bin = dynamic_cast<const BinaryExpression*>(e)) {
        // Short-circuit logical ops must not force both sides
        if (bin->op == "and") {
            if (!eval_expression(bin->left.get(), env).is_truthy())
                return Value::make_bool(false);
            return Value::make_bool(
                eval_expression(bin->right.get(), env).is_truthy());
        }
        if (bin->op == "or") {
            if (eval_expression(bin->left.get(), env).is_truthy())
                return Value::make_bool(true);
            return Value::make_bool(
                eval_expression(bin->right.get(), env).is_truthy());
        }

        Value left  = eval_expression(bin->left.get(),  env);
        Value right = eval_expression(bin->right.get(), env);

        if (bin->op == "+") {
            // String coercion: if either side is a string, concatenate
            if (left.kind  == Value::Kind::String ||
                right.kind == Value::Kind::String)
                return Value::make_string(left.to_string() + right.to_string());
            return Value::make_number(left.number + right.number);
        }
        if (bin->op == "-") return Value::make_number(left.number - right.number);
        if (bin->op == "*") return Value::make_number(left.number * right.number);
        if (bin->op == "/") {
            if (right.number == 0)
                throw std::runtime_error(
                    make_error("Division by zero", bin->line));
            return Value::make_number(left.number / right.number);
        }
        if (bin->op == ">") return Value::make_bool(left.number > right.number);
        if (bin->op == "<") return Value::make_bool(left.number < right.number);

        if (bin->op == "==") {
            if (left.kind != right.kind) return Value::make_bool(false);
            switch (left.kind) {
                case Value::Kind::Number: return Value::make_bool(left.number  == right.number);
                case Value::Kind::String: return Value::make_bool(left.str     == right.str);
                case Value::Kind::Bool:   return Value::make_bool(left.boolean == right.boolean);
                case Value::Kind::Nil:    return Value::make_bool(true);
                case Value::Kind::List:   return Value::make_bool(left.list    == right.list);
                // Two shapes are equal only if they share the same underlying instance
                case Value::Kind::Shape:  return Value::make_bool(left.shape   == right.shape);
            }
        }
        if (bin->op == "!=") {
            if (left.kind != right.kind) return Value::make_bool(true);
            switch (left.kind) {
                case Value::Kind::Number: return Value::make_bool(left.number  != right.number);
                case Value::Kind::String: return Value::make_bool(left.str     != right.str);
                case Value::Kind::Bool:   return Value::make_bool(left.boolean != right.boolean);
                case Value::Kind::Nil:    return Value::make_bool(false);
                case Value::Kind::List:   return Value::make_bool(left.list    != right.list);
                case Value::Kind::Shape:  return Value::make_bool(left.shape   != right.shape);
            }
        }

        throw std::runtime_error("Unknown operator: " + bin->op);
    }

    // ── List ──────────────────────────────────────────────────────────────────

    if (auto* le = dynamic_cast<const ListExpression*>(e)) {
        std::vector<Value> items;
        for (auto& elem : le->elements)
            items.push_back(eval_expression(elem.get(), env));
        return Value::make_list(std::move(items));
    }

    // collection[i] or string[i]
    if (auto* ie = dynamic_cast<const IndexExpression*>(e)) {
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

    // ── Shape ─────────────────────────────────────────────────────────────────

    // Person { name: "sam", age: 20 }
    if (auto* si = dynamic_cast<const ShapeInstanceExpression*>(e)) {
        auto it = shapes.find(si->shape_name);
        if (it == shapes.end())
            throw std::runtime_error(
                "Unknown shape '" + si->shape_name + "'");

        std::unordered_map<std::string, Value> fields;
        for (auto& [fname, fexpr] : si->fields) {
            // Find declared type for this field
            const ShapeField* def = nullptr;
            for (auto& f : it->second.fields)
                if (f.name == fname) { def = &f; break; }
            if (!def)
                throw std::runtime_error(
                    "Shape '" + si->shape_name +
                    "' has no field '" + fname + "'");

            Value val = eval_expression(fexpr.get(), env);
            if (!value_matches_type(val, def->type))
                throw std::runtime_error(
                    "Field '" + fname + "' expects " + def->type +
                    " but got " + type_name_of(val));

            fields[fname] = std::move(val);
        }

        // Ensure all declared fields are provided
        for (auto& f : it->second.fields)
            if (fields.find(f.name) == fields.end())
                throw std::runtime_error(
                    "Missing field '" + f.name +
                    "' in " + si->shape_name + " instantiation");

        return Value::make_shape(si->shape_name, std::move(fields));
    }

    // sam.name
    if (auto* fa = dynamic_cast<const FieldAccessExpression*>(e)) {
        Value obj = eval_expression(fa->object.get(), env);
        if (obj.kind != Value::Kind::Shape)
            throw std::runtime_error(make_error(
                "Cannot access field on non-shape value", fa->line));
        auto it = obj.shape->find(fa->field);
        if (it == obj.shape->end())
            throw std::runtime_error(make_error(
                "Shape '" + obj.shape_type +
                "' has no field '" + fa->field + "'", fa->line));
        return it->second;
    }

    // ── Function calls ────────────────────────────────────────────────────────

    if (auto* c = dynamic_cast<const CallExpression*>(e)) {
        // Built-ins checked before user-defined functions

        if (c->callee == "print") {
            for (auto& arg : c->args)
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
            Value lst  = eval_expression(c->args[0].get(), env);
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
                    throw std::runtime_error(
                        "Cannot convert '" + val.str + "' to number");
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

        // Fall through to user-defined function
        std::vector<Value> args;
        for (auto& arg : c->args)
            args.push_back(eval_expression(arg.get(), env));
        return call_function(c->callee, args, c->line);
    }

    throw std::runtime_error("Unknown expression type");
}

// ── User-defined function calls ───────────────────────────────────────────────

Value Interpreter::call_function(const std::string& name,
                                 const std::vector<Value>& args, int line) {
    auto it = functions.find(name);
    if (it == functions.end())
        throw std::runtime_error(make_error(
            "Undefined function '" + name + "'", line));

    SprigFunction& fn = it->second;
    if (args.size() != fn.params.size())
        throw std::runtime_error(make_error(
            "'" + name + "' expects " + std::to_string(fn.params.size()) +
            " args, got " + std::to_string(args.size()), line));

    // Each call gets its own scope; global is the shared outer for all functions.
    // bind() ensures params never accidentally shadow or update outer variables.
    Environment fn_env(&global);
    for (size_t i = 0; i < fn.params.size(); i++)
        fn_env.bind(fn.params[i], args[i], true);

    try {
        eval_block(*fn.body, fn_env);
    } catch (ReturnSignal& ret) {
        return ret.value;
    }
    return Value::make_nil();
}

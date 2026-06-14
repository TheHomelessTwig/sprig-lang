#include "typechecker.hpp"

#include <filesystem>
#include <functional>
#include <fstream>
#include <sstream>

#include "lexer.hpp"
#include "parser.hpp"

// ── Public entry point ────────────────────────────────────────────────────────

std::vector<TypeError> TypeChecker::check(const Program& program,
                                           const std::string& source,
                                           const std::string& file_path) {
    // Split source into lines for error context display
    std::istringstream ss(source);
    std::string ln;
    while (std::getline(ss, ln))
        source_lines.push_back(ln);

    if (!file_path.empty()) {
        base_path = std::filesystem::path(file_path).parent_path().string();
        try {
            included_files.insert(std::filesystem::canonical(file_path).string());
        } catch (...) {
            included_files.insert(file_path);
        }
    }

    push_scope();

    // Seed built-in function types.
    // print() accepts any single value — use fresh() so any type unifies.
    bind("print",     Type::make_function({fresh()},            Type::make_nothing()));
    bind("input",     Type::make_function({fresh()},            Type::make_text()));
    bind("length",    Type::make_function({fresh()},            Type::make_number()));
    bind("append",    Type::make_function({fresh(), fresh()},   Type::make_nothing()));
    bind("first",     Type::make_function({fresh()},            fresh()));
    bind("last",      Type::make_function({fresh()},            fresh()));
    bind("to_number", Type::make_function({Type::make_text()},  Type::make_number()));
    bind("to_text",   Type::make_function({fresh()},            Type::make_text()));

    process_program(program);

    pop_scope();
    return errors;
}

// ── Two-pass program processing ───────────────────────────────────────────────

// First pass: register all shape schemas and function signatures so forward
// references work without requiring declaration-before-use ordering.
// Second pass: fully type-check every statement.
void TypeChecker::process_program(const Program& program) {
    // First pass — shapes and function skeletons
    for (auto& stmt : program.stmts) {
        if (auto* inc = dynamic_cast<const IncludeStatement*>(stmt.get())) {
            // Process include immediately so its exports are visible to the rest
            // of this file's first pass
            process_include(inc);
            continue;
        }
        if (auto* sd = dynamic_cast<const ShapeDefinitionStatement*>(stmt.get())) {
            std::vector<std::pair<std::string, TypePtr>> fields;
            for (auto& f : sd->fields) {
                TypePtr ft;
                if      (f.type == "text")    ft = Type::make_text();
                else if (f.type == "number")  ft = Type::make_number();
                else if (f.type == "decimal") ft = Type::make_number();
                else if (f.type == "flag")    ft = Type::make_flag();
                else                          ft = fresh();
                fields.push_back({f.name, ft});
            }
            shape_types[sd->name] = std::move(fields);
            bind(sd->name, Type::make_shape(sd->name));
        }
        if (auto* fn = dynamic_cast<const FunctionStatement*>(stmt.get())) {
            std::vector<TypePtr> param_types;
            for (size_t i = 0; i < fn->params.size(); i++)
                param_types.push_back(fresh());
            bind(fn->name, Type::make_function(std::move(param_types), fresh()));
        }
    }

    // Second pass — full type checking
    TypePtr no_return = Type::make_nothing();
    for (auto& stmt : program.stmts)
        check_statement(stmt.get(), no_return);
}

// Load, parse, and process an included file in the current scope.
void TypeChecker::process_include(const IncludeStatement* inc) {
    std::string path = inc->path;
    if (!base_path.empty() && !path.empty() && path[0] != '/')
        path = base_path + "/" + path;

    std::string canonical;
    try {
        canonical = std::filesystem::canonical(path).string();
    } catch (...) {
        canonical = path;
    }
    if (included_files.count(canonical)) return;
    included_files.insert(canonical);

    std::ifstream file(path);
    if (!file) { error("Cannot open include file '" + path + "'", inc->line); return; }

    std::stringstream buf;
    buf << file.rdbuf();

    Lexer   lex(buf.str());
    Parser  par(lex.tokenize());
    process_program(par.parse());
}

// ── Scope helpers ─────────────────────────────────────────────────────────────

TypePtr TypeChecker::fresh()    { return Type::make_var(next_var_id++); }
void    TypeChecker::push_scope() { scopes.push_back({}); }
void    TypeChecker::pop_scope()  { scopes.pop_back(); }

void TypeChecker::bind(const std::string& name, TypePtr type) {
    scopes.back()[name] = std::move(type);
}

TypePtr TypeChecker::lookup(const std::string& name, int line) {
    for (int i = (int)scopes.size() - 1; i >= 0; i--) {
        auto it = scopes[i].find(name);
        if (it != scopes[i].end()) return resolve(it->second);
    }
    error("Undefined variable '" + name + "'", line);
    return fresh(); // placeholder so checking can continue after this error
}

// ── Unification ───────────────────────────────────────────────────────────────

// Follow the substitution chain to the concrete type (or an unresolved Var).
TypePtr TypeChecker::resolve(TypePtr t) {
    while (t->kind == Type::Kind::Var) {
        auto it = substitutions.find(t->var_id);
        if (it == substitutions.end()) break;
        t = it->second;
    }
    return t;
}

// Make types a and b equal by extending the substitution map, or record an error.
void TypeChecker::unify(TypePtr a, TypePtr b, int line) {
    a = resolve(a);
    b = resolve(b);

    if (a->kind == b->kind) {
        if (a->kind == Type::Kind::List)
            unify(a->element_type, b->element_type, line);
        if (a->kind == Type::Kind::Function) {
            if (a->param_types.size() != b->param_types.size()) {
                error("Function argument count mismatch", line);
                return;
            }
            for (size_t i = 0; i < a->param_types.size(); i++)
                unify(a->param_types[i], b->param_types[i], line);
            unify(a->return_type, b->return_type, line);
        }
        if (a->kind == Type::Kind::Shape && a->shape_name != b->shape_name)
            error("Type mismatch: expected " + a->to_string() +
                  " but got " + b->to_string(), line);
        return;
    }

    // Bind a type variable to the other type
    if (a->kind == Type::Kind::Var) { substitutions[a->var_id] = b; return; }
    if (b->kind == Type::Kind::Var) { substitutions[b->var_id] = a; return; }

    error("Type mismatch: expected " + a->to_string() +
          " but got " + b->to_string(), line);
}

// ── Instantiation ─────────────────────────────────────────────────────────────

// Replace all unbound type variables in t with fresh ones, using a mapping so
// the same var ID always maps to the same fresh var within one instantiation.
// This gives each call site its own copy of a polymorphic function's type.
TypePtr TypeChecker::instantiate(TypePtr t) {
    std::unordered_map<int, TypePtr> mapping;
    std::function<TypePtr(TypePtr)> go = [&](TypePtr t) -> TypePtr {
        t = resolve(t);
        switch (t->kind) {
            case Type::Kind::Var: {
                auto it = mapping.find(t->var_id);
                if (it != mapping.end()) return it->second;
                TypePtr nv = fresh();
                mapping[t->var_id] = nv;
                return nv;
            }
            case Type::Kind::List:
                return Type::make_list(go(t->element_type));
            case Type::Kind::Function: {
                std::vector<TypePtr> params;
                for (auto& p : t->param_types)
                    params.push_back(go(p));
                return Type::make_function(std::move(params), go(t->return_type));
            }
            default:
                return t;  // concrete types are returned as-is
        }
    };
    return go(t);
}

// ── Expression inference ──────────────────────────────────────────────────────

TypePtr TypeChecker::infer_expression(const Expression* e) {
    auto record = [&](TypePtr t) -> TypePtr {
        expr_types[e] = resolve(t);
        return t;
    };

    // ── Literals ──────────────────────────────────────────────────────────────

    if (dynamic_cast<const NumberExpression*>(e))  return record(Type::make_number());
    if (dynamic_cast<const StringExpression*>(e))  return record(Type::make_text());
    if (dynamic_cast<const BoolExpression*>(e))    return record(Type::make_flag());
    if (dynamic_cast<const NothingExpression*>(e)) return record(Type::make_nothing());

    // ── Variable lookup ───────────────────────────────────────────────────────

    if (auto* i = dynamic_cast<const IdentExpression*>(e))
        return record(lookup(i->name, i->line));

    // ── Borrow expressions ────────────────────────────────────────────────────

    if (auto* be = dynamic_cast<const BorrowExpression*>(e))
        return record(lookup(be->source, be->line));
    if (auto* mbe = dynamic_cast<const MutableBorrowExpression*>(e))
        return record(lookup(mbe->source, mbe->line));

    // ── Unary ─────────────────────────────────────────────────────────────────

    if (auto* u = dynamic_cast<const UnaryExpression*>(e)) {
        TypePtr operand = infer_expression(u->operand.get());
        if (u->op == "not") {
            unify(operand, Type::make_flag(), 0);
            return record(Type::make_flag());
        }
        return record(fresh());
    }

    // ── Binary ────────────────────────────────────────────────────────────────

    if (auto* bin = dynamic_cast<const BinaryExpression*>(e)) {
        TypePtr lt = infer_expression(bin->left.get());
        TypePtr rt = infer_expression(bin->right.get());

        // '+' is overloaded: number + number → number, or text + any → text.
        // Check resolved types before committing to a number constraint.
        if (bin->op == "+") {
            TypePtr rlt = resolve(lt);
            TypePtr rrt = resolve(rt);
            if (rlt->kind == Type::Kind::Text || rrt->kind == Type::Kind::Text)
                return record(Type::make_text());
            unify(lt, Type::make_number(), bin->line);
            unify(rt, Type::make_number(), bin->line);
            return record(Type::make_number());
        }
        if (bin->op == "-" || bin->op == "*" || bin->op == "/") {
            unify(lt, Type::make_number(), bin->line);
            unify(rt, Type::make_number(), bin->line);
            return record(Type::make_number());
        }
        if (bin->op == ">" || bin->op == "<") {
            unify(lt, Type::make_number(), bin->line);
            unify(rt, Type::make_number(), bin->line);
            return record(Type::make_flag());
        }
        if (bin->op == "==" || bin->op == "!=") {
            unify(lt, rt, bin->line);
            return record(Type::make_flag());
        }
        if (bin->op == "and" || bin->op == "or") {
            unify(lt, Type::make_flag(), bin->line);
            unify(rt, Type::make_flag(), bin->line);
            return record(Type::make_flag());
        }
        return record(fresh());
    }

    // ── Function call ─────────────────────────────────────────────────────────

    if (auto* c = dynamic_cast<const CallExpression*>(e)) {
        TypePtr fn_type = instantiate(resolve(lookup(c->callee, c->line)));

        std::vector<TypePtr> arg_types;
        for (auto& arg : c->args)
            arg_types.push_back(infer_expression(arg.get()));

        TypePtr ret = fresh();
        unify(fn_type, Type::make_function(std::move(arg_types), ret), c->line);
        return record(resolve(ret));
    }

    // ── List ──────────────────────────────────────────────────────────────────

    if (auto* le = dynamic_cast<const ListExpression*>(e)) {
        TypePtr elem = fresh();
        for (auto& el : le->elements)
            unify(elem, infer_expression(el.get()), 0);
        return record(Type::make_list(resolve(elem)));
    }

    // collection[i]
    if (auto* ie = dynamic_cast<const IndexExpression*>(e)) {
        TypePtr obj  = infer_expression(ie->object.get());
        TypePtr idx  = infer_expression(ie->index.get());
        TypePtr elem = fresh();
        unify(idx, Type::make_number(), 0);
        unify(obj, Type::make_list(elem), 0);
        return record(resolve(elem));
    }

    // ── Shape ─────────────────────────────────────────────────────────────────

    // sam.name
    if (auto* fa = dynamic_cast<const FieldAccessExpression*>(e)) {
        TypePtr obj = resolve(infer_expression(fa->object.get()));
        if (obj->kind == Type::Kind::Shape) {
            auto it = shape_types.find(obj->shape_name);
            if (it != shape_types.end()) {
                for (auto& [fname, ftype] : it->second)
                    if (fname == fa->field) return record(ftype);
                error("Shape '" + obj->shape_name +
                      "' has no field '" + fa->field + "'", fa->line);
            }
        } else if (obj->kind != Type::Kind::Var) {
            error("Cannot access field on non-shape type '" +
                  obj->to_string() + "'", fa->line);
        }
        return record(fresh());
    }

    // Person { name: "sam", age: 20 }
    if (auto* si = dynamic_cast<const ShapeInstanceExpression*>(e)) {
        auto it = shape_types.find(si->shape_name);
        if (it == shape_types.end()) {
            error("Unknown shape '" + si->shape_name + "'", 0);
            return record(fresh());
        }
        for (auto& [fname, fexpr] : si->fields) {
            TypePtr actual = infer_expression(fexpr.get());
            for (auto& [def_name, def_type] : it->second)
                if (def_name == fname) { unify(actual, def_type, 0); break; }
        }
        return record(Type::make_shape(si->shape_name));
    }

    return record(fresh());
}

// ── Statement checking ────────────────────────────────────────────────────────

// check_block pushes its own scope so nested blocks are properly isolated.
void TypeChecker::check_block(const Block& b, TypePtr return_type) {
    push_scope();
    for (auto& stmt : b.stmts)
        check_statement(stmt.get(), return_type);
    pop_scope();
}

void TypeChecker::check_statement(const Statement* s, TypePtr return_type) {

    // include — already processed in first pass, skip here
    if (dynamic_cast<const IncludeStatement*>(s)) return;

    // shape — already registered in first pass
    if (dynamic_cast<const ShapeDefinitionStatement*>(s)) return;

    // let [mutable] x = expr
    if (auto* vs = dynamic_cast<const VariableStatement*>(s)) {
        bind(vs->name, infer_expression(vs->value.get()));
        return;
    }

    // let x borrow [mutable] y — target gets same type as source
    if (auto* bs = dynamic_cast<const BorrowStatement*>(s)) {
        bind(bs->target, lookup(bs->source, bs->line));
        return;
    }
    if (auto* mbs = dynamic_cast<const MutableBorrowStatement*>(s)) {
        bind(mbs->target, lookup(mbs->source, mbs->line));
        return;
    }

    // define name(params): body
    if (auto* fn = dynamic_cast<const FunctionStatement*>(s)) {
        // Retrieve the signature registered in the first pass
        TypePtr fn_type = resolve(lookup(fn->name, 0));
        // Params go in a fresh scope that wraps the body scope
        push_scope();
        if (fn_type->kind == Type::Kind::Function) {
            for (size_t i = 0; i < fn->params.size(); i++)
                bind(fn->params[i], fn_type->param_types[i]);
            check_block(fn->body, fn_type->return_type);
        }
        pop_scope();
        return;
    }

    // give back expr
    if (auto* rs = dynamic_cast<const ReturnStatement*>(s)) {
        unify(infer_expression(rs->value.get()), return_type, 0);
        return;
    }

    // when cond: ...
    if (auto* is = dynamic_cast<const IfStatement*>(s)) {
        unify(infer_expression(is->condition.get()), Type::make_flag(), 0);
        check_block(is->then_block, return_type);
        if (is->else_block) check_block(*is->else_block, return_type);
        return;
    }

    // as long as cond: ...
    if (auto* ws = dynamic_cast<const WhileStatement*>(s)) {
        unify(infer_expression(ws->condition.get()), Type::make_flag(), 0);
        check_block(ws->body, return_type);
        return;
    }

    // for each x in list: ...
    if (auto* fe = dynamic_cast<const ForEachStatement*>(s)) {
        TypePtr elem = fresh();
        unify(infer_expression(fe->iterable.get()), Type::make_list(elem), 0);
        // Loop variable scope wraps the body scope
        push_scope();
        bind(fe->variable, resolve(elem));
        check_block(fe->body, return_type);
        pop_scope();
        return;
    }

    // sam.age = 21
    if (auto* fa = dynamic_cast<const FieldAssignStatement*>(s)) {
        TypePtr obj = resolve(lookup(fa->variable, fa->line));
        TypePtr val = infer_expression(fa->value.get());
        if (obj->kind == Type::Kind::Shape) {
            auto it = shape_types.find(obj->shape_name);
            if (it != shape_types.end()) {
                for (auto& [fname, ftype] : it->second) {
                    if (fname == fa->field) { unify(val, ftype, fa->line); return; }
                }
                error("Shape '" + obj->shape_name +
                      "' has no field '" + fa->field + "'", fa->line);
            }
        }
        return;
    }

    // Bare expression statement (e.g. a print() call)
    if (auto* es = dynamic_cast<const ExpressionStatement*>(s)) {
        infer_expression(es->expr.get());
        return;
    }

    // stop, skip — no type information to check
}

// ── Error helpers ─────────────────────────────────────────────────────────────

void TypeChecker::error(const std::string& msg, int line) {
    errors.push_back({msg, line});
}

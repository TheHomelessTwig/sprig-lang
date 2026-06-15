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
    std::istringstream source_stream(source);
    std::string line_text;
    while (std::getline(source_stream, line_text))
        source_lines.push_back(line_text);

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
    bind("pop",       Type::make_function({Type::make_list(fresh())}, fresh()));
    bind("to_number", Type::make_function({Type::make_text()},  Type::make_number()));
    bind("to_text",   Type::make_function({fresh()},            Type::make_text()));
    bind("char_code",      Type::make_function({Type::make_text()},                              Type::make_number()));
    bind("char_from_code", Type::make_function({Type::make_number()},                            Type::make_text()));
    bind("substring",      Type::make_function({Type::make_text(), Type::make_number(), Type::make_number()}, Type::make_text()));
    bind("string_contains",Type::make_function({Type::make_text(), Type::make_text()},           Type::make_flag()));
    bind("read_file",      Type::make_function({Type::make_text()},                              Type::make_text()));
    bind("write_file",     Type::make_function({Type::make_text(), Type::make_text()},           Type::make_nothing()));
    bind("args_count",     Type::make_function({},                                               Type::make_number()));
    bind("args_get",       Type::make_function({Type::make_number()},                            Type::make_text()));
    bind("exit",           Type::make_function({Type::make_number()},                            Type::make_nothing()));

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
        if (auto* include_stmt = dynamic_cast<const IncludeStatement*>(stmt.get())) {
            // Process include immediately so its exports are visible to the rest
            // of this file's first pass
            process_include(include_stmt);
            continue;
        }
        if (auto* shape_definition = dynamic_cast<const ShapeDefinitionStatement*>(stmt.get())) {
            std::vector<std::pair<std::string, TypePtr>> fields;
            for (auto& field : shape_definition->fields) {
                TypePtr field_type;
                if      (field.type == "text")    field_type = Type::make_text();
                else if (field.type == "number")  field_type = Type::make_number();
                else if (field.type == "decimal") field_type = Type::make_number();
                else if (field.type == "flag")    field_type = Type::make_flag();
                else if (field.type.substr(0, 4) == "own ") {
                    std::string inner_name = field.type.substr(4);
                    field_type = Type::make_own(Type::make_shape(inner_name));
                } else                            field_type = fresh();
                fields.push_back({field.name, field_type});
            }
            shape_types[shape_definition->name] = std::move(fields);
            bind(shape_definition->name, Type::make_shape(shape_definition->name));
        }
        if (auto* function_stmt = dynamic_cast<const FunctionStatement*>(stmt.get())) {
            std::vector<TypePtr> param_types;
            for (size_t i = 0; i < function_stmt->params.size(); i++)
                param_types.push_back(fresh());
            bind(function_stmt->name, Type::make_function(std::move(param_types), fresh()));
        }
    }

    // Second pass — full type checking
    TypePtr no_return = Type::make_nothing();
    for (auto& stmt : program.stmts)
        check_statement(stmt.get(), no_return);
}

// Load, parse, and process an included file in the current scope.
void TypeChecker::process_include(const IncludeStatement* include_stmt) {
    std::string path = include_stmt->path;
    if (path[0] != '/') {
        std::string cwd_path = std::filesystem::current_path().string() + "/" + path;
        if (!std::filesystem::exists(cwd_path) && !base_path.empty())
            path = base_path + "/" + path;
        else
            path = cwd_path;
    }

    std::string canonical;
    try {
        canonical = std::filesystem::canonical(path).string();
    } catch (...) {
        canonical = path;
    }
    if (included_files.count(canonical)) return;
    included_files.insert(canonical);

    std::ifstream file(path);
    if (!file) { error("Cannot open include file '" + path + "'", include_stmt->line); return; }

    std::stringstream file_content;
    file_content << file.rdbuf();

    Lexer  file_lexer(file_content.str());
    Parser file_parser(file_lexer.tokenize());
    process_program(file_parser.parse());
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
        auto scope_entry = scopes[i].find(name);
        if (scope_entry != scopes[i].end()) return resolve(scope_entry->second);
    }
    error("Undefined variable '" + name + "'", line);
    return fresh(); // placeholder so checking can continue after this error
}

// ── Unification ───────────────────────────────────────────────────────────────

// Follow the substitution chain to the concrete type (or an unresolved Var).
TypePtr TypeChecker::resolve(TypePtr type) {
    while (type->kind == Type::Kind::Var) {
        auto sub_entry = substitutions.find(type->var_id);
        if (sub_entry == substitutions.end()) break;
        type = sub_entry->second;
    }
    return type;
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

    // nothing is a valid null for own<T> and raw_ptr
    if (a->kind == Type::Kind::Nothing &&
        (b->kind == Type::Kind::Own || b->kind == Type::Kind::RawPtr)) return;
    if (b->kind == Type::Kind::Nothing &&
        (a->kind == Type::Kind::Own || a->kind == Type::Kind::RawPtr)) return;

    error("Type mismatch: expected " + a->to_string() +
          " but got " + b->to_string(), line);
}

// ── Instantiation ─────────────────────────────────────────────────────────────

// Replace all unbound type variables in t with fresh ones, using a mapping so
// the same var ID always maps to the same fresh var within one instantiation.
// This gives each call site its own copy of a polymorphic function's type.
TypePtr TypeChecker::instantiate(TypePtr type) {
    std::unordered_map<int, TypePtr> mapping;
    std::function<TypePtr(TypePtr)> go = [&](TypePtr t) -> TypePtr {
        t = resolve(t);
        switch (t->kind) {
            case Type::Kind::Var: {
                auto var_entry = mapping.find(t->var_id);
                if (var_entry != mapping.end()) return var_entry->second;
                TypePtr new_var = fresh();
                mapping[t->var_id] = new_var;
                return new_var;
            }
            case Type::Kind::List:
                return Type::make_list(go(t->element_type));
            case Type::Kind::Function: {
                std::vector<TypePtr> params;
                for (auto& param : t->param_types)
                    params.push_back(go(param));
                return Type::make_function(std::move(params), go(t->return_type));
            }
            default:
                return t;  // concrete types are returned as-is
        }
    };
    return go(type);
}

// ── Expression inference ──────────────────────────────────────────────────────

TypePtr TypeChecker::infer_expression(const Expression* e) {
    auto record = [&](TypePtr type) -> TypePtr {
        expr_types[e] = resolve(type);
        return type;
    };

    // ── Literals ──────────────────────────────────────────────────────────────

    if (dynamic_cast<const NumberExpression*>(e))  return record(Type::make_number());
    if (dynamic_cast<const StringExpression*>(e))  return record(Type::make_text());
    if (dynamic_cast<const BoolExpression*>(e))    return record(Type::make_flag());
    if (dynamic_cast<const NothingExpression*>(e)) return record(Type::make_nothing());

    // ── Variable lookup ───────────────────────────────────────────────────────

    if (auto* ident_expr = dynamic_cast<const IdentExpression*>(e))
        return record(lookup(ident_expr->name, ident_expr->line));

    // ── Borrow expressions ────────────────────────────────────────────────────

    if (auto* borrow_expr = dynamic_cast<const BorrowExpression*>(e))
        return record(lookup(borrow_expr->source, borrow_expr->line));
    if (auto* mutable_borrow_expr = dynamic_cast<const MutableBorrowExpression*>(e))
        return record(lookup(mutable_borrow_expr->source, mutable_borrow_expr->line));

    // ── Unary ─────────────────────────────────────────────────────────────────

    if (auto* unary_expr = dynamic_cast<const UnaryExpression*>(e)) {
        TypePtr operand = infer_expression(unary_expr->operand.get());
        if (unary_expr->op == "not") {
            unify(operand, Type::make_flag(), 0);
            return record(Type::make_flag());
        }
        if (unary_expr->op == "-") {
            unify(operand, Type::make_number(), 0);
            return record(Type::make_number());
        }
        return record(fresh());
    }

    // ── Binary ────────────────────────────────────────────────────────────────

    if (auto* binary_expr = dynamic_cast<const BinaryExpression*>(e)) {
        TypePtr left_type  = infer_expression(binary_expr->left.get());
        TypePtr right_type = infer_expression(binary_expr->right.get());

        // '+' is overloaded: number + number → number, or text + any → text.
        // Check resolved types before committing to a number constraint.
        if (binary_expr->op == "+") {
            TypePtr resolved_left  = resolve(left_type);
            TypePtr resolved_right = resolve(right_type);
            if (resolved_left->kind == Type::Kind::Text || resolved_right->kind == Type::Kind::Text)
                return record(Type::make_text());
            unify(left_type,  Type::make_number(), binary_expr->line);
            unify(right_type, Type::make_number(), binary_expr->line);
            return record(Type::make_number());
        }
        if (binary_expr->op == "-" || binary_expr->op == "*" || binary_expr->op == "/") {
            unify(left_type,  Type::make_number(), binary_expr->line);
            unify(right_type, Type::make_number(), binary_expr->line);
            return record(Type::make_number());
        }
        if (binary_expr->op == ">" || binary_expr->op == "<") {
            unify(left_type,  Type::make_number(), binary_expr->line);
            unify(right_type, Type::make_number(), binary_expr->line);
            return record(Type::make_flag());
        }
        if (binary_expr->op == "==" || binary_expr->op == "!=") {
            unify(left_type, right_type, binary_expr->line);
            return record(Type::make_flag());
        }
        if (binary_expr->op == "and" || binary_expr->op == "or") {
            unify(left_type,  Type::make_flag(), binary_expr->line);
            unify(right_type, Type::make_flag(), binary_expr->line);
            return record(Type::make_flag());
        }
        return record(fresh());
    }

    // ── Function call ─────────────────────────────────────────────────────────

    if (auto* call_expr = dynamic_cast<const CallExpression*>(e)) {
        // ── Raw pointer built-ins ─────────────────────────────────────────────
        if (call_expr->callee == "allocate") {
            if (call_expr->args.size() != 1) error("allocate() takes 1 argument", call_expr->line);
            else infer_expression(call_expr->args[0].get());
            return record(Type::make_raw_ptr());
        }
        if (call_expr->callee == "read") {
            if (call_expr->args.size() != 1) error("read() takes 1 argument", call_expr->line);
            else infer_expression(call_expr->args[0].get());
            return record(Type::make_number());
        }
        if (call_expr->callee == "write") {
            if (call_expr->args.size() != 2) error("write() takes 2 arguments", call_expr->line);
            else { infer_expression(call_expr->args[0].get()); infer_expression(call_expr->args[1].get()); }
            return record(Type::make_nothing());
        }
        if (call_expr->callee == "ptr_add") {
            if (call_expr->args.size() != 2) error("ptr_add() takes 2 arguments", call_expr->line);
            else { infer_expression(call_expr->args[0].get()); infer_expression(call_expr->args[1].get()); }
            return record(Type::make_raw_ptr());
        }
        if (call_expr->callee == "ptr_to_number") {
            if (call_expr->args.size() != 1) error("ptr_to_number() takes 1 argument", call_expr->line);
            else infer_expression(call_expr->args[0].get());
            return record(Type::make_number());
        }
        if (call_expr->callee == "number_to_ptr") {
            if (call_expr->args.size() != 1) error("number_to_ptr() takes 1 argument", call_expr->line);
            else infer_expression(call_expr->args[0].get());
            return record(Type::make_raw_ptr());
        }
        if (call_expr->callee == "free") {
            if (call_expr->args.size() != 1) error("free() takes 1 argument", call_expr->line);
            else infer_expression(call_expr->args[0].get());
            return record(Type::make_nothing());
        }

        TypePtr function_type = instantiate(resolve(lookup(call_expr->callee, call_expr->line)));

        std::vector<TypePtr> arg_types;
        for (auto& arg : call_expr->args)
            arg_types.push_back(infer_expression(arg.get()));

        TypePtr return_type = fresh();
        unify(function_type, Type::make_function(std::move(arg_types), return_type), call_expr->line);
        return record(resolve(return_type));
    }

    // ── List ──────────────────────────────────────────────────────────────────

    if (auto* list_expr = dynamic_cast<const ListExpression*>(e)) {
        TypePtr element_type = fresh();
        for (auto& element : list_expr->elements)
            unify(element_type, infer_expression(element.get()), 0);
        return record(Type::make_list(resolve(element_type)));
    }

    // collection[i]
    if (auto* index_expr = dynamic_cast<const IndexExpression*>(e)) {
        TypePtr object_type  = infer_expression(index_expr->object.get());
        TypePtr index_type   = infer_expression(index_expr->index.get());
        TypePtr element_type = fresh();
        unify(index_type,  Type::make_number(), 0);
        unify(object_type, Type::make_list(element_type), 0);
        return record(resolve(element_type));
    }

    // ── Shape ─────────────────────────────────────────────────────────────────

    // own expr — wraps inner type in Own<T>
    if (auto* own_expr = dynamic_cast<const OwnExpression*>(e)) {
        TypePtr inner_type = infer_expression(own_expr->inner.get());
        return record(Type::make_own(resolve(inner_type)));
    }

    // sam.name
    if (auto* field_access = dynamic_cast<const FieldAccessExpression*>(e)) {
        TypePtr object_type = resolve(infer_expression(field_access->object.get()));
        // Auto-deref own<T> for field access
        if (object_type->kind == Type::Kind::Own)
            object_type = resolve(object_type->element_type);
        if (object_type->kind == Type::Kind::Shape) {
            auto shape_info = shape_types.find(object_type->shape_name);
            if (shape_info != shape_types.end()) {
                for (auto& [field_name, field_type] : shape_info->second)
                    if (field_name == field_access->field) return record(field_type);
                error("Shape '" + object_type->shape_name +
                      "' has no field '" + field_access->field + "'", field_access->line);
            }
        } else if (object_type->kind != Type::Kind::Var) {
            error("Cannot access field on non-shape type '" +
                  object_type->to_string() + "'", field_access->line);
        }
        return record(fresh());
    }

    // Person { name: "sam", age: 20 }
    if (auto* shape_instance = dynamic_cast<const ShapeInstanceExpression*>(e)) {
        auto shape_info = shape_types.find(shape_instance->shape_name);
        if (shape_info == shape_types.end()) {
            error("Unknown shape '" + shape_instance->shape_name + "'", 0);
            return record(fresh());
        }
        for (auto& [field_name, field_expr] : shape_instance->fields) {
            TypePtr actual = infer_expression(field_expr.get());
            for (auto& [def_name, def_type] : shape_info->second)
                if (def_name == field_name) { unify(actual, def_type, 0); break; }
        }
        return record(Type::make_shape(shape_instance->shape_name));
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
    if (auto* variable_stmt = dynamic_cast<const VariableStatement*>(s)) {
        bind(variable_stmt->name, infer_expression(variable_stmt->value.get()));
        return;
    }

    // let x borrow [mutable] y — target gets same type as source; deref own<T>
    if (auto* borrow_stmt = dynamic_cast<const BorrowStatement*>(s)) {
        TypePtr source_type = lookup(borrow_stmt->source, borrow_stmt->line);
        if (source_type && source_type->kind == Type::Kind::Own)
            bind(borrow_stmt->target, source_type->element_type);
        else
            bind(borrow_stmt->target, source_type);
        return;
    }
    if (auto* mutable_borrow_stmt = dynamic_cast<const MutableBorrowStatement*>(s)) {
        TypePtr source_type = lookup(mutable_borrow_stmt->source, mutable_borrow_stmt->line);
        if (source_type && source_type->kind == Type::Kind::Own)
            bind(mutable_borrow_stmt->target, source_type->element_type);
        else
            bind(mutable_borrow_stmt->target, source_type);
        return;
    }

    // unsafe: block — type-check contents normally
    if (auto* unsafe_stmt = dynamic_cast<const UnsafeStatement*>(s)) {
        check_block(unsafe_stmt->body, return_type);
        return;
    }

    // define name(params): body
    if (auto* function_stmt = dynamic_cast<const FunctionStatement*>(s)) {
        // Retrieve the signature registered in the first pass
        TypePtr function_type = resolve(lookup(function_stmt->name, 0));
        // Params go in a fresh scope that wraps the body scope
        push_scope();
        if (function_type->kind == Type::Kind::Function) {
            for (size_t i = 0; i < function_stmt->params.size(); i++)
                bind(function_stmt->params[i], function_type->param_types[i]);
            check_block(function_stmt->body, function_type->return_type);
        }
        pop_scope();
        return;
    }

    // give back expr
    if (auto* return_stmt = dynamic_cast<const ReturnStatement*>(s)) {
        unify(infer_expression(return_stmt->value.get()), return_type, 0);
        return;
    }

    // when cond: ...
    if (auto* if_stmt = dynamic_cast<const IfStatement*>(s)) {
        unify(infer_expression(if_stmt->condition.get()), Type::make_flag(), 0);
        check_block(if_stmt->then_block, return_type);
        if (if_stmt->else_block) check_block(*if_stmt->else_block, return_type);
        return;
    }

    // as long as cond: ...
    if (auto* while_stmt = dynamic_cast<const WhileStatement*>(s)) {
        unify(infer_expression(while_stmt->condition.get()), Type::make_flag(), 0);
        check_block(while_stmt->body, return_type);
        return;
    }

    // for each x in list: ...
    if (auto* for_each_stmt = dynamic_cast<const ForEachStatement*>(s)) {
        TypePtr element_type = fresh();
        unify(infer_expression(for_each_stmt->iterable.get()), Type::make_list(element_type), 0);
        // Loop variable scope wraps the body scope
        push_scope();
        bind(for_each_stmt->variable, resolve(element_type));
        check_block(for_each_stmt->body, return_type);
        pop_scope();
        return;
    }

    // sam.age = 21
    if (auto* field_assign_stmt = dynamic_cast<const FieldAssignStatement*>(s)) {
        TypePtr object_type      = resolve(lookup(field_assign_stmt->variable, field_assign_stmt->line));
        TypePtr field_value_type = infer_expression(field_assign_stmt->value.get());
        if (object_type->kind == Type::Kind::Shape) {
            auto shape_info = shape_types.find(object_type->shape_name);
            if (shape_info != shape_types.end()) {
                for (auto& [field_name, field_type] : shape_info->second) {
                    if (field_name == field_assign_stmt->field) {
                        unify(field_value_type, field_type, field_assign_stmt->line);
                        return;
                    }
                }
                error("Shape '" + object_type->shape_name +
                      "' has no field '" + field_assign_stmt->field + "'", field_assign_stmt->line);
            }
        }
        return;
    }

    // Bare expression statement (e.g. a print() call)
    if (auto* expr_stmt = dynamic_cast<const ExpressionStatement*>(s)) {
        infer_expression(expr_stmt->expr.get());
        return;
    }

    // stop, skip — no type information to check
}

// ── Error helpers ─────────────────────────────────────────────────────────────

void TypeChecker::error(const std::string& msg, int line) {
    errors.push_back({msg, line});
}

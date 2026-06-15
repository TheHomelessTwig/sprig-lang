#include "interpreter.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
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
    if (type.substr(0, 4) == "own ") return v.kind == Value::Kind::Shape ||
                                             v.kind == Value::Kind::Nil;
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
                      const std::string& file_path,
                      std::vector<std::string> args) {
    program_args = std::move(args);
    // Split source into lines so make_error() can show the offending line.
    std::istringstream stream(source);
    std::string        line_text;
    while (std::getline(stream, line_text))
        source_lines.push_back(line_text);

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

void Interpreter::eval_block(const Block& block, Environment& env) {
    for (auto& stmt : block.stmts)
        eval_statement(stmt.get(), env);
}

// ── Statement evaluation ──────────────────────────────────────────────────────

void Interpreter::eval_statement(const Statement* stmt, Environment& env) {

    // let [mutable] x = expr
    if (auto* variable_stmt = dynamic_cast<const VariableStatement*>(stmt)) {
        Value new_value = eval_expression(variable_stmt->value.get(), env);
        try {
            env.declare(variable_stmt->name, std::move(new_value), variable_stmt->is_mutable);
        } catch (std::runtime_error& err) {
            throw std::runtime_error(make_error(err.what(), variable_stmt->line));
        }
        return;
    }

    // let x borrow [mutable] y — alias: binds target to source's current value
    // Lists and shapes use shared_ptr so mutations are visible through the alias.
    if (auto* borrow_stmt = dynamic_cast<const BorrowStatement*>(stmt)) {
        env.declare(borrow_stmt->target, env.get(borrow_stmt->source), false);
        return;
    }
    if (auto* mutable_borrow_stmt = dynamic_cast<const MutableBorrowStatement*>(stmt)) {
        env.declare(mutable_borrow_stmt->target, env.get(mutable_borrow_stmt->source), false);
        return;
    }

    // define name(params): body  — register without executing
    if (auto* function_stmt = dynamic_cast<const FunctionStatement*>(stmt)) {
        functions[function_stmt->name] = SprigFunction{function_stmt->params, &function_stmt->body};
        return;
    }

    // shape Person:  — register field schema
    if (auto* shape_definition = dynamic_cast<const ShapeDefinitionStatement*>(stmt)) {
        shapes[shape_definition->name] = SprigShapeDefinition{shape_definition->fields};
        return;
    }

    // sam.age = 21  — mutate through shared_ptr + enforce declared type
    if (auto* field_assign = dynamic_cast<const FieldAssignStatement*>(stmt)) {
        Value obj = env.get(field_assign->variable);
        if (obj.kind != Value::Kind::Shape)
            throw std::runtime_error(make_error(
                "'" + field_assign->variable + "' is not a shape", field_assign->line));

        auto shape_it = shapes.find(obj.shape_type);
        if (shape_it == shapes.end())
            throw std::runtime_error(make_error(
                "Unknown shape type '" + obj.shape_type + "'", field_assign->line));

        // Find declared type for this field
        const ShapeField* field_def = nullptr;
        for (auto& f : shape_it->second.fields)
            if (f.name == field_assign->field) { field_def = &f; break; }
        if (!field_def)
            throw std::runtime_error(make_error(
                "Shape '" + obj.shape_type +
                "' has no field '" + field_assign->field + "'", field_assign->line));

        Value new_val = eval_expression(field_assign->value.get(), env);
        if (!value_matches_type(new_val, field_def->type))
            throw std::runtime_error(make_error(
                "Field '" + field_assign->field + "' expects " + field_def->type +
                " but got " + type_name_of(new_val), field_assign->line));

        (*obj.shape)[field_assign->field] = std::move(new_val);
        return;
    }

    // include "path/to/file.sprig"  — lex, parse, run in current context
    if (auto* include_stmt = dynamic_cast<const IncludeStatement*>(stmt)) {
        std::string path = include_stmt->path;

        // Resolve relative to the including file's directory
        if (path[0] != '/') {
            std::string cwd_path = std::filesystem::current_path().string() + "/" + path;
            if (!std::filesystem::exists(cwd_path) && !base_path.empty())
                path = base_path + "/" + path;
            else
                path = cwd_path;
        }

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
                "Cannot open include file '" + path + "'", include_stmt->line));

        std::stringstream file_content;
        file_content << file.rdbuf();
        std::string file_source = file_content.str();

        Lexer  file_lexer(file_source);
        Parser file_parser(file_lexer.tokenize());
        // Store the program in owned_programs before running — SprigFunction holds raw
        // pointers into the AST (&function_stmt->body), so the Program must outlive the interpreter.
        owned_programs.push_back(file_parser.parse());
        Program& prog = owned_programs.back();
        for (auto& stmt : prog.stmts)
            eval_statement(stmt.get(), global);
        return;
    }

    // when cond: ...
    if (auto* if_stmt = dynamic_cast<const IfStatement*>(stmt)) {
        Value condition = eval_expression(if_stmt->condition.get(), env);
        if (condition.is_truthy()) {
            Environment inner(&env);
            eval_block(if_stmt->then_block, inner);
        } else if (if_stmt->else_block) {
            Environment inner(&env);
            eval_block(*if_stmt->else_block, inner);
        }
        return;
    }

    // as long as cond: ...
    if (auto* while_stmt = dynamic_cast<const WhileStatement*>(stmt)) {
        while (eval_expression(while_stmt->condition.get(), env).is_truthy()) {
            Environment inner(&env);
            try {
                eval_block(while_stmt->body, inner);
            } catch (StopSignal&) {
                break;
            } catch (SkipSignal&) {
                continue;
            }
        }
        return;
    }

    // for each x in list: ...
    if (auto* for_each_stmt = dynamic_cast<const ForEachStatement*>(stmt)) {
        Value iterable = eval_expression(for_each_stmt->iterable.get(), env);
        if (iterable.kind != Value::Kind::List)
            throw std::runtime_error("'for each' requires a list");
        for (auto& item : *iterable.list) {
            Environment inner(&env);
            // bind() ensures the loop variable is always local to this iteration
            inner.bind(for_each_stmt->variable, item, true);
            try {
                eval_block(for_each_stmt->body, inner);
            } catch (StopSignal&) {
                break;
            } catch (SkipSignal&) {
                continue;
            }
        }
        return;
    }

    // give back expr — unwind via signal to call_function()
    if (auto* return_stmt = dynamic_cast<const ReturnStatement*>(stmt)) {
        throw ReturnSignal{eval_expression(return_stmt->value.get(), env)};
    }

    // stop / skip — unwind to the nearest enclosing loop
    if (dynamic_cast<const StopStatement*>(stmt)) throw StopSignal{};
    if (dynamic_cast<const SkipStatement*>(stmt)) throw SkipSignal{};

    // unsafe: block — borrow checker already validated; run normally
    if (auto* unsafe_stmt = dynamic_cast<const UnsafeStatement*>(stmt)) {
        Environment inner(&env);
        eval_block(unsafe_stmt->body, inner);
        return;
    }

    // Bare expression statement (e.g. a print() call)
    if (auto* expr_stmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
        eval_expression(expr_stmt->expr.get(), env);
        return;
    }

    throw std::runtime_error("Unknown statement type");
}

// ── Expression evaluation ─────────────────────────────────────────────────────

Value Interpreter::eval_expression(const Expression* expr, Environment& env) {

    // ── Literals ──────────────────────────────────────────────────────────────

    if (auto* number_expr = dynamic_cast<const NumberExpression*>(expr))
        return Value::make_number(number_expr->value);

    if (auto* string_expr = dynamic_cast<const StringExpression*>(expr))
        return Value::make_string(string_expr->value);

    if (auto* bool_expr = dynamic_cast<const BoolExpression*>(expr))
        return Value::make_bool(bool_expr->value);

    if (dynamic_cast<const NothingExpression*>(expr))
        return Value::make_nil();

    // ── Variable lookup / borrow expressions ─────────────────────────────────

    // borrow [mutable] x — at runtime just produces the value of x
    if (auto* borrow_expr = dynamic_cast<const BorrowExpression*>(expr))
        return env.get(borrow_expr->source);
    if (auto* mutable_borrow_expr = dynamic_cast<const MutableBorrowExpression*>(expr))
        return env.get(mutable_borrow_expr->source);

    if (auto* ident_expr = dynamic_cast<const IdentExpression*>(expr)) {
        try {
            return env.get(ident_expr->name);
        } catch (std::runtime_error&) {
            throw std::runtime_error(make_error(
                "Undefined variable '" + ident_expr->name + "'", ident_expr->line));
        }
    }

    // ── Unary ─────────────────────────────────────────────────────────────────

    if (auto* unary_expr = dynamic_cast<const UnaryExpression*>(expr)) {
        Value operand = eval_expression(unary_expr->operand.get(), env);
        if (unary_expr->op == "not") return Value::make_bool(!operand.is_truthy());
        if (unary_expr->op == "-")   return Value::make_number(-operand.number);
        throw std::runtime_error("Unknown unary operator: " + unary_expr->op);
    }

    // ── Binary ────────────────────────────────────────────────────────────────

    if (auto* binary_expr = dynamic_cast<const BinaryExpression*>(expr)) {
        // Short-circuit logical ops must not force both sides
        if (binary_expr->op == "and") {
            if (!eval_expression(binary_expr->left.get(), env).is_truthy())
                return Value::make_bool(false);
            return Value::make_bool(
                eval_expression(binary_expr->right.get(), env).is_truthy());
        }
        if (binary_expr->op == "or") {
            if (eval_expression(binary_expr->left.get(), env).is_truthy())
                return Value::make_bool(true);
            return Value::make_bool(
                eval_expression(binary_expr->right.get(), env).is_truthy());
        }

        Value left  = eval_expression(binary_expr->left.get(),  env);
        Value right = eval_expression(binary_expr->right.get(), env);

        if (binary_expr->op == "+") {
            // String coercion: if either side is a string, concatenate
            if (left.kind  == Value::Kind::String ||
                right.kind == Value::Kind::String)
                return Value::make_string(left.to_string() + right.to_string());
            return Value::make_number(left.number + right.number);
        }
        if (binary_expr->op == "-") return Value::make_number(left.number - right.number);
        if (binary_expr->op == "*") return Value::make_number(left.number * right.number);
        if (binary_expr->op == "/") {
            if (right.number == 0)
                throw std::runtime_error(
                    make_error("Division by zero", binary_expr->line));
            return Value::make_number(left.number / right.number);
        }
        if (binary_expr->op == ">")  return Value::make_bool(left.number >  right.number);
        if (binary_expr->op == "<")  return Value::make_bool(left.number <  right.number);
        if (binary_expr->op == ">=") return Value::make_bool(left.number >= right.number);
        if (binary_expr->op == "<=") return Value::make_bool(left.number <= right.number);

        if (binary_expr->op == "==") {
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
        if (binary_expr->op == "!=") {
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

        throw std::runtime_error("Unknown operator: " + binary_expr->op);
    }

    // ── List ──────────────────────────────────────────────────────────────────

    if (auto* list_expr = dynamic_cast<const ListExpression*>(expr)) {
        std::vector<Value> items;
        for (auto& element : list_expr->elements)
            items.push_back(eval_expression(element.get(), env));
        return Value::make_list(std::move(items));
    }

    // collection[i] or string[i]
    if (auto* index_expr = dynamic_cast<const IndexExpression*>(expr)) {
        Value obj        = eval_expression(index_expr->object.get(), env);
        Value index_val  = eval_expression(index_expr->index.get(), env);
        if (index_val.kind != Value::Kind::Number)
            throw std::runtime_error("Index must be a number");
        int index = (int)index_val.number;
        if (obj.kind == Value::Kind::List) {
            if (index < 0 || index >= (int)obj.list->size())
                throw std::runtime_error("List index out of range");
            return (*obj.list)[index];
        }
        if (obj.kind == Value::Kind::String) {
            if (index < 0 || index >= (int)obj.str.size())
                throw std::runtime_error("String index out of range");
            return Value::make_string(std::string(1, obj.str[index]));
        }
        throw std::runtime_error("Cannot index into this type");
    }

    // ── Shape ─────────────────────────────────────────────────────────────────

    // Person { name: "sam", age: 20 }
    if (auto* shape_instance = dynamic_cast<const ShapeInstanceExpression*>(expr)) {
        auto shape_def = shapes.find(shape_instance->shape_name);
        if (shape_def == shapes.end())
            throw std::runtime_error(
                "Unknown shape '" + shape_instance->shape_name + "'");

        std::unordered_map<std::string, Value> fields;
        for (auto& [field_name, field_expr] : shape_instance->fields) {
            // Find declared type for this field
            const ShapeField* def = nullptr;
            for (auto& f : shape_def->second.fields)
                if (f.name == field_name) { def = &f; break; }
            if (!def)
                throw std::runtime_error(
                    "Shape '" + shape_instance->shape_name +
                    "' has no field '" + field_name + "'");

            Value field_value = eval_expression(field_expr.get(), env);
            if (!value_matches_type(field_value, def->type))
                throw std::runtime_error(
                    "Field '" + field_name + "' expects " + def->type +
                    " but got " + type_name_of(field_value));

            fields[field_name] = std::move(field_value);
        }

        // Ensure all declared fields are provided
        for (auto& f : shape_def->second.fields)
            if (fields.find(f.name) == fields.end())
                throw std::runtime_error(
                    "Missing field '" + f.name +
                    "' in " + shape_instance->shape_name + " instantiation");

        return Value::make_shape(shape_instance->shape_name, std::move(fields));
    }

    // sam.name
    if (auto* field_access = dynamic_cast<const FieldAccessExpression*>(expr)) {
        Value obj = eval_expression(field_access->object.get(), env);
        if (obj.kind != Value::Kind::Shape)
            throw std::runtime_error(make_error(
                "Cannot access field on non-shape value", field_access->line));
        auto field_entry = obj.shape->find(field_access->field);
        if (field_entry == obj.shape->end())
            throw std::runtime_error(make_error(
                "Shape '" + obj.shape_type +
                "' has no field '" + field_access->field + "'", field_access->line));
        return field_entry->second;
    }

    // own expr — interpreter: heap allocation handled by shared_ptr; just eval inner
    if (auto* own_expr = dynamic_cast<const OwnExpression*>(expr))
        return eval_expression(own_expr->inner.get(), env);

    // ── Function calls ────────────────────────────────────────────────────────

    if (auto* call_expr = dynamic_cast<const CallExpression*>(expr)) {
        // Built-ins checked before user-defined functions

        if (call_expr->callee == "print") {
            for (auto& arg : call_expr->args)
                std::cout << eval_expression(arg.get(), env).to_string();
            std::cout << "\n";
            return Value::make_nil();
        }

        if (call_expr->callee == "length") {
            if (call_expr->args.size() != 1)
                throw std::runtime_error("length() takes 1 argument");
            Value val = eval_expression(call_expr->args[0].get(), env);
            if (val.kind == Value::Kind::List)
                return Value::make_number((double)val.list->size());
            if (val.kind == Value::Kind::String)
                return Value::make_number((double)val.str.size());
            throw std::runtime_error("length() requires a list or string");
        }

        if (call_expr->callee == "append") {
            if (call_expr->args.size() != 2)
                throw std::runtime_error("append() takes 2 arguments");
            Value list  = eval_expression(call_expr->args[0].get(), env);
            Value item  = eval_expression(call_expr->args[1].get(), env);
            if (list.kind != Value::Kind::List)
                throw std::runtime_error("append() requires a list");
            list.list->push_back(std::move(item));
            return Value::make_nil();
        }

        if (call_expr->callee == "first") {
            if (call_expr->args.size() != 1)
                throw std::runtime_error("first() takes 1 argument");
            Value val = eval_expression(call_expr->args[0].get(), env);
            if (val.kind != Value::Kind::List || val.list->empty())
                throw std::runtime_error("first() requires a non-empty list");
            return val.list->front();
        }

        if (call_expr->callee == "last") {
            if (call_expr->args.size() != 1)
                throw std::runtime_error("last() takes 1 argument");
            Value val = eval_expression(call_expr->args[0].get(), env);
            if (val.kind != Value::Kind::List || val.list->empty())
                throw std::runtime_error("last() requires a non-empty list");
            return val.list->back();
        }

        if (call_expr->callee == "pop") {
            if (call_expr->args.size() != 1)
                throw std::runtime_error("pop() takes 1 argument");
            Value list_val = eval_expression(call_expr->args[0].get(), env);
            if (list_val.kind != Value::Kind::List || list_val.list->empty())
                throw std::runtime_error("pop() requires a non-empty list");
            Value last_val = list_val.list->back();
            list_val.list->pop_back();
            return last_val;
        }

        if (call_expr->callee == "input") {
            if (call_expr->args.size() > 1)
                throw std::runtime_error("input() takes 0 or 1 arguments");
            if (call_expr->args.size() == 1)
                std::cout << eval_expression(call_expr->args[0].get(), env).to_string();
            std::string line;
            std::getline(std::cin, line);
            return Value::make_string(line);
        }

        if (call_expr->callee == "to_number") {
            if (call_expr->args.size() != 1)
                throw std::runtime_error("to_number() takes 1 argument");
            Value val = eval_expression(call_expr->args[0].get(), env);
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

        if (call_expr->callee == "to_text") {
            if (call_expr->args.size() != 1)
                throw std::runtime_error("to_text() takes 1 argument");
            return Value::make_string(
                eval_expression(call_expr->args[0].get(), env).to_string());
        }

        // ── Raw pointer built-ins ─────────────────────────────────────────────

        if (call_expr->callee == "allocate") {
            Value size_val = eval_expression(call_expr->args[0].get(), env);
            void* raw_ptr  = std::malloc((size_t)size_val.number);
            return Value::make_number((double)(uintptr_t)raw_ptr);
        }
        if (call_expr->callee == "read") {
            Value addr_val = eval_expression(call_expr->args[0].get(), env);
            void* raw_ptr  = (void*)(uintptr_t)(uint64_t)addr_val.number;
            double read_val = 0;
            std::memcpy(&read_val, raw_ptr, sizeof(double));
            return Value::make_number(read_val);
        }
        if (call_expr->callee == "write") {
            Value addr_val = eval_expression(call_expr->args[0].get(), env);
            Value data_val = eval_expression(call_expr->args[1].get(), env);
            void*  raw_ptr   = (void*)(uintptr_t)(uint64_t)addr_val.number;
            double write_val = data_val.number;
            std::memcpy(raw_ptr, &write_val, sizeof(double));
            return Value::make_nil();
        }
        if (call_expr->callee == "free") {
            Value addr_val = eval_expression(call_expr->args[0].get(), env);
            std::free((void*)(uintptr_t)(uint64_t)addr_val.number);
            return Value::make_nil();
        }
        if (call_expr->callee == "ptr_add") {
            Value addr_val   = eval_expression(call_expr->args[0].get(), env);
            Value offset_val = eval_expression(call_expr->args[1].get(), env);
            uintptr_t addr   = (uintptr_t)(uint64_t)addr_val.number;
            addr            += (uintptr_t)(size_t)offset_val.number;
            return Value::make_number((double)addr);
        }
        if (call_expr->callee == "ptr_to_number" || call_expr->callee == "number_to_ptr") {
            return eval_expression(call_expr->args[0].get(), env);
        }

        // ── String built-ins ─────────────────────────────────────────────────

        if (call_expr->callee == "char_code") {
            Value val = eval_expression(call_expr->args[0].get(), env);
            if (val.kind != Value::Kind::String || val.str.empty())
                throw std::runtime_error("char_code() requires a non-empty string");
            return Value::make_number((double)(unsigned char)val.str[0]);
        }

        if (call_expr->callee == "char_from_code") {
            Value val      = eval_expression(call_expr->args[0].get(), env);
            char char_byte = (char)(int)val.number;
            return Value::make_string(std::string(1, char_byte));
        }

        if (call_expr->callee == "substring") {
            if (call_expr->args.size() != 3)
                throw std::runtime_error("substring() takes 3 arguments");
            Value str_val   = eval_expression(call_expr->args[0].get(), env);
            Value start_val = eval_expression(call_expr->args[1].get(), env);
            Value len_val   = eval_expression(call_expr->args[2].get(), env);
            if (str_val.kind != Value::Kind::String)
                throw std::runtime_error("substring() requires a string");
            int start      = (int)start_val.number;
            int len        = (int)len_val.number;
            int source_len = (int)str_val.str.size();
            if (start < 0) start = 0;
            if (start >= source_len) return Value::make_string("");
            if (start + len > source_len) len = source_len - start;
            return Value::make_string(str_val.str.substr(start, len));
        }

        if (call_expr->callee == "string_contains") {
            if (call_expr->args.size() != 2)
                throw std::runtime_error("string_contains() takes 2 arguments");
            Value haystack = eval_expression(call_expr->args[0].get(), env);
            Value needle   = eval_expression(call_expr->args[1].get(), env);
            return Value::make_bool(haystack.str.find(needle.str) != std::string::npos);
        }

        // ── File I/O ──────────────────────────────────────────────────────────

        if (call_expr->callee == "read_file") {
            if (call_expr->args.size() != 1)
                throw std::runtime_error("read_file() takes 1 argument");
            Value path_val = eval_expression(call_expr->args[0].get(), env);
            std::ifstream file(path_val.str);
            if (!file)
                throw std::runtime_error("read_file(): cannot open '" + path_val.str + "'");
            std::stringstream buffer;
            buffer << file.rdbuf();
            return Value::make_string(buffer.str());
        }

        if (call_expr->callee == "write_file") {
            if (call_expr->args.size() != 2)
                throw std::runtime_error("write_file() takes 2 arguments");
            Value path_val    = eval_expression(call_expr->args[0].get(), env);
            Value content_val = eval_expression(call_expr->args[1].get(), env);
            std::ofstream file(path_val.str);
            if (!file)
                throw std::runtime_error("write_file(): cannot open '" + path_val.str + "'");
            file << content_val.str;
            return Value::make_nil();
        }

        // ── Process arguments ─────────────────────────────────────────────────

        if (call_expr->callee == "args_count") {
            return Value::make_number((double)program_args.size());
        }

        if (call_expr->callee == "args_get") {
            Value index_val = eval_expression(call_expr->args[0].get(), env);
            int index = (int)index_val.number;
            if (index < 0 || index >= (int)program_args.size())
                throw std::runtime_error("args_get(): index out of range");
            return Value::make_string(program_args[index]);
        }

        if (call_expr->callee == "exit") {
            Value code_val = eval_expression(call_expr->args[0].get(), env);
            std::exit((int)code_val.number);
        }

        // Fall through to user-defined function
        std::vector<Value> args;
        for (auto& arg : call_expr->args)
            args.push_back(eval_expression(arg.get(), env));
        return call_function(call_expr->callee, args, call_expr->line);
    }

    throw std::runtime_error("Unknown expression type");
}

// ── User-defined function calls ───────────────────────────────────────────────

Value Interpreter::call_function(const std::string& name,
                                 const std::vector<Value>& args, int line) {
    auto function_entry = functions.find(name);
    if (function_entry == functions.end())
        throw std::runtime_error(make_error(
            "Undefined function '" + name + "'", line));

    SprigFunction& function = function_entry->second;
    if (args.size() != function.params.size())
        throw std::runtime_error(make_error(
            "'" + name + "' expects " + std::to_string(function.params.size()) +
            " args, got " + std::to_string(args.size()), line));

    // Each call gets its own scope; global is the shared outer for all functions.
    // bind() ensures params never accidentally shadow or update outer variables.
    Environment function_env(&global);
    for (size_t i = 0; i < function.params.size(); i++)
        function_env.bind(function.params[i], args[i], true);

    try {
        eval_block(*function.body, function_env);
    } catch (ReturnSignal& ret) {
        return ret.value;
    }
    return Value::make_nil();
}

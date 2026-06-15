#include "borrowchecker.hpp"

#include <sstream>

// ── Entry point ───────────────────────────────────────────────────────────────

std::vector<BorrowError> BorrowChecker::check(const Program& program,
                                               const std::string& source) {
    std::istringstream source_stream(source);
    std::string line_text;
    while (std::getline(source_stream, line_text))
        source_lines.push_back(line_text);

    push_scope();
    for (auto& stmt : program.stmts)
        check_statement(stmt.get());
    pop_scope();

    return errors;
}

// ── Scope management ──────────────────────────────────────────────────────────

void BorrowChecker::push_scope() {
    scopes.push_back({});
}

void BorrowChecker::pop_scope() {
    // Release borrows held by variables leaving this scope
    for (auto& [name, state] : scopes.back()) {
        if (state == OwnershipState::Borrowed ||
            state == OwnershipState::MutBorrowed) {
            auto borrow_it = borrow_source.find(name);
            if (borrow_it != borrow_source.end()) {
                release_borrows(name);
                borrow_source.erase(borrow_it);
            }
        }
    }
    scopes.pop_back();
}

void BorrowChecker::declare(const std::string& name, OwnershipState state) {
    scopes.back()[name] = state;
}

OwnershipState* BorrowChecker::lookup(const std::string& name) {
    for (int i = (int)scopes.size() - 1; i >= 0; i--) {
        auto scope_entry = scopes[i].find(name);
        if (scope_entry != scopes[i].end()) return &scope_entry->second;
    }
    return nullptr;
}

void BorrowChecker::error(const std::string& msg, int line) {
    std::string out = "Borrow error at line " + std::to_string(line) + ":\n";
    if (line >= 1 && line <= (int)source_lines.size())
        out += "  " + source_lines[line - 1] + "\n";
    errors.push_back({out + msg, line});
}

// ── Ownership state checks ────────────────────────────────────────────────────

void BorrowChecker::check_readable(const std::string& name, int line) {
    auto* state = lookup(name);
    if (!state) return;
    if (*state == OwnershipState::Moved)
        error("Use of moved variable '" + name + "'", line);
    if (*state == OwnershipState::MutBorrowed)
        error("Cannot read '" + name + "' while it is mutably borrowed", line);
}

void BorrowChecker::check_movable(const std::string& name, int line) {
    auto* state = lookup(name);
    if (!state) return;
    if (*state == OwnershipState::Moved)
        error("Cannot move '" + name + "' — already moved", line);
    if (*state == OwnershipState::Borrowed)
        error("Cannot move '" + name + "' while it is borrowed", line);
    if (*state == OwnershipState::MutBorrowed)
        error("Cannot move '" + name + "' while it is mutably borrowed", line);
}

void BorrowChecker::check_borrowable(const std::string& name, int line) {
    auto* state = lookup(name);
    if (!state) return;
    if (*state == OwnershipState::Moved)
        error("Cannot borrow '" + name + "' — already moved", line);
    if (*state == OwnershipState::MutBorrowed)
        error("Cannot borrow '" + name + "' while it is mutably borrowed", line);
}

void BorrowChecker::check_mut_borrowable(const std::string& name, int line) {
    auto* state = lookup(name);
    if (!state) return;
    if (*state == OwnershipState::Moved)
        error("Cannot mutably borrow '" + name + "' — already moved", line);
    if (*state == OwnershipState::Borrowed)
        error("Cannot mutably borrow '" + name + "' while it is borrowed", line);
    if (*state == OwnershipState::MutBorrowed)
        error("Cannot mutably borrow '" + name + "' — already mutably borrowed", line);
}

void BorrowChecker::release_borrows(const std::string& borrow_name) {
    auto source_it = borrow_source.find(borrow_name);
    if (source_it == borrow_source.end()) return;

    const std::string& source_name = source_it->second;
    auto* state = lookup(source_name);
    if (!state) return;

    auto count_entry = borrow_count.find(source_name);
    if (count_entry != borrow_count.end()) {
        count_entry->second--;
        if (count_entry->second <= 0) {
            borrow_count.erase(count_entry);
            *state = OwnershipState::Owned;
        }
    } else {
        *state = OwnershipState::Owned;
    }
}

// ── Statement checking ────────────────────────────────────────────────────────

void BorrowChecker::check_statement(const Statement* stmt) {

    // let [mutable] x = expr
    if (auto* variable_stmt = dynamic_cast<const VariableStatement*>(stmt)) {
        check_expression(variable_stmt->value.get());
        declare(variable_stmt->name, OwnershipState::Owned);
        return;
    }

    // let x borrow y — x is the reference (Owned/usable), y is locked as Borrowed
    if (auto* borrow_stmt = dynamic_cast<const BorrowStatement*>(stmt)) {
        check_borrowable(borrow_stmt->source, borrow_stmt->line);
        auto* src_state = lookup(borrow_stmt->source);
        if (src_state) {
            borrow_count[borrow_stmt->source]++;
            *src_state = OwnershipState::Borrowed;
        }
        borrow_source[borrow_stmt->target] = borrow_stmt->source;
        declare(borrow_stmt->target, OwnershipState::Owned); // reference itself is freely usable
        return;
    }

    // let x borrow mutable y — x is usable, y is locked as MutBorrowed
    if (auto* mutable_borrow_stmt = dynamic_cast<const MutableBorrowStatement*>(stmt)) {
        check_mut_borrowable(mutable_borrow_stmt->source, mutable_borrow_stmt->line);
        auto* src_state = lookup(mutable_borrow_stmt->source);
        if (src_state) *src_state = OwnershipState::MutBorrowed;
        borrow_source[mutable_borrow_stmt->target] = mutable_borrow_stmt->source;
        declare(mutable_borrow_stmt->target, OwnershipState::Owned); // reference itself is freely usable
        return;
    }

    // give back expr
    if (auto* return_stmt = dynamic_cast<const ReturnStatement*>(stmt)) {
        check_expression(return_stmt->value.get());
        return;
    }

    // define f(params): body
    if (auto* function_stmt = dynamic_cast<const FunctionStatement*>(stmt)) {
        push_scope();
        for (auto& param : function_stmt->params)
            declare(param, OwnershipState::Owned);
        check_block(function_stmt->body);
        pop_scope();
        return;
    }

    // when cond: ...
    if (auto* if_stmt = dynamic_cast<const IfStatement*>(stmt)) {
        check_expression(if_stmt->condition.get());
        check_block(if_stmt->then_block);
        if (if_stmt->else_block) check_block(*if_stmt->else_block);
        return;
    }

    // as long as cond: ...
    if (auto* while_stmt = dynamic_cast<const WhileStatement*>(stmt)) {
        check_expression(while_stmt->condition.get());
        check_block(while_stmt->body);
        return;
    }

    // for each x in list: ...
    if (auto* for_each_stmt = dynamic_cast<const ForEachStatement*>(stmt)) {
        check_expression(for_each_stmt->iterable.get());
        push_scope();
        declare(for_each_stmt->variable, OwnershipState::Owned);
        check_block(for_each_stmt->body);
        pop_scope();
        return;
    }

    // sam.field = val
    if (auto* field_assign_stmt = dynamic_cast<const FieldAssignStatement*>(stmt)) {
        check_readable(field_assign_stmt->variable, field_assign_stmt->line);
        check_expression(field_assign_stmt->value.get());
        return;
    }

    // standalone expression
    if (auto* expr_stmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
        check_expression(expr_stmt->expr.get());
        return;
    }

    // unsafe: — borrow rules still apply inside
    if (auto* unsafe_stmt = dynamic_cast<const UnsafeStatement*>(stmt)) {
        check_block(unsafe_stmt->body);
        return;
    }

    // shape / include / stop / skip — no ownership concerns
}

void BorrowChecker::check_block(const Block& block) {
    push_scope();
    for (auto& stmt : block.stmts)
        check_statement(stmt.get());
    pop_scope();
}

// ── Expression checking ───────────────────────────────────────────────────────

void BorrowChecker::check_expression(const Expression* expr) {

    // Variable read — must not be moved or mut-borrowed
    if (auto* ident_expr = dynamic_cast<const IdentExpression*>(expr)) {
        check_readable(ident_expr->name, ident_expr->line);
        return;
    }

    // borrow x — transient immutable borrow (function argument); just check validity
    if (auto* borrow_expr = dynamic_cast<const BorrowExpression*>(expr)) {
        check_borrowable(borrow_expr->source, borrow_expr->line);
        return;
    }

    // borrow mutable x — transient mutable borrow; just check validity
    if (auto* mutable_borrow_expr = dynamic_cast<const MutableBorrowExpression*>(expr)) {
        check_mut_borrowable(mutable_borrow_expr->source, mutable_borrow_expr->line);
        return;
    }

    // Function call — borrow args are handled above; ident args are reads
    if (auto* call_expr = dynamic_cast<const CallExpression*>(expr)) {
        for (auto& arg : call_expr->args)
            check_expression(arg.get());
        return;
    }

    // Binary / unary — recurse
    if (auto* binary_expr = dynamic_cast<const BinaryExpression*>(expr)) {
        check_expression(binary_expr->left.get());
        check_expression(binary_expr->right.get());
        return;
    }
    if (auto* unary_expr = dynamic_cast<const UnaryExpression*>(expr)) {
        check_expression(unary_expr->operand.get());
        return;
    }

    // Index — check object and index
    if (auto* index_expr = dynamic_cast<const IndexExpression*>(expr)) {
        check_expression(index_expr->object.get());
        check_expression(index_expr->index.get());
        return;
    }

    // Field access
    if (auto* field_access_expr = dynamic_cast<const FieldAccessExpression*>(expr)) {
        check_expression(field_access_expr->object.get());
        return;
    }

    // List literal
    if (auto* list_expr = dynamic_cast<const ListExpression*>(expr)) {
        for (auto& element : list_expr->elements)
            check_expression(element.get());
        return;
    }

    // Shape instantiation
    if (auto* shape_instance_expr = dynamic_cast<const ShapeInstanceExpression*>(expr)) {
        for (auto& [field_name, field_expr] : shape_instance_expr->fields)
            check_expression(field_expr.get());
        return;
    }

    // own expr — inner value consumed into heap allocation
    if (auto* own_expr = dynamic_cast<const OwnExpression*>(expr)) {
        check_expression(own_expr->inner.get());
        return;
    }

    // Literals — always safe
}

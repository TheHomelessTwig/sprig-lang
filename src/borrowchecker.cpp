#include "borrowchecker.hpp"

#include <sstream>

// ── Entry point ───────────────────────────────────────────────────────────────

std::vector<BorrowError> BorrowChecker::check(const Program& program,
                                               const std::string& source) {
    std::istringstream ss(source);
    std::string ln;
    while (std::getline(ss, ln))
        source_lines.push_back(ln);

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
            auto it = borrow_source.find(name);
            if (it != borrow_source.end()) {
                release_borrows(name);
                borrow_source.erase(it);
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
        auto it = scopes[i].find(name);
        if (it != scopes[i].end()) return &it->second;
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
    auto src_it = borrow_source.find(borrow_name);
    if (src_it == borrow_source.end()) return;

    const std::string& source_name = src_it->second;
    auto* state = lookup(source_name);
    if (!state) return;

    auto cnt_it = borrow_count.find(source_name);
    if (cnt_it != borrow_count.end()) {
        cnt_it->second--;
        if (cnt_it->second <= 0) {
            borrow_count.erase(cnt_it);
            *state = OwnershipState::Owned;
        }
    } else {
        *state = OwnershipState::Owned;
    }
}

// ── Statement checking ────────────────────────────────────────────────────────

void BorrowChecker::check_statement(const Statement* s) {

    // let [mutable] x = expr
    if (auto* vs = dynamic_cast<const VariableStatement*>(s)) {
        check_expression(vs->value.get());
        declare(vs->name, OwnershipState::Owned);
        return;
    }

    // let x borrow y — x is the reference (Owned/usable), y is locked as Borrowed
    if (auto* bs = dynamic_cast<const BorrowStatement*>(s)) {
        check_borrowable(bs->source, bs->line);
        auto* src_state = lookup(bs->source);
        if (src_state) {
            borrow_count[bs->source]++;
            *src_state = OwnershipState::Borrowed;
        }
        borrow_source[bs->target] = bs->source;
        declare(bs->target, OwnershipState::Owned); // reference itself is freely usable
        return;
    }

    // let x borrow mutable y — x is usable, y is locked as MutBorrowed
    if (auto* mbs = dynamic_cast<const MutableBorrowStatement*>(s)) {
        check_mut_borrowable(mbs->source, mbs->line);
        auto* src_state = lookup(mbs->source);
        if (src_state) *src_state = OwnershipState::MutBorrowed;
        borrow_source[mbs->target] = mbs->source;
        declare(mbs->target, OwnershipState::Owned); // reference itself is freely usable
        return;
    }

    // give back expr
    if (auto* rs = dynamic_cast<const ReturnStatement*>(s)) {
        check_expression(rs->value.get());
        return;
    }

    // define f(params): body
    if (auto* fn = dynamic_cast<const FunctionStatement*>(s)) {
        push_scope();
        for (auto& p : fn->params)
            declare(p, OwnershipState::Owned);
        check_block(fn->body);
        pop_scope();
        return;
    }

    // when cond: ...
    if (auto* is = dynamic_cast<const IfStatement*>(s)) {
        check_expression(is->condition.get());
        check_block(is->then_block);
        if (is->else_block) check_block(*is->else_block);
        return;
    }

    // as long as cond: ...
    if (auto* ws = dynamic_cast<const WhileStatement*>(s)) {
        check_expression(ws->condition.get());
        check_block(ws->body);
        return;
    }

    // for each x in list: ...
    if (auto* fe = dynamic_cast<const ForEachStatement*>(s)) {
        check_expression(fe->iterable.get());
        push_scope();
        declare(fe->variable, OwnershipState::Owned);
        check_block(fe->body);
        pop_scope();
        return;
    }

    // sam.field = val
    if (auto* fa = dynamic_cast<const FieldAssignStatement*>(s)) {
        check_readable(fa->variable, fa->line);
        check_expression(fa->value.get());
        return;
    }

    // standalone expression
    if (auto* es = dynamic_cast<const ExpressionStatement*>(s)) {
        check_expression(es->expr.get());
        return;
    }

    // shape / include / stop / skip — no ownership concerns
}

void BorrowChecker::check_block(const Block& b) {
    push_scope();
    for (auto& stmt : b.stmts)
        check_statement(stmt.get());
    pop_scope();
}

// ── Expression checking ───────────────────────────────────────────────────────

void BorrowChecker::check_expression(const Expression* e) {

    // Variable read — must not be moved or mut-borrowed
    if (auto* i = dynamic_cast<const IdentExpression*>(e)) {
        check_readable(i->name, i->line);
        return;
    }

    // borrow x — transient immutable borrow (function argument); just check validity
    if (auto* be = dynamic_cast<const BorrowExpression*>(e)) {
        check_borrowable(be->source, be->line);
        return;
    }

    // borrow mutable x — transient mutable borrow; just check validity
    if (auto* mbe = dynamic_cast<const MutableBorrowExpression*>(e)) {
        check_mut_borrowable(mbe->source, mbe->line);
        return;
    }

    // Function call — borrow args are handled above; ident args are reads
    if (auto* c = dynamic_cast<const CallExpression*>(e)) {
        for (auto& arg : c->args)
            check_expression(arg.get());
        return;
    }

    // Binary / unary — recurse
    if (auto* bin = dynamic_cast<const BinaryExpression*>(e)) {
        check_expression(bin->left.get());
        check_expression(bin->right.get());
        return;
    }
    if (auto* u = dynamic_cast<const UnaryExpression*>(e)) {
        check_expression(u->operand.get());
        return;
    }

    // Index — check object and index
    if (auto* ie = dynamic_cast<const IndexExpression*>(e)) {
        check_expression(ie->object.get());
        check_expression(ie->index.get());
        return;
    }

    // Field access
    if (auto* fa = dynamic_cast<const FieldAccessExpression*>(e)) {
        check_expression(fa->object.get());
        return;
    }

    // List literal
    if (auto* le = dynamic_cast<const ListExpression*>(e)) {
        for (auto& el : le->elements)
            check_expression(el.get());
        return;
    }

    // Shape instantiation
    if (auto* si = dynamic_cast<const ShapeInstanceExpression*>(e)) {
        for (auto& [fname, fexpr] : si->fields)
            check_expression(fexpr.get());
        return;
    }

    // Literals — always safe
}

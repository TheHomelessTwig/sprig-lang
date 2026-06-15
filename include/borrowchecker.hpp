#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hpp"

// ── Ownership state ───────────────────────────────────────────────────────────

enum class OwnershipState {
    Owned,       // variable owns its value, full access
    Moved,       // value was moved out, variable is invalid
    Borrowed,    // one or more active immutable borrows exist
    MutBorrowed, // one active mutable borrow exists, original is locked
};

struct BorrowError {
    std::string message;
    int         line;
};

// ── BorrowChecker ─────────────────────────────────────────────────────────────

class BorrowChecker {
    // Stack of scopes — each scope maps variable names to ownership state
    std::vector<std::unordered_map<std::string, OwnershipState>> scopes;

    // borrow_source["view"] = "data" means view borrows from data
    std::unordered_map<std::string, std::string> borrow_source;

    // Count of active immutable borrows per variable
    std::unordered_map<std::string, int> borrow_count;

    std::vector<BorrowError> errors;
    std::vector<std::string> source_lines;

public:
    std::vector<BorrowError> check(const Program& program,
                                   const std::string& source);

private:
    void           push_scope();
    void           pop_scope();
    void           declare(const std::string& name, OwnershipState state);
    OwnershipState* lookup(const std::string& name);
    void           error(const std::string& msg, int line);

    void check_readable(const std::string& name, int line);
    void check_movable(const std::string& name, int line);
    void check_borrowable(const std::string& name, int line);
    void check_mut_borrowable(const std::string& name, int line);

    void release_borrows(const std::string& borrow_name);

    void check_statement(const Statement* stmt);
    void check_expression(const Expression* expr);
    void check_block(const Block& block);
};

#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.hpp"
#include "types.hpp"

// ── Type error ────────────────────────────────────────────────────────────────

struct TypeError {
    std::string message;
    int         line;
};

// ── Type checker ──────────────────────────────────────────────────────────────

// Hindley-Milner style inference with unification.
// Runs over the AST before interpretation; returns all errors found (not just the first).
class TypeChecker {
    // Fresh type variable counter
    int next_var_id = 0;

    // Substitution map: type variable ID → resolved type
    std::unordered_map<int, TypePtr> substitutions;

    // Lexical scope stack: each frame maps names to types
    std::vector<std::unordered_map<std::string, TypePtr>> scopes;

    // Shape definitions: shape name → list of (field name, field type)
    std::unordered_map<std::string,
        std::vector<std::pair<std::string, TypePtr>>> shape_types;

    // Accumulated errors (checking continues after each error)
    std::vector<TypeError> errors;

    // Source lines for error context display (main file only)
    std::vector<std::string> source_lines;

    // Paths of already-processed include files (prevents double-include)
    std::unordered_set<std::string> included_files;

    // Directory of the main file — used to resolve relative include paths
    std::string base_path;

public:
    // file_path is optional — used to resolve relative includes and deduplicate.
    std::vector<TypeError> check(const Program& program,
                                 const std::string& source,
                                 const std::string& file_path = "");

private:
    // ── Two-pass program processing ───────────────────────────────────────────

    // Register shapes and forward-declare functions, then fully type-check.
    void process_program(const Program& program);

    // Handle a single include statement: load, parse, and process included file.
    void process_include(const IncludeStatement* inc);

    // ── Type variables ────────────────────────────────────────────────────────

    TypePtr fresh();

    // ── Scope management ──────────────────────────────────────────────────────

    void    push_scope();
    void    pop_scope();
    void    bind(const std::string& name, TypePtr type);
    TypePtr lookup(const std::string& name, int line);

    // ── Unification ───────────────────────────────────────────────────────────

    TypePtr resolve(TypePtr t);                    // follow substitution chain
    void    unify(TypePtr a, TypePtr b, int line); // make two types equal or error
    TypePtr instantiate(TypePtr t);               // replace unbound Vars with fresh ones (for polymorphic calls)

    // ── Inference / checking ──────────────────────────────────────────────────

    TypePtr infer_expression(const Expression* e);
    void    check_statement(const Statement* s, TypePtr return_type);
    void    check_block(const Block& b, TypePtr return_type);

    // ── Error helpers ─────────────────────────────────────────────────────────

    void error(const std::string& msg, int line);
};

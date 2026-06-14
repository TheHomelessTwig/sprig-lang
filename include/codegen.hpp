#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"

#include "ast.hpp"

class CodeGen {
    llvm::LLVMContext                  context;
    std::unique_ptr<llvm::Module>      module;
    std::unique_ptr<llvm::IRBuilder<>> builder;

    // Variable scopes: name → alloca slot
    // All allocas live in the function entry block (required for mem2reg)
    std::vector<std::unordered_map<std::string, llvm::AllocaInst*>> var_scopes;

    // User-defined function table
    std::unordered_map<std::string, llvm::Function*> functions;

    // Current function's return infrastructure
    llvm::BasicBlock* return_block    = nullptr;
    llvm::AllocaInst* return_val_slot = nullptr;

    // Loop exit/continue targets for stop/skip
    std::vector<llvm::BasicBlock*> loop_exit_blocks;
    std::vector<llvm::BasicBlock*> loop_header_blocks;

public:
    // Compile program to LLVM IR text at output_path (.ll file)
    void compile(const Program& program, const std::string& output_path);

private:
    // ── Types ─────────────────────────────────────────────────────────────────
    llvm::Type* double_type();
    llvm::Type* bool_type();
    llvm::Type* ptr_type();
    llvm::Type* void_type();

    // ── Runtime helpers ───────────────────────────────────────────────────────
    void            declare_runtime();
    llvm::Function* get_or_declare(const std::string& name,
                                   llvm::FunctionType* type);
    void            emit_print(llvm::Value* val);

    // ── Variable scope ────────────────────────────────────────────────────────
    void              push_scope();
    void              pop_scope();
    llvm::AllocaInst* alloca_at_entry(llvm::Function* fn,
                                      llvm::Type* type,
                                      const std::string& name);
    void              set_var(const std::string& name, llvm::AllocaInst* slot);
    llvm::AllocaInst* get_var(const std::string& name);

    // ── Type coercion ─────────────────────────────────────────────────────────
    llvm::Value* to_bool(llvm::Value* val);              // any → i1 for branches
    llvm::Value* coerce(llvm::Value* val, llvm::Type* t); // val → t for stores

    // ── Code generation ───────────────────────────────────────────────────────
    llvm::Value* gen_expr(const Expression* e);
    void         gen_stmt(const Statement* s);
    void         gen_block(const Block& b);
};

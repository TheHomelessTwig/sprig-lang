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
#include "types.hpp"

using ShapeTypeMap = std::unordered_map<std::string,
    std::vector<std::pair<std::string, TypePtr>>>;

class CodeGen {
    llvm::LLVMContext                  context;
    std::unique_ptr<llvm::Module>      module;
    std::unique_ptr<llvm::IRBuilder<>> builder;

    // Variable scopes: name → alloca slot
    std::vector<std::unordered_map<std::string, llvm::AllocaInst*>> var_scopes;

    // Sprig type of each named variable (parallel to var_scopes)
    std::vector<std::unordered_map<std::string, TypePtr>> type_scopes;

    // User-defined function table
    std::unordered_map<std::string, llvm::Function*> functions;

    // Current function return infrastructure
    llvm::BasicBlock* return_block    = nullptr;
    llvm::AllocaInst* return_val_slot = nullptr;
    TypePtr           return_sprig_type;

    // Loop exit/continue targets
    std::vector<llvm::BasicBlock*> loop_exit_blocks;
    std::vector<llvm::BasicBlock*> loop_header_blocks;

    // Expression type map from type checker
    const ExprTypeMap* expr_types = nullptr;

    // Shape field definitions from type checker
    const ShapeTypeMap* shape_types = nullptr;

    // LLVM struct type cache for each shape name
    std::unordered_map<std::string, llvm::StructType*> shape_llvm_types;

    // SprigList struct type (length i64, capacity i64, data ptr)
    llvm::StructType* list_struct_type = nullptr;

public:
    void compile(const Program& program,
                 const ExprTypeMap& expr_types,
                 const ShapeTypeMap& shape_types,
                 const std::string& output_path);

private:
    // ── LLVM primitive types ──────────────────────────────────────────────────
    llvm::Type* double_type();
    llvm::Type* bool_type();
    llvm::Type* ptr_type();
    llvm::Type* void_type();
    llvm::Type* i64_type();
    llvm::Type* i8_type();

    // ── Type mapping ──────────────────────────────────────────────────────────
    llvm::Type*      sprig_to_llvm(const TypePtr& t);
    llvm::StructType* get_shape_llvm_type(const std::string& name);
    llvm::StructType* get_list_struct();
    int               shape_field_index(const std::string& shape_name,
                                         const std::string& field);

    // ── Runtime helpers ───────────────────────────────────────────────────────
    void            declare_runtime();
    llvm::Function* get_or_declare(const std::string& name,
                                   llvm::FunctionType* type);
    void            emit_print(llvm::Value* val, TypePtr sprig_type);

    // ── List helpers (inline codegen) ─────────────────────────────────────────
    llvm::Value* list_new(int initial_cap = 4);
    void         list_append(llvm::Function* fn, llvm::Value* list_ptr,
                             llvm::Value* elem);
    llvm::Value* list_length(llvm::Value* list_ptr);
    llvm::Value* list_get(llvm::Value* list_ptr, llvm::Value* idx);

    // ── Value encoding for list storage (everything as i64) ───────────────────
    llvm::Value* encode_for_list(llvm::Value* val);
    llvm::Value* decode_from_list(llvm::Value* raw, const TypePtr& elem_type);

    // ── Variable scope ────────────────────────────────────────────────────────
    void              push_scope();
    void              pop_scope();
    llvm::AllocaInst* alloca_at_entry(llvm::Function* fn,
                                      llvm::Type* type,
                                      const std::string& name);
    void              set_var(const std::string& name,
                              llvm::AllocaInst* slot,
                              TypePtr sprig_type = nullptr);
    llvm::AllocaInst* get_var(const std::string& name);
    TypePtr           get_var_type(const std::string& name);

    // ── Type coercion ─────────────────────────────────────────────────────────
    llvm::Value* to_bool(llvm::Value* val);
    llvm::Value* coerce(llvm::Value* val, llvm::Type* t);

    // ── Sprig-type lookup for an expression ───────────────────────────────────
    TypePtr expr_type(const Expression* e);

    // ── Code generation ───────────────────────────────────────────────────────
    llvm::Value* gen_expr(const Expression* e);
    void         gen_stmt(const Statement* s);
    void         gen_block(const Block& b);
};

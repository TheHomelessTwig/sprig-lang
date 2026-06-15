#include "codegen.hpp"

#include <stdexcept>
#include <system_error>

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

// ── LLVM primitive types ──────────────────────────────────────────────────────

llvm::Type* CodeGen::double_type() { return llvm::Type::getDoubleTy(context); }
llvm::Type* CodeGen::bool_type()   { return llvm::Type::getInt1Ty(context); }
llvm::Type* CodeGen::ptr_type()    { return llvm::PointerType::get(context, 0); }
llvm::Type* CodeGen::void_type()   { return llvm::Type::getVoidTy(context); }
llvm::Type* CodeGen::i64_type()    { return llvm::Type::getInt64Ty(context); }
llvm::Type* CodeGen::i8_type()     { return llvm::Type::getInt8Ty(context); }
llvm::Type* CodeGen::i32_type()    { return llvm::Type::getInt32Ty(context); }

// ── Type mapping ──────────────────────────────────────────────────────────────

llvm::Type* CodeGen::sprig_to_llvm(const TypePtr& type) {
    if (!type) return double_type();
    switch (type->kind) {
        case Type::Kind::Number:  return double_type();
        case Type::Kind::Flag:    return bool_type();
        case Type::Kind::Text:    return ptr_type();
        case Type::Kind::Nothing: return double_type();
        case Type::Kind::Shape:   return ptr_type();
        case Type::Kind::List:    return ptr_type();
        case Type::Kind::Own:     return ptr_type();  // Box<T> is a heap pointer
        case Type::Kind::RawPtr:  return ptr_type();  // raw void*
        default:                  return double_type();
    }
}

llvm::StructType* CodeGen::get_shape_llvm_type(const std::string& name) {
    auto cached = shape_llvm_types.find(name);
    if (cached != shape_llvm_types.end()) return cached->second;

    auto shape_info = shape_types->find(name);
    if (shape_info == shape_types->end())
        throw std::runtime_error("Unknown shape: " + name);

    std::vector<llvm::Type*> field_types;
    for (auto& [_, field_type] : shape_info->second)
        field_types.push_back(sprig_to_llvm(field_type));

    auto* struct_type = llvm::StructType::create(context, field_types, "shape." + name);
    shape_llvm_types[name] = struct_type;
    return struct_type;
}

int CodeGen::shape_field_index(const std::string& shape_name,
                                const std::string& field) {
    auto shape_info = shape_types->find(shape_name);
    if (shape_info == shape_types->end()) return -1;
    for (int i = 0; i < (int)shape_info->second.size(); i++)
        if (shape_info->second[i].first == field) return i;
    return -1;
}

llvm::StructType* CodeGen::get_list_struct() {
    if (!list_struct_type)
        list_struct_type = llvm::StructType::create(
            context,
            {i64_type(), i64_type(), ptr_type()},
            "SprigList");
    return list_struct_type;
}

// ── Entry point ───────────────────────────────────────────────────────────────

void CodeGen::compile(const Program& program,
                      const ExprTypeMap& expression_types,
                      const ShapeTypeMap& shape_type_map,
                      const std::string& output_path) {
    module      = std::make_unique<llvm::Module>("sprig", context);
    builder     = std::make_unique<llvm::IRBuilder<>>(context);
    expr_types  = &expression_types;
    shape_types = &shape_type_map;

    declare_runtime();

    // Pre-build all shape LLVM struct types
    for (auto& [name, _] : shape_type_map)
        get_shape_llvm_type(name);

    // ── First pass: forward-declare all user functions ────────────────────────
    for (auto& stmt : program.stmts) {
        if (auto* function_stmt = dynamic_cast<const FunctionStatement*>(stmt.get())) {
            std::vector<llvm::Type*> param_types(function_stmt->params.size(), double_type());
            auto* function_type = llvm::FunctionType::get(double_type(), param_types, false);
            auto* llvm_function = llvm::Function::Create(
                function_type, llvm::Function::ExternalLinkage, function_stmt->name, module.get());
            size_t i = 0;
            for (auto& arg : llvm_function->args()) arg.setName(function_stmt->params[i++]);
            functions[function_stmt->name] = llvm_function;
        }
    }

    // ── Second pass: compile function bodies ──────────────────────────────────
    for (auto& stmt : program.stmts) {
        if (auto* function_stmt = dynamic_cast<const FunctionStatement*>(stmt.get())) {
            auto* llvm_function = functions[function_stmt->name];
            auto* entry         = llvm::BasicBlock::Create(context, "entry", llvm_function);
            builder->SetInsertPoint(entry);
            push_scope();

            size_t i = 0;
            for (auto& arg : llvm_function->args()) {
                auto* slot = alloca_at_entry(llvm_function, double_type(),
                                             std::string(arg.getName()));
                builder->CreateStore(&arg, slot);
                set_var(std::string(arg.getName()), slot);
                i++;
            }

            return_block       = llvm::BasicBlock::Create(context, "return", llvm_function);
            return_val_slot    = alloca_at_entry(llvm_function, double_type(), "retval");
            return_sprig_type  = Type::make_number();
            builder->CreateStore(llvm::ConstantFP::get(double_type(), 0.0),
                                 return_val_slot);

            gen_block(function_stmt->body);

            if (!builder->GetInsertBlock()->getTerminator())
                builder->CreateBr(return_block);

            builder->SetInsertPoint(return_block);
            auto* return_val = builder->CreateLoad(double_type(), return_val_slot, "ret");
            builder->CreateRet(return_val);

            pop_scope();
            return_block      = nullptr;
            return_val_slot   = nullptr;
            return_sprig_type = nullptr;
        }
    }

    // ── main() ────────────────────────────────────────────────────────────────
    auto* main_fn = llvm::Function::Create(
        llvm::FunctionType::get(i32_type(), {i32_type(), ptr_type()}, false),
        llvm::Function::ExternalLinkage, "main", module.get());
    auto arg_iterator = main_fn->arg_begin();
    arg_iterator->setName("argc"); auto* argc_val = &*arg_iterator++;
    arg_iterator->setName("argv"); auto* argv_val = &*arg_iterator;
    auto* main_entry = llvm::BasicBlock::Create(context, "entry", main_fn);
    builder->SetInsertPoint(main_entry);
    push_scope();

    // Store argc/argv in globals so args_count()/args_get() can access them
    auto* argc_global = new llvm::GlobalVariable(*module, i32_type(), false,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantInt::get(i32_type(), 0), "__sprig_argc");
    auto* argv_global = new llvm::GlobalVariable(*module, ptr_type(), false,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(ptr_type())), "__sprig_argv");
    builder->CreateStore(argc_val, argc_global);
    builder->CreateStore(argv_val, argv_global);

    for (auto& stmt : program.stmts)
        if (!dynamic_cast<const FunctionStatement*>(stmt.get()))
            gen_stmt(stmt.get());

    pop_scope();
    if (!builder->GetInsertBlock()->getTerminator())
        builder->CreateRet(llvm::ConstantInt::get(i32_type(), 0));

    // ── Verify and emit ───────────────────────────────────────────────────────
    std::string verification_error;
    llvm::raw_string_ostream error_stream(verification_error);
    if (llvm::verifyModule(*module, &error_stream))
        throw std::runtime_error("LLVM verification failed:\n" + error_stream.str());

    std::error_code file_error;
    llvm::raw_fd_ostream out(output_path, file_error, llvm::sys::fs::OF_Text);
    if (file_error) throw std::runtime_error("Cannot write output: " + file_error.message());
    module->print(out, nullptr);
}

// ── Runtime declarations ──────────────────────────────────────────────────────

void CodeGen::declare_runtime() {
    // printf(ptr fmt, ...) -> i32
    get_or_declare("printf",
        llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                                {ptr_type()}, true));
    // sprintf(ptr buf, ptr fmt, ...) -> i32
    get_or_declare("sprintf",
        llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                                {ptr_type(), ptr_type()}, true));
    // atof(ptr) -> double
    get_or_declare("atof",
        llvm::FunctionType::get(double_type(), {ptr_type()}, false));
    // malloc(i64) -> ptr
    get_or_declare("malloc",
        llvm::FunctionType::get(ptr_type(), {i64_type()}, false));
    // realloc(ptr, i64) -> ptr
    get_or_declare("realloc",
        llvm::FunctionType::get(ptr_type(), {ptr_type(), i64_type()}, false));
    // fgets(ptr buf, i32 n, ptr stream) -> ptr
    get_or_declare("fgets",
        llvm::FunctionType::get(ptr_type(),
            {ptr_type(), llvm::Type::getInt32Ty(context), ptr_type()}, false));
    // strlen(ptr) -> i64
    get_or_declare("strlen",
        llvm::FunctionType::get(i64_type(), {ptr_type()}, false));
    // free(ptr) -> void
    get_or_declare("free",
        llvm::FunctionType::get(void_type(), {ptr_type()}, false));
    // strncpy(ptr dst, ptr src, i64 n) -> ptr
    get_or_declare("strncpy",
        llvm::FunctionType::get(ptr_type(),
            {ptr_type(), ptr_type(), i64_type()}, false));
    // strstr(ptr haystack, ptr needle) -> ptr
    get_or_declare("strstr",
        llvm::FunctionType::get(ptr_type(),
            {ptr_type(), ptr_type()}, false));
    // fopen(ptr path, ptr mode) -> ptr
    get_or_declare("fopen",
        llvm::FunctionType::get(ptr_type(),
            {ptr_type(), ptr_type()}, false));
    // fclose(ptr) -> i32
    get_or_declare("fclose",
        llvm::FunctionType::get(i32_type(), {ptr_type()}, false));
    // fputs(ptr str, ptr stream) -> i32
    get_or_declare("fputs",
        llvm::FunctionType::get(i32_type(),
            {ptr_type(), ptr_type()}, false));
    // fread(ptr buf, i64 size, i64 count, ptr stream) -> i64
    get_or_declare("fread",
        llvm::FunctionType::get(i64_type(),
            {ptr_type(), i64_type(), i64_type(), ptr_type()}, false));
    // fseek(ptr stream, i64 offset, i32 whence) -> i32
    get_or_declare("fseek",
        llvm::FunctionType::get(i32_type(),
            {ptr_type(), i64_type(), i32_type()}, false));
    // ftell(ptr stream) -> i64
    get_or_declare("ftell",
        llvm::FunctionType::get(i64_type(), {ptr_type()}, false));
    // exit(i32 code) -> void
    get_or_declare("exit",
        llvm::FunctionType::get(void_type(), {i32_type()}, false));
}

llvm::Function* CodeGen::get_or_declare(const std::string& name,
                                         llvm::FunctionType* type) {
    if (auto* existing = module->getFunction(name)) return existing;
    return llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                  name, module.get());
}

// ── Variable scope ────────────────────────────────────────────────────────────

void CodeGen::push_scope() {
    var_scopes.push_back({});
    type_scopes.push_back({});
}
void CodeGen::pop_scope() {
    var_scopes.pop_back();
    type_scopes.pop_back();
}

llvm::AllocaInst* CodeGen::alloca_at_entry(llvm::Function* parent_function,
                                            llvm::Type* type,
                                            const std::string& name) {
    llvm::IRBuilder<> temp_builder(&parent_function->getEntryBlock(),
                                   parent_function->getEntryBlock().begin());
    return temp_builder.CreateAlloca(type, nullptr, name);
}

void CodeGen::set_var(const std::string& name, llvm::AllocaInst* slot,
                      TypePtr sprig_type) {
    var_scopes.back()[name]  = slot;
    type_scopes.back()[name] = std::move(sprig_type);
}

llvm::AllocaInst* CodeGen::get_var(const std::string& name) {
    for (int i = (int)var_scopes.size() - 1; i >= 0; i--) {
        auto var_entry = var_scopes[i].find(name);
        if (var_entry != var_scopes[i].end()) return var_entry->second;
    }
    return nullptr;
}

TypePtr CodeGen::get_var_type(const std::string& name) {
    for (int i = (int)type_scopes.size() - 1; i >= 0; i--) {
        auto type_entry = type_scopes[i].find(name);
        if (type_entry != type_scopes[i].end()) return type_entry->second;
    }
    return nullptr;
}

// ── Type coercion ─────────────────────────────────────────────────────────────

llvm::Value* CodeGen::to_bool(llvm::Value* val) {
    if (val->getType() == bool_type()) return val;
    if (val->getType() == double_type())
        return builder->CreateFCmpONE(val,
            llvm::ConstantFP::get(double_type(), 0.0), "to_bool");
    return val;
}

llvm::Value* CodeGen::coerce(llvm::Value* val, llvm::Type* target_type) {
    if (val->getType() == target_type) return val;
    // bool ↔ double
    if (target_type == double_type() && val->getType() == bool_type())
        return builder->CreateUIToFP(val, double_type(), "bool_to_f64");
    if (target_type == bool_type() && val->getType() == double_type())
        return builder->CreateFCmpONE(val,
            llvm::ConstantFP::get(double_type(), 0.0), "f64_to_bool");
    // ptr ↔ double — store pointer bits in double via i64 (64-bit round-trip)
    if (target_type == double_type() && val->getType() == ptr_type()) {
        auto* as_i64 = builder->CreatePtrToInt(val, i64_type(), "ptr_to_i64");
        return builder->CreateBitCast(as_i64, double_type(), "i64_to_f64");
    }
    if (target_type == ptr_type() && val->getType() == double_type()) {
        auto* as_i64 = builder->CreateBitCast(val, i64_type(), "f64_to_i64");
        return builder->CreateIntToPtr(as_i64, ptr_type(), "i64_to_ptr");
    }
    return val;
}

// ── Sprig type lookup for an expression ──────────────────────────────────────

TypePtr CodeGen::expr_type(const Expression* expr_ptr) {
    if (!expr_types) return nullptr;
    auto type_entry = expr_types->find(expr_ptr);
    if (type_entry == expr_types->end()) return nullptr;
    return type_entry->second;
}

// ── List storage encoding (all elements stored as i64 in data array) ──────────

llvm::Value* CodeGen::encode_for_list(llvm::Value* val) {
    auto* value_type = val->getType();
    if (value_type == i64_type()) return val;
    if (value_type == double_type())
        return builder->CreateBitCast(val, i64_type(), "enc_f64");
    if (value_type == bool_type())
        return builder->CreateZExt(val, i64_type(), "enc_bool");
    if (value_type == ptr_type())
        return builder->CreatePtrToInt(val, i64_type(), "enc_ptr");
    return builder->CreateZExtOrBitCast(val, i64_type(), "enc");
}

llvm::Value* CodeGen::decode_from_list(llvm::Value* raw, const TypePtr& element_type) {
    if (!element_type || element_type->kind == Type::Kind::Number)
        return builder->CreateBitCast(raw, double_type(), "dec_f64");
    if (element_type->kind == Type::Kind::Flag)
        return builder->CreateTrunc(raw, bool_type(), "dec_bool");
    if (element_type->kind == Type::Kind::Text ||
        element_type->kind == Type::Kind::Shape ||
        element_type->kind == Type::Kind::List)
        return builder->CreateIntToPtr(raw, ptr_type(), "dec_ptr");
    return builder->CreateBitCast(raw, double_type(), "dec_f64");
}

// ── List helpers ──────────────────────────────────────────────────────────────

// Allocate a new SprigList on the heap with given initial capacity.
llvm::Value* CodeGen::list_new(int initial_cap) {
    auto* list_struct  = get_list_struct();
    auto* malloc_fn    = module->getFunction("malloc");

    // malloc(sizeof SprigList)
    auto struct_size = module->getDataLayout().getTypeAllocSize(list_struct);
    auto* list_ptr   = builder->CreateCall(
        malloc_fn->getFunctionType(), malloc_fn,
        {llvm::ConstantInt::get(i64_type(), struct_size)}, "list_ptr");

    // Store length = 0
    auto* len_ptr = builder->CreateStructGEP(list_struct, list_ptr, 0, "len_ptr");
    builder->CreateStore(llvm::ConstantInt::get(i64_type(), 0), len_ptr);

    // Store capacity
    auto* cap_ptr = builder->CreateStructGEP(list_struct, list_ptr, 1, "cap_ptr");
    builder->CreateStore(llvm::ConstantInt::get(i64_type(), initial_cap), cap_ptr);

    // malloc(capacity * 8) for data array
    auto* data_bytes = llvm::ConstantInt::get(i64_type(), initial_cap * 8LL);
    auto* data_ptr   = builder->CreateCall(
        malloc_fn->getFunctionType(), malloc_fn, {data_bytes}, "data_ptr");
    auto* data_slot  = builder->CreateStructGEP(list_struct, list_ptr, 2, "data_slot");
    builder->CreateStore(data_ptr, data_slot);

    return list_ptr;
}

// Append an i64-encoded element to a SprigList, growing if needed.
void CodeGen::list_append(llvm::Function* parent_function, llvm::Value* list_ptr,
                          llvm::Value* elem) {
    auto* list_struct  = get_list_struct();
    auto* realloc_fn   = module->getFunction("realloc");

    auto* len_ptr = builder->CreateStructGEP(list_struct, list_ptr, 0, "len_ptr");
    auto* cap_ptr = builder->CreateStructGEP(list_struct, list_ptr, 1, "cap_ptr");
    auto* dat_ptr = builder->CreateStructGEP(list_struct, list_ptr, 2, "dat_ptr");

    auto* length   = builder->CreateLoad(i64_type(), len_ptr, "len");
    auto* capacity = builder->CreateLoad(i64_type(), cap_ptr, "cap");
    auto* data     = builder->CreateLoad(ptr_type(),  dat_ptr, "dat");

    // if (len == cap): realloc data to cap*2
    auto* need_grow = builder->CreateICmpEQ(length, capacity, "need_grow");
    auto* grow_bb   = llvm::BasicBlock::Create(context, "grow",    parent_function);
    auto* store_bb  = llvm::BasicBlock::Create(context, "store",   parent_function);
    builder->CreateCondBr(need_grow, grow_bb, store_bb);

    builder->SetInsertPoint(grow_bb);
    auto* new_capacity = builder->CreateMul(capacity,
        llvm::ConstantInt::get(i64_type(), 2), "new_cap");
    auto* new_bytes    = builder->CreateMul(new_capacity,
        llvm::ConstantInt::get(i64_type(), 8), "new_bytes");
    auto* new_data     = builder->CreateCall(
        realloc_fn->getFunctionType(), realloc_fn, {data, new_bytes}, "new_dat");
    builder->CreateStore(new_data,     dat_ptr);
    builder->CreateStore(new_capacity, cap_ptr);
    builder->CreateBr(store_bb);

    builder->SetInsertPoint(store_bb);
    // data ptr may have changed — reload
    auto* current_data = builder->CreateLoad(ptr_type(), dat_ptr, "cur_dat");
    auto* element_ptr  = builder->CreateGEP(i64_type(), current_data, {length}, "elem_ptr");
    builder->CreateStore(elem, element_ptr);
    auto* new_length = builder->CreateAdd(length,
        llvm::ConstantInt::get(i64_type(), 1), "new_len");
    builder->CreateStore(new_length, len_ptr);
}

llvm::Value* CodeGen::list_length(llvm::Value* list_ptr) {
    auto* list_struct = get_list_struct();
    auto* len_ptr     = builder->CreateStructGEP(list_struct, list_ptr, 0, "len_ptr");
    auto* len_i64     = builder->CreateLoad(i64_type(), len_ptr, "len");
    return builder->CreateSIToFP(len_i64, double_type(), "len_f64");
}

llvm::Value* CodeGen::list_get(llvm::Value* list_ptr, llvm::Value* index) {
    auto* list_struct = get_list_struct();
    auto* dat_ptr     = builder->CreateStructGEP(list_struct, list_ptr, 2, "dat_ptr");
    auto* data        = builder->CreateLoad(ptr_type(), dat_ptr, "dat");
    auto* index_i64   = builder->CreateFPToSI(index, i64_type(), "idx_i64");
    auto* element_ptr = builder->CreateGEP(i64_type(), data, {index_i64}, "ep");
    return builder->CreateLoad(i64_type(), element_ptr, "raw");
}

// ── Print helper ──────────────────────────────────────────────────────────────

void CodeGen::emit_print(llvm::Value* val, TypePtr sprig_type) {
    auto* printf_fn  = module->getFunction("printf");
    auto* value_type = val->getType();
    // Treat unresolved type variables as unknown — fall back on LLVM value type
    if (sprig_type && sprig_type->kind == Type::Kind::Var) sprig_type = nullptr;
    // Auto-deref own<T> for printing
    if (sprig_type && sprig_type->kind == Type::Kind::Own) sprig_type = sprig_type->element_type;
    // Raw pointer: print as hex address
    if (sprig_type && sprig_type->kind == Type::Kind::RawPtr) {
        auto* fmt    = builder->CreateGlobalStringPtr("0x%llx\n", ".fmt_ptr");
        auto* as_i64 = builder->CreateBitCast(val, i64_type(), "ptr_i64");
        builder->CreateCall(printf_fn->getFunctionType(), printf_fn, {fmt, as_i64});
        return;
    }

    if (value_type == double_type()) {
        auto* fmt = builder->CreateGlobalStringPtr("%g\n", ".fmt_num");
        builder->CreateCall(printf_fn->getFunctionType(), printf_fn, {fmt, val});
    } else if (value_type == bool_type()) {
        auto* true_str  = builder->CreateGlobalStringPtr("true\n",  ".true");
        auto* false_str = builder->CreateGlobalStringPtr("false\n", ".false");
        auto* selected  = builder->CreateSelect(val, true_str, false_str, "bool_str");
        builder->CreateCall(printf_fn->getFunctionType(), printf_fn, {selected});
    } else {
        // ptr — check if it's a list
        if (sprig_type && sprig_type->kind == Type::Kind::List) {
            // Print list as [a, b, c]
            auto* open = builder->CreateGlobalStringPtr("[", ".lst_open");
            builder->CreateCall(printf_fn->getFunctionType(), printf_fn, {open});

            auto* current_function = builder->GetInsertBlock()->getParent();
            auto* list_struct      = get_list_struct();
            auto* len_ptr          = builder->CreateStructGEP(list_struct, val, 0);
            auto* length           = builder->CreateLoad(i64_type(), len_ptr, "len");
            auto* dat_ptr          = builder->CreateStructGEP(list_struct, val, 2);
            auto* data             = builder->CreateLoad(ptr_type(), dat_ptr, "dat");

            auto* index_slot = alloca_at_entry(current_function, i64_type(), "prnt_i");
            builder->CreateStore(llvm::ConstantInt::get(i64_type(), 0), index_slot);

            auto* loop_header = llvm::BasicBlock::Create(context, "prnt_hdr",  current_function);
            auto* loop_body   = llvm::BasicBlock::Create(context, "prnt_body", current_function);
            auto* loop_exit   = llvm::BasicBlock::Create(context, "prnt_exit", current_function);
            builder->CreateBr(loop_header);

            builder->SetInsertPoint(loop_header);
            auto* current_index = builder->CreateLoad(i64_type(), index_slot, "ci");
            builder->CreateCondBr(builder->CreateICmpSLT(current_index, length), loop_body, loop_exit);

            builder->SetInsertPoint(loop_body);
            auto* element_ptr = builder->CreateGEP(i64_type(), data, {current_index}, "ep");
            auto* raw_element = builder->CreateLoad(i64_type(), element_ptr, "raw");

            TypePtr element_type = sprig_type->element_type;
            auto* decoded = decode_from_list(raw_element, element_type);
            // print separator
            auto* sep_needed = builder->CreateICmpSGT(current_index,
                llvm::ConstantInt::get(i64_type(), 0));
            auto* separator_block = llvm::BasicBlock::Create(context, "sep_bb",  current_function);
            auto* element_block   = llvm::BasicBlock::Create(context, "elem_bb", current_function);
            builder->CreateCondBr(sep_needed, separator_block, element_block);
            builder->SetInsertPoint(separator_block);
            auto* separator = builder->CreateGlobalStringPtr(", ", ".sep");
            builder->CreateCall(printf_fn->getFunctionType(), printf_fn, {separator});
            builder->CreateBr(element_block);
            builder->SetInsertPoint(element_block);

            emit_print(decoded, element_type);

            auto* next_index = builder->CreateAdd(current_index,
                llvm::ConstantInt::get(i64_type(), 1), "ni");
            builder->CreateStore(next_index, index_slot);
            builder->CreateBr(loop_header);

            builder->SetInsertPoint(loop_exit);
            auto* close = builder->CreateGlobalStringPtr("]\n", ".lst_close");
            builder->CreateCall(printf_fn->getFunctionType(), printf_fn, {close});
        } else {
            // String or shape pointer — print as string
            auto* fmt = builder->CreateGlobalStringPtr("%s\n", ".fmt_str");
            builder->CreateCall(printf_fn->getFunctionType(), printf_fn, {fmt, val});
        }
    }
}

// ── Expression codegen ────────────────────────────────────────────────────────

llvm::Value* CodeGen::gen_expr(const Expression* expr) {

    // ── Literals ──────────────────────────────────────────────────────────────

    if (auto* number_expr = dynamic_cast<const NumberExpression*>(expr))
        return llvm::ConstantFP::get(double_type(), number_expr->value);

    if (auto* string_expr = dynamic_cast<const StringExpression*>(expr))
        return builder->CreateGlobalStringPtr(string_expr->value, ".str");

    if (auto* bool_expr = dynamic_cast<const BoolExpression*>(expr))
        return llvm::ConstantInt::get(bool_type(), bool_expr->value ? 1 : 0);

    if (dynamic_cast<const NothingExpression*>(expr))
        return llvm::ConstantFP::get(double_type(), 0.0);

    // ── Variable read ─────────────────────────────────────────────────────────

    if (auto* ident_expr = dynamic_cast<const IdentExpression*>(expr)) {
        auto* slot = get_var(ident_expr->name);
        if (!slot)
            throw std::runtime_error("Undefined variable '" + ident_expr->name + "'");
        return builder->CreateLoad(slot->getAllocatedType(), slot, ident_expr->name);
    }

    // ── Borrow expressions ────────────────────────────────────────────────────

    if (auto* borrow_expr = dynamic_cast<const BorrowExpression*>(expr)) {
        auto* slot = get_var(borrow_expr->source);
        if (!slot) throw std::runtime_error("Undefined variable '" + borrow_expr->source + "'");
        return builder->CreateLoad(slot->getAllocatedType(), slot, borrow_expr->source);
    }
    if (auto* mutable_borrow_expr = dynamic_cast<const MutableBorrowExpression*>(expr)) {
        auto* slot = get_var(mutable_borrow_expr->source);
        if (!slot) throw std::runtime_error("Undefined variable '" + mutable_borrow_expr->source + "'");
        return builder->CreateLoad(slot->getAllocatedType(), slot, mutable_borrow_expr->source);
    }

    // ── own expr — Box<T> ────────────────────────────────────────────────────

    if (auto* own_expr = dynamic_cast<const OwnExpression*>(expr)) {
        auto* inner_value = gen_expr(own_expr->inner.get());
        // Shapes and lists are already heap pointers — return as-is
        if (inner_value->getType() == ptr_type()) return inner_value;
        // Scalar (number, bool) — box onto heap
        auto* malloc_fn = module->getFunction("malloc");
        uint64_t byte_size = module->getDataLayout().getTypeAllocSize(inner_value->getType());
        auto* memory       = builder->CreateCall(malloc_fn->getFunctionType(), malloc_fn,
            {llvm::ConstantInt::get(i64_type(), byte_size)}, "boxed");
        builder->CreateStore(inner_value, memory);
        return memory;
    }

    // ── List literal ──────────────────────────────────────────────────────────

    if (auto* list_expr = dynamic_cast<const ListExpression*>(expr)) {
        auto* current_function = builder->GetInsertBlock()->getParent();
        int   initial_capacity = std::max(4, (int)list_expr->elements.size());
        auto* list_ptr         = list_new(initial_capacity);
        for (auto& element : list_expr->elements) {
            auto* element_value = gen_expr(element.get());
            list_append(current_function, list_ptr, encode_for_list(element_value));
        }
        return list_ptr;
    }

    // ── Index access ──────────────────────────────────────────────────────────

    if (auto* index_expr = dynamic_cast<const IndexExpression*>(expr)) {
        auto* list_value  = gen_expr(index_expr->object.get());
        auto* index_value = gen_expr(index_expr->index.get());
        auto* raw_element = list_get(list_value, index_value);
        TypePtr list_type    = expr_type(index_expr->object.get());
        TypePtr element_type = (list_type && list_type->kind == Type::Kind::List)
                                   ? list_type->element_type : nullptr;
        return decode_from_list(raw_element, element_type);
    }

    // ── Shape instantiation ───────────────────────────────────────────────────

    if (auto* shape_instance = dynamic_cast<const ShapeInstanceExpression*>(expr)) {
        auto* struct_type = get_shape_llvm_type(shape_instance->shape_name);
        auto* malloc_fn   = module->getFunction("malloc");
        auto  struct_size = module->getDataLayout().getTypeAllocSize(struct_type);
        auto* shape_ptr   = builder->CreateCall(
            malloc_fn->getFunctionType(), malloc_fn,
            {llvm::ConstantInt::get(i64_type(), struct_size)}, "shape_ptr");

        // Build a name→value map for the field initializers
        std::unordered_map<std::string, llvm::Value*> field_values;
        for (auto& [field_name, field_expr] : shape_instance->fields)
            field_values[field_name] = gen_expr(field_expr.get());

        auto shape_info = shape_types->find(shape_instance->shape_name);
        if (shape_info != shape_types->end()) {
            for (int i = 0; i < (int)shape_info->second.size(); i++) {
                auto& [field_name, field_type] = shape_info->second[i];
                auto* field_ptr  = builder->CreateStructGEP(struct_type, shape_ptr, i, field_name);
                llvm::Value* field_value = nullptr;
                auto value_it = field_values.find(field_name);
                if (value_it != field_values.end())
                    field_value = value_it->second;
                else
                    field_value = llvm::Constant::getNullValue(sprig_to_llvm(field_type));
                builder->CreateStore(coerce(field_value, sprig_to_llvm(field_type)), field_ptr);
            }
        }
        return shape_ptr;
    }

    // ── Field access ──────────────────────────────────────────────────────────

    if (auto* field_access = dynamic_cast<const FieldAccessExpression*>(expr)) {
        auto* object_value = gen_expr(field_access->object.get());
        TypePtr object_type = expr_type(field_access->object.get());
        // Auto-deref own<T> — the ptr value is already the heap-allocated struct
        if (object_type && object_type->kind == Type::Kind::Own)
            object_type = object_type->element_type;
        if (!object_type || object_type->kind != Type::Kind::Shape)
            throw std::runtime_error("Field access on non-shape");
        auto* struct_type  = get_shape_llvm_type(object_type->shape_name);
        int   field_index  = shape_field_index(object_type->shape_name, field_access->field);
        if (field_index < 0)
            throw std::runtime_error("No field '" + field_access->field + "'");
        auto* field_ptr    = builder->CreateStructGEP(struct_type, object_value, field_index, field_access->field);
        // Determine field LLVM type
        auto shape_info = shape_types->find(object_type->shape_name);
        llvm::Type* field_llvm_type = double_type();
        if (shape_info != shape_types->end() && field_index < (int)shape_info->second.size())
            field_llvm_type = sprig_to_llvm(shape_info->second[field_index].second);
        return builder->CreateLoad(field_llvm_type, field_ptr, field_access->field);
    }

    // ── Binary expressions ────────────────────────────────────────────────────

    if (auto* binary_expr = dynamic_cast<const BinaryExpression*>(expr)) {

        if (binary_expr->op == "and") {
            auto* current_function = builder->GetInsertBlock()->getParent();
            auto* right_block      = llvm::BasicBlock::Create(context, "and_rhs",   current_function);
            auto* end_block        = llvm::BasicBlock::Create(context, "and_merge", current_function);
            auto* left_value       = to_bool(gen_expr(binary_expr->left.get()));
            auto* left_block       = builder->GetInsertBlock();
            builder->CreateCondBr(left_value, right_block, end_block);
            builder->SetInsertPoint(right_block);
            auto* right_value        = to_bool(gen_expr(binary_expr->right.get()));
            auto* right_block_after  = builder->GetInsertBlock();
            builder->CreateBr(end_block);
            builder->SetInsertPoint(end_block);
            auto* phi = builder->CreatePHI(bool_type(), 2, "and");
            phi->addIncoming(llvm::ConstantInt::get(bool_type(), 0), left_block);
            phi->addIncoming(right_value, right_block_after);
            return phi;
        }
        if (binary_expr->op == "or") {
            auto* current_function = builder->GetInsertBlock()->getParent();
            auto* right_block      = llvm::BasicBlock::Create(context, "or_rhs",   current_function);
            auto* end_block        = llvm::BasicBlock::Create(context, "or_merge", current_function);
            auto* left_value       = to_bool(gen_expr(binary_expr->left.get()));
            auto* left_block       = builder->GetInsertBlock();
            builder->CreateCondBr(left_value, end_block, right_block);
            builder->SetInsertPoint(right_block);
            auto* right_value       = to_bool(gen_expr(binary_expr->right.get()));
            auto* right_block_after = builder->GetInsertBlock();
            builder->CreateBr(end_block);
            builder->SetInsertPoint(end_block);
            auto* phi = builder->CreatePHI(bool_type(), 2, "or");
            phi->addIncoming(llvm::ConstantInt::get(bool_type(), 1), left_block);
            phi->addIncoming(right_value, right_block_after);
            return phi;
        }

        auto* left_value  = gen_expr(binary_expr->left.get());
        auto* right_value = gen_expr(binary_expr->right.get());

        // '+' overloaded: numeric add or string concat
        if (binary_expr->op == "+") {
            bool left_is_string  = left_value->getType()  == ptr_type();
            bool right_is_string = right_value->getType() == ptr_type();
            if (left_is_string || right_is_string) {
                auto* current_function = builder->GetInsertBlock()->getParent();
                auto* arr_type         = llvm::ArrayType::get(i8_type(), 1024);
                auto* concat_buffer    = alloca_at_entry(current_function, arr_type, "concat_buf");
                auto* sprintf_fn       = module->getFunction("sprintf");

                auto num_to_str = [&](llvm::Value* num_val,
                                      const char* tmp_name) -> llvm::Value* {
                    auto* tmp = alloca_at_entry(current_function,
                        llvm::ArrayType::get(i8_type(), 64), tmp_name);
                    auto* fmt = builder->CreateGlobalStringPtr("%g", ".nfmt");
                    builder->CreateCall(sprintf_fn->getFunctionType(), sprintf_fn,
                                       {tmp, fmt, num_val});
                    return tmp;
                };

                llvm::Value* left_str  = left_is_string  ? left_value  : num_to_str(left_value,  "lhs_tmp");
                llvm::Value* right_str = right_is_string ? right_value : num_to_str(right_value, "rhs_tmp");
                auto* concat_format = builder->CreateGlobalStringPtr("%s%s", ".cat_fmt");
                builder->CreateCall(sprintf_fn->getFunctionType(), sprintf_fn,
                                   {concat_buffer, concat_format, left_str, right_str});
                return concat_buffer;
            }
            return builder->CreateFAdd(left_value, right_value, "add");
        }

        if (binary_expr->op == "-")  return builder->CreateFSub(left_value, right_value, "sub");
        if (binary_expr->op == "*")  return builder->CreateFMul(left_value, right_value, "mul");
        if (binary_expr->op == "/")  return builder->CreateFDiv(left_value, right_value, "div");
        if (binary_expr->op == ">")  return builder->CreateFCmpOGT(left_value, right_value, "gt");
        if (binary_expr->op == "<")  return builder->CreateFCmpOLT(left_value, right_value, "lt");
        if (binary_expr->op == ">=") return builder->CreateFCmpOGE(left_value, right_value, "ge");
        if (binary_expr->op == "<=") return builder->CreateFCmpOLE(left_value, right_value, "le");
        if (binary_expr->op == "==" || binary_expr->op == "is") {
            if (left_value->getType() == bool_type())
                return builder->CreateICmpEQ(left_value, right_value, "eq");
            return builder->CreateFCmpOEQ(left_value, right_value, "eq");
        }
        if (binary_expr->op == "!=") {
            if (left_value->getType() == bool_type())
                return builder->CreateICmpNE(left_value, right_value, "ne");
            return builder->CreateFCmpONE(left_value, right_value, "ne");
        }
        throw std::runtime_error("Unknown binary operator: " + binary_expr->op);
    }

    // ── Unary ─────────────────────────────────────────────────────────────────

    if (auto* unary_expr = dynamic_cast<const UnaryExpression*>(expr)) {
        auto* operand_value = gen_expr(unary_expr->operand.get());
        if (unary_expr->op == "not") return builder->CreateNot(to_bool(operand_value), "not");
        if (unary_expr->op == "-")   return builder->CreateFNeg(operand_value, "neg");
        throw std::runtime_error("Unknown unary operator: " + unary_expr->op);
    }

    // ── Function calls ────────────────────────────────────────────────────────

    if (auto* call_expr = dynamic_cast<const CallExpression*>(expr)) {

        if (call_expr->callee == "print") {
            for (auto& arg : call_expr->args) {
                auto* arg_value = gen_expr(arg.get());
                emit_print(arg_value, expr_type(arg.get()));
            }
            return llvm::ConstantFP::get(double_type(), 0.0);
        }

        if (call_expr->callee == "to_number") {
            auto* arg_value = gen_expr(call_expr->args[0].get());
            if (arg_value->getType() == double_type()) return arg_value;
            if (arg_value->getType() == bool_type())
                return builder->CreateUIToFP(arg_value, double_type(), "b_to_f");
            auto* atof_fn = module->getFunction("atof");
            return builder->CreateCall(atof_fn->getFunctionType(), atof_fn,
                                       {arg_value}, "to_num");
        }

        if (call_expr->callee == "to_text") {
            auto* arg_value        = gen_expr(call_expr->args[0].get());
            if (arg_value->getType() == ptr_type()) return arg_value;
            auto* current_function = builder->GetInsertBlock()->getParent();
            auto* arr_type         = llvm::ArrayType::get(i8_type(), 64);
            auto* text_buffer      = alloca_at_entry(current_function, arr_type, "txt_buf");
            auto* sprintf_fn       = module->getFunction("sprintf");
            if (arg_value->getType() == bool_type()) {
                auto* true_str  = builder->CreateGlobalStringPtr("true",  ".true_s");
                auto* false_str = builder->CreateGlobalStringPtr("false", ".false_s");
                return builder->CreateSelect(arg_value, true_str, false_str, "bool_s");
            }
            auto* fmt = builder->CreateGlobalStringPtr("%g", ".tfmt");
            builder->CreateCall(sprintf_fn->getFunctionType(), sprintf_fn,
                               {text_buffer, fmt, arg_value});
            return text_buffer;
        }

        if (call_expr->callee == "length") {
            auto* arg_value = gen_expr(call_expr->args[0].get());
            TypePtr arg_type = expr_type(call_expr->args[0].get());
            if (arg_type && arg_type->kind == Type::Kind::Text) {
                // strlen for text
                auto* strlen_fn = module->getFunction("strlen");
                auto* len_i64   = builder->CreateCall(
                    strlen_fn->getFunctionType(), strlen_fn, {arg_value}, "slen");
                return builder->CreateSIToFP(len_i64, double_type(), "slen_f");
            }
            // list length
            return list_length(arg_value);
        }

        if (call_expr->callee == "append") {
            auto* current_function = builder->GetInsertBlock()->getParent();
            auto* list_ptr         = gen_expr(call_expr->args[0].get());
            auto* element_value    = gen_expr(call_expr->args[1].get());
            list_append(current_function, list_ptr, encode_for_list(element_value));
            return llvm::ConstantFP::get(double_type(), 0.0);
        }

        if (call_expr->callee == "first") {
            auto* list_ptr   = gen_expr(call_expr->args[0].get());
            TypePtr list_type    = expr_type(call_expr->args[0].get());
            TypePtr element_type = (list_type && list_type->kind == Type::Kind::List)
                                       ? list_type->element_type : nullptr;
            auto* raw_element = list_get(list_ptr,
                llvm::ConstantFP::get(double_type(), 0.0));
            return decode_from_list(raw_element, element_type);
        }

        if (call_expr->callee == "last") {
            auto* list_ptr   = gen_expr(call_expr->args[0].get());
            TypePtr list_type    = expr_type(call_expr->args[0].get());
            TypePtr element_type = (list_type && list_type->kind == Type::Kind::List)
                                       ? list_type->element_type : nullptr;
            auto* length_float = list_length(list_ptr);
            auto* one          = llvm::ConstantFP::get(double_type(), 1.0);
            auto* last_index   = builder->CreateFSub(length_float, one, "last_idx");
            auto* raw_element  = list_get(list_ptr, last_index);
            return decode_from_list(raw_element, element_type);
        }

        if (call_expr->callee == "pop") {
            auto* list_ptr    = gen_expr(call_expr->args[0].get());
            TypePtr list_type = expr_type(call_expr->args[0].get());
            TypePtr elem_type = (list_type && list_type->kind == Type::Kind::List)
                                    ? list_type->element_type : nullptr;
            auto* list_struct = get_list_struct();
            auto* len_ptr     = builder->CreateStructGEP(list_struct, list_ptr, 0, "len_ptr");
            auto* len         = builder->CreateLoad(i64_type(), len_ptr, "len");
            auto* new_len     = builder->CreateSub(len,
                llvm::ConstantInt::get(i64_type(), 1), "new_len");
            builder->CreateStore(new_len, len_ptr);
            auto* dat_ptr  = builder->CreateStructGEP(list_struct, list_ptr, 2, "dat_ptr");
            auto* data     = builder->CreateLoad(ptr_type(), dat_ptr, "data");
            auto* elem_ptr = builder->CreateGEP(i64_type(), data, {new_len}, "pop_elem");
            auto* raw      = builder->CreateLoad(i64_type(), elem_ptr, "pop_raw");
            return decode_from_list(raw, elem_type);
        }

        if (call_expr->callee == "input") {
            auto* current_function = builder->GetInsertBlock()->getParent();
            auto* buf_type         = llvm::ArrayType::get(i8_type(), 1024);
            auto* input_buffer     = alloca_at_entry(current_function, buf_type, "input_buf");
            auto* fgets_fn         = module->getFunction("fgets");
            // stdin — declare as external global if needed
            auto* stdin_global = module->getOrInsertGlobal("stdin", ptr_type());
            auto* stdin_value  = builder->CreateLoad(ptr_type(), stdin_global, "stdin");
            builder->CreateCall(fgets_fn->getFunctionType(), fgets_fn,
                {input_buffer, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 1024),
                 stdin_value});
            // strip trailing newline: find '\n' and replace with '\0'
            auto* strlen_fn     = module->getFunction("strlen");
            auto* string_length = builder->CreateCall(
                strlen_fn->getFunctionType(), strlen_fn, {input_buffer}, "slen");
            auto* has_nl        = builder->CreateICmpSGT(string_length,
                llvm::ConstantInt::get(i64_type(), 0), "has_nl");
            auto* newline_block = llvm::BasicBlock::Create(context, "nl_strip", current_function);
            auto* end_block     = llvm::BasicBlock::Create(context, "nl_done",  current_function);
            builder->CreateCondBr(has_nl, newline_block, end_block);
            builder->SetInsertPoint(newline_block);
            auto* last_pos = builder->CreateSub(string_length,
                llvm::ConstantInt::get(i64_type(), 1), "last_pos");
            auto* last_ptr = builder->CreateGEP(i8_type(), input_buffer, {last_pos}, "last_ptr");
            auto* nl_char  = builder->CreateLoad(i8_type(), last_ptr, "nl_char");
            auto* is_nl    = builder->CreateICmpEQ(nl_char,
                llvm::ConstantInt::get(i8_type(), '\n'), "is_nl");
            auto* strip_block    = llvm::BasicBlock::Create(context, "strip",    current_function);
            auto* no_strip_block = llvm::BasicBlock::Create(context, "no_strip", current_function);
            builder->CreateCondBr(is_nl, strip_block, no_strip_block);
            builder->SetInsertPoint(strip_block);
            builder->CreateStore(llvm::ConstantInt::get(i8_type(), 0), last_ptr);
            builder->CreateBr(no_strip_block);
            builder->SetInsertPoint(no_strip_block);
            builder->CreateBr(end_block);
            builder->SetInsertPoint(end_block);
            return input_buffer;
        }

        // ── Raw pointer built-ins ─────────────────────────────────────────────

        // Helper: raw address value (double or ptr) → ptr
        auto addr_to_ptr = [&](llvm::Value* address) -> llvm::Value* {
            if (address->getType() == ptr_type()) return address;
            // double → i64 → ptr
            auto* as_i64 = builder->CreateBitCast(address, i64_type(), "adr_i64");
            return builder->CreateIntToPtr(as_i64, ptr_type(), "adr_ptr");
        };
        // Helper: ptr → double (for returning raw addresses)
        auto pointer_to_double = [&](llvm::Value* pointer) -> llvm::Value* {
            auto* as_i64 = builder->CreatePtrToInt(pointer, i64_type(), "adr_i64");
            return builder->CreateBitCast(as_i64, double_type(), "adr_f64");
        };

        if (call_expr->callee == "allocate") {
            auto* byte_count     = gen_expr(call_expr->args[0].get());
            auto* byte_count_i64 = builder->CreateFPToSI(byte_count, i64_type(), "alloc_n");
            auto* malloc_fn      = module->getFunction("malloc");
            auto* raw_memory     = builder->CreateCall(malloc_fn->getFunctionType(),
                malloc_fn, {byte_count_i64}, "raw_ptr");
            return pointer_to_double(raw_memory);
        }
        if (call_expr->callee == "read") {
            auto* as_ptr = addr_to_ptr(gen_expr(call_expr->args[0].get()));
            return builder->CreateLoad(double_type(), as_ptr, "raw_read");
        }
        if (call_expr->callee == "write") {
            auto* as_ptr      = addr_to_ptr(gen_expr(call_expr->args[0].get()));
            auto* write_value = gen_expr(call_expr->args[1].get());
            builder->CreateStore(coerce(write_value, double_type()), as_ptr);
            return llvm::ConstantFP::get(double_type(), 0.0);
        }
        if (call_expr->callee == "free") {
            auto* as_ptr  = addr_to_ptr(gen_expr(call_expr->args[0].get()));
            auto* free_fn = module->getFunction("free");
            builder->CreateCall(free_fn->getFunctionType(), free_fn, {as_ptr});
            return llvm::ConstantFP::get(double_type(), 0.0);
        }
        if (call_expr->callee == "ptr_add") {
            auto* as_ptr       = addr_to_ptr(gen_expr(call_expr->args[0].get()));
            auto* offset_float = gen_expr(call_expr->args[1].get());
            auto* offset_i64   = builder->CreateFPToSI(offset_float, i64_type(), "offset");
            auto* new_ptr      = builder->CreateGEP(i8_type(), as_ptr, {offset_i64}, "new_ptr");
            return pointer_to_double(new_ptr);
        }
        if (call_expr->callee == "ptr_to_number" || call_expr->callee == "number_to_ptr")
            return gen_expr(call_expr->args[0].get());

        // ── String built-ins ──────────────────────────────────────────────────

        if (call_expr->callee == "char_code") {
            auto* str_ptr = gen_expr(call_expr->args[0].get());
            auto* ch      = builder->CreateLoad(i8_type(), str_ptr, "char");
            auto* as_i64  = builder->CreateZExt(ch, i64_type(), "char_i64");
            return builder->CreateSIToFP(as_i64, double_type(), "char_f64");
        }

        if (call_expr->callee == "char_from_code") {
            auto* code             = gen_expr(call_expr->args[0].get());
            auto* current_function = builder->GetInsertBlock()->getParent();
            auto* buf              = alloca_at_entry(current_function,
                llvm::ArrayType::get(i8_type(), 2), "char_buf");
            auto* ch_i64  = builder->CreateFPToSI(code, i64_type(), "code_i64");
            auto* ch_i8   = builder->CreateTrunc(ch_i64, i8_type(), "char_i8");
            auto* slot0   = builder->CreateGEP(i8_type(), buf,
                {llvm::ConstantInt::get(i64_type(), 0)});
            auto* slot1   = builder->CreateGEP(i8_type(), buf,
                {llvm::ConstantInt::get(i64_type(), 1)});
            builder->CreateStore(ch_i8, slot0);
            builder->CreateStore(llvm::ConstantInt::get(i8_type(), 0), slot1);
            return buf;
        }

        if (call_expr->callee == "substring") {
            auto* src     = gen_expr(call_expr->args[0].get());
            auto* start_f = gen_expr(call_expr->args[1].get());
            auto* len_f   = gen_expr(call_expr->args[2].get());
            auto* start_i = builder->CreateFPToSI(start_f, i64_type(), "sub_start");
            auto* len_i   = builder->CreateFPToSI(len_f,   i64_type(), "sub_len");
            auto* src_off = builder->CreateGEP(i8_type(), src, {start_i}, "src_off");
            auto* buf_sz  = builder->CreateAdd(len_i,
                llvm::ConstantInt::get(i64_type(), 1), "sub_bufsz");
            auto* malloc_fn  = module->getFunction("malloc");
            auto* buf        = builder->CreateCall(malloc_fn->getFunctionType(),
                malloc_fn, {buf_sz}, "sub_buf");
            auto* strncpy_fn = module->getFunction("strncpy");
            builder->CreateCall(strncpy_fn->getFunctionType(), strncpy_fn,
                {buf, src_off, len_i});
            auto* null_pos = builder->CreateGEP(i8_type(), buf, {len_i}, "sub_end");
            builder->CreateStore(llvm::ConstantInt::get(i8_type(), 0), null_pos);
            return buf;
        }

        if (call_expr->callee == "string_contains") {
            auto* hay       = gen_expr(call_expr->args[0].get());
            auto* ndl       = gen_expr(call_expr->args[1].get());
            auto* strstr_fn = module->getFunction("strstr");
            auto* result    = builder->CreateCall(strstr_fn->getFunctionType(),
                strstr_fn, {hay, ndl}, "strstr_r");
            auto* null_val  = llvm::Constant::getNullValue(ptr_type());
            return builder->CreateICmpNE(result, null_val, "contains");
        }

        // ── File I/O ──────────────────────────────────────────────────────────

        if (call_expr->callee == "read_file") {
            auto* path     = gen_expr(call_expr->args[0].get());
            auto* fopen_fn = module->getFunction("fopen");
            auto* mode_r   = builder->CreateGlobalStringPtr("r", ".mode_r");
            auto* fp       = builder->CreateCall(fopen_fn->getFunctionType(),
                fopen_fn, {path, mode_r}, "fp");
            auto* fseek_fn  = module->getFunction("fseek");
            auto* ftell_fn  = module->getFunction("ftell");
            builder->CreateCall(fseek_fn->getFunctionType(), fseek_fn,
                {fp, llvm::ConstantInt::get(i64_type(), 0),
                 llvm::ConstantInt::get(i32_type(), 2)});
            auto* file_size = builder->CreateCall(ftell_fn->getFunctionType(),
                ftell_fn, {fp}, "fsize");
            builder->CreateCall(fseek_fn->getFunctionType(), fseek_fn,
                {fp, llvm::ConstantInt::get(i64_type(), 0),
                 llvm::ConstantInt::get(i32_type(), 0)});
            auto* malloc_fn = module->getFunction("malloc");
            auto* buf_size  = builder->CreateAdd(file_size,
                llvm::ConstantInt::get(i64_type(), 1), "buf_sz");
            auto* buf       = builder->CreateCall(malloc_fn->getFunctionType(),
                malloc_fn, {buf_size}, "file_buf");
            auto* fread_fn  = module->getFunction("fread");
            builder->CreateCall(fread_fn->getFunctionType(), fread_fn,
                {buf, llvm::ConstantInt::get(i64_type(), 1), file_size, fp});
            auto* end_ptr   = builder->CreateGEP(i8_type(), buf, {file_size}, "file_end");
            builder->CreateStore(llvm::ConstantInt::get(i8_type(), 0), end_ptr);
            auto* fclose_fn = module->getFunction("fclose");
            builder->CreateCall(fclose_fn->getFunctionType(), fclose_fn, {fp});
            return buf;
        }

        if (call_expr->callee == "write_file") {
            auto* path     = gen_expr(call_expr->args[0].get());
            auto* content  = gen_expr(call_expr->args[1].get());
            auto* fopen_fn = module->getFunction("fopen");
            auto* mode_w   = builder->CreateGlobalStringPtr("w", ".mode_w");
            auto* fp       = builder->CreateCall(fopen_fn->getFunctionType(),
                fopen_fn, {path, mode_w}, "fp");
            auto* fputs_fn  = module->getFunction("fputs");
            builder->CreateCall(fputs_fn->getFunctionType(), fputs_fn, {content, fp});
            auto* fclose_fn = module->getFunction("fclose");
            builder->CreateCall(fclose_fn->getFunctionType(), fclose_fn, {fp});
            return llvm::ConstantFP::get(double_type(), 0.0);
        }

        // ── Process arguments ─────────────────────────────────────────────────

        if (call_expr->callee == "args_count") {
            auto* argc_g = module->getNamedGlobal("__sprig_argc");
            auto* argc   = builder->CreateLoad(i32_type(), argc_g, "argc");
            return builder->CreateSIToFP(argc, double_type(), "argc_f64");
        }

        if (call_expr->callee == "args_get") {
            auto* idx_f   = gen_expr(call_expr->args[0].get());
            auto* idx_i   = builder->CreateFPToSI(idx_f, i64_type(), "arg_idx");
            auto* argv_g  = module->getNamedGlobal("__sprig_argv");
            auto* argv    = builder->CreateLoad(ptr_type(), argv_g, "argv");
            auto* arg_ptr = builder->CreateGEP(ptr_type(), argv, {idx_i}, "arg_ptr");
            return builder->CreateLoad(ptr_type(), arg_ptr, "arg");
        }

        if (call_expr->callee == "exit") {
            auto* code_f   = gen_expr(call_expr->args[0].get());
            auto* code_i   = builder->CreateFPToSI(code_f, i32_type(), "exit_code");
            auto* exit_fn  = module->getFunction("exit");
            builder->CreateCall(exit_fn->getFunctionType(), exit_fn, {code_i});
            builder->CreateUnreachable();
            auto* current_function = builder->GetInsertBlock()->getParent();
            auto* dead = llvm::BasicBlock::Create(context, "dead", current_function);
            builder->SetInsertPoint(dead);
            return llvm::ConstantFP::get(double_type(), 0.0);
        }

        // User-defined function
        auto function_it = functions.find(call_expr->callee);
        if (function_it == functions.end())
            throw std::runtime_error("Undefined function: " + call_expr->callee);
        std::vector<llvm::Value*> call_args;
        for (auto& arg : call_expr->args) {
            auto* arg_value = gen_expr(arg.get());
            call_args.push_back(coerce(arg_value, double_type()));
        }
        return builder->CreateCall(function_it->second->getFunctionType(),
                                   function_it->second, call_args, call_expr->callee + "_ret");
    }

    throw std::runtime_error("Expression type not supported in compiled mode");
}

// ── Statement codegen ─────────────────────────────────────────────────────────

void CodeGen::gen_stmt(const Statement* stmt) {

    // include / shape definition — already processed; skip
    if (dynamic_cast<const IncludeStatement*>(stmt))        return;
    if (dynamic_cast<const ShapeDefinitionStatement*>(stmt)) return;

    // unsafe: — same as a normal block at IR level
    if (auto* unsafe_stmt = dynamic_cast<const UnsafeStatement*>(stmt)) {
        gen_block(unsafe_stmt->body);
        return;
    }

    // let [mutable] x = expr
    if (auto* variable_stmt = dynamic_cast<const VariableStatement*>(stmt)) {
        auto* value         = gen_expr(variable_stmt->value.get());
        TypePtr sprig_type  = expr_type(variable_stmt->value.get());
        llvm::Type* llvm_type = sprig_type ? sprig_to_llvm(sprig_type) : value->getType();
        auto* existing      = get_var(variable_stmt->name);
        if (existing) {
            builder->CreateStore(coerce(value, existing->getAllocatedType()), existing);
        } else {
            auto* current_function = builder->GetInsertBlock()->getParent();
            auto* slot             = alloca_at_entry(current_function, llvm_type, variable_stmt->name);
            builder->CreateStore(coerce(value, llvm_type), slot);
            set_var(variable_stmt->name, slot, sprig_type);
        }
        return;
    }

    // let x borrow [mutable] y — alias source slot
    if (auto* borrow_stmt = dynamic_cast<const BorrowStatement*>(stmt)) {
        auto* slot = get_var(borrow_stmt->source);
        if (slot) set_var(borrow_stmt->target, slot, get_var_type(borrow_stmt->source));
        return;
    }
    if (auto* mutable_borrow_stmt = dynamic_cast<const MutableBorrowStatement*>(stmt)) {
        auto* slot = get_var(mutable_borrow_stmt->source);
        if (slot) set_var(mutable_borrow_stmt->target, slot, get_var_type(mutable_borrow_stmt->source));
        return;
    }

    // give back expr
    if (auto* return_stmt = dynamic_cast<const ReturnStatement*>(stmt)) {
        auto* return_value = gen_expr(return_stmt->value.get());
        if (return_val_slot)
            builder->CreateStore(
                coerce(return_value, return_val_slot->getAllocatedType()), return_val_slot);
        if (return_block) builder->CreateBr(return_block);
        auto* current_function = builder->GetInsertBlock()->getParent();
        auto* dead = llvm::BasicBlock::Create(context, "dead", current_function);
        builder->SetInsertPoint(dead);
        return;
    }

    // when cond: ... [otherwise: ...]
    if (auto* if_stmt = dynamic_cast<const IfStatement*>(stmt)) {
        auto* current_function = builder->GetInsertBlock()->getParent();
        auto* then_block       = llvm::BasicBlock::Create(context, "then",  current_function);
        auto* merge_block      = llvm::BasicBlock::Create(context, "merge", current_function);
        auto* else_block       = if_stmt->else_block
                                    ? llvm::BasicBlock::Create(context, "else", current_function)
                                    : merge_block;

        builder->CreateCondBr(to_bool(gen_expr(if_stmt->condition.get())), then_block, else_block);

        builder->SetInsertPoint(then_block);
        gen_block(if_stmt->then_block);
        if (!builder->GetInsertBlock()->getTerminator())
            builder->CreateBr(merge_block);

        if (if_stmt->else_block) {
            builder->SetInsertPoint(else_block);
            gen_block(*if_stmt->else_block);
            if (!builder->GetInsertBlock()->getTerminator())
                builder->CreateBr(merge_block);
        }

        builder->SetInsertPoint(merge_block);
        return;
    }

    // as long as cond: body
    if (auto* while_stmt = dynamic_cast<const WhileStatement*>(stmt)) {
        auto* current_function = builder->GetInsertBlock()->getParent();
        auto* header           = llvm::BasicBlock::Create(context, "while_hdr",  current_function);
        auto* loop_body        = llvm::BasicBlock::Create(context, "while_body", current_function);
        auto* exit_block       = llvm::BasicBlock::Create(context, "while_exit", current_function);

        builder->CreateBr(header);
        builder->SetInsertPoint(header);
        builder->CreateCondBr(to_bool(gen_expr(while_stmt->condition.get())), loop_body, exit_block);

        builder->SetInsertPoint(loop_body);
        loop_exit_blocks.push_back(exit_block);
        loop_header_blocks.push_back(header);
        gen_block(while_stmt->body);
        loop_exit_blocks.pop_back();
        loop_header_blocks.pop_back();
        if (!builder->GetInsertBlock()->getTerminator())
            builder->CreateBr(header);

        builder->SetInsertPoint(exit_block);
        return;
    }

    // for each x in list: body
    if (auto* for_each_stmt = dynamic_cast<const ForEachStatement*>(stmt)) {
        auto* current_function = builder->GetInsertBlock()->getParent();
        auto* list_ptr         = gen_expr(for_each_stmt->iterable.get());
        TypePtr iterable_type  = expr_type(for_each_stmt->iterable.get());
        TypePtr element_type   = (iterable_type && iterable_type->kind == Type::Kind::List)
                                     ? iterable_type->element_type : nullptr;

        // index counter slot
        auto* index_slot = alloca_at_entry(current_function, i64_type(), "fe_i");
        builder->CreateStore(llvm::ConstantInt::get(i64_type(), 0), index_slot);

        auto* loop_header    = llvm::BasicBlock::Create(context, "fe_hdr",  current_function);
        auto* loop_body      = llvm::BasicBlock::Create(context, "fe_body", current_function);
        auto* increment_block = llvm::BasicBlock::Create(context, "fe_incr", current_function);
        auto* exit_block     = llvm::BasicBlock::Create(context, "fe_exit", current_function);

        builder->CreateBr(loop_header);
        builder->SetInsertPoint(loop_header);
        auto* len_i64         = builder->CreateFPToSI(list_length(list_ptr), i64_type(), "len_i64");
        auto* current_index   = builder->CreateLoad(i64_type(), index_slot, "ci");
        builder->CreateCondBr(builder->CreateICmpSLT(current_index, len_i64), loop_body, exit_block);

        builder->SetInsertPoint(loop_body);
        push_scope();

        // Get element value and bind loop variable
        auto* raw_element  = list_get(list_ptr,
            builder->CreateSIToFP(builder->CreateLoad(i64_type(), index_slot), double_type()));
        auto* element_value = decode_from_list(raw_element, element_type);
        auto* element_slot  = alloca_at_entry(current_function, element_value->getType(), for_each_stmt->variable);
        builder->CreateStore(element_value, element_slot);
        set_var(for_each_stmt->variable, element_slot, element_type);

        // skip → increment block (not header), so the index advances before re-checking
        loop_exit_blocks.push_back(exit_block);
        loop_header_blocks.push_back(increment_block);
        gen_block(for_each_stmt->body);
        loop_exit_blocks.pop_back();
        loop_header_blocks.pop_back();

        pop_scope();

        if (!builder->GetInsertBlock()->getTerminator())
            builder->CreateBr(increment_block);

        // Increment block — reached by normal fall-through and by skip
        builder->SetInsertPoint(increment_block);
        auto* current_count = builder->CreateLoad(i64_type(), index_slot, "cur_i");
        auto* next_count    = builder->CreateAdd(current_count,
            llvm::ConstantInt::get(i64_type(), 1), "next_i");
        builder->CreateStore(next_count, index_slot);
        builder->CreateBr(loop_header);

        builder->SetInsertPoint(exit_block);
        return;
    }

    // sam.field = value
    if (auto* field_assign_stmt = dynamic_cast<const FieldAssignStatement*>(stmt)) {
        auto* obj_slot = get_var(field_assign_stmt->variable);
        if (!obj_slot)
            throw std::runtime_error("Undefined variable '" + field_assign_stmt->variable + "'");
        auto* obj_ptr       = builder->CreateLoad(ptr_type(), obj_slot, field_assign_stmt->variable);
        TypePtr object_type = get_var_type(field_assign_stmt->variable);
        if (!object_type || object_type->kind != Type::Kind::Shape)
            throw std::runtime_error("Field assign on non-shape");
        auto* struct_type = get_shape_llvm_type(object_type->shape_name);
        int   field_index = shape_field_index(object_type->shape_name, field_assign_stmt->field);
        if (field_index < 0)
            throw std::runtime_error("No field '" + field_assign_stmt->field + "'");
        auto* field_ptr    = builder->CreateStructGEP(struct_type, obj_ptr, field_index, field_assign_stmt->field);
        auto* assign_value = gen_expr(field_assign_stmt->value.get());
        // find field llvm type
        auto shape_info = shape_types->find(object_type->shape_name);
        llvm::Type* field_llvm_type = double_type();
        if (shape_info != shape_types->end() && field_index < (int)shape_info->second.size())
            field_llvm_type = sprig_to_llvm(shape_info->second[field_index].second);
        builder->CreateStore(coerce(assign_value, field_llvm_type), field_ptr);
        return;
    }

    // stop
    if (dynamic_cast<const StopStatement*>(stmt)) {
        if (!loop_exit_blocks.empty())
            builder->CreateBr(loop_exit_blocks.back());
        auto* current_function = builder->GetInsertBlock()->getParent();
        auto* dead = llvm::BasicBlock::Create(context, "dead", current_function);
        builder->SetInsertPoint(dead);
        return;
    }

    // skip
    if (dynamic_cast<const SkipStatement*>(stmt)) {
        if (!loop_header_blocks.empty())
            builder->CreateBr(loop_header_blocks.back());
        auto* current_function = builder->GetInsertBlock()->getParent();
        auto* dead = llvm::BasicBlock::Create(context, "dead", current_function);
        builder->SetInsertPoint(dead);
        return;
    }

    // Standalone expression
    if (auto* expr_stmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
        gen_expr(expr_stmt->expr.get());
        return;
    }
}

void CodeGen::gen_block(const Block& block) {
    push_scope();
    for (auto& stmt : block.stmts)
        gen_stmt(stmt.get());
    pop_scope();
}

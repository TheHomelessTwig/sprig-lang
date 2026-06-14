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

// ── Type mapping ──────────────────────────────────────────────────────────────

llvm::Type* CodeGen::sprig_to_llvm(const TypePtr& t) {
    if (!t) return double_type();
    switch (t->kind) {
        case Type::Kind::Number:  return double_type();
        case Type::Kind::Flag:    return bool_type();
        case Type::Kind::Text:    return ptr_type();
        case Type::Kind::Nothing: return double_type();
        case Type::Kind::Shape:   return ptr_type();  // heap pointer
        case Type::Kind::List:    return ptr_type();  // heap pointer to SprigList
        default:                  return double_type();
    }
}

llvm::StructType* CodeGen::get_shape_llvm_type(const std::string& name) {
    auto it = shape_llvm_types.find(name);
    if (it != shape_llvm_types.end()) return it->second;

    auto sit = shape_types->find(name);
    if (sit == shape_types->end())
        throw std::runtime_error("Unknown shape: " + name);

    std::vector<llvm::Type*> field_types;
    for (auto& [_, ft] : sit->second)
        field_types.push_back(sprig_to_llvm(ft));

    auto* st = llvm::StructType::create(context, field_types, "shape." + name);
    shape_llvm_types[name] = st;
    return st;
}

int CodeGen::shape_field_index(const std::string& shape_name,
                                const std::string& field) {
    auto it = shape_types->find(shape_name);
    if (it == shape_types->end()) return -1;
    for (int i = 0; i < (int)it->second.size(); i++)
        if (it->second[i].first == field) return i;
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
                      const ExprTypeMap& etypes,
                      const ShapeTypeMap& stypes,
                      const std::string& output_path) {
    module      = std::make_unique<llvm::Module>("sprig", context);
    builder     = std::make_unique<llvm::IRBuilder<>>(context);
    expr_types  = &etypes;
    shape_types = &stypes;

    declare_runtime();

    // Pre-build all shape LLVM struct types
    for (auto& [name, _] : stypes)
        get_shape_llvm_type(name);

    // ── First pass: forward-declare all user functions ────────────────────────
    for (auto& stmt : program.stmts) {
        if (auto* fn = dynamic_cast<const FunctionStatement*>(stmt.get())) {
            std::vector<llvm::Type*> param_types(fn->params.size(), double_type());
            auto* fn_type = llvm::FunctionType::get(double_type(), param_types, false);
            auto* llvm_fn = llvm::Function::Create(
                fn_type, llvm::Function::ExternalLinkage, fn->name, module.get());
            size_t i = 0;
            for (auto& arg : llvm_fn->args()) arg.setName(fn->params[i++]);
            functions[fn->name] = llvm_fn;
        }
    }

    // ── Second pass: compile function bodies ──────────────────────────────────
    for (auto& stmt : program.stmts) {
        if (auto* fn = dynamic_cast<const FunctionStatement*>(stmt.get())) {
            auto* llvm_fn = functions[fn->name];
            auto* entry   = llvm::BasicBlock::Create(context, "entry", llvm_fn);
            builder->SetInsertPoint(entry);
            push_scope();

            size_t i = 0;
            for (auto& arg : llvm_fn->args()) {
                auto* slot = alloca_at_entry(llvm_fn, double_type(),
                                             std::string(arg.getName()));
                builder->CreateStore(&arg, slot);
                set_var(std::string(arg.getName()), slot);
                i++;
            }

            return_block       = llvm::BasicBlock::Create(context, "return", llvm_fn);
            return_val_slot    = alloca_at_entry(llvm_fn, double_type(), "retval");
            return_sprig_type  = Type::make_number();
            builder->CreateStore(llvm::ConstantFP::get(double_type(), 0.0),
                                 return_val_slot);

            gen_block(fn->body);

            if (!builder->GetInsertBlock()->getTerminator())
                builder->CreateBr(return_block);

            builder->SetInsertPoint(return_block);
            auto* ret = builder->CreateLoad(double_type(), return_val_slot, "ret");
            builder->CreateRet(ret);

            pop_scope();
            return_block      = nullptr;
            return_val_slot   = nullptr;
            return_sprig_type = nullptr;
        }
    }

    // ── main() ────────────────────────────────────────────────────────────────
    auto* main_fn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {}, false),
        llvm::Function::ExternalLinkage, "main", module.get());
    auto* main_entry = llvm::BasicBlock::Create(context, "entry", main_fn);
    builder->SetInsertPoint(main_entry);
    push_scope();

    for (auto& stmt : program.stmts)
        if (!dynamic_cast<const FunctionStatement*>(stmt.get()))
            gen_stmt(stmt.get());

    pop_scope();
    if (!builder->GetInsertBlock()->getTerminator())
        builder->CreateRet(
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));

    // ── Verify and emit ───────────────────────────────────────────────────────
    std::string err;
    llvm::raw_string_ostream err_stream(err);
    if (llvm::verifyModule(*module, &err_stream))
        throw std::runtime_error("LLVM verification failed:\n" + err_stream.str());

    std::error_code ec;
    llvm::raw_fd_ostream out(output_path, ec, llvm::sys::fs::OF_Text);
    if (ec) throw std::runtime_error("Cannot write output: " + ec.message());
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
    // stdin is a global — accessed via declare of stdin pointer symbol via fgets
}

llvm::Function* CodeGen::get_or_declare(const std::string& name,
                                         llvm::FunctionType* type) {
    if (auto* f = module->getFunction(name)) return f;
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

llvm::AllocaInst* CodeGen::alloca_at_entry(llvm::Function* fn,
                                            llvm::Type* type,
                                            const std::string& name) {
    llvm::IRBuilder<> tmp(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    return tmp.CreateAlloca(type, nullptr, name);
}

void CodeGen::set_var(const std::string& name, llvm::AllocaInst* slot,
                      TypePtr sprig_type) {
    var_scopes.back()[name]  = slot;
    type_scopes.back()[name] = std::move(sprig_type);
}

llvm::AllocaInst* CodeGen::get_var(const std::string& name) {
    for (int i = (int)var_scopes.size() - 1; i >= 0; i--) {
        auto it = var_scopes[i].find(name);
        if (it != var_scopes[i].end()) return it->second;
    }
    return nullptr;
}

TypePtr CodeGen::get_var_type(const std::string& name) {
    for (int i = (int)type_scopes.size() - 1; i >= 0; i--) {
        auto it = type_scopes[i].find(name);
        if (it != type_scopes[i].end()) return it->second;
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

llvm::Value* CodeGen::coerce(llvm::Value* val, llvm::Type* t) {
    if (val->getType() == t) return val;
    // bool ↔ double
    if (t == double_type() && val->getType() == bool_type())
        return builder->CreateUIToFP(val, double_type(), "bool_to_f64");
    if (t == bool_type() && val->getType() == double_type())
        return builder->CreateFCmpONE(val,
            llvm::ConstantFP::get(double_type(), 0.0), "f64_to_bool");
    // ptr ↔ double — store pointer bits in double via i64 (64-bit round-trip)
    if (t == double_type() && val->getType() == ptr_type()) {
        auto* as_i64 = builder->CreatePtrToInt(val, i64_type(), "ptr_to_i64");
        return builder->CreateBitCast(as_i64, double_type(), "i64_to_f64");
    }
    if (t == ptr_type() && val->getType() == double_type()) {
        auto* as_i64 = builder->CreateBitCast(val, i64_type(), "f64_to_i64");
        return builder->CreateIntToPtr(as_i64, ptr_type(), "i64_to_ptr");
    }
    return val;
}

// ── Sprig type lookup for an expression ──────────────────────────────────────

TypePtr CodeGen::expr_type(const Expression* e) {
    if (!expr_types) return nullptr;
    auto it = expr_types->find(e);
    if (it == expr_types->end()) return nullptr;
    return it->second;
}

// ── List storage encoding (all elements stored as i64 in data array) ──────────

llvm::Value* CodeGen::encode_for_list(llvm::Value* val) {
    auto* t = val->getType();
    if (t == i64_type()) return val;
    if (t == double_type())
        return builder->CreateBitCast(val, i64_type(), "enc_f64");
    if (t == bool_type())
        return builder->CreateZExt(val, i64_type(), "enc_bool");
    if (t == ptr_type())
        return builder->CreatePtrToInt(val, i64_type(), "enc_ptr");
    return builder->CreateZExtOrBitCast(val, i64_type(), "enc");
}

llvm::Value* CodeGen::decode_from_list(llvm::Value* raw, const TypePtr& elem_type) {
    if (!elem_type || elem_type->kind == Type::Kind::Number)
        return builder->CreateBitCast(raw, double_type(), "dec_f64");
    if (elem_type->kind == Type::Kind::Flag)
        return builder->CreateTrunc(raw, bool_type(), "dec_bool");
    if (elem_type->kind == Type::Kind::Text ||
        elem_type->kind == Type::Kind::Shape ||
        elem_type->kind == Type::Kind::List)
        return builder->CreateIntToPtr(raw, ptr_type(), "dec_ptr");
    return builder->CreateBitCast(raw, double_type(), "dec_f64");
}

// ── List helpers ──────────────────────────────────────────────────────────────

// Allocate a new SprigList on the heap with given initial capacity.
llvm::Value* CodeGen::list_new(int initial_cap) {
    auto* list_st  = get_list_struct();
    auto* malloc_fn = module->getFunction("malloc");

    // malloc(sizeof SprigList)
    auto struct_size = module->getDataLayout().getTypeAllocSize(list_st);
    auto* list_ptr  = builder->CreateCall(
        malloc_fn->getFunctionType(), malloc_fn,
        {llvm::ConstantInt::get(i64_type(), struct_size)}, "list_ptr");

    // Store length = 0
    auto* len_ptr = builder->CreateStructGEP(list_st, list_ptr, 0, "len_ptr");
    builder->CreateStore(llvm::ConstantInt::get(i64_type(), 0), len_ptr);

    // Store capacity
    auto* cap_ptr = builder->CreateStructGEP(list_st, list_ptr, 1, "cap_ptr");
    builder->CreateStore(llvm::ConstantInt::get(i64_type(), initial_cap), cap_ptr);

    // malloc(capacity * 8) for data array
    auto* data_bytes = llvm::ConstantInt::get(i64_type(), initial_cap * 8LL);
    auto* data_ptr   = builder->CreateCall(
        malloc_fn->getFunctionType(), malloc_fn, {data_bytes}, "data_ptr");
    auto* data_slot  = builder->CreateStructGEP(list_st, list_ptr, 2, "data_slot");
    builder->CreateStore(data_ptr, data_slot);

    return list_ptr;
}

// Append an i64-encoded element to a SprigList, growing if needed.
void CodeGen::list_append(llvm::Function* fn, llvm::Value* list_ptr,
                          llvm::Value* elem) {
    auto* list_st    = get_list_struct();
    auto* realloc_fn = module->getFunction("realloc");

    auto* len_ptr = builder->CreateStructGEP(list_st, list_ptr, 0, "len_ptr");
    auto* cap_ptr = builder->CreateStructGEP(list_st, list_ptr, 1, "cap_ptr");
    auto* dat_ptr = builder->CreateStructGEP(list_st, list_ptr, 2, "dat_ptr");

    auto* len = builder->CreateLoad(i64_type(), len_ptr, "len");
    auto* cap = builder->CreateLoad(i64_type(), cap_ptr, "cap");
    auto* dat = builder->CreateLoad(ptr_type(),  dat_ptr, "dat");

    // if (len == cap): realloc data to cap*2
    auto* need_grow = builder->CreateICmpEQ(len, cap, "need_grow");
    auto* grow_bb   = llvm::BasicBlock::Create(context, "grow",    fn);
    auto* store_bb  = llvm::BasicBlock::Create(context, "store",   fn);
    builder->CreateCondBr(need_grow, grow_bb, store_bb);

    builder->SetInsertPoint(grow_bb);
    auto* new_cap   = builder->CreateMul(cap,
        llvm::ConstantInt::get(i64_type(), 2), "new_cap");
    auto* new_bytes = builder->CreateMul(new_cap,
        llvm::ConstantInt::get(i64_type(), 8), "new_bytes");
    auto* new_dat   = builder->CreateCall(
        realloc_fn->getFunctionType(), realloc_fn, {dat, new_bytes}, "new_dat");
    builder->CreateStore(new_dat,  dat_ptr);
    builder->CreateStore(new_cap,  cap_ptr);
    builder->CreateBr(store_bb);

    builder->SetInsertPoint(store_bb);
    // data ptr may have changed — reload
    auto* cur_dat = builder->CreateLoad(ptr_type(), dat_ptr, "cur_dat");
    auto* elem_ptr = builder->CreateGEP(i64_type(), cur_dat, {len}, "elem_ptr");
    builder->CreateStore(elem, elem_ptr);
    auto* new_len = builder->CreateAdd(len,
        llvm::ConstantInt::get(i64_type(), 1), "new_len");
    builder->CreateStore(new_len, len_ptr);
}

llvm::Value* CodeGen::list_length(llvm::Value* list_ptr) {
    auto* list_st = get_list_struct();
    auto* len_ptr = builder->CreateStructGEP(list_st, list_ptr, 0, "len_ptr");
    auto* len_i64 = builder->CreateLoad(i64_type(), len_ptr, "len");
    return builder->CreateSIToFP(len_i64, double_type(), "len_f64");
}

llvm::Value* CodeGen::list_get(llvm::Value* list_ptr, llvm::Value* idx) {
    auto* list_st = get_list_struct();
    auto* dat_ptr = builder->CreateStructGEP(list_st, list_ptr, 2, "dat_ptr");
    auto* dat     = builder->CreateLoad(ptr_type(), dat_ptr, "dat");
    auto* idx_i64 = builder->CreateFPToSI(idx, i64_type(), "idx_i64");
    auto* ep      = builder->CreateGEP(i64_type(), dat, {idx_i64}, "ep");
    return builder->CreateLoad(i64_type(), ep, "raw");
}

// ── Print helper ──────────────────────────────────────────────────────────────

void CodeGen::emit_print(llvm::Value* val, TypePtr sprig_type) {
    auto* printf_fn = module->getFunction("printf");
    auto* type      = val->getType();
    // Treat unresolved type variables as unknown — fall back on LLVM value type
    if (sprig_type && sprig_type->kind == Type::Kind::Var) sprig_type = nullptr;

    if (type == double_type()) {
        auto* fmt = builder->CreateGlobalStringPtr("%g\n", ".fmt_num");
        builder->CreateCall(printf_fn->getFunctionType(), printf_fn, {fmt, val});
    } else if (type == bool_type()) {
        auto* t = builder->CreateGlobalStringPtr("true\n",  ".true");
        auto* f = builder->CreateGlobalStringPtr("false\n", ".false");
        auto* s = builder->CreateSelect(val, t, f, "bool_str");
        builder->CreateCall(printf_fn->getFunctionType(), printf_fn, {s});
    } else {
        // ptr — check if it's a list
        if (sprig_type && sprig_type->kind == Type::Kind::List) {
            // Print list as [a, b, c]
            auto* open = builder->CreateGlobalStringPtr("[", ".lst_open");
            builder->CreateCall(printf_fn->getFunctionType(), printf_fn, {open});

            auto* fn      = builder->GetInsertBlock()->getParent();
            auto* list_st = get_list_struct();
            auto* len_ptr = builder->CreateStructGEP(list_st, val, 0);
            auto* len     = builder->CreateLoad(i64_type(), len_ptr, "len");
            auto* dat_ptr = builder->CreateStructGEP(list_st, val, 2);
            auto* dat     = builder->CreateLoad(ptr_type(), dat_ptr, "dat");

            auto* i_slot = alloca_at_entry(fn, i64_type(), "prnt_i");
            builder->CreateStore(llvm::ConstantInt::get(i64_type(), 0), i_slot);

            auto* hdr  = llvm::BasicBlock::Create(context, "prnt_hdr",  fn);
            auto* body = llvm::BasicBlock::Create(context, "prnt_body", fn);
            auto* exit = llvm::BasicBlock::Create(context, "prnt_exit", fn);
            builder->CreateBr(hdr);

            builder->SetInsertPoint(hdr);
            auto* ci = builder->CreateLoad(i64_type(), i_slot, "ci");
            builder->CreateCondBr(builder->CreateICmpSLT(ci, len), body, exit);

            builder->SetInsertPoint(body);
            auto* ep  = builder->CreateGEP(i64_type(), dat, {ci}, "ep");
            auto* raw = builder->CreateLoad(i64_type(), ep, "raw");

            TypePtr elem_t = sprig_type->element_type;
            auto* decoded = decode_from_list(raw, elem_t);
            // print separator
            auto* sep_needed = builder->CreateICmpSGT(ci,
                llvm::ConstantInt::get(i64_type(), 0));
            auto* sep_bb  = llvm::BasicBlock::Create(context, "sep_bb",  fn);
            auto* elem_bb = llvm::BasicBlock::Create(context, "elem_bb", fn);
            builder->CreateCondBr(sep_needed, sep_bb, elem_bb);
            builder->SetInsertPoint(sep_bb);
            auto* sep = builder->CreateGlobalStringPtr(", ", ".sep");
            builder->CreateCall(printf_fn->getFunctionType(), printf_fn, {sep});
            builder->CreateBr(elem_bb);
            builder->SetInsertPoint(elem_bb);

            emit_print(decoded, elem_t);

            auto* ni = builder->CreateAdd(ci,
                llvm::ConstantInt::get(i64_type(), 1), "ni");
            builder->CreateStore(ni, i_slot);
            builder->CreateBr(hdr);

            builder->SetInsertPoint(exit);
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

llvm::Value* CodeGen::gen_expr(const Expression* e) {

    // ── Literals ──────────────────────────────────────────────────────────────

    if (auto* n = dynamic_cast<const NumberExpression*>(e))
        return llvm::ConstantFP::get(double_type(), n->value);

    if (auto* s = dynamic_cast<const StringExpression*>(e))
        return builder->CreateGlobalStringPtr(s->value, ".str");

    if (auto* b = dynamic_cast<const BoolExpression*>(e))
        return llvm::ConstantInt::get(bool_type(), b->value ? 1 : 0);

    if (dynamic_cast<const NothingExpression*>(e))
        return llvm::ConstantFP::get(double_type(), 0.0);

    // ── Variable read ─────────────────────────────────────────────────────────

    if (auto* i = dynamic_cast<const IdentExpression*>(e)) {
        auto* slot = get_var(i->name);
        if (!slot)
            throw std::runtime_error("Undefined variable '" + i->name + "'");
        return builder->CreateLoad(slot->getAllocatedType(), slot, i->name);
    }

    // ── Borrow expressions ────────────────────────────────────────────────────

    if (auto* be = dynamic_cast<const BorrowExpression*>(e)) {
        auto* slot = get_var(be->source);
        if (!slot) throw std::runtime_error("Undefined variable '" + be->source + "'");
        return builder->CreateLoad(slot->getAllocatedType(), slot, be->source);
    }
    if (auto* mbe = dynamic_cast<const MutableBorrowExpression*>(e)) {
        auto* slot = get_var(mbe->source);
        if (!slot) throw std::runtime_error("Undefined variable '" + mbe->source + "'");
        return builder->CreateLoad(slot->getAllocatedType(), slot, mbe->source);
    }

    // ── List literal ──────────────────────────────────────────────────────────

    if (auto* le = dynamic_cast<const ListExpression*>(e)) {
        auto* fn       = builder->GetInsertBlock()->getParent();
        int   cap      = std::max(4, (int)le->elements.size());
        auto* list_ptr = list_new(cap);
        for (auto& el : le->elements) {
            auto* val = gen_expr(el.get());
            list_append(fn, list_ptr, encode_for_list(val));
        }
        return list_ptr;
    }

    // ── Index access ──────────────────────────────────────────────────────────

    if (auto* ie = dynamic_cast<const IndexExpression*>(e)) {
        auto* lst  = gen_expr(ie->object.get());
        auto* idx  = gen_expr(ie->index.get());
        auto* raw  = list_get(lst, idx);
        TypePtr lt = expr_type(ie->object.get());
        TypePtr et = (lt && lt->kind == Type::Kind::List) ? lt->element_type : nullptr;
        return decode_from_list(raw, et);
    }

    // ── Shape instantiation ───────────────────────────────────────────────────

    if (auto* si = dynamic_cast<const ShapeInstanceExpression*>(e)) {
        auto* st       = get_shape_llvm_type(si->shape_name);
        auto* malloc_fn = module->getFunction("malloc");
        auto  sz       = module->getDataLayout().getTypeAllocSize(st);
        auto* ptr      = builder->CreateCall(
            malloc_fn->getFunctionType(), malloc_fn,
            {llvm::ConstantInt::get(i64_type(), sz)}, "shape_ptr");

        // Build a name→value map for the field initializers
        std::unordered_map<std::string, llvm::Value*> field_vals;
        for (auto& [fname, fexpr] : si->fields)
            field_vals[fname] = gen_expr(fexpr.get());

        auto sit = shape_types->find(si->shape_name);
        if (sit != shape_types->end()) {
            for (int i = 0; i < (int)sit->second.size(); i++) {
                auto& [fname, ftype] = sit->second[i];
                auto* fptr = builder->CreateStructGEP(st, ptr, i, fname);
                llvm::Value* val = nullptr;
                auto vit = field_vals.find(fname);
                if (vit != field_vals.end())
                    val = vit->second;
                else
                    val = llvm::Constant::getNullValue(sprig_to_llvm(ftype));
                builder->CreateStore(coerce(val, sprig_to_llvm(ftype)), fptr);
            }
        }
        return ptr;
    }

    // ── Field access ──────────────────────────────────────────────────────────

    if (auto* fa = dynamic_cast<const FieldAccessExpression*>(e)) {
        auto* obj_val = gen_expr(fa->object.get());
        TypePtr obj_t = expr_type(fa->object.get());
        if (!obj_t || obj_t->kind != Type::Kind::Shape)
            throw std::runtime_error("Field access on non-shape");
        auto* st  = get_shape_llvm_type(obj_t->shape_name);
        int   idx = shape_field_index(obj_t->shape_name, fa->field);
        if (idx < 0)
            throw std::runtime_error("No field '" + fa->field + "'");
        auto* fptr = builder->CreateStructGEP(st, obj_val, idx, fa->field);
        // Determine field LLVM type
        auto sit = shape_types->find(obj_t->shape_name);
        llvm::Type* ft = double_type();
        if (sit != shape_types->end() && idx < (int)sit->second.size())
            ft = sprig_to_llvm(sit->second[idx].second);
        return builder->CreateLoad(ft, fptr, fa->field);
    }

    // ── Binary expressions ────────────────────────────────────────────────────

    if (auto* bin = dynamic_cast<const BinaryExpression*>(e)) {

        if (bin->op == "and") {
            auto* fn    = builder->GetInsertBlock()->getParent();
            auto* rhs_b = llvm::BasicBlock::Create(context, "and_rhs",   fn);
            auto* end_b = llvm::BasicBlock::Create(context, "and_merge", fn);
            auto* lhs   = to_bool(gen_expr(bin->left.get()));
            auto* lhs_b = builder->GetInsertBlock();
            builder->CreateCondBr(lhs, rhs_b, end_b);
            builder->SetInsertPoint(rhs_b);
            auto* rhs    = to_bool(gen_expr(bin->right.get()));
            auto* rhs_b2 = builder->GetInsertBlock();
            builder->CreateBr(end_b);
            builder->SetInsertPoint(end_b);
            auto* phi = builder->CreatePHI(bool_type(), 2, "and");
            phi->addIncoming(llvm::ConstantInt::get(bool_type(), 0), lhs_b);
            phi->addIncoming(rhs, rhs_b2);
            return phi;
        }
        if (bin->op == "or") {
            auto* fn    = builder->GetInsertBlock()->getParent();
            auto* rhs_b = llvm::BasicBlock::Create(context, "or_rhs",   fn);
            auto* end_b = llvm::BasicBlock::Create(context, "or_merge", fn);
            auto* lhs   = to_bool(gen_expr(bin->left.get()));
            auto* lhs_b = builder->GetInsertBlock();
            builder->CreateCondBr(lhs, end_b, rhs_b);
            builder->SetInsertPoint(rhs_b);
            auto* rhs    = to_bool(gen_expr(bin->right.get()));
            auto* rhs_b2 = builder->GetInsertBlock();
            builder->CreateBr(end_b);
            builder->SetInsertPoint(end_b);
            auto* phi = builder->CreatePHI(bool_type(), 2, "or");
            phi->addIncoming(llvm::ConstantInt::get(bool_type(), 1), lhs_b);
            phi->addIncoming(rhs, rhs_b2);
            return phi;
        }

        auto* lhs = gen_expr(bin->left.get());
        auto* rhs = gen_expr(bin->right.get());

        // '+' overloaded: numeric add or string concat
        if (bin->op == "+") {
            bool lhs_str = lhs->getType() == ptr_type();
            bool rhs_str = rhs->getType() == ptr_type();
            if (lhs_str || rhs_str) {
                auto* fn         = builder->GetInsertBlock()->getParent();
                auto* arr_type   = llvm::ArrayType::get(i8_type(), 1024);
                auto* buf        = alloca_at_entry(fn, arr_type, "concat_buf");
                auto* sprintf_fn = module->getFunction("sprintf");

                auto num_to_str = [&](llvm::Value* v,
                                      const char* tmp_name) -> llvm::Value* {
                    auto* tmp = alloca_at_entry(fn,
                        llvm::ArrayType::get(i8_type(), 64), tmp_name);
                    auto* fmt = builder->CreateGlobalStringPtr("%g", ".nfmt");
                    builder->CreateCall(sprintf_fn->getFunctionType(), sprintf_fn,
                                       {tmp, fmt, v});
                    return tmp;
                };

                llvm::Value* ls = lhs_str ? lhs : num_to_str(lhs, "lhs_tmp");
                llvm::Value* rs = rhs_str ? rhs : num_to_str(rhs, "rhs_tmp");
                auto* fmt = builder->CreateGlobalStringPtr("%s%s", ".cat_fmt");
                builder->CreateCall(sprintf_fn->getFunctionType(), sprintf_fn,
                                   {buf, fmt, ls, rs});
                return buf;
            }
            return builder->CreateFAdd(lhs, rhs, "add");
        }

        if (bin->op == "-")  return builder->CreateFSub(lhs, rhs, "sub");
        if (bin->op == "*")  return builder->CreateFMul(lhs, rhs, "mul");
        if (bin->op == "/")  return builder->CreateFDiv(lhs, rhs, "div");
        if (bin->op == ">")  return builder->CreateFCmpOGT(lhs, rhs, "gt");
        if (bin->op == "<")  return builder->CreateFCmpOLT(lhs, rhs, "lt");
        if (bin->op == "==" || bin->op == "is") {
            if (lhs->getType() == bool_type())
                return builder->CreateICmpEQ(lhs, rhs, "eq");
            return builder->CreateFCmpOEQ(lhs, rhs, "eq");
        }
        if (bin->op == "!=") {
            if (lhs->getType() == bool_type())
                return builder->CreateICmpNE(lhs, rhs, "ne");
            return builder->CreateFCmpONE(lhs, rhs, "ne");
        }
        throw std::runtime_error("Unknown binary operator: " + bin->op);
    }

    // ── Unary ─────────────────────────────────────────────────────────────────

    if (auto* u = dynamic_cast<const UnaryExpression*>(e)) {
        auto* val = gen_expr(u->operand.get());
        if (u->op == "not") return builder->CreateNot(to_bool(val), "not");
        throw std::runtime_error("Unknown unary operator: " + u->op);
    }

    // ── Function calls ────────────────────────────────────────────────────────

    if (auto* c = dynamic_cast<const CallExpression*>(e)) {

        if (c->callee == "print") {
            for (auto& arg : c->args) {
                auto* val = gen_expr(arg.get());
                emit_print(val, expr_type(arg.get()));
            }
            return llvm::ConstantFP::get(double_type(), 0.0);
        }

        if (c->callee == "to_number") {
            auto* val = gen_expr(c->args[0].get());
            if (val->getType() == double_type()) return val;
            if (val->getType() == bool_type())
                return builder->CreateUIToFP(val, double_type(), "b_to_f");
            auto* atof_fn = module->getFunction("atof");
            return builder->CreateCall(atof_fn->getFunctionType(), atof_fn,
                                       {val}, "to_num");
        }

        if (c->callee == "to_text") {
            auto* val = gen_expr(c->args[0].get());
            if (val->getType() == ptr_type()) return val;
            auto* fn       = builder->GetInsertBlock()->getParent();
            auto* arr_type = llvm::ArrayType::get(i8_type(), 64);
            auto* buf        = alloca_at_entry(fn, arr_type, "txt_buf");
            auto* sprintf_fn = module->getFunction("sprintf");
            if (val->getType() == bool_type()) {
                auto* t = builder->CreateGlobalStringPtr("true",  ".true_s");
                auto* f = builder->CreateGlobalStringPtr("false", ".false_s");
                return builder->CreateSelect(val, t, f, "bool_s");
            }
            auto* fmt = builder->CreateGlobalStringPtr("%g", ".tfmt");
            builder->CreateCall(sprintf_fn->getFunctionType(), sprintf_fn,
                               {buf, fmt, val});
            return buf;
        }

        if (c->callee == "length") {
            auto* val = gen_expr(c->args[0].get());
            TypePtr t = expr_type(c->args[0].get());
            if (t && t->kind == Type::Kind::Text) {
                // strlen for text
                auto* strlen_fn = module->getFunction("strlen");
                auto* len_i64   = builder->CreateCall(
                    strlen_fn->getFunctionType(), strlen_fn, {val}, "slen");
                return builder->CreateSIToFP(len_i64, double_type(), "slen_f");
            }
            // list length
            return list_length(val);
        }

        if (c->callee == "append") {
            auto* fn       = builder->GetInsertBlock()->getParent();
            auto* list_ptr = gen_expr(c->args[0].get());
            auto* elem_val = gen_expr(c->args[1].get());
            list_append(fn, list_ptr, encode_for_list(elem_val));
            return llvm::ConstantFP::get(double_type(), 0.0);
        }

        if (c->callee == "first") {
            auto* list_ptr = gen_expr(c->args[0].get());
            TypePtr lt = expr_type(c->args[0].get());
            TypePtr et = (lt && lt->kind == Type::Kind::List) ? lt->element_type : nullptr;
            auto* raw  = list_get(list_ptr,
                llvm::ConstantFP::get(double_type(), 0.0));
            return decode_from_list(raw, et);
        }

        if (c->callee == "last") {
            auto* list_ptr = gen_expr(c->args[0].get());
            TypePtr lt  = expr_type(c->args[0].get());
            TypePtr et  = (lt && lt->kind == Type::Kind::List) ? lt->element_type : nullptr;
            auto* len_f    = list_length(list_ptr);
            auto* one      = llvm::ConstantFP::get(double_type(), 1.0);
            auto* last_idx = builder->CreateFSub(len_f, one, "last_idx");
            auto* raw      = list_get(list_ptr, last_idx);
            return decode_from_list(raw, et);
        }

        if (c->callee == "input") {
            auto* fn       = builder->GetInsertBlock()->getParent();
            auto* buf_type = llvm::ArrayType::get(i8_type(), 1024);
            auto* buf      = alloca_at_entry(fn, buf_type, "input_buf");
            auto* fgets_fn = module->getFunction("fgets");
            // stdin — declare as external global if needed
            auto* stdin_g  = module->getOrInsertGlobal("stdin", ptr_type());
            auto* stdin_v  = builder->CreateLoad(ptr_type(), stdin_g, "stdin");
            builder->CreateCall(fgets_fn->getFunctionType(), fgets_fn,
                {buf, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 1024),
                 stdin_v});
            // strip trailing newline: find '\n' and replace with '\0'
            auto* strlen_fn = module->getFunction("strlen");
            auto* slen      = builder->CreateCall(
                strlen_fn->getFunctionType(), strlen_fn, {buf}, "slen");
            auto* has_nl    = builder->CreateICmpSGT(slen,
                llvm::ConstantInt::get(i64_type(), 0), "has_nl");
            auto* nl_bb  = llvm::BasicBlock::Create(context, "nl_strip", fn);
            auto* end_bb = llvm::BasicBlock::Create(context, "nl_done",  fn);
            builder->CreateCondBr(has_nl, nl_bb, end_bb);
            builder->SetInsertPoint(nl_bb);
            auto* last_pos = builder->CreateSub(slen,
                llvm::ConstantInt::get(i64_type(), 1), "last_pos");
            auto* last_ptr = builder->CreateGEP(i8_type(), buf, {last_pos}, "last_ptr");
            auto* nl_char  = builder->CreateLoad(i8_type(), last_ptr, "nl_char");
            auto* is_nl    = builder->CreateICmpEQ(nl_char,
                llvm::ConstantInt::get(i8_type(), '\n'), "is_nl");
            auto* strip_bb = llvm::BasicBlock::Create(context, "strip", fn);
            auto* no_strip = llvm::BasicBlock::Create(context, "no_strip", fn);
            builder->CreateCondBr(is_nl, strip_bb, no_strip);
            builder->SetInsertPoint(strip_bb);
            builder->CreateStore(llvm::ConstantInt::get(i8_type(), 0), last_ptr);
            builder->CreateBr(no_strip);
            builder->SetInsertPoint(no_strip);
            builder->CreateBr(end_bb);
            builder->SetInsertPoint(end_bb);
            return buf;
        }

        // User-defined function
        auto it = functions.find(c->callee);
        if (it == functions.end())
            throw std::runtime_error("Undefined function: " + c->callee);
        std::vector<llvm::Value*> args;
        for (auto& arg : c->args) {
            auto* v = gen_expr(arg.get());
            args.push_back(coerce(v, double_type()));
        }
        return builder->CreateCall(it->second->getFunctionType(),
                                   it->second, args, c->callee + "_ret");
    }

    throw std::runtime_error("Expression type not supported in compiled mode");
}

// ── Statement codegen ─────────────────────────────────────────────────────────

void CodeGen::gen_stmt(const Statement* s) {

    // include / shape definition — already processed; skip
    if (dynamic_cast<const IncludeStatement*>(s))        return;
    if (dynamic_cast<const ShapeDefinitionStatement*>(s)) return;

    // let [mutable] x = expr
    if (auto* vs = dynamic_cast<const VariableStatement*>(s)) {
        auto* val       = gen_expr(vs->value.get());
        TypePtr st      = expr_type(vs->value.get());
        llvm::Type* llt = st ? sprig_to_llvm(st) : val->getType();
        auto* existing  = get_var(vs->name);
        if (existing) {
            builder->CreateStore(coerce(val, existing->getAllocatedType()), existing);
        } else {
            auto* fn   = builder->GetInsertBlock()->getParent();
            auto* slot = alloca_at_entry(fn, llt, vs->name);
            builder->CreateStore(coerce(val, llt), slot);
            set_var(vs->name, slot, st);
        }
        return;
    }

    // let x borrow [mutable] y — alias source slot
    if (auto* bs = dynamic_cast<const BorrowStatement*>(s)) {
        auto* slot = get_var(bs->source);
        if (slot) set_var(bs->target, slot, get_var_type(bs->source));
        return;
    }
    if (auto* mbs = dynamic_cast<const MutableBorrowStatement*>(s)) {
        auto* slot = get_var(mbs->source);
        if (slot) set_var(mbs->target, slot, get_var_type(mbs->source));
        return;
    }

    // give back expr
    if (auto* rs = dynamic_cast<const ReturnStatement*>(s)) {
        auto* val = gen_expr(rs->value.get());
        if (return_val_slot)
            builder->CreateStore(
                coerce(val, return_val_slot->getAllocatedType()), return_val_slot);
        if (return_block) builder->CreateBr(return_block);
        auto* fn   = builder->GetInsertBlock()->getParent();
        auto* dead = llvm::BasicBlock::Create(context, "dead", fn);
        builder->SetInsertPoint(dead);
        return;
    }

    // when cond: ... [otherwise: ...]
    if (auto* is = dynamic_cast<const IfStatement*>(s)) {
        auto* fn    = builder->GetInsertBlock()->getParent();
        auto* then  = llvm::BasicBlock::Create(context, "then",  fn);
        auto* merge = llvm::BasicBlock::Create(context, "merge", fn);
        auto* else_ = is->else_block
                        ? llvm::BasicBlock::Create(context, "else", fn)
                        : merge;

        builder->CreateCondBr(to_bool(gen_expr(is->condition.get())), then, else_);

        builder->SetInsertPoint(then);
        gen_block(is->then_block);
        if (!builder->GetInsertBlock()->getTerminator())
            builder->CreateBr(merge);

        if (is->else_block) {
            builder->SetInsertPoint(else_);
            gen_block(*is->else_block);
            if (!builder->GetInsertBlock()->getTerminator())
                builder->CreateBr(merge);
        }

        builder->SetInsertPoint(merge);
        return;
    }

    // as long as cond: body
    if (auto* ws = dynamic_cast<const WhileStatement*>(s)) {
        auto* fn     = builder->GetInsertBlock()->getParent();
        auto* header = llvm::BasicBlock::Create(context, "while_hdr",  fn);
        auto* body   = llvm::BasicBlock::Create(context, "while_body", fn);
        auto* exit   = llvm::BasicBlock::Create(context, "while_exit", fn);

        builder->CreateBr(header);
        builder->SetInsertPoint(header);
        builder->CreateCondBr(to_bool(gen_expr(ws->condition.get())), body, exit);

        builder->SetInsertPoint(body);
        loop_exit_blocks.push_back(exit);
        loop_header_blocks.push_back(header);
        gen_block(ws->body);
        loop_exit_blocks.pop_back();
        loop_header_blocks.pop_back();
        if (!builder->GetInsertBlock()->getTerminator())
            builder->CreateBr(header);

        builder->SetInsertPoint(exit);
        return;
    }

    // for each x in list: body
    if (auto* fe = dynamic_cast<const ForEachStatement*>(s)) {
        auto* fn       = builder->GetInsertBlock()->getParent();
        auto* list_ptr = gen_expr(fe->iterable.get());
        TypePtr iter_t = expr_type(fe->iterable.get());
        TypePtr elem_t = (iter_t && iter_t->kind == Type::Kind::List)
                             ? iter_t->element_type : nullptr;

        // index counter slot
        auto* i_slot = alloca_at_entry(fn, i64_type(), "fe_i");
        builder->CreateStore(llvm::ConstantInt::get(i64_type(), 0), i_slot);

        auto* hdr  = llvm::BasicBlock::Create(context, "fe_hdr",  fn);
        auto* body = llvm::BasicBlock::Create(context, "fe_body", fn);
        auto* incr = llvm::BasicBlock::Create(context, "fe_incr", fn);
        auto* exit = llvm::BasicBlock::Create(context, "fe_exit", fn);

        builder->CreateBr(hdr);
        builder->SetInsertPoint(hdr);
        auto* len_i64  = builder->CreateFPToSI(list_length(list_ptr), i64_type(), "len_i64");
        auto* ci       = builder->CreateLoad(i64_type(), i_slot, "ci");
        builder->CreateCondBr(builder->CreateICmpSLT(ci, len_i64), body, exit);

        builder->SetInsertPoint(body);
        push_scope();

        // Get element value and bind loop variable
        auto* raw     = list_get(list_ptr,
            builder->CreateSIToFP(builder->CreateLoad(i64_type(), i_slot), double_type()));
        auto* elem_v  = decode_from_list(raw, elem_t);
        auto* elem_slot = alloca_at_entry(fn, elem_v->getType(), fe->variable);
        builder->CreateStore(elem_v, elem_slot);
        set_var(fe->variable, elem_slot, elem_t);

        // skip → increment block (not header), so the index advances before re-checking
        loop_exit_blocks.push_back(exit);
        loop_header_blocks.push_back(incr);
        gen_block(fe->body);
        loop_exit_blocks.pop_back();
        loop_header_blocks.pop_back();

        pop_scope();

        if (!builder->GetInsertBlock()->getTerminator())
            builder->CreateBr(incr);

        // Increment block — reached by normal fall-through and by skip
        builder->SetInsertPoint(incr);
        auto* cur_i  = builder->CreateLoad(i64_type(), i_slot, "cur_i");
        auto* next_i = builder->CreateAdd(cur_i,
            llvm::ConstantInt::get(i64_type(), 1), "next_i");
        builder->CreateStore(next_i, i_slot);
        builder->CreateBr(hdr);

        builder->SetInsertPoint(exit);
        return;
    }

    // sam.field = value
    if (auto* fa = dynamic_cast<const FieldAssignStatement*>(s)) {
        auto* obj_slot = get_var(fa->variable);
        if (!obj_slot)
            throw std::runtime_error("Undefined variable '" + fa->variable + "'");
        auto* obj_ptr  = builder->CreateLoad(ptr_type(), obj_slot, fa->variable);
        TypePtr obj_t  = get_var_type(fa->variable);
        if (!obj_t || obj_t->kind != Type::Kind::Shape)
            throw std::runtime_error("Field assign on non-shape");
        auto* st  = get_shape_llvm_type(obj_t->shape_name);
        int   idx = shape_field_index(obj_t->shape_name, fa->field);
        if (idx < 0)
            throw std::runtime_error("No field '" + fa->field + "'");
        auto* fptr  = builder->CreateStructGEP(st, obj_ptr, idx, fa->field);
        auto* val   = gen_expr(fa->value.get());
        // find field llvm type
        auto sit = shape_types->find(obj_t->shape_name);
        llvm::Type* ft = double_type();
        if (sit != shape_types->end() && idx < (int)sit->second.size())
            ft = sprig_to_llvm(sit->second[idx].second);
        builder->CreateStore(coerce(val, ft), fptr);
        return;
    }

    // stop
    if (dynamic_cast<const StopStatement*>(s)) {
        if (!loop_exit_blocks.empty())
            builder->CreateBr(loop_exit_blocks.back());
        auto* fn   = builder->GetInsertBlock()->getParent();
        auto* dead = llvm::BasicBlock::Create(context, "dead", fn);
        builder->SetInsertPoint(dead);
        return;
    }

    // skip
    if (dynamic_cast<const SkipStatement*>(s)) {
        if (!loop_header_blocks.empty())
            builder->CreateBr(loop_header_blocks.back());
        auto* fn   = builder->GetInsertBlock()->getParent();
        auto* dead = llvm::BasicBlock::Create(context, "dead", fn);
        builder->SetInsertPoint(dead);
        return;
    }

    // Standalone expression
    if (auto* es = dynamic_cast<const ExpressionStatement*>(s)) {
        gen_expr(es->expr.get());
        return;
    }
}

void CodeGen::gen_block(const Block& b) {
    push_scope();
    for (auto& stmt : b.stmts)
        gen_stmt(stmt.get());
    pop_scope();
}

#include "codegen.hpp"

#include <stdexcept>
#include <system_error>

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

// ── Type helpers ──────────────────────────────────────────────────────────────

llvm::Type* CodeGen::double_type() { return llvm::Type::getDoubleTy(context); }
llvm::Type* CodeGen::bool_type()   { return llvm::Type::getInt1Ty(context); }
llvm::Type* CodeGen::ptr_type()    { return llvm::PointerType::get(context, 0); }
llvm::Type* CodeGen::void_type()   { return llvm::Type::getVoidTy(context); }

// ── Entry point ───────────────────────────────────────────────────────────────

void CodeGen::compile(const Program& program, const std::string& output_path) {
    module  = std::make_unique<llvm::Module>("sprig", context);
    builder = std::make_unique<llvm::IRBuilder<>>(context);

    declare_runtime();

    // ── First pass: forward-declare all user functions ────────────────────────
    // Allows mutual recursion and forward calls.
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

            // Allocate a stack slot for each parameter and store the arg value.
            // alloca+store/load lets mem2reg promote these to SSA registers.
            size_t i = 0;
            for (auto& arg : llvm_fn->args()) {
                auto* slot = alloca_at_entry(llvm_fn, double_type(),
                                             std::string(arg.getName()));
                builder->CreateStore(&arg, slot);
                set_var(std::string(arg.getName()), slot);
                i++;
            }

            // Single return block + slot — avoids duplicate terminators when
            // multiple 'give back' statements exist in a function.
            return_block    = llvm::BasicBlock::Create(context, "return", llvm_fn);
            return_val_slot = alloca_at_entry(llvm_fn, double_type(), "retval");
            builder->CreateStore(llvm::ConstantFP::get(double_type(), 0.0),
                                 return_val_slot);

            gen_block(fn->body);

            if (!builder->GetInsertBlock()->getTerminator())
                builder->CreateBr(return_block);

            builder->SetInsertPoint(return_block);
            auto* ret = builder->CreateLoad(double_type(), return_val_slot, "ret");
            builder->CreateRet(ret);

            pop_scope();
            return_block    = nullptr;
            return_val_slot = nullptr;
        }
    }

    // ── Create main() — wraps all top-level statements ────────────────────────
    auto* main_type = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(context), {}, false);
    auto* main_fn = llvm::Function::Create(
        main_type, llvm::Function::ExternalLinkage, "main", module.get());
    auto* main_entry = llvm::BasicBlock::Create(context, "entry", main_fn);
    builder->SetInsertPoint(main_entry);
    push_scope();

    for (auto& stmt : program.stmts)
        if (!dynamic_cast<const FunctionStatement*>(stmt.get()))
            gen_stmt(stmt.get());

    pop_scope();
    if (!builder->GetInsertBlock()->getTerminator())
        builder->CreateRet(llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(context), 0));

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
    auto* printf_type = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(context), {ptr_type()}, /*variadic=*/true);
    get_or_declare("printf", printf_type);

    // sprintf(ptr buf, ptr fmt, ...) -> i32
    auto* sprintf_type = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(context), {ptr_type(), ptr_type()}, true);
    get_or_declare("sprintf", sprintf_type);

    // atof(ptr str) -> double
    auto* atof_type = llvm::FunctionType::get(double_type(), {ptr_type()}, false);
    get_or_declare("atof", atof_type);
}

llvm::Function* CodeGen::get_or_declare(const std::string& name,
                                         llvm::FunctionType* type) {
    if (auto* f = module->getFunction(name)) return f;
    return llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                  name, module.get());
}

// ── Variable scope ────────────────────────────────────────────────────────────

void CodeGen::push_scope() { var_scopes.push_back({}); }
void CodeGen::pop_scope()  { var_scopes.pop_back(); }

// All allocas go in the function entry block — standard LLVM idiom that lets
// mem2reg promote alloca+load/store pairs to SSA registers in optimised builds.
llvm::AllocaInst* CodeGen::alloca_at_entry(llvm::Function* fn,
                                            llvm::Type* type,
                                            const std::string& name) {
    llvm::IRBuilder<> tmp(&fn->getEntryBlock(),
                          fn->getEntryBlock().begin());
    return tmp.CreateAlloca(type, nullptr, name);
}

void CodeGen::set_var(const std::string& name, llvm::AllocaInst* slot) {
    var_scopes.back()[name] = slot;
}

llvm::AllocaInst* CodeGen::get_var(const std::string& name) {
    for (int i = (int)var_scopes.size() - 1; i >= 0; i--) {
        auto it = var_scopes[i].find(name);
        if (it != var_scopes[i].end()) return it->second;
    }
    return nullptr;
}

// ── Type coercion ────────────────────────────────────────────────────────────

// Convert any value to i1 for use as a branch condition.
// User-defined functions all return double, so boolean results need coercion.
llvm::Value* CodeGen::to_bool(llvm::Value* val) {
    if (val->getType() == bool_type()) return val;
    if (val->getType() == double_type())
        return builder->CreateFCmpONE(val,
            llvm::ConstantFP::get(double_type(), 0.0), "to_bool");
    return val;
}

// Coerce val to match target type before a store.
// Needed because all user functions return double, but boolean expressions
// inside them produce i1 which must be widened before storing into the slot.
llvm::Value* CodeGen::coerce(llvm::Value* val, llvm::Type* t) {
    if (val->getType() == t) return val;
    if (t == double_type() && val->getType() == bool_type())
        return builder->CreateUIToFP(val, double_type(), "bool_to_f64");
    if (t == bool_type() && val->getType() == double_type())
        return builder->CreateFCmpONE(val,
            llvm::ConstantFP::get(double_type(), 0.0), "f64_to_bool");
    return val;
}

// ── Print helper ──────────────────────────────────────────────────────────────

void CodeGen::emit_print(llvm::Value* val) {
    auto* printf_fn = module->getFunction("printf");
    auto* type      = val->getType();

    if (type == double_type()) {
        auto* fmt = builder->CreateGlobalStringPtr("%g\n", ".fmt_num");
        builder->CreateCall(printf_fn->getFunctionType(), printf_fn, {fmt, val});
    } else if (type == bool_type()) {
        auto* t = builder->CreateGlobalStringPtr("true\n",  ".true");
        auto* f = builder->CreateGlobalStringPtr("false\n", ".false");
        auto* s = builder->CreateSelect(val, t, f, "bool_str");
        builder->CreateCall(printf_fn->getFunctionType(), printf_fn, {s});
    } else {
        // ptr — treat as string
        auto* fmt = builder->CreateGlobalStringPtr("%s\n", ".fmt_str");
        builder->CreateCall(printf_fn->getFunctionType(), printf_fn, {fmt, val});
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

    // ── Borrow expressions — read through the alias ───────────────────────────

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

    // ── Binary expressions ────────────────────────────────────────────────────

    if (auto* bin = dynamic_cast<const BinaryExpression*>(e)) {

        // Short-circuit operators use branches + phi nodes
        if (bin->op == "and") {
            auto* fn    = builder->GetInsertBlock()->getParent();
            auto* rhs_b = llvm::BasicBlock::Create(context, "and_rhs",   fn);
            auto* end_b = llvm::BasicBlock::Create(context, "and_merge", fn);
            auto* lhs   = gen_expr(bin->left.get());
            auto* lhs_b = builder->GetInsertBlock();
            builder->CreateCondBr(lhs, rhs_b, end_b);
            builder->SetInsertPoint(rhs_b);
            auto* rhs    = gen_expr(bin->right.get());
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
            auto* lhs   = gen_expr(bin->left.get());
            auto* lhs_b = builder->GetInsertBlock();
            builder->CreateCondBr(lhs, end_b, rhs_b);
            builder->SetInsertPoint(rhs_b);
            auto* rhs    = gen_expr(bin->right.get());
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

        // '+' is overloaded: number addition or string concatenation
        if (bin->op == "+") {
            bool lhs_str = lhs->getType() == ptr_type();
            bool rhs_str = rhs->getType() == ptr_type();
            if (lhs_str || rhs_str) {
                auto* fn       = builder->GetInsertBlock()->getParent();
                auto* arr_type = llvm::ArrayType::get(
                    llvm::Type::getInt8Ty(context), 1024);
                auto* buf        = alloca_at_entry(fn, arr_type, "concat_buf");
                auto* sprintf_fn = module->getFunction("sprintf");

                auto num_to_str = [&](llvm::Value* v, const char* tmp_name) -> llvm::Value* {
                    auto* tmp_arr = alloca_at_entry(fn,
                        llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 64),
                        tmp_name);
                    auto* fmt = builder->CreateGlobalStringPtr("%g", ".nfmt");
                    builder->CreateCall(sprintf_fn->getFunctionType(), sprintf_fn,
                                       {tmp_arr, fmt, v});
                    return tmp_arr;
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
        if (u->op == "not") return builder->CreateNot(val, "not");
        throw std::runtime_error("Unknown unary operator: " + u->op);
    }

    // ── Function calls ────────────────────────────────────────────────────────

    if (auto* c = dynamic_cast<const CallExpression*>(e)) {

        if (c->callee == "print") {
            for (auto& arg : c->args)
                emit_print(gen_expr(arg.get()));
            return llvm::ConstantFP::get(double_type(), 0.0);
        }

        if (c->callee == "to_number") {
            auto* val = gen_expr(c->args[0].get());
            if (val->getType() == double_type()) return val;
            auto* atof_fn = module->getFunction("atof");
            return builder->CreateCall(atof_fn->getFunctionType(), atof_fn,
                                       {val}, "to_num");
        }

        if (c->callee == "to_text") {
            auto* val = gen_expr(c->args[0].get());
            if (val->getType() == ptr_type()) return val;
            auto* fn       = builder->GetInsertBlock()->getParent();
            auto* arr_type = llvm::ArrayType::get(
                llvm::Type::getInt8Ty(context), 64);
            auto* buf        = alloca_at_entry(fn, arr_type, "txt_buf");
            auto* sprintf_fn = module->getFunction("sprintf");
            auto* fmt        = builder->CreateGlobalStringPtr("%g", ".tfmt");
            builder->CreateCall(sprintf_fn->getFunctionType(), sprintf_fn,
                               {buf, fmt, val});
            return buf;
        }

        if (c->callee == "length" || c->callee == "append" ||
            c->callee == "first"  || c->callee == "last"   ||
            c->callee == "input")
            throw std::runtime_error(
                "Built-in '" + c->callee + "' is not yet supported in "
                "compiled mode. Use the interpreter for programs that "
                "use lists or input.");

        // User-defined function
        auto it = functions.find(c->callee);
        if (it == functions.end())
            throw std::runtime_error("Undefined function: " + c->callee);
        std::vector<llvm::Value*> args;
        for (auto& arg : c->args) args.push_back(gen_expr(arg.get()));
        return builder->CreateCall(it->second->getFunctionType(),
                                   it->second, args, c->callee + "_ret");
    }

    throw std::runtime_error("Expression type not yet supported in compiled mode");
}

// ── Statement codegen ─────────────────────────────────────────────────────────

void CodeGen::gen_stmt(const Statement* s) {

    // let [mutable] x = expr
    // If x already exists in any scope, update the existing slot.
    // Otherwise allocate a new one — mirrors interpreter declare() semantics.
    if (auto* vs = dynamic_cast<const VariableStatement*>(s)) {
        auto* val      = gen_expr(vs->value.get());
        auto* existing = get_var(vs->name);
        if (existing) {
            builder->CreateStore(val, existing);
        } else {
            auto* fn   = builder->GetInsertBlock()->getParent();
            auto* slot = alloca_at_entry(fn, val->getType(), vs->name);
            builder->CreateStore(val, slot);
            set_var(vs->name, slot);
        }
        return;
    }

    // let x borrow [mutable] y — alias the source slot directly
    if (auto* bs = dynamic_cast<const BorrowStatement*>(s)) {
        auto* slot = get_var(bs->source);
        if (slot) set_var(bs->target, slot);
        return;
    }
    if (auto* mbs = dynamic_cast<const MutableBorrowStatement*>(s)) {
        auto* slot = get_var(mbs->source);
        if (slot) set_var(mbs->target, slot);
        return;
    }

    // give back expr
    if (auto* rs = dynamic_cast<const ReturnStatement*>(s)) {
        auto* val = gen_expr(rs->value.get());
        if (return_val_slot)
            builder->CreateStore(
                coerce(val, return_val_slot->getAllocatedType()), return_val_slot);
        if (return_block)    builder->CreateBr(return_block);
        // Dead block absorbs IR the builder may emit after an early return
        // (e.g. unreachable statements following 'give back')
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

    // stop — branch to innermost loop exit
    if (dynamic_cast<const StopStatement*>(s)) {
        if (!loop_exit_blocks.empty())
            builder->CreateBr(loop_exit_blocks.back());
        auto* fn   = builder->GetInsertBlock()->getParent();
        auto* dead = llvm::BasicBlock::Create(context, "dead", fn);
        builder->SetInsertPoint(dead);
        return;
    }

    // skip — branch back to innermost loop header
    if (dynamic_cast<const SkipStatement*>(s)) {
        if (!loop_header_blocks.empty())
            builder->CreateBr(loop_header_blocks.back());
        auto* fn   = builder->GetInsertBlock()->getParent();
        auto* dead = llvm::BasicBlock::Create(context, "dead", fn);
        builder->SetInsertPoint(dead);
        return;
    }

    // Standalone expression (e.g. a print() call)
    if (auto* es = dynamic_cast<const ExpressionStatement*>(s)) {
        gen_expr(es->expr.get());
        return;
    }

    // for each — not yet in compiler
    if (dynamic_cast<const ForEachStatement*>(s))
        throw std::runtime_error(
            "'for each' is not yet supported in compiled mode. "
            "Use 'as long as' with an index variable instead.");

    // FunctionStatement handled in compile() first/second pass — skip here
}

void CodeGen::gen_block(const Block& b) {
    push_scope();
    for (auto& stmt : b.stmts)
        gen_stmt(stmt.get());
    pop_scope();
}

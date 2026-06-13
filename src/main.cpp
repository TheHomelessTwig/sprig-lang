#include "lexer.hpp"
#include "parser.hpp"
#include <iomanip>
#include <iostream>

// Prints the AST with indentation so you can see the tree structure
void print_expr(const Expression *e, int depth = 0) {
  std::string pad(depth * 2, ' ');
  if (auto *n = dynamic_cast<const NumberExpression *>(e))
    std::cout << pad << "Number(" << n->value << ")\n";
  else if (auto *s = dynamic_cast<const StringExpression *>(e))
    std::cout << pad << "String(\"" << s->value << "\")\n";
  else if (auto *b = dynamic_cast<const BoolExpression *>(e))
    std::cout << pad << "Bool(" << (b->value ? "true" : "false") << ")\n";
  else if (auto *i = dynamic_cast<const IdentExpression *>(e))
    std::cout << pad << "Ident(" << i->name << ")\n";
  else if (auto *bin = dynamic_cast<const BinaryExpression *>(e)) {
    std::cout << pad << "Binary(" << bin->op << ")\n";
    print_expr(bin->left.get(), depth + 1);
    print_expr(bin->right.get(), depth + 1);
  } else if (auto *c = dynamic_cast<const CallExpression *>(e)) {
    std::cout << pad << "Call(" << c->callee << ")\n";
    for (auto &arg : c->args)
      print_expr(arg.get(), depth + 1);
  }
}

void print_block(const Block &b, int depth);

void print_stmt(const Statement *s, int depth = 0) {
  std::string pad(depth * 2, ' ');
  if (auto *l = dynamic_cast<const VariableStatement *>(s)) {
    std::cout << pad << "Variable(" << l->name << ")\n";
    print_expr(l->value.get(), depth + 1);
  } else if (auto *r = dynamic_cast<const ReturnStatement *>(s)) {
    std::cout << pad << "Return\n";
    print_expr(r->value.get(), depth + 1);
  } else if (auto *e = dynamic_cast<const ExpressionStatement *>(s)) {
    std::cout << pad << "ExprStmt\n";
    print_expr(e->expr.get(), depth + 1);
  } else if (auto *i = dynamic_cast<const IfStatement *>(s)) {
    std::cout << pad << "If\n";
    std::cout << pad << "  condition:\n";
    print_expr(i->condition.get(), depth + 2);
    std::cout << pad << "  then:\n";
    print_block(i->then_block, depth + 2);
    if (i->else_block) {
      std::cout << pad << "  else:\n";
      print_block(*i->else_block, depth + 2);
    }
  } else if (auto *f = dynamic_cast<const FunctionStatement *>(s)) {
    std::cout << pad << "Function(" << f->name << ")(";
    for (size_t i = 0; i < f->params.size(); i++) {
      if (i)
        std::cout << ", ";
      std::cout << f->params[i];
    }
    std::cout << ")\n";
    print_block(f->body, depth + 1);
  } else if (auto *w = dynamic_cast<const WhileStatement *>(s)) {
    std::cout << pad << "While\n";
    std::cout << pad << "  condition:\n";
    print_expr(w->condition.get(), depth + 2);
    std::cout << pad << "  body:\n";
    print_block(w->body, depth + 2);
  }
}

void print_block(const Block &b, int depth) {
  for (auto &s : b.stmts)
    print_stmt(s.get(), depth);
}

int main() {
  std::string source = R"(
allow x = 10
allow y = 20
given x > 5 {
    output x
} otherwise {
    output 0
}
module add(a, b) {
    output a + b
}
as long as x == y {
    print(x)
}
)";
  Lexer lexer(source);
  auto tokens = lexer.tokenize();

  Parser parser(std::move(tokens));
  Program program = parser.parse();

  for (auto &stmt : program.stmts)
    print_stmt(stmt.get());
}

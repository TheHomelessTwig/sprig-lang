#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "borrowchecker.hpp"
#include "interpreter.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "typechecker.hpp"
#include "version.hpp"

// Split a string into lines for error context display.
static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream ss(s);
    std::string ln;
    while (std::getline(ss, ln))
        lines.push_back(ln);
    return lines;
}

int main(int argc, char* argv[]) {
    if (argc == 2) {
        std::string arg = argv[1];
        if (arg == "--version" || arg == "-v") {
            std::cout << "sprig " << SPRIG_VERSION << "\n";
            return 0;
        }
    }

    if (argc != 2) {
        std::cerr << "Usage: sprig <file.sprig>\n";
        std::cerr << "       sprig --version\n";
        return 1;
    }

    std::ifstream file(argv[1]);
    if (!file) {
        std::cerr << "Error: could not open '" << argv[1] << "'\n";
        return 1;
    }
    std::stringstream buf;
    buf << file.rdbuf();
    std::string source = buf.str();

    try {
        Lexer   lexer(source);
        Parser  parser(lexer.tokenize());
        Program program = parser.parse();

        // ── Type checking pass ────────────────────────────────────────────────
        TypeChecker checker;
        auto type_errors = checker.check(program, source, argv[1]);
        if (!type_errors.empty()) {
            auto lines = split_lines(source);
            for (auto& err : type_errors) {
                if (err.line > 0) {
                    std::cerr << "Type error at line " << err.line << ":\n";
                    if (err.line <= (int)lines.size())
                        std::cerr << "  " << lines[err.line - 1] << "\n";
                } else {
                    std::cerr << "Type error:\n";
                }
                std::cerr << err.message << "\n";
            }
            return 1;
        }

        // ── Borrow checking pass ──────────────────────────────────────────────
        BorrowChecker borrow_checker;
        auto borrow_errors = borrow_checker.check(program, source);
        if (!borrow_errors.empty()) {
            for (auto& err : borrow_errors)
                std::cerr << err.message << "\n";
            return 1;
        }

        // ── Interpret ─────────────────────────────────────────────────────────
        Interpreter interp;
        interp.run(program, source, argv[1]);

    } catch (std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    return 0;
}

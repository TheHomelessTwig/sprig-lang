#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "borrowchecker.hpp"
#include "codegen.hpp"
#include "interpreter.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "typechecker.hpp"
#include "version.hpp"

static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream ss(s);
    std::string ln;
    while (std::getline(ss, ln)) lines.push_back(ln);
    return lines;
}

int main(int argc, char* argv[]) {
    bool        compile_mode = false;
    std::string output_file  = "output.ll";
    std::string input_file;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            std::cout << "sprig " << SPRIG_VERSION << "\n";
            return 0;
        } else if (arg == "--compile") {
            compile_mode = true;
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            output_file = argv[++i];
        } else {
            input_file = arg;
        }
    }

    if (input_file.empty()) {
        std::cerr << "Usage: sprig [--compile [-o output.ll]] <file.sprig>\n";
        std::cerr << "       sprig --version\n";
        return 1;
    }

    std::ifstream file(input_file);
    if (!file) {
        std::cerr << "Error: could not open '" << input_file << "'\n";
        return 1;
    }
    std::stringstream buf;
    buf << file.rdbuf();
    std::string source = buf.str();

    try {
        Lexer   lexer(source);
        Parser  parser(lexer.tokenize());
        Program program = parser.parse();

        // ── Type checking ─────────────────────────────────────────────────────
        TypeChecker checker;
        auto type_errors = checker.check(program, source, input_file);
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

        // ── Borrow checking ───────────────────────────────────────────────────
        BorrowChecker borrow_checker;
        auto borrow_errors = borrow_checker.check(program, source);
        if (!borrow_errors.empty()) {
            for (auto& err : borrow_errors)
                std::cerr << err.message << "\n";
            return 1;
        }

        // ── Compile or interpret ──────────────────────────────────────────────
        if (compile_mode) {
            CodeGen codegen;
            codegen.compile(program, output_file);
            std::cout << "Compiled to " << output_file << "\n";
            std::cout << "Build:  clang " << output_file
                      << " -o program -lm\n";
            std::cout << "Run:    ./program\n";
        } else {
            Interpreter interp;
            interp.run(program, source, input_file);
        }

    } catch (std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}

#include <fstream>
#include <iostream>
#include <sstream>

#include "interpreter.hpp"
#include "lexer.hpp"
#include "parser.hpp"

int main(int argc, char *argv[]) {
    // Require exactly one argument — the .sprig file path
    if (argc != 2) {
        std::cerr << "Usage: sprig <file.sprig>\n";
        return 1;
    }

    // Read the file into a string
    std::ifstream file(argv[1]);
    if (!file) {
        std::cerr << "Error: could not open '" << argv[1] << "'\n";
        return 1;
    }
    std::stringstream buf;
    buf << file.rdbuf();
    std::string source = buf.str();

    // Run it
    try {
        Lexer lexer(source);
        auto tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        Program program = parser.parse();

        Interpreter interp;
        interp.run(program);
    } catch (std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

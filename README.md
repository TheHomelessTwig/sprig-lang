# twig-lang

A toy programming language with a custom syntax. Written in C++17.

Currently implements a lexer and recursive-descent parser that produces an AST. No interpreter yet.

## Syntax

```
allow x = 10                  # variable declaration
allow name = "hello"

module add(a, b) {            # function definition
    output a + b              # return
}

given x > 5 {                 # if
    output x
} otherwise {                 # else
    output 0
}

as long as x == y {           # while
    print(x)
}
```

### Keywords

| Keyword       | Meaning          |
|---------------|------------------|
| `allow`       | variable (`let`) |
| `module`      | function (`fn`)  |
| `given`       | `if`             |
| `otherwise`   | `else`           |
| `output`      | `return`         |
| `as long as`  | `while`          |

## Build

```bash
cmake -B build
cmake --build build
./build/TWIG-LANG
```

Requires CMake ≥ 3.19 and a C++17 compiler.

## Project structure

```
src/
  main.cpp      — entry point, AST printer
  lexer.cpp     — tokeniser
  parser.cpp    — recursive-descent parser
include/
  lexer.hpp     — Token, TokenType, Lexer
  parser.hpp    — Parser
  ast.hpp       — AST node types
```

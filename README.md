# Sprig

A toy programming language with a custom syntax. Written in C++17.

Implements a lexer, recursive-descent parser, and tree-walk interpreter. Indentation-based — minimal braces and semicolons.

## Syntax

```
let x = 10                    // variable declaration
let name = "hello"

define add(a, b):             // function definition
    give back a + b

when x > 5:                   // if
    print(x)
otherwise when x == 0:        // else if
    print("zero")
otherwise:                    // else
    print(0)

as long as x > 0:             // while loop
    print(x)
    x = x - 1

let nums = [1, 2, 3]          // list literal
for each n in nums:           // for-each loop
    when n is 2:
        skip                  // continue
    print(n)
```

### Keywords

| Keyword        | Meaning               |
|----------------|-----------------------|
| `let`          | variable declaration  |
| `define`       | function definition   |
| `give back`    | return                |
| `when`         | `if`                  |
| `otherwise`    | `else` / `else if`    |
| `as long as`   | `while`               |
| `for each in`  | for-each loop         |
| `stop`         | break                 |
| `skip`         | continue              |
| `nothing`      | nil / null            |
| `and` `or` `not` | logical operators   |
| `is`           | equality (`==`)       |

### Types

| Type    | Example                   |
|---------|---------------------------|
| number  | `42`, `3.14`              |
| text    | `"hello"`                 |
| flag    | `true`, `false`           |
| list    | `[1, 2, 3]`, `["a", "b"]` |
| nil     | `nothing`                 |

### Built-in functions

| Function             | Description                        |
|----------------------|------------------------------------|
| `print(x, ...)`      | print to stdout                    |
| `input(prompt?)`     | read a line from stdin             |
| `length(x)`          | length of list or string           |
| `append(list, item)` | append item to list (mutates)      |
| `first(list)`        | first element                      |
| `last(list)`         | last element                       |
| `to_number(x)`       | parse string to number             |
| `to_text(x)`         | convert any value to string        |

### Operators

`+` `-` `*` `/` `==` `!=` `<` `>` `and` `or` `not` `is`

String concatenation with `+`. Index into lists and strings with `list[i]`.

## Build

```bash
cmake -B build
cmake --build build
./build/SPRIG-LANG file.sprig
```

Requires CMake ≥ 3.19 and a C++17 compiler.

Alternatively, use the `sprig` wrapper script if installed:

```bash
sprig file.sprig
```

## Editor support

A [tree-sitter grammar](https://github.com/TheHomelessTwig/tree-sitter-sprig) is available for Neovim (via nvim-treesitter), providing syntax highlighting and indentation.

## Project structure

```
src/
  main.cpp          — entry point
  lexer.cpp         — tokeniser (with indent/dedent handling)
  parser.cpp        — recursive-descent parser
  interpreter.cpp   — tree-walk interpreter
include/
  lexer.hpp         — Token, TokenType, Lexer
  parser.hpp        — Parser
  ast.hpp           — AST node types
  interpreter.hpp   — Value, Environment, Interpreter
tests/
  hello.sprig       — list operations demo
  input.sprig       — user input demo
```

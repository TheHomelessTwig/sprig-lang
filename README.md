# Sprig

A toy programming language with a custom syntax. Written in C++17.

Implements a lexer, recursive-descent parser, static type checker, and tree-walk interpreter. Indentation-based — minimal braces and semicolons.

## Syntax

```
let x = 10                    // immutable variable
let mutable count = 0         // mutable variable
let name = "hello"

define add(a, b):             // function definition
    give back a + b

when x > 5:                   // if
    print(x)
otherwise when x == 0:        // else if
    print("zero")
otherwise:                    // else
    print(0)

as long as count < 3:         // while loop
    count = count + 1

let nums = [1, 2, 3]          // list literal
for each n in nums:           // for-each loop
    when n == 2:
        skip                  // continue
    print(n)

include "utils.sprig"         // multi-file programs
```

### Shapes

User-defined record types:

```
shape Person:
    name: text
    age: number
    active: flag

let sam = Person { name: "sam", age: 20, active: true }
print(sam.name)   // sam
sam.age = 21      // field mutation
```

Shape field types are enforced at instantiation and assignment.

### Keywords

| Keyword          | Meaning               |
|------------------|-----------------------|
| `let`            | immutable variable    |
| `let mutable`    | mutable variable      |
| `define`         | function definition   |
| `give back`      | return                |
| `when`           | `if`                  |
| `otherwise`      | `else` / `else if`    |
| `as long as`     | `while`               |
| `for each in`    | for-each loop         |
| `stop`           | break                 |
| `skip`           | continue              |
| `nothing`        | nil / null            |
| `and` `or` `not` | logical operators     |
| `shape`          | define a record type  |
| `include`        | import another file   |

### Types

| Type    | Example                   |
|---------|---------------------------|
| number  | `42`, `3.14`              |
| text    | `"hello"`                 |
| flag    | `true`, `false`           |
| list    | `[1, 2, 3]`, `["a", "b"]` |
| nothing | `nothing`                 |
| shape   | user-defined              |

### Built-in functions

| Function             | Description                        |
|----------------------|------------------------------------|
| `print(x)`           | print to stdout                    |
| `input(prompt)`      | read a line from stdin             |
| `length(x)`          | length of list or string           |
| `append(list, item)` | append item to list (mutates)      |
| `first(list)`        | first element                      |
| `last(list)`         | last element                       |
| `to_number(x)`       | parse string to number             |
| `to_text(x)`         | convert any value to string        |

### Operators

`+` `-` `*` `/` `==` `!=` `<` `>` `and` `or` `not`

`+` is overloaded for string concatenation. Index into lists with `list[i]`.

## Type checker

Sprig runs a Hindley-Milner style type checker before interpretation. It infers types and reports all errors with source context before any code executes:

```
Type error at line 3:
  let x = "hello" + 42
Type mismatch: expected text but got number
```

The checker handles shapes, lists, function return types, and multi-file programs via `include`.

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
  typechecker.cpp   — Hindley-Milner type checker
  interpreter.cpp   — tree-walk interpreter
include/
  lexer.hpp         — Token, TokenType, Lexer
  parser.hpp        — Parser
  ast.hpp           — AST node types
  types.hpp         — type representation for the type checker
  typechecker.hpp   — TypeChecker, TypeError
  interpreter.hpp   — Value, Environment, Interpreter
tests/
  hello.sprig       — list operations demo
  input.sprig       — user input demo
  shapes.sprig      — shape types demo
  types.sprig       — type enforcement demo
  mutability.sprig  — mutability demo
  include/          — multi-file include demo
```

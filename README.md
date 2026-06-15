# Sprig

A toy programming language with a custom English-like syntax. Written in C++17.

Implements a lexer, recursive-descent parser, Hindley-Milner type checker, borrow checker, tree-walk interpreter, and LLVM IR code generator. Indentation-based — no braces or semicolons.

## Build

```bash
cmake -B build
cmake --build build
./build/SPRIG-LANG file.sprig          # interpret
./build/SPRIG-LANG --compile file.sprig -o out.ll  # compile to LLVM IR
```

Requires CMake ≥ 3.19, a C++17 compiler, and LLVM 18.

Compile LLVM IR to a native binary:

```bash
clang out.ll -o program -lm
./program
```

The build also produces `sprig-lsp`, an LSP server that push-publishes diagnostics (lex, parse, type, and borrow errors) on every file change. Install it and point your editor at it:

```bash
sudo cmake --install build   # installs sprig-lsp to /usr/local/bin
```

## Syntax

### Variables

```sprig
let x = 10                // immutable
let mutable count = 0     // mutable — reassign with let inside same scope
let name = "hello\nworld" // escape sequences: \n \t \r \\ \" \0
```

### Functions

```sprig
define add(a, b):
    give back a + b

define factorial(n):
    when n < 2:
        give back 1
    give back n * factorial(n - 1)

print(add(3, 4))   // 7
```

### Control flow

```sprig
when x > 5:
    print("big")
otherwise when x == 0:
    print("zero")
otherwise:
    print("small")
```

Single-line blocks are also valid when the body is simple:

```sprig
when x < 0: give back nothing
when done: stop
```

### Loops

```sprig
let mutable i = 0
as long as i < 5:
    let i = i + 1        // creates new binding in loop scope

let nums = [1, 2, 3, 4, 5]
for each n in nums:
    when n is 3:
        skip             // continue
    when n is 5:
        stop             // break
    print(n)
```

### Lists

```sprig
let items = [10, 20, 30]
print(items[0])          // 10
print(length(items))     // 3
append(items, 40)        // mutates in place
let last = pop(items)    // removes and returns last element
```

### String operations

```sprig
let greeting = "hello " + "world"
print(length(greeting))                  // 11
print(substring(greeting, 6, 5))         // world
print(char_code("A"))                    // 65
print(char_from_code(65))               // A
when string_contains(greeting, "world"):
    print("found")
```

### Shapes

User-defined record types with enforced field types:

```sprig
shape Person:
    name:   text
    age:    number
    active: flag

let sam = Person { name: "sam", age: 20, active: true }
print(sam.name)    // sam
sam.age = 21       // field mutation
print(sam.age)     // 21
```

Field types: `text`, `number`, `decimal`, `flag`, `own <ShapeName>`.

Shapes can be recursive via `own`:

```sprig
shape Node:
    value: number
    next:  own Node

let node = Node { value: 1, next: nothing }
```

### Ownership and borrowing

```sprig
let data = [1, 2, 3]
let x borrow data           // immutable borrow — data is locked
print(x[0])
// data is freed when x goes out of scope

let mutable val = 42
let ref borrow mutable val  // mutable borrow — val locked for mutation
```

The borrow checker runs before execution and rejects use-after-move and aliased mutation.

### Unsafe blocks

Raw pointer operations require an `unsafe:` block to signal intent:

```sprig
unsafe:
    let ptr = allocate(8)
    write(ptr, 99.5)
    print(read(ptr))
    free(ptr)
```

### Multi-file programs

```sprig
include "utils.sprig"
```

Included files are processed once (deduplicated). Paths are resolved relative to the working directory first, then relative to the including file as a fallback.

## Keywords

| Keyword            | Meaning                        |
|--------------------|--------------------------------|
| `let`              | immutable variable             |
| `let mutable`      | mutable variable               |
| `define`           | function definition            |
| `give back`        | return value from function     |
| `when`             | if                             |
| `otherwise`        | else / else if                 |
| `as long as`       | while loop                     |
| `for each x in y`  | for-each loop                  |
| `stop`             | break out of loop              |
| `skip`             | continue to next iteration     |
| `nothing`          | nil / null value               |
| `and` `or` `not`   | logical operators              |
| `is`               | equality (`==`)                |
| `is not`           | inequality (`!=`)              |
| `shape`            | define a record type           |
| `include`          | import another file            |
| `borrow`           | immutable reference            |
| `borrow mutable`   | mutable reference              |
| `own`              | heap-allocated value (Box<T>)  |
| `unsafe`           | block allowing raw pointer ops |

## Types

| Type       | Example                            |
|------------|------------------------------------|
| `number`   | `42`, `3.14`                       |
| `text`     | `"hello"`, `"line\n"`              |
| `flag`     | `true`, `false`                    |
| `list`     | `[1, 2, 3]`, `["a", "b"]`          |
| `nothing`  | `nothing`                          |
| shape      | user-defined via `shape`           |
| `own T`    | heap-owned value (used in shapes)  |
| `raw_ptr`  | untyped heap address (unsafe only) |

## Built-in functions

### I/O

| Function           | Description                                       |
|--------------------|---------------------------------------------------|
| `print(x)`         | print value to stdout followed by newline         |
| `input(prompt?)`   | read a line from stdin, optional prompt           |

### Type conversion

| Function           | Description                                       |
|--------------------|---------------------------------------------------|
| `to_number(x)`     | parse text to number                              |
| `to_text(x)`       | convert any value to its text representation      |

### Lists

| Function              | Description                                    |
|-----------------------|------------------------------------------------|
| `length(x)`           | number of elements in list, or chars in text   |
| `append(list, item)`  | append item to list (mutates in place)         |
| `pop(list)`           | remove and return last element (mutates)       |
| `first(list)`         | first element                                  |
| `last(list)`          | last element                                   |

### Strings

| Function                         | Description                              |
|----------------------------------|------------------------------------------|
| `substring(str, start, length)`  | extract substring                        |
| `char_code(str)`                 | ASCII code of first character            |
| `char_from_code(n)`              | single-character string from ASCII code  |
| `string_contains(haystack, needle)` | true if needle found in haystack      |

### File I/O

| Function                   | Description                          |
|----------------------------|--------------------------------------|
| `read_file(path)`          | read entire file as text             |
| `write_file(path, content)`| write text to file (overwrites)      |

### Process

| Function       | Description                                         |
|----------------|-----------------------------------------------------|
| `args_count()` | number of command-line arguments (including argv[0])|
| `args_get(i)`  | get argument at index i                             |
| `exit(code)`   | exit with status code                               |

### Raw pointers (unsafe only)

| Function              | Description                                 |
|-----------------------|---------------------------------------------|
| `allocate(n)`         | allocate n bytes, return address as number  |
| `free(addr)`          | free allocation at addr                     |
| `read(addr)`          | read a `number` (f64) from addr             |
| `write(addr, value)`  | write a `number` (f64) to addr             |
| `ptr_add(addr, n)`    | advance pointer by n bytes                  |
| `ptr_to_number(ptr)`  | cast pointer to number                      |
| `number_to_ptr(n)`    | cast number to pointer                      |

## Operators

| Operator                  | Description                             |
|---------------------------|-----------------------------------------|
| `+` `-` `*` `/`           | arithmetic (`+` also concatenates text) |
| `-x`                      | unary negation                          |
| `==` `!=` `is` `is not`   | equality                                |
| `<` `>` `<=` `>=`         | comparison                              |
| `and` `or` `not`          | logical                                 |
| `list[i]`                 | index access                            |
| `shape.field`             | field access                            |

## Type checker

Sprig runs Hindley-Milner type inference before execution. All type errors are reported with source context before any code runs:

```
Type error at line 3:
  let x = "hello" + 42
Type mismatch: expected text but got number
```

The checker handles shapes, lists, generics, function signatures, includes, and `own`/`raw_ptr` types.

## Borrow checker

A borrow checker enforces ownership rules:

- A value can have many immutable borrows or exactly one mutable borrow — not both.
- Moved values cannot be used after the move.
- Borrows are released when they leave scope.

```
Borrow error at line 7:
  print(x)
Use of moved variable 'x'
```

## Scoping note

Inside loops and conditionals, `let x = expr` creates a **new binding** in the inner scope — it does not update the outer `x` in the interpreter. For variables that must be updated across iterations, use shape field mutation:

```sprig
shape State:
    count: number

let s = State { count: 0 }
as long as s.count < 10:
    s.count = s.count + 1   // updates through shared_ptr — works in both modes
```

This is consistent between interpreter and compiled mode.

## Project structure

```
src/
  main.cpp           — entry point, CLI flag parsing
  lexer.cpp          — tokeniser with indent/dedent and escape sequences
  parser.cpp         — recursive-descent parser
  typechecker.cpp    — Hindley-Milner type inference
  borrowchecker.cpp  — ownership and borrow analysis
  interpreter.cpp    — tree-walk interpreter
  codegen.cpp        — LLVM IR code generator
  lsp.cpp            — JSON-RPC LSP server (push diagnostics on change)
include/
  lexer.hpp          — Token, TokenType, Lexer
  parser.hpp         — Parser
  ast.hpp            — AST node definitions
  types.hpp          — type representation
  typechecker.hpp    — TypeChecker, TypeError
  borrowchecker.hpp  — BorrowChecker, BorrowError
  interpreter.hpp    — Value, Environment, Interpreter
  codegen.hpp        — CodeGen
tests/
  hello.sprig         — lists and functions
  shapes.sprig        — shape types and field mutation
  types.sprig         — type inference examples
  mutability.sprig    — mutability rules and rebinding
  ownership.sprig     — borrow checker demo
  pointers.sprig      — raw pointer and own<T> demo
  builtins.sprig      — string/file/args built-ins
  input.sprig         — stdin input demo
  guess.sprig         — number guessing game
  errors.sprig        — error message formatting tests (blocks commented out)
  compiled.sprig      — compiler smoke test
  compiled_full.sprig — compiled-mode full feature test
  include/
    main.sprig        — multi-file include demo entry point
    utils.sprig       — shared utilities for include demo
```

## Editor support

A [tree-sitter grammar](https://github.com/TheHomelessTwig/tree-sitter-sprig) is available for Neovim (via nvim-treesitter), providing syntax highlighting.

`sprig-lsp` speaks the Language Server Protocol and works with any LSP-capable editor. It runs the full lex → parse → typecheck → borrow pipeline and publishes diagnostics on every file change. No configuration beyond pointing your editor at the binary.

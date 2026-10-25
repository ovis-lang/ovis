# OVIS

> [!CAUTION]
> This is an experiment. The language is changing.

Ovis is a programming language whose source code is plain English. You write
lines made of phrases, the way you'd say them, and Ovis compiles them through
LLVM into a native executable that can use any C library.

```text
# Reads some whole numbers and reports the smallest, the largest and the
# average.
count = ask "How many numbers?" for a whole number
numbers = empty list of whole numbers
repeat count times:
    add (ask for a whole number) to numbers

show "Numbers:", numbers
show "Smallest:", smallest of numbers, "Largest:", largest of numbers
show "Average:", mean of numbers rounded to 2 decimals
```

**Contents:** [The idea](#the-idea) ·
[Getting started](#getting-started) · [The language](#the-language) ·
[Programs with several files](#programs-with-several-files) ·
[The model](#the-model) · [Usage](#usage) · [Projects](#projects) ·
[Packages](#packages) · [Building with CMake](#building-with-cmake) ·
[How it works](#how-it-works) · [Further reading](#further-reading)

## The idea

A deterministic parser reads every line. The language model built into
the compiler only steps in when a line is unclear, and even then it
**never writes code**. It picks one of the readings that already
type-check.

"mean of numbers" isn't a phrase Ovis knows. So Ovis lists the readings
that make sense there, the model picks one, and Ovis writes the line as it
reads it just above, as a `@[meaning: …]` sigil:

```text
@[meaning: show "Average:", average of numbers rounded to 2 decimals]
show "Average:", mean of numbers rounded to 2 decimals
```

You can read the sigil, change it, or delete it to ask again. When no
reading fits, the model may write the line again in words Ovis reads. The
rewrite is kept only if the line then reads and keeps every name and
value. When the model isn't sure, the build stops and asks you:

```text
$ ovis run examples/vague.ovis
examples/vague.ovis, line 4: "show square root of x + 7" is unclear. Did you
mean one of these? Write it in the source:
    show ((square root of x) + 7)
    show square root of (x + 7)
```

Everything the model chose is also kept in `<file>.lock`. Commit it with
the source. Builds then never need the model, and building again changes
no file.

## Getting started

Install the latest release with ovisup (Linux x86-64 and arm64, macOS
arm64):

```sh
curl --proto '=https' --tlsv1.2 -sSf \
  https://raw.githubusercontent.com/ovis-lang/ovis/main/tools/ovisup | sh

ovis new hello      # a project: ovis.toml and a main.ovis that says hello
cd hello
ovis run
```

It puts `ovis` in `~/.ovis/bin` with LLVM inside; you need only a C
compiler for linking (`ovisup check` says what is missing). `ovisup
update` updates it, `ovisup install nightly` adds a build of `main` (`ovis
+nightly …`), and `ovisup self uninstall` removes it all. Until the first
tagged release, the installer sets up `nightly`. See
[docs/deployment.md](docs/deployment.md).

To build from source, you need:

- CMake 3.20 or newer
- a C++23 compiler
- LLVM (tested with 18)
- a C compiler, for linking

llama.cpp is fetched and built with Ovis. libclang is optional; you only
need it for C libraries.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/test_ovis

# The examples have their readings written in, so they need no model:
./build/ovis run examples/primes.ovis

# Download the default model, then run your own program:
./build/ovis model pull
./build/ovis run my_program.ovis
```

`ovis model pull` fetches Qwen2.5-Coder-7B-Instruct (Q4_K_M, about 4.7 GB)
if a GPU can run it, and the 3B model (about 2 GB) otherwise. Use `ovis
model pull 3b` or `7b` to choose. See [The model](#the-model) for GPUs and
other options.

[`examples/`](examples/) has a program for each part of the language.

## The language

### Lines, variables and blocks

```text
# Finds the prime numbers below 50.
to (n) is prime:
    if n < 2:
        give back false
    divisor = 2
    while divisor * divisor <= n:
        if n is divisible by divisor:
            give back false
        divisor = divisor + 1
    give back true

primes = empty list of whole numbers
for each n in the numbers from 2 to 49:
    if n is prime:
        add n to primes
show primes
show "Found", length of primes, "primes"
```

- **Lines and blocks:** a line ending in `:` opens a block indented under
  it, as in Python. `#` starts a comment.
- **Variables:** `name = value`. Ovis works out the types.
- **Blocks:** `if … :`, `otherwise if … :`, `otherwise:`, `while … :`,
  `for each x in list:`, `repeat n times:`. Use `stop` and `skip` to leave
  a loop or go to its next pass.
- **Operators:** `+ - * / mod = != < > <= >= and or not` work as in
  Python.
- **Phrases:** the library has 171 built-in phrases, such as `average of
  numbers`, `add x to list`, `text in capitals` and `x rounded to 2
  decimals`. Each is a template with typed slots. Articles are optional,
  and singular and plural match. `ovis phrases round decimals` lists the
  ones with those words. The phrases are in [`phrases/`](phrases/), and
  each one has an example in
  [`test/phrase_library.txt`](test/phrase_library.txt).
- **Lists and maps:** `[1, 2, 3]`, `empty list of text`, `item 2 of xs`;
  `empty map of text to whole numbers`, `set m at k to v`, `m at k`.

### Your own phrases

`to WORDS (parameter) WORDS …:` defines a phrase. Use `give back` for its
result.

- Parameters may say their type: `(width: whole number)`,
  `(names: list of text)`.
- A phrase works on any line, before or after it is defined, as a
  statement or as a value.
- A phrase is also a function named by its words: `to pad (text) to
  (width):` is `pad_to(text, width)`. Write `as NAME` before the `:` to
  pick the name yourself.
- `name(arguments)` calls your phrases and C functions.
- Variables set in a phrase belong to it.

### Records

```text
a book has:
    title (text)
    pages (whole number)
    price (decimal, 0)          # a field with a default
    lent (truth value, false)

emma = a book with title "Emma", pages 474, price 8.5
show emma's title, title of emma, emma.title
lent of emma = true
```

A field reads three ways: `emma's title`, `title of emma` and
`emma.title`. Library phrases work on any record's fields: `books sorted by
title`, `total price of books`, `the book in books with the most pages`.
See [`examples/library.ovis`](examples/library.ovis).

### Traits

A trait says what a value can do. Each record says how it does it, and
phrases on the trait pick the right one when the program runs:

```text
a shape can:
    area of (it) gives a decimal
    describe (it)

a circle is a shape:
    area of (it):
        give back 3.14159 * radius of it * radius of it
    describe (it):
        show "a circle of radius", radius of it
```

A trait is a type: `(s: shape)`, `list of shape`, and lists that mix
records. Built-in traits are `showable`, `comparable` and `equatable`. A
record gets one by defining its phrase, for example `to (a: book) comes
before (b: book):`, or by saying `a book is comparable:`. See
[`examples/shapes.ovis`](examples/shapes.ovis).

### Generics

A record or phrase written once for any type:

```text
a stack of (item) has:
    items (list of items)
    size (whole number, 0)

to push (x: any item) onto (s: stack of item):
    add x to items of s
    increase size of s by 1

to the biggest of (xs: list of any comparable thing):
    ...
```

`any NAME` introduces a type, and a trait in front of it is a bound. Each
use gets its own copy when the program is built. See
[`examples/stack.ovis`](examples/stack.ovis) and
[`examples/collections`](examples/collections).

### Numbers of a size and bits

`byte`, `short`, `int8`…`uint64`, `float32` and `16-bit whole number` are
types like any other. Arithmetic happens at that size, and overflow and
narrowing are checked. `x as a byte`, `wrapped to`, `clamped to`,
`0x4F56`, `0b1010`, `with bit 2 set`, `shifted left by`, `in hex` and
`in binary` all work. A record can be laid out exactly as a file format
wants it with `a file header has, packed:`. See
[`examples/bits.ovis`](examples/bits.ovis).

### Memory

Ovis has no garbage collector. Text, lists and records live until you free
them, or until the memory they were made in is freed:

```text
names = empty list of text
count = 0
while count < 1000000:
    with new memory:                 # everything made here goes when it ends
        label = "item " + count
        if count = 7:
            add keep label to names  # `keep` copies it out first
    count = count + 1
free names
```

That is the whole vocabulary:

- `free x`
- `scratch = new memory`, then `with scratch:` and `clear scratch`
- `keep x`
- `a copy of x`. Values are shared by `=`, never copied for you.

Programs are checked. Using a value after it, or its memory, was freed is
an error that names the lines. The build also refuses a new text, list or
record made where nothing can free it (`show "n: " + n`): store it in a
variable first.

- `--fast` turns the checks off.
- `--report-leaks` lists at exit what was never freed.
- `--lax-memory` allows values that nothing can free.

The design is in [docs/proposals/memory.md](docs/proposals/memory.md).

### At the end

`at the end, …` runs when its block ends, by any way out. It is useful for
closing what you opened:

```text
file = fopen(path, "r")
at the end, fclose(file)
```

### Using C libraries

```text
use "sqlite3.h" from "sqlite3"
db = null
if sqlite3_open(":memory:", ref db) = SQLITE_OK:
    show "opened"
```

Ovis reads the header with libclang. Its functions, structs, enum values
and `#define`s can then be used on any line.

- `ref x` passes a variable for a C function to write into.
- C structs read like records (`entry's d_name`). A pointer that may be
  null is `T or nothing`, checked with `is not nothing`.
- Include and link flags come from `pkg-config`, or `-l<library>`. Use
  `-I` and `-L` for other places.
- The C standard library (`printf`, `strlen`, `sqrt`, `fopen`, ...) needs
  no `use`.

See [docs/c-libraries.md](docs/c-libraries.md) for what is supported.

## Programs with several files

A folder with a `main.ovis` is a program. Every other `.ovis` file in the
folder and its subfolders is a module: `pricing/tax.ovis` is
`pricing.tax`. `main.ovis` says what the program does. The other files
only define phrases, `use` C libraries and set constants, and every file
can use them.

```text
examples/shop/
  main.ovis               discounted = apply the discount to the total of basket ...
  pricing/discount.ovis   to apply the discount to (total): ...
  pricing/tax.ovis        to add tax to (amount: decimal): ...

$ ovis run examples/shop
After discount: 42.08
With tax: 45.44
```

A folder with an `ovis.toml` is also a [project](#projects): its `build/`
and `vendor/` folders are Ovis's, not modules.

**Your own phrase files.** A program folder's `phrases/` folder, or
`--phrases DIR`, holds phrase files in the same format as the repository's
[`phrases/`](phrases/). Ovis reads them at every build, so there is no
rebuild. `ovis check-phrases PROGRAM` tells whether your own phrases make
the questions Ovis asks about other lines worse.

## The model

Programs whose lines are all phrases Ovis knows never load the model.

**A GPU is the intended setup.** On held-out wording far from the phrases,
7B settles 17.5 of 20 cases, with 89% of its confident picks right; 3B
settles 13.5, with 68% right. Each question is a pass over the prompt. On
four CPU cores that takes about 2.3 s with 3B and 5.3 s with 7B; a GPU
takes a fraction of a second.

Macs use Metal with no setup. For other GPUs, build llama.cpp's backend
in:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON    # NVIDIA
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_VULKAN=ON  # others (Vulkan SDK)
```

`--cpu` keeps the model off the GPU, and prefers 3B.

**Where the weights come from**, in order:

1. `--model-path`
2. `$OVIS_MODEL_PATH`
3. weights embedded in the `ovis` executable
4. a default model next to `ovis`, in `../share/ovis`, or in the cache
   directory (`~/.cache/ovis/models`): 7B first if a GPU can run it, else
   3B first

**The daemon.** The first build that needs the model starts a copy of
`ovis` in the background. It keeps the model loaded and quits after 15
minutes unused, which saves loading it in every later build. Use `ovis
model stop` to stop it sooner, or `--no-daemon` to load the model in the
build instead.

**An embedded model.** The **Embedded model** workflow in GitHub Actions
builds `ovis` with the model inside. To make one yourself:

```sh
ovis model embed qwen2.5-coder-3b-instruct-q4_k_m.gguf -o ovis-with-model
# or at build time:
cmake -S . -B build -DOVIS_EMBED_MODEL=/path/to/model.gguf
```

**A model server.** `--endpoint http://127.0.0.1:8080/v1` uses any
OpenAI-compatible API that gives log-probabilities, such as llama-server.
Configure with `-DOVIS_BUILTIN_LLM=OFF` to build without llama.cpp.

**Same builds everywhere.** Builds are reproducible through the committed
`.lock` file and `@[meaning: …]` lines, not through the model: another CPU
or GPU may settle a close question the other way. For a binary you will
run on other machines, add `-DOVIS_PORTABLE=ON` (no CPU-specific tuning).

## Usage

In a [project](#projects), the program can be left out: the commands below
build the nearest `ovis.toml`'s program. Naming one `.ovis` file builds it
on its own, as before.

```text
ovis run   <file or folder>   compile and run
ovis build <file or folder>   compile to an executable (-o to name it)
ovis ir    <file or folder>   print the LLVM IR
ovis check <file or folder>   report problems; build nothing, write nothing
                              (--json for editors)
ovis fix   <file or folder>   choose what each unclear line means; written
                              above it as @[meaning: …]
ovis fmt   <file or folder>   put the @[…] lines in order (--check for CI)
ovis phrases [words…]         the built-in phrases with these words
ovis manual [words…]          the phrases a question is about, with what
                              each does and an example (--in FILE, --line N)
ovis check-phrases <folder>   whether the program's own phrases make
                              questions worse
ovis model path | pull [3b|7b] | embed MODEL -o OUTPUT | stop
ovis --version
```

For projects and packages:

```text
ovis new NAME | init          make a project (ovis.toml, main.ovis, .gitignore, git)
ovis add NAME --git URL [--version V] | --path DIR
                              depend on a package (its newest version by default)
ovis add c NAME --pkg-config P | --git URL --tag T | --path DIR [--targets A,B]
                              depend on a C library
ovis remove [c] NAME          the other way round
ovis update [NAME]            newer versions of packages, new commits for C tags
ovis tree                     the packages used, and why each version
ovis fetch | vendor           download what ovis.lock names; copy it into vendor/
ovis release X.Y.Z            in a package: check it, then tag vX.Y.Z
ovis cmake [--print]          the CMake project generated from ovis.toml
```

Common options:

```text
--offline            never ask the model; unclear lines are reported
--model-budget <s>   seconds the model may spend on questions (default 10)
--keep-source        don't write @[meaning: …] lines into the source
--no-prompt          in a terminal, report unclear lines instead of
                     offering their readings
-I <dir>, -L <dir>   where C headers and libraries are
--fast, --report-leaks, --lax-memory     memory checks (see Memory)
--cpu, --threads <n>, --model-path <gguf>, --endpoint <url>
--no-daemon, --daemon-idle <minutes>
```

`ovis --help` lists them all. In a terminal, a live line at the bottom
shows what the build is doing and for how long. In a pipe or CI the output
stays plain.

## Projects

A project is a folder with an `ovis.toml` saying what its program needs.
`ovis new hello` makes one; inside it, `ovis run` and `ovis build` need no
program named (`build` writes `build/hello`):

```toml
[project]
name = "packer"
toolchain = "0.1.0"          # optional: the ovisup toolchain to build with

[build]
include = ["include"]        # where the C headers of `use` lines are
c = ["c/*.c"]                # the project's own C files, compiled by cc

[c.zlib]
pkg-config = "zlib"          # a system library

[c.cjson]                    # a CMake project from git, pinned in ovis.lock
git = "https://github.com/DaveGamble/cJSON"
tag = "v1.7.18"
targets = ["cjson"]
```

With only `pkg-config` and `c` files, Ovis compiles and links the C
itself. A `git`, `path` (a CMake folder in the project) or `cmake` (your
own CMake file) table makes Ovis generate a CMake project into
`build/cmake/` from `ovis.toml` at every build: you never edit CMake, and
`ovis cmake --print` shows what it wrote.

**`ovis.lock`**, beside `ovis.toml`, pins every git dependency (C or
[package](#packages)) to its commit and the files' hash, so a tag moved
upstream doesn't change your build; `ovis update` moves the pins on
purpose. Commit it, as you commit each file's `<file>.lock`.

**The toolchain line.** `toolchain = "0.1.0"` makes ovisup's `ovis` run
that version inside the project, from any folder in it (`ovis +nightly …`
and `OVIS_TOOLCHAIN` still come first).

See [`examples/project`](examples/project) and
[docs/projects.md](docs/projects.md).

## Packages

Packages are other people's phrases, records and traits, hosted anywhere
git is (GitHub, a company server, a folder): there is no registry. A
package is a repository with a `[package]` table in its `ovis.toml`, its
versions are its `vX.Y.Z` tags, and its URL is its name.

```sh
ovis add csv --git https://github.com/someone/csv   # its newest version, locked
ovis tree                                           # what is used, and why
ovis update                                         # newer versions, on purpose
```

```toml
[dependencies]
csv = { git = "https://github.com/someone/csv", version = "1.4" }  # 1.4.0 or later 1.x
mine = { path = "../mine" }                                        # a folder

[overrides]                                  # yours alone: replaces it for everyone
log = { git = "https://github.com/me/log-fixed", tag = "v1.4.1" }
```

**Versions.** Each package gets the least version that satisfies everyone
who asks for it, so nothing changes until you `ovis update`. That is one
version per major version (before 1.0, per minor): when one package needs
`log` 1.2 and another `log` 1.4, the program has `log` 1.4.0. When one
needs `log` 1 and another `log` 2, both are in the program and kept
apart; passing one's `entry` to the other is an error that names both
versions and which package uses which.

**What a file sees.** A file reads with its own package's phrases, and a
direct dependency's only where it says so with `use csv` (`ovis add`
writes it into `main.ovis`; `ovis fix` adds it where a line needs it), so
a package you don't use in a file can't change how its lines read. A
dependency's dependencies are never seen. Where two give the same
words, the question's readings say whose, and you can write it:
`csv's read the table in "x.csv"`, which needs no `use` line. What a file under `X/internal/`
defines (a package's or your program's) only the files under `X/` see. A package may give a type a trait only if it declares
the trait or the type (Rust's orphan rule), so adding a dependency never
breaks an impl.

**A package's C** (its headers, own C files and `[c.*]` tables) is built
with your program; a program has one version of each C library, and when
two packages want different tags of one, `[overrides]` `"c.NAME" = { tag
= … }` chooses.

**Nothing is asked of your model** for a dependency: a package ships its
`@[meaning: …]` lines and lockfiles, and is always read as its author
settled it. Its files stay read-only in the cache
(`~/.cache/ovis/packages`); `ovis vendor` copies them into `vendor/` for
builds with no network.

**Publishing** is a git tag. In the package, set `version` in its
`ovis.toml`, commit, then:

```sh
ovis release 1.2.0     # checks it builds as its users will, then tags v1.2.0
git push origin v1.2.0
```

See [docs/projects.md](docs/projects.md) and
[docs/proposals/packages.md](docs/proposals/packages.md) for the design.

## Building with CMake

For a CMake project of your own where Ovis is one part. (A
[project](#projects) needs none of this: Ovis writes its CMake itself.)
Install Ovis with `cmake --install build --component ovis --prefix
PREFIX`, or use an ovisup toolchain (`-DCMAKE_PREFIX_PATH="$(ovisup
prefix)"`). Then, in your project:

```cmake
find_package(Ovis)
ovis_add_executable(app main.ovis LINK mylib)
```

Ovis writes an object file, and CMake links it with `mylib` and the
runtime. Builds under CMake are `--locked`: they never ask the model or
change a file. Run `ovis run` once to settle any questions first. See
[`examples/cmake`](examples/cmake) and
[docs/c-libraries.md](docs/c-libraries.md).

## How it works

```text
 your lines ──► chart parser ──► one reading ─────────────────► Ovis Core ──► LLVM ──► executable
                (phrases, types)   │                               ▲
                                   └─ none, or several: a question ┘
                                      @[meaning] / lockfile / rule /
                                      ranking / the model chooses / you decide
```

1. **Parsing.** A chart parser matches each line against the phrases and
   keeps only the readings that type-check.
2. **Questions.** A line that reads several ways, or uses words no phrase
   has, becomes a question with a few candidate readings. In order, what
   settles it:
   - a `@[meaning: …]` sigil above it, yours or Ovis's. The lockfile
     notices when Ovis's was written for a line you have since edited, and
     asks again.
   - the lockfile
   - a rule: the one name a misspelling is close to, or the one value `it`
     can mean
   - the ranking, when one reading clearly shares the line's words
   - the model, if it is confident (built in, using llama.cpp)
   - you: the build stops and lists the readings
3. **Compilation.** Each phrase stands for an [Ovis Core](docs/ovis-core.md)
   form, a small typed Lisp. The program is type-checked as a whole, turned
   into LLVM IR, optimised, and linked with a small runtime (lists, text,
   input) and libc.

A phrase template, from [`phrases/`](phrases/):

```text
expr  average of {xs: int[]}   : float  => (/ (float (prelude_sum_int $xs)) (float (len $xs)))
      alias: average value of {xs}
stmt  add {x: T} to {xs: T[]}           => (append $xs $x)
```

## Further reading

| Document | What |
| --- | --- |
| [docs/architecture-spec.md](docs/architecture-spec.md) | how the compiler works, and the invariants it keeps |
| [docs/ovis-core.md](docs/ovis-core.md) | the Ovis Core reference, for writing phrase templates |
| [docs/questions.md](docs/questions.md) | questions, rewrites and the lockfile |
| [docs/records-and-traits.md](docs/records-and-traits.md) | records and traits |
| [docs/proposals/generics.md](docs/proposals/generics.md) | generics |
| [docs/c-libraries.md](docs/c-libraries.md) | C libraries and CMake |
| [docs/memory.md](docs/memory.md) | memory at run time: arenas, `free`, checked builds |
| [docs/projects.md](docs/projects.md) | projects and packages: `ovis.toml`, building their C, `ovis.lock`, versions |
| [docs/proposals/projects.md](docs/proposals/projects.md), [docs/proposals/packages.md](docs/proposals/packages.md) | why projects and packages are as they are |
| [docs/deployment.md](docs/deployment.md) | ovisup and the release packages it installs |
| [docs/model-backends.md](docs/model-backends.md) | the built-in model, the daemon and model servers |
| [docs/state.md](docs/state.md), [docs/remaining-tasks.md](docs/remaining-tasks.md) | what is built and what is next |

Where the code is:

| Path | What |
| --- | --- |
| `src/ovis/phrase/` | the front end: lexer, templates, chart parser, questions, lockfile, formatter |
| `phrases/` | the built-in phrases and the Ovis Core prelude behind them |
| `src/ovis/builtin_model.cpp`, `language_model.cpp`, `model_daemon.cpp` | in-process llama.cpp, the HTTP model client, the daemon |
| `src/ovis/sexpr.cpp`, `ast.cpp` | Ovis Core reader and parser |
| `src/ovis/elaborate.cpp`, `checker.cpp`, `monomorphise.cpp` | type inference, checking, copies of generics |
| `src/ovis/memory_check.cpp` | the memory rules checked at build time |
| `src/ovis/libc.cpp`, `c_import.cpp`, `modules.cpp` | the C standard library, C headers, modules |
| `src/ovis/llvm_codegen.cpp`, `runtime.cpp` | LLVM code generation and the runtime linked into every program |
| `src/ovis/project/` | `ovis.toml`, a project's C by cc, the generated CMake project, `ovis.lock`, packages |
| `tools/ovisup`, `tools/package.sh`, `.github/workflows/release.yml` | the installer, release packages, and the workflow that publishes them |
| `examples/` | example programs |
| `eval/` | labelled questions and programs for `ovis_eval`, which measures the model |

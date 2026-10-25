# Ovis architecture spec

This describes how Ovis works and the rules its parts must keep. It describes
what is built, not plans; plans are in [remaining-tasks.md](remaining-tasks.md)
and progress is in [state.md](state.md). The design came from
[proposals/phrase-grammar.md](proposals/phrase-grammar.md).

## 1. Goal

A programming language whose source code is plain English, compiled through
LLVM to native code.

Principles:
- **A deterministic parser reads the program.** Lines are made of phrases
  from a library (and the program's own `to …:` phrases), with fixed
  operators and Python-like blocks.
- **The model chooses, or rewrites in phrases.** Where a line reads
  several ways, or uses words no phrase has, the compiler lists the
  readings that type-check and the built-in model picks one, or none. If
  none is taken it may write the line again in the program's words, which
  the parser must read ([questions.md](questions.md)). It never writes code.
- **Nothing is hidden.** Every line the compiler read otherwise than
  written gets a sigil above it, `@[meaning: LINE]` (the whole line as
  read), which the author can edit; it is authoritative.
- **Reproducible.** Meanings and the lockfile settle every question
  the model answered, so rebuilds never need the model.
- **Unsure means ask.** A question the rules, the ranking and the model
  can't settle confidently stops the build, listing the readings.

## 2. Pipeline

```
program (file, or folder with main.ovis)       load_phrase_program
  ▼
lines of tokens, blocks, @[…] sigils           lexer.cpp
  ▼
`to …:` headers and `use "x.h"` of every file  front_end.cpp
  ▼
each line: shape, chart parse, type filter     parser.cpp
  │ one reading → lowered to Ovis Core text
  │ none/several → question: meaning, lockfile, rule, ranking shortcut,
  │                 model (choose, rewrite), else reported  questions.cpp
  ▼
Ovis Core units (one per top-level line) + prelude
  ▼
join modules, import C headers                 modules.cpp, c_import.cpp
  ▼
copy field generics (`NAME:RECORD:FIELD`)      monomorphise.cpp
  ▼
elaborate, check, copy generics per use         elaborate.cpp, checker.cpp,
                                                monomorphise.cpp
  ▼
write @[meaning: …] lines back + lockfile      main.cpp
  ▼
LLVM IR → -O2 → object → cc link with runtime + libs     llvm_codegen.cpp, runtime.cpp
```

## 3. Programs and lines

### Programs and modules (`load_phrase_program`)

- **A folder with a `main.ovis` is a program.** Every `.ovis` file in the
  folder and its subfolders is a module. Subfolders nest:
  `pricing/tax.ovis` is the module `pricing.tax`, and `main.ovis` is the
  module `main`.
- Left out: hidden folders, and subfolders with their own `main.ovis`
  (those are separate programs).
- `ovis run folder/` and `ovis run folder/main.ovis` are the same. Any other
  `.ovis` file is a program on its own, with no module name.
- **Order:** helper files sorted by path, then `main.ovis`. Helpers' data is
  initialised before `main`'s statements run.
- **Only `main.ovis` does things.** A helper may contain only phrases
  (`to …:`), `use` lines and top-level variables whose values need no
  input, randomness or calls (checked by `resolve_modules`).
- Each file has its own lockfile and its own sigils. Reports name
  the file: `pricing/tax.ovis, line 3: …`.
- A folder with `ovis.toml` is a project: its settings, C and pinned
  dependencies are in [projects.md](projects.md).

**Lexer** (`split_lines`): logical lines with tokens, block depth and
annotations. Python-like blocks (a deeper line only under a `:` line; no
tabs; dedents must line up); open brackets continue a line; `"…"`, `'…'`,
`“…”` texts; `'s` after a word is a symbol, `don't` one word, `2nd` a word.
A line with a `:` outside brackets and text after it (`to double (n): give back n * 2`) is split by the lexer into the header and one body line at the next depth (`split_one_line_blocks`); sigils above it belong to the body.
**Sigils**: a line `@[KIND: TEXT]` is about the next code line, right above
it at its indentation (a blank line, a comment or another indentation
between is an error); it is never code, so it opens no block. Its `]` is
the first outside a text. Kinds: `meaning` (the line as the compiler reads
it) and `note` (something it filled in, `annotation::kind_type::compiler`).
Old builds wrote `Read as: "SPAN" is READING` and `Compiler: …` lines
indented under a line; they are still read and rewritten as sigils.

## 4. Phrases

**Templates** (`read_templates`, `.phr`): `expr PATTERN : TYPE => CORE` or
`stmt PATTERN [: BLOCK] => CORE`, then indented `alias:`, `about:` (the manual's sentence) and `example:` (a line that reads to this template, tested) lines. Patterns:
words, `[optional]`, `(a|b c)`, `word[suffix]`, `{name: type}`, one
`{name: type, ...}`. Types: `int float num str bool any record type size`
(`size`: a type of number only), `[]`,
uppercase variables, a `type` or `size` slot's name (`t[]` a list of it), `[TYPE] field of R` (R a
variable or `type` slot; a result may name a field slot: that field's
type). Checked (errors at `file:line`): one Core form, every `$name` a
slot and every slot used (a `type` slot may only type others),
`$name...` for the variadic one, `BLOCK` iff a block, an always-present
word, a word between slots, result variables and fields' records bound by
a slot.
- `phrases/*.phr` and the Ovis Core prelude `phrases/*.core` (functions
  multi-step phrases call, many of them generic) are embedded at build
  time; a phrase library's `.generic` files (`NAME:$T:$F`) are read as Core
  generics (`NAME (forall T (field F))`).
- **Traits, records, `nothing`**: built-in traits as slot types and
  how a record implements them: [records-and-traits.md](records-and-traits.md).
- **Pointers in words** (references.md R7, `pointers.phr`): type words
  for C numbers (`int32`, `byte`, `float32`…), `pointer`, `pointer to an
  int32`, `pointer to many bytes`, `C pointer to …` (a record is a pointer
  to its struct already), `array of bytes`, `phrase (whole number, text)
  giving a truth value` (or `action`, `function`, `method`, `procedure`,
  `routine`; `C phrase` for C's types); `value_kind::exact` keeps the Core
  type the parser's kind (`ptr`, a list) leaves out. Phrases: `the int32 at
  p`, `set the int32 at p to v`, `p moved by 3 int32s`, `size of int32`,
  `ref x` (`[the] address of x`), `p as a pointer to an int32`, `book as a
  pointer`, `p as its address`, `a new array of 256 bytes`, `the 5 int32s
  at p`, `call f [with a, b and c]`. A type slot may follow a value slot
  (`3 int32s`). A phrase as a value is its header's words with `_` for
  its slots (`compare _ and _`; `vocabulary::phrase_values`), after `the
  phrase` (or a synonym), which a phrase without slots needs.
- **Numbers of a size** ([proposals/numbers.md](proposals/numbers.md) §2,
  §6): `number_type_words` reads `int32`, `short`, `16-bit whole number`…
  in every type position; `int`/`float` slots take any size
  (`value_kind::exact` carries it); `{t: size}` slots; `0x1F 0b101 1_000`.
- **Field slots** match the words of some record's field (every length
  that matches: `number of wheels`, or `number` if a field is called that);
  `build_phrase` checks it against R's record once the other slots bind R
  and expands `$f` to `RECORD:FIELD`. Their Core calls generics over a
  record and one of its fields, `(prelude_NAME:$f …)`: the prelude's `(fn
  prelude_total_by (forall r (field f)) …)`, which `copy_field_generics`
  copies per record and field (`prelude_total_by__book__pages`) before
  elaboration.
- The library (171 templates): `core.phr` (output, input, prompts,
  conversions, buffers), `pointers.phr` (below), `maps.phr` (`map of
  text to whole numbers`, `set m at k to v`, `m at k`, `m has k`, `remove
  the entry for k from m`, `the keys of m`, `for each k in m`), `lists.phr`, `numbers.phr` (also bits, bytes,
  hex, wrapping), `text.phr` (positions count
  from 1), `records.phr` (`sorted by`, `sort … by`, `total … of`, `the T
  in xs with the highest/lowest F`, `X is nothing`). Element-typed
  phrases call one generic prelude function each (`prelude_contains
  (forall (item equatable))`, `prelude_smallest`, `prelude_sum_int (forall
  (item number))`…), copied per element type, records included; built-in
  templates have no `for records:` lines. `test/phrase_library.txt` has a program and its output for
  every template; `templates_used` in the result shows which were used.

## 5. Reading a program

**Reading** (`compile_phrase_program`, `compile_phrases` for one text;
`parser.cpp`, `questions.cpp`, `front_end.cpp`). A line's
first words decide its shape (`x =`, `if`, `otherwise`, `while`, `for
each`, `repeat … times`, `stop`, `skip`, `to …:` (with `gives TYPE` before
the `:` or `as NAME`, its result type), `give back`, `at the end[,]
STATEMENT` / `at the end:` (or `defer`), `do:` / `NAME = do:` and `leave
[with VALUE]` (Core `(block :block_N …)` and `(leave :block_N …)`; names
first set inside stay in it, as in `at the end:`), else a statement phrase); its span is read by a chart parser (fixed
operator precedence, phrases, optional articles, plurals of words of 3+
letters) filtered by
types. A reading brackets a phrase with a slot at its edge that fills a
slot at an expression's edge (`(add tax to x) with 2 decimals`), so
different groupings never read the same. One reading: lowered. Several, or none: a question (below), whose
answer is applied as `"SPAN" is READING` (whole-word text substitution).
The physical line's final text is written above it as `@[meaning: …]`
(header and body of a one-line block together; none for a line continued
over several, whose readings only the lockfile keeps). On a later build a
meaning becomes one decision again (`meaning_decision`: what lies between
its common start and end with the line, widened to whole words), unless
the lockfile's `meanings` say it was written for another line: the line
was edited, so it is read afresh. A meaning the lockfile doesn't know is
the author's and wins. Each top-level line is one
unit; variables first set in a block are hoisted to a `let` at the top of
the program or phrase. `to WORDS (p[: TYPE]) … [as NAME]:` headers are read
first, so a phrase works on any line, as a statement and an expression; it
is an Ovis Core `fn` named by its words (`pad_to`; commas may separate
parameters, and match `,` in lines), also called as
`pad_to(a, b)` like C functions. An untyped parameter the body uses as a
number (`total >= 40`), as text (`p in capitals`) or as a list (`for each
x in p`) is a slot of that type (`numeric_parameters`, `use_of`), so the
phrase isn't offered for other values. Assigning in a phrase makes a local.
A variable's name is a word, or 2–4 words without `of` (`fan speed = 100`,
`set the top speed to …`; a leading article isn't part of it; Core
`fan_speed`), read as one value once set; a line that reads as a statement
(`show x = 3`) isn't an assignment to `show x`.
Variables named like Ovis Core words get a `_`; `;@LINE` comments in the
Core map checker problems back to source lines. Reports quote the line
under the problem and speak the program's words (`in_authors_words`):
types in English (`describe_in_english`, done by the checker), phrases by
their header (`'greet (who)'`), `prelude_*` as "a built-in phrase", Core
operations as phrases (`len` → `length of`), renamed variables without
their `_`. A variable first set in a
block is declared `(let x TYPE)` (or `?`) at the top. A `for each`
variable ends with its loop; `for each i in the numbers from A to B` (or
`down to`) is Core's counting `for`, so no list is made. A variadic slot's items are split at commas,
and also with neighbouring pieces kept together (up to 4 commas), for
phrases that have commas.

**Records and traits** (`a NAME has:`, `a NAME can:`, `a NAME is a
TRAIT:`, fields in lines, dispatch): [records-and-traits.md](records-and-traits.md).

**Generics** ([proposals/generics.md](proposals/generics.md)). `a stack of
(item) has:` is a generic record (`shape_of`'s `parameters` and `header`;
`record_info::parameters`); `stack of books`, `table from text to whole
numbers` are type words everywhere (`generic_type_words`); `a stack of
whole numbers with …`, `a new stack of text`, or `a stack with items [1,
2]` (the fields decide; `arguments_given`). In a `to …:` header `any
NAME` (bounds before the name: `any comparable thing`, `any hashable and
equatable key`) introduces a type variable at its first mention
(`type_variables_of`); later mentions, in the header and body, are the
bare word (`vocabulary::type_variables` while it is read). The phrase is
Core `(fn NAME (forall …) …)`; its template has `ITEM` for the variable
and a generic record of variables as `type_name::arguments`, so the filter
keeps a use's types (`push 3 onto` a stack of text doesn't read). A
generic record's field in a use has the use's types (`in_use`).
`value_kind::arguments` holds a record's types (an unknown one matches
any). A type variable reads wherever a trait goes and in arithmetic and
comparisons: Ovis Core checks its bounds once. `a stack of (comparable
item) is comparable:` and `a stack of (item) is a container:` are impls
for every stack; `a whole number is hashable:` (`text is …`) one for a
number or text (§4 of the proposal). An untyped parameter given two types
says to write `(a: any thing)`.

**Programs and C.** All files share one `program_context`: the vocabulary, the asker and its
budget, phrase names (a phrase is defined in one file only), and the
top-level variables of the files read so far (assigning one in a later
file makes that file's own). Units carry their module; the prelude is
module `prelude`; then `resolve_modules` (helper rule, names) and
`resolve_c_imports` run. `use [words] "x.h" [words "lib"]` lowers to
`(use-c …)` and imports the header while headers are collected, so its
functions (`name(args)`) and constants work on every line; `ref x` as a
call argument is `(ref x)`. Header hashes go in each file's lockfile, with
a warning when one changed.

**Questions** are in [questions.md](questions.md): the unclear span and
its kind, candidates (the span's words in phrases, and small edits of the
span read by the parser), the rules, the ranking shortcut, `choose`, the
rewrite, and `front_end_result::decisions`.

## 6. Ovis Core

S-expressions (`;` comments) with `int float bool str ptr`, numbers of a
size (`i8`…`u64`, `f32`), records,
traits and lists; top-level `fn extern link use-c record trait impl` and
statements (run in `main`); names across modules resolved by
`resolve_modules`; `defer`; by-reference records and dispatched trait
values. The full reference is [ovis-core.md](ovis-core.md); the canonical
list of built-ins is `builtin_names()` in `libc.cpp`.

## 7. Elaboration (`elaborate.cpp`)

Deterministic, run on the whole program before checking. Each decision is
emitted as a note, which becomes a `@[note: …]` sigil in plain English.

1. **Type inference.** A slot per variable, parameter, return value and
   expression. A generic's type variable is a rigid slot (`kind::variable`,
   only itself); each call of a generic function instantiates its
   variables as fresh slots (`instantiate`), written to
   `expression::type_arguments`; a generic record's use has a slot per type
   (`slot::arguments`), joined across uses, its fields instantiated with
   them. A number written in the program meeting a variable takes it.
   - Union-find for equality.
   - Flow edges for "value goes into" relations, with widening
     int → float and numbers → text for `+`. Sizes widen to their common
     type; a number written in the program is a *soft* slot taking the size
     of what it meets, 64 bits in a variable it reaches alone.
   - Pinned slots for stated types.
   - Records unify by name. Each declared field has a slot; `field` and
     `set-field` join their value to it once X's record is known. If
     nothing else says what X is, it is the only record with that field
     (`(fn shift (p) …(field p x)…)`). Field types left out are inferred
     from `new` and `set-field` values, with a note.
   - Fallback hints apply only to slots left unconstrained.
   - Unconstrained types default to `int`. A function with no valued
     `return` is `void`.
2. **Name repair.** An unknown name is read as the single closest known
   name: equal ignoring case, `_` and `-`, or within edit distance 1 (names
   of 4–6 characters) or 2 (7 or more). If two names tie, nothing is
   changed.
3. **Implicit return:** the last line's value, when the function declares no
   `return`.
4. **Number as condition** means `!= 0`.
5. **Missing trailing pointer argument** of a non-variadic C function becomes
   `null`.

The checker adds notes for printf conversions (float shown by `%d`, int
shown by `%f`), applies numbers.md §3 (`give_number_type`,
`unify_numbers`) and copies a function taking `int[]` for a list of a size
(`copy_for_sized_lists`, for a program's own phrases). **Generics** are
monomorphised there: a generic body is checked once, a variable allowing
what its bounds do (`meets`, `expect_trait`); a call is checked against the
bounds and, with concrete types, calls a copy made once (`make_copy`:
`push__int`); impls for every use of a generic record are copied per use
(`copy_generic_impls`); then `make_record_copies` makes each concrete use
a record of its own (`stack__int`) and removes the generics. A copy
compares, shows and sorts records by their authors' phrases. A generic C
macro on a type variable's values is asked about in each copy.

## 8. Code generation and runtime

- Type mapping:
  - `int` → i64, `float` → double, `bool` → i1; a number of a size its
    LLVM type, converted where stored by `coerce` (`convert_number`,
    narrowing checked); the module has the host's data layout before code
    is generated, so `size of` and record layouts match C's.
  - `str` and `ptr` → ptr
  - a list → ptr to `{i64 length, i64 capacity, char* data}`. Lists are
    heap-allocated, have reference semantics, and are bounds-checked. Bools
    are stored as bytes inside lists.
  - a record → ptr to the struct `ovis.record.NAME` of its tag (8 bytes)
    and fields (a packed record: an LLVM packed struct, no tag),
    allocated zeroed by `ovis_record_new`. Only `(maybe (record NAME))`
    (`NAME or nothing`) and C records may hold null; the checker lets a
    field be read only once a check narrows the value (`if`/`while`
    conditions, `and`/`or`, leaving early, assignment of a record that
    isn't nothing). `ovis_record_check` still guards fields at run time
    (casts, C records).
- `defer`: `generate_block` keeps a frame of the defers reached so far,
  copied (last first) to every exit of the block: falling off its end,
  `break`/`continue` (down to the loop body's frame depth), `return` (after
  its value is worked out), the end of `main`. While a frame is emitted
  the frames above it are cut off, so a nested defer belongs to its body.
  The surface writes `@[note: runs at the end of every pass of the loop.]`
  (or "of this branch"/"of this block") above an `at the end` that is not
  in a function or at the top level (`note_defer`; `written_source` keeps it
  byte-identical on rebuilds). Names first set at the top of an `at the
  end:` block are its own, never hoisted.
- **Memory and checked builds**: arenas (`with-memory`), `free`, `keep`,
  the checked table, `--report-leaks` and `check_memory` are in
  [memory.md](memory.md).
- `print` of a record writes it as the surface creates one: `a book with
  title "Dune", pages 412`; inside a list or another record it is
  bracketed, text is quoted, and null is `nothing`. Each record gets an
  `ovis.print.NAME` function, so records holding their own kind work.
- User functions have internal linkage and are named `ovis.NAME`, so they
  never clash with libc. Top-level code becomes `int main()`, which calls
  `ovis_init()` first.
- The C runtime (`runtime.cpp`) is embedded as a string, written next to the
  object file at link time, and compiled by `cc`/`clang`/`gcc`. It provides:
  - lists
  - printing (floats print `%.1f` when whole, otherwise `%.15g`)
  - text helpers
  - input, sorting and randomness
  - stdio handles and checked integer division
- Calls into C convert between Ovis and C types (`c_type` in `libc.hpp`):
  - C `int` is 32 bits: arguments are narrowed as at any store (checked,
    numbers.md §3), results sign-extended to whole numbers;
  - `float` is narrowed and widened;
  - a bool passed as a C `int` is zero-extended.
- printf-family calls with a literal format get every integer conversion
  rewritten to `ll`, and arguments converted to what each conversion shows.
- Linking flags (`libraries_of`): `-lm`, every `(link ...)`, and the flags
  of imported C libraries.
- Under CMake `ovis build --object` stops at `emit_object`; CMake links the
  object with the runtime (`ovis runtime` writes its C source) and the C
  libraries. `--locked` fails instead of asking or writing; see
  [c-libraries.md](c-libraries.md) and [proposals/cmake.md](proposals/cmake.md).

### C libraries

`(use-c "zlib.h" "z")` reads the header with libclang; types, `ref`, flags,
macros (a C macro of one expression or of statements is a generic C
phrase: each call is a copy for its arguments' C types, asked of clang,
kept in the lockfile's `c_macros`) and what is skipped are in
[c-libraries.md](c-libraries.md).

## 9. Model backends

`language_model::choose(messages, n)` gives the probability of each option
`1`..`n` and `embed(texts)` unit vectors; backends (built-in llama.cpp, the
daemon `ovis build`/`run` use, an endpoint) and weights lookup are in
[model-backends.md](model-backends.md). Answers are bit-identical across
the daemon and in-process, so lockfiles don't depend on who answered.

## 10. Lockfile

`<file>.lock`, `ovis::phrase::lockfile`, version 3 (the old pipeline's
version 2 is refused): `model`, `model_fingerprint`,
`c_headers`, `readings` of `{key, span, reading}` for questions the
model settled, and `meanings` of `{line, meaning}`: each meaning written
and the line it was for (only when there are any). The key is an FNV-1a hash of the line, the span, the names
the model is told about and the candidate readings, so any change asks
again; an entry is used only if its reading is still a candidate. Entries
are kept while the question was asked or a meaning in the source still
carries them, sorted by key, so rebuilding changes neither file.

**Determinism holds on one machine and build.** The model gives the same
answers for the same inputs there (greedy choice, fixed prompts). Another
CPU, a GPU, or another build of llama.cpp may round differently and settle
a close question the other way. What makes builds the same across machines
is the committed lockfile and the `@[meaning: …]` sigils, which are read
before the model is asked; no fingerprint is kept per entry. Commit them.
Builds meant for other machines use `-DOVIS_PORTABLE=ON` (no native CPU
tuning; AVX2/FMA/F16C stay on for x86-64, NEON on ARM). `ovis run` saves it when it changed, after a failed build only if the
model answered something.

## 11. Invariants

1. Building a program with its meanings and lockfile changes
   neither file and needs no model (idempotent). The tests check this on
   every example.
2. The same weights, request and machine give the same `choose`
   probabilities.
3. Every reading offered is one the line reads with and that type-checks
   in the parser's filter; the elaborator and checker run once on the
   whole program after.
4. A meaning wins over everything, unless the lockfile says it was
   written for another line (then it is dropped and the line read again).
5. The source file is replaced atomically (write to a temporary file, then
   rename), and only when its text changes. `--keep-source` disables
   writing it.
6. Reports cite line numbers of the file as it is on disk after the build:
   problems are `diagnostic`s until the source is written, then numbered
   by the writer's `line_map` (as read when `sources_written` is false:
   `check`, `--keep-source`).
7. A number's size is written by the author or is 64 bits: never inferred
   from its values, never chosen by the model, and a number that doesn't
   fit where it is stored is an error, not silently cut (unless the line
   says `wrapped to`, `clamped to`, `wrapping` or `clamped`).
8. ovisup replaces an installed toolchain only with a package whose
   checksum matches and whose `ovis --version` runs here, swapping it in
   whole; it removes only what it added (`~/.ovis`, profile lines with its
   marker; the models only when asked). [deployment.md](deployment.md).
9. A dependency's files are read, never written or asked about: its own
   meanings settle its lines, and the same ovis.lock builds the same graph
   (commits and tree hashes; [projects.md](projects.md)). A file reads
   with a dependency's phrases only where it says `use NAME`, and a line
   only an unused dependency reads is reported, never asked about. A
   question offers a dependency's phrase only if it shares a word of
   meaning with the span. What a file under `X/internal/` defines (a
   program's or a package's) is seen by the files under `X/` alone; a line
   only such a hidden phrase reads is reported, never asked about.

## 12. Code map

| Path | Role |
| --- | --- |
| `src/ovis/phrase/format.cpp` | `ovis fmt`: sigils put in order, old annotation lines migrated |
| `src/main.cpp` | CLI: run/build/ir/check/fix/fmt/phrases on a file or folder; in a terminal (not `--quiet`/`--verbose`) a live status line (`status_line`: spinner, stage from `front_end_options::progress` and codegen, elapsed, "loading the model" on the first slow question) with dimmed event lines and a summary in words; `-I`/`-L`, `model path|pull|embed|serve|stop`, the daemon or in-process model, writing sources and lockfiles back, offering fixes in a terminal |
| `src/ovis/phrase/` | the front end: `lexer`, `templates`, `parser` (chart parser, line shapes), `questions` (candidates, `choose`), `front_end` (programs, reading, lowering), `lockfile` |
| `phrases/*.phr`, `*.core`, `*.generic` | the phrase library, its Ovis Core prelude and generic functions, compiled in by `cmake/embed_phrases.cmake` |
| `src/ovis/language_model.cpp` | `choose` and its softmax, the HTTP endpoint model |
| `src/ovis/builtin_model.cpp` | llama.cpp inference, model lookup, weights embedded in `ovis`, `embed` |
| `src/ovis/model_daemon.cpp` | `ovis model serve`, `daemon_model`, the socket's place and name |
| `src/ovis/sexpr.cpp`, `ast.cpp` | Ovis Core reader and lenient parser |
| `src/ovis/elaborate.cpp` | type inference and deterministic fill-ins |
| `src/ovis/monomorphise.cpp` | generics: substitution, matching, copies' names, field generics' copies, generic records' copies |
| `src/ovis/checker.cpp` | name/type/format checking, messages with types in English |
| `src/ovis/libc.cpp` | libc binding table, C constants, built-in names, printf format parsing |
| `src/ovis/c_import.cpp` | libclang header import, pkg-config flags, a release package's own clang headers (`bundled_resource_directory`) |
| `src/ovis/modules.cpp` | joining modules: naming, resolution, the helper-file rule |
| `src/ovis/llvm_codegen.cpp` | IR generation, optimisation, object emission, linking |
| `src/ovis/runtime.cpp` | the embedded C runtime |
| `tools/eval.cpp` | `ovis_eval questions` and `programs`: the model and ranking measured |
| `test/` | Catch2 tests: Core and codegen (`core_test`, `number_test`, `model_test`, `c_import_test`, `modules_test`, `docs_test`, `ovisup_test`, shared helpers in `support`, records in `record_test`, traits in `trait_test`), the front end (`phrase_test`, `function_test`, `question_test`, `program_test`, `record_phrase_test`, `trait_phrase_test`, `defer_test`), the library (`library_test` with `phrase_library.txt`), `choose` (`choice_test`, with a fake HTTP server) |
| `test/clib/` | a small C library the tests import (every type-mapping case) |
| `src/ovis/project/` | `ovis.toml` (`toml`, `project`), a project's C by cc (`native`), the generated CMake project (`cmake_project`), `ovis.lock` (`lock`) ([projects.md](projects.md)) |
| `tools/ovisup`, `tools/package.sh`, `.github/workflows/release.yml` | installing, updating and removing Ovis; relocatable release packages with LLVM and libclang inside ([deployment.md](deployment.md)) |
| `tools/make_tiny_model.py` | tiny random GGUF for testing inference plumbing |
| `eval/` | `questions.txt` and `programs/`, labelled with the readings meant |
| `examples/` | example programs; `statistics.ovis` has a model's reading, `vague.ovis` a question, `shop/` several files |

# Proposal: a manual the model searches, and checking a line when it is done

Status: **accepted**, then narrowed (the owner's idea, 2026-10-23). On
2026-10-24 the owner dropped all editor work, the VS Code extension
included: the language matches English to syntax, so completion and
highlighting aren't needed, and the smart manual is the help. What stays
is plan L1–L10 and R1. The editor section (§3, V1–V12) is archived in
[dream/state](../dream/state/2026-10-24-editor-plans-dropped.md); `ovis
lsp` was removed too.

The owner's idea: Ovis has a model and little documentation, so the model
should help an author find and understand the phrases Ovis offers, a
"smart manual". In the editor, completion doesn't fit (the compiler
already matches whole lines), so tools help in three ways: highlighting,
adding sigils (checking a line's phrases), and sending the author to the
manual when a line reads no phrase. A phrase is read as a whole, not word
by word, and asking the model costs time, so a line is checked only once
it is finished.

## 1. What exists, and what's missing

Already built ([quick-fix.md](quick-fix.md), [questions.md](../questions.md)):

- `ovis phrases WORDS`: the 12 built-in phrases whose words are like
  WORDS (`closest_templates`: the same word or a shared 4-letter start).
  With no words, all of them by file.
- A report of a line nothing reads ends with "Phrases with these words:",
  up to 4, from the same search.
- `ovis lsp` and the VS Code extension: problems underlined, a quick fix
  per reading (writes `@[meaning: …]`), hover on a meaning, sigils dimmed,
  a TextMate grammar (sigils, comments, texts, numbers, a fixed list of
  keywords). The program is checked when a file is opened or saved, with
  no model.
- Embeddings of phrases (kept, off: `--embeddings-only`) and `choose`
  with "none of these".

Missing, and why it matters:

1. **Nothing describes a phrase.** A template is a pattern, its aliases
   and its types. The `#` comments above some templates are dropped when
   they're read, and the examples are test programs
   (`test/phrase_library.txt`), not in the binary. So `ovis phrases` can
   show `TEXT in capitals` but can't say what it does or show it in use.
2. **The search needs the phrase's words.** "How do I make every
   letter big" shares no word with `in capitals` or its aliases (`in
   upper case`, `in uppercase`), so the lexical search misses it. Synonyms are what the model (and embeddings) are for.
3. **Only the library is searched.** `ovis phrases` knows the built-in
   phrases, not what the program and its packages define: their phrases,
   records, fields, traits and values, its variables, its C functions.
   And what a line can use depends on its file (`use NAME`, `internal/`,
   `use "x.h"`) and, for variables, on where the line is (§2.4).
4. **Highlighting can't know phrases.** A TextMate grammar sees words, and
   which words are a phrase's and which are names depends on the program.
   Only the parser knows.
5. **Checking waits for a save.** Problems go stale while typing; on the
   other hand, checking every keystroke would underline every half-typed
   line.

A note on words: an author writes phrases; Ovis Core is what the
compiler reads them into (`(sort $xs)`), and the prelude's Core functions
are behind the phrases. The manual is about the phrases (and the built-in
forms, records and C functions a line can use). It shows the Core of a
phrase only when asked (`--core`), for the curious and for library
authors.

## 2. The manual

### 2.1 Documentation lives beside each phrase

Each template gets a description and examples in its `.phr` file, as
indented lines like `alias:`:

```text
expr  {s: str} in capitals                      : str    => (prelude_upper $s)
      about: The text with every letter a capital: "Ada" in capitals is "ADA".
      example: show "Ada" in capitals
      example: shout = message in capitals
```

- `about:` is one or two sentences of plain English. Required: a test
  fails for a template without one (as `[docs]` does for the tracking
  documents), so the manual can't fall behind the library.
- `example:` lines are checked by a test: each reads, alone or after
  the template's test program in `phrase_library.txt` (which gives it its
  names), to this template. An example that stops reading fails the
  build, so the manual never shows a line Ovis can't read.
- What a program or package defines is documented by its `#` comment
  lines and its own uses (§2.4), with no new syntax for authors.

This is the base of everything below, and useful without the model:
`ovis phrases` shows `about` and the first example.

### 2.2 `ovis manual`

```text
$ ovis manual how do I make every letter big
TEXT in capitals                                         (text)
    The text with every letter a capital: "Ada" in capitals is "ADA".
    show "Ada" in capitals
    Also: TEXT in upper case, TEXT in uppercase

TEXT in small letters                                    (text)
    ...
```

- **Finding.** Three steps, cheapest first, like a question:
  1. Lexical (`closest_templates`), over the pattern, aliases and
     `about`.
  2. Embeddings of the query against each entry's pattern and `about`
     (the embedding code we have; entries embedded once and cached by
     the library's hash). This finds synonyms (`big letters` → `capitals`).
  3. When the top few are close, `choose` is asked which entries do what
     the query asks, with "none of these". Only entries found in 1–2 are
     options.
- **The model finds; it never writes.** Every word shown comes from the
  library: the pattern, `about`, examples, aliases. The model only orders
  real entries, the job it already does for questions (the owner's
  decision: no generative job). It can't invent a phrase, and an answer
  is as right as the documentation.
- **What a line can see.** In a program (`ovis manual --in FILE QUERY`),
  the entries are those in sight of FILE: the library, the program's own
  phrases, the dependencies it `use`s, its records and its C headers. An
  entry from a package names it ("from `geometry`"). Without a program,
  the library.
- **No model, still a manual.** `--offline` (or no model installed) uses
  steps 1–2 only. `ovis manual` with no query lists every entry by topic
  (the `.phr` file), and `ovis manual --html DIR` writes the whole
  reference as pages (for the website, or a package's own).
- **From a line.** `ovis manual --line main.ovis:12` searches with the
  line's words of meaning (as "Phrases with these words" does) and shows
  each entry's example rewritten with the line's own names where the
  slots fit, read by the parser before it is shown.

### 2.4 What the program and its dependencies define

The library is a small part of what an author needs to look up. Most of
a program's words are its own and its packages': `to …:` phrases,
records and their fields, traits, top-level values, variables, C
functions. They are entries too, and the same search finds them.

**Where entries come from.** Not from a second reading of the program:
from the check that already runs (on a line done, a save, or `ovis manual
--in`, which checks without the model). Every file's `to …:` headers,
records and traits are declared before any line is read, so they're
entries even while some lines still have problems. The index is per
file (what that file sees) and as fresh as the last check of the
editor's text.

| Entry | What it shows, from the compiler | Its documentation |
| --- | --- | --- |
| A `to …:` phrase | its pattern with slots as kinds, the result, where it's defined | the `#` lines above its header |
| A record | its fields with types and defaults, how a type of it is written (`stack of books`), the traits it implements | the `#` lines above `a NAME has:`, and above each field |
| A trait | its required and default phrases, the records and types that implement it here | the `#` lines above `a NAME can:` |
| A top-level value (a package's constant) | its type and value, if fixed | the `#` lines above it |
| A variable | its type, where it is first set and last set before this line, its fields if a record | the `#` lines above where it's first set |
| A parameter | its type and its phrase | the phrase's |
| A C function, constant or struct | its C signature and Ovis types, its header | the header's comment, when libclang gives one |

- **Examples are the program's own lines.** A program's phrase or record
  needs no `example:` lines: the lines that read to it are examples,
  already checked. The manual shows up to two, with their file and line
  (and a link in the editor). A package shows its own uses the same way,
  and may add `# example: LINE` comment lines for a phrase it never calls
  itself, checked when the package is checked (and by `ovis release`).
- **Nothing undocumented is missing.** An entry with no comment still
  shows what the compiler knows: its pattern, types, fields, where it's
  defined and used. The model doesn't fill the gap with a summary
  (decision 1).
- **Variables depend on the line.** A name means something only from
  where it's set. `ovis manual --line main.ovis:12` (and hover on a name
  in the editor) includes the names in scope there, their types, and
  where each was set; a name set further down isn't offered. Variables
  are found by their words like anything else (`how many guests` finds
  `guest count`), but only in the line's scope.
- **"What can I do with this?"** `ovis manual --for NAME` (or a record or
  type: `--for book`) lists the phrases whose slots take its type, the
  same type check questions use: for `scores`, a list of numbers, `sort
  …`, `total of …`, `average of …`; for a `book`, the program's own
  `to lend (b: book) to (p: person):` and the traits' phrases a book
  implements. This search needs no words, and so no model.
- **Packages, by what the file sees.** A dependency's entries name the
  package and its version line ("from `geometry` v2"); two lines side by
  side are two entries. A direct dependency the file doesn't `use` is
  listed after the rest, marked "add `use geometry` to use it" (as the
  `needs_use` problem says). Another folder's `internal/` and a
  dependency's dependencies are never listed: the file can't use them.
- **Same words, two meanings.** When the program's phrase and the
  library's (or two packages') answer one query, both are shown with
  where each is from, and which one a line would read (the parser's
  answer, not the model's).
- **Embeddings are cached per entry.** The library by its hash, a
  package by its tree hash in `ovis.lock`, the program's entries by each
  entry's text, in the build folder: after an edit only the changed
  entries are embedded again.

### 2.5 C's entries

What a file's `use "x.h"` lines import are entries like the rest:
functions, constants (`EOF`, `Z_OK`), structs and unions (records, with
their fields), function-like macros and generic C macros, and the structs
their functions reach in other headers (`struct tm` for `gmtime`). Only
the headers the file `use`s, and the one C library per program its
packages share.

- **What an entry shows.** The C signature beside the Ovis types it's
  called with (`char *` as text, `struct S *` as the record `S`, a struct
  by value through its wrapper), whether it's a macro, and its header and
  library.
- **Few words to find it by.** C names aren't English. The search splits
  them into words (`deflateInit` → `deflate init`, `strncpy` → `str n
  cpy`) and adds the parameters' names; synonyms (`compress some bytes`
  → `compress`, `deflate`) are left to embeddings, which matter more
  here than for phrases.
- **Documentation when the header has it.** libclang gives a
  declaration's doc comment (`/** … */`, `///`): zlib and SQLite have
  them. Many headers don't, and their entries show the signature alone;
  the model doesn't write one (decision 1).
- **Big headers.** `stdio.h` or `sqlite3.h` bring hundreds of entries.
  A header's entries are indexed and embedded once, cached by its
  contents (and the flags it was read with). The functions the program
  already calls rank first on ties, and their calls are the examples.

### 2.6 Measured, like the questions

`eval/manual-queries.txt`: queries in an author's words, about the
library, about the programs in `eval/programs` and a test package, and
about C (zlib, SQLite, the C standard library) (held out, written
far from the phrases' words, as `hard-questions.txt` is) and the entry
meant. `ovis_eval manual` reports the entry meant first, and in the first
5, for lexical alone, with embeddings, and with the model; CI keeps a
floor without the model. The model is used here only if it beats
embeddings by enough to pay for its time.

## 4. Plan

| Step | What | Test | Size |
| --- | --- | --- | --- |
| L1 | `about:` and `example:` lines in `.phr`; kept on `template`; `#` comments above `to …:` headers kept as their `about` | every template has `about`; every example reads to its template | M |
| L2 | `about` and examples written for the 171 templates | the L1 test | M |
| L3 | `ovis manual QUERY` lexical over pattern, aliases, `about`; `--in FILE` (phrases in sight), `--line`, no query lists by topic; `ovis phrases` shows `about` | the entries for known queries, in order | S |
| L7 | The program's and packages' entries (§2.4): from the check's vocabulary, `#` comments as documentation, uses as examples, origin and version, unused dependencies marked | the entries of `examples/shop`, a test package, a file that doesn't `use` it | M |
| L8 | Variables in scope at a line (`--line`); `--for NAME\|TYPE` | the names and phrases for known lines | S |
| L9 | A package's `# example:` lines, checked by `check` and `release` | a broken example fails | S |
| L10 | C's entries (§2.5): what `use "x.h"` imports, signatures beside Ovis types, doc comments from libclang, names split into words, cached by header contents | the entries of the test library, zlib and SQLite (skipped without their headers) | M |
| L4 | Embeddings for the manual (cached by library hash, tree hash, entry) | `eval/manual-queries.txt`, `ovis_eval manual` | M |
| L5 | `choose` among the entries; measured against L4 | the same eval; CI floor without the model | S |
| L6 (done) | `ovis manual --html DIR` | pages for every entry, links checked | S |
| R1 | **Version 0.1.0**, when every step above is done: `CMakeLists.txt` and `ovisup_version` say 0.1.0 (one version for the toolchain from now on, checked by a test); state.md and deployment.md updated; then the tag `v0.1.0`, which `release.yml` publishes as the first `stable` | the test that the two versions agree; `release.yml` refuses a tag that isn't the version; ovisup installs `stable` | S |

Order: L1–L3, L7–L8, L10 (useful without the model), then L4–L5 (the
model, measured), L6 and L9. R1 is last: version 0.1.0 is the release
with the manual in it.

## 5. Decisions (owner, 2026-10-23: every recommendation)

1. **The model's job in the manual.** Decided: it orders real entries
   and never writes the explanation (the answer is the documentation).
   Not taken: a sentence written by the model about the query,
   which reads nicer but can be wrong with nothing to check it.
2. **Where documentation lives.** Decided: `about:` / `example:` in
   the `.phr` files, checked by tests. Not taken: a separate manual
   written by hand, which falls behind the library.
3–5. **Editor decisions** (sigils from the editor, completion, when a line is done): dropped with the editor work, 2026-10-24.
6. **Documenting the program's own words.** Decided: `#` comment
   lines above a definition, and its own uses as examples, so authors
   write nothing new. Not taken: `about:`-style lines in programs
   too, more uniform but asking authors for syntax most won't write.
7. **The extension marketplaces**: dropped with the extension.

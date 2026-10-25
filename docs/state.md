# Project state

Last updated: 2026-10-25 (L5, the model's sure pick first in `ovis manual`; L4 with EmbeddingGemma; L6, `ovis manual --html`; R1, version 0.1.0; L10, C's entries in the manual; L9, a package's `# example:` lines; phrase scope S3, dependencies' phrases need a shared word; S2, `use NAME`; phrases in sight measured; packages H1–H9, projects J1–J8; deployment:
release packages and ovisup)
(branch `claude/llm-matching-failure-hypothesis-xwp3nu`).
Last dreamed: 2026-10-25 at e79728f (history to the manual's plan, scope S1–S5, archived).
[dream/state](dream/state/INDEX.md).

Ovis is a compiler whose source code is plain English: lines of phrases,
read by a deterministic parser, with a built-in model choosing between
readings only where a line is unclear. The design is in
[architecture-spec.md](architecture-spec.md) and the open work is in
[remaining-tasks.md](remaining-tasks.md).

## Status at a glance

| Area | Status |
| --- | --- |
| Phrase front end: lines, blocks, phrases, `to …:` phrases, calls | Built, tested (front end, functions, programs) |
| Phrase files read at run time: `phrases/` beside a folder program's `main.ovis`, or `--phrases DIR` (`.phr`, `.core`, `.generic`); `ovis check-phrases PROGRAM` | Built, tested (`program_test`, `fix_test`) |
| Phrase library (171 templates, prelude, generic functions) | Built; every template run by a test program |
| Numbers of a size and bits ([proposals/numbers.md](proposals/numbers.md), N1–N7): `int8`…`uint64`, `float32`, `byte`, `short`, `16-bit whole number` as types anywhere; arithmetic at the size, overflow and narrowing checked; `as`, `wrapped to`, `clamped to`, `wrapping`, `clamped`; bits, bytes, hex/binary literals and output; sized record fields, `a header has, packed:`; lists and map values of a size | Built, tested (`number_test`, `phrase_library.txt`, `examples/bits.ovis`, `examples/hash.ovis`); measured with 3B (`eval/programs/checksum.ovis`) |
| `at the end` / `defer` (D1–D3): `(defer …)` in Ovis Core, copied to every exit of its block; the surface forms with the `@[note: …]` sigil | Built, tested (`defer_test`, `examples/notes.ovis`, `eval/programs/tidy.ovis`) |
| Built-in traits: slot types `showable`, `comparable`, `equatable`, `T:trait`; a record implements one with `to (a: book) comes before (b: book):`, `is the same as`, `text of`, or `a book is comparable:` (equatable, showable); `nothing` only in `T or nothing` and C records, read after a check (narrowing); lists of records with `text of` show as `[a, b]` | Built, tested (`record_phrase_test`, `phrase_library.txt`); static dispatch |
| User traits: `a NAME can:` (required and default phrases, `it` the value), `a NAME is a TRAIT:`; a trait is a type (`(s: shape)`, `list of shape`, mixed lists), phrases on it dispatch when the program runs; untyped values used only through one trait's phrases get that trait; numbers and text have a trait statically (`a whole number is hashable:`) | Built, tested (`trait_test`, `trait_phrase_test`, `generic_test`, `examples/shapes.ovis`) |
| Generics ([proposals/generics.md](proposals/generics.md) G1–G8): Core `forall`/`(var T)`, copies per use checked once against bounds; generic records (`a stack of (item) has:`, `stack of books`) and their impls; `any NAME` with bounds in headers; typed memory (`new room for 4 items`); the library's element-typed phrases and field-slot phrases as generics; C macros copied per call's C types | Built, tested (`generic_test`, `generic_phrase_test`, `c_import_test`, `checked_test`, `examples/stack.ovis`, `examples/collections`); measured against the built-ins (below) |
| Questions: rules, ranking shortcut, `choose`, rewrites, `@[meaning: …]` sigils, lockfile | Built; tested with scripted models; measured with 0.5B–7B (M0, archived; M1.14 below) |
| Deterministic elaboration, checker, LLVM codegen, C runtime | Built, tested by compiling and running programs |
| Records in Ovis Core (`record`, `new`, `field`, `set-field`) | Built, tested (`record_test`) |
| Records in phrases: `a NAME has:`, `a NAME with …`, fields read and set 3 ways, `field of T` slots, questions about fields and `the NAME` | Built, tested (`record_phrase_test`, `phrase_library.txt`, `examples/library.ovis`); measured with 3B (eval programs, below) |
| Explicit memory: `with new memory:`, `new memory`, `with M:`, `clear`, `free`, `keep`; checked builds (`--fast` off, `--report-leaks`); strict rules (on; `--lax-memory` off) | Built, tested (`memory_test`, `checked_test`, `memory_check_test`, `phrase_library.txt`); the examples follow them; `examples/visits.ovis` has a named memory and `clear` |
| Building with CMake: `--object`, `--link-by-caller`, `@FILE`, `--locked`, `--depfile`, `ovis runtime`, `find_package(Ovis)`, `ovis_add_executable` | Built, tested (`cmake_test`, `examples/cmake`, CI) |
| C standard library, any C library via `use "x.h"` (libclang, `ref x`, pkg-config) | Built, tested with a test library, real zlib and SQLite |
| Multi-file programs, nested modules, helper-file rule | Built, tested (`examples/shop`) |
| Built-in llama.cpp model, embedding, lookup, `ovis model pull` (3B) | Built; determinism tested with a tiny random model |
| Model daemon (`ovis model serve`, started by builds, in-process fallback) | Built; tested with a scripted model and, `[model]`, started by `ovis` |
| Deployment ([deployment.md](deployment.md)): `ovisup` sets up, updates (`stable`, `nightly`, pinned versions, `ovis +T`) and uninstalls; `tools/package.sh` makes relocatable packages with LLVM, libclang and clang's headers inside; `release.yml` publishes tags and nightly; `ovis --version` | Built, tested (`ovisup_test`); a Linux x86-64 package installed by ovisup ran the examples in a clean `ubuntu:24.04` container; on GitHub, nightly published from `main`; version 0.1.0 in CMakeLists.txt and ovisup (R1), the tag `v0.1.0` not yet made |
| Projects ([projects.md](projects.md), J1–J8): `ovis.toml` (a TOML subset), `ovis new`/`init`, commands with no program; a project's C files and pkg-config built by cc; git, path and cmake tables through a CMake project generated into `build/` each build; `ovis.lock` pins git tags; `ovis update`, `ovis cmake`; the toolchain line in ovisup's shim | Built, tested (`project_test`, `project_build_test`, `project_cmake_test`, `ovisup_test`, `examples/project`) |
| Packages on git ([projects.md](projects.md), H1–H9): versions as tags, minimal version selection per line, ovis.lock with commits and tree hashes, a read-only cache and `vendor/`; packages read locked with the program; scope per file (own + direct dependencies), Core names per package, two lines of one package side by side; `NAME's phrase`; nested `internal/` (S4, programs too); the orphan rule; a package's C and one C library per program; `ovis add`/`remove`/`update`/`tree`/`fetch`/`vendor`/`release` | Built, tested (`packages_test`, with git repositories the test makes) |
| GPU (Metal etc.) | On by default, `--cpu` to turn off. Measured by the owner on a Mac (M0.5, below) |

## Verified

- `./build/test_ovis`: 256 test cases (253 pass, 3 skipped) and 3544 assertions pass. Three cases are skipped unless
  `OVIS_TEST_MODEL` is set. The C-library cases skip themselves when
  libclang, zlib or SQLite headers are missing. The `[docs]` case keeps the
  tracking documents under 500 lines and checks their links.
- `OVIS_TEST_MODEL=<tiny.gguf> ./build/test_ovis "[model]"`: the built-in
  model gives the same reply and probabilities from a fresh instance,
  after an unrelated request (prefix reuse), and with embedded weights.
- Every example builds offline from its meanings and lockfile, twice,
  with no file changed; `vague.ovis` reports its question (test and CI).
- **GPU (M0.5)**, by the owner on a Mac with Metal, 1.5B, `ovis_eval
  questions --compare-cpu`: 20 questions compared, the largest difference
  in a probability 0.0199, no case decided differently; ~0.23 s per
  question (1.5B on 4 CPU cores here: ~0.9 s).
- One-off manual checks of the old pipeline: archived in
  [dream/state](dream/state/INDEX.md).

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request, on Ubuntu
24.04 with LLVM 18. It:
- builds the project and runs `test_ovis`, uploading `test-results.xml`;
- builds every example offline and fails if any example file changes, and
  checks `vague.ovis` fails;
- checks every case of `eval/questions.txt` is still a question
  (`ovis_eval questions --no-model`), and that the ranking keeps its
  floor (`--min-first 11 --min-shown 23`), with the numbers in the job
  summary;
- generates the tiny random model and runs the `[model]` tests with it.

llama.cpp is built with `-DOVIS_PORTABLE=ON` in CI (`GGML_NATIVE` off, AVX2/FMA/F16C on).

A second job in `ci.yml` runs on macOS 15 (arm64): Apple clang, linked
against Homebrew's LLVM (22 at the time of writing). It builds, runs
`test_ovis` and checks the examples offline. The real-C-library test skips
there (headers are in the SDK, not `/usr/include`), and the job skips the
`[model]` tests and the question ranking, which the Ubuntu job covers.

`.github/workflows/release.yml` packages Linux x86-64 and arm64 and macOS
arm64, installs each package with ovisup on a machine without LLVM and runs
examples there, then publishes a tag `vX.Y.Z` as a release or a push to
`main` as the `nightly` pre-release ([deployment.md](deployment.md)). It
has not run on GitHub yet.

`.github/workflows/embedded-model.yml` is started by hand (3B by default,
1.5B or 0.5B). It builds `ovis` with the model embedded, publishes it as an
artifact for 14 days, and reports `ovis_eval questions` and `ovis_eval
programs` in the job summary. These steps have not run on GitHub yet.

## Memory measured

`tools/peak_memory.py` numbers for E1–E7 (checked mode 3–6× for programs
that make many values): [dream/state](dream/state/INDEX.md).

## Collections measured

`examples/collections`' growing list and hash table, written in Ovis on
generics, against the built-in list and map (G7;
[eval/collections](../eval/collections/README.md)): 200000 lists of 50
whole numbers made, read and freed; 2M whole-number keys put and looked
up. `tools/peak_memory.py`, 4 CPUs, peak MB / seconds, 2026-10-20:

| Program | `--fast` | Checked |
| --- | --- | --- |
| `list_builtin` | 11.9 / 0.09 | 11.9 / 0.18 |
| `list_growing` | 12.1 / 0.19 | 12.1 / 1.07 |
| `map_builtin` | 68.1 / 0.32 | 68.2 / 0.43 |
| `map_table` | 105.5 / 0.56 | 137.8 / 2.01 |

Unchecked, the Ovis list takes twice the time (each `item` is a call with
its own bounds check, against the runtime's inlined `get`) and the table
1.75× the time and 1.5× the memory (three rooms of 2^22+ places at half
full; the runtime's table packs keys and values). Checked, each read and
write through room is looked up in the checked table (a ring of the rooms
used last makes it 5–6×). So the built-ins stay: replacing them (G7) waits
on inlining `item` and a packed table.

## Phrase front end: questions measured

M0's measurements of ranking and of 0.5B–7B on `eval/questions.txt` (3B
settled 21 of 26, all right; decided: 3B is the default) are archived in
[dream/state](dream/state/INDEX.md).

Ranking now (CI's floor): the right reading first in 11 of 24 and shown in
the first question in 23 of 24. M0.4 measured 13 and 24 with 8 readings;
since then (the library, aliases, 9 shown) "3.7 rounded down" fell to 10th
of 14, so the 9 shown miss it.

Why, and what was tried (2026-10-18, archived in
[dream/state](dream/state/INDEX.md)): the right reading always ties for the
top lexical score, which is blind to synonyms, so ties fall to library
order. Breaking ties by embeddings ranked it first far more often but
didn't raise the model's right picks; option order alone moves 3B by 3 of
26 (`--orders`: 22–23, mean 22.7); embeddings alone get 20–21 of 26 but
every miss is a wrong pick, never "none". Code kept, off
(`--embeddings-only`).

Hard wording (`eval/hard-questions.txt`, 20 held-out cases: one phrase,
values written plainly, words far from the phrase's), 4 orders:

| Settled by | Right (of 20) | Confident picks right | Per question |
| --- | --- | --- | --- |
| Embeddings only, 0.5B / 3B | 4 / 4 | (always picks) | — |
| 3B asked | 12–15 (mean 13.5) | 13 of 19 (68%) | 2.3 s |
| 7B asked | 16–18 (mean 17.5) | 16 of 18 (89%) | 5.3 s |

Embeddings collapse: shared words give a wrong reading a top score of its
own (`how many folks` → `length`), so ties rarely reach them. 3B stays
ahead but is confidently wrong on 6. Model size is what reaches hard
wording; the cost is the time per question (4 CPUs here).

Cheaper or steadier questions (2026-10-19, 3B, 4 orders, right of 26 /
of 20, mean):

| Variant | `questions.txt` | `hard-questions.txt` | Asks |
| --- | --- | --- | --- |
| As built | 23.0 (19 in every order) | 11.2 (4 depend on order) | 156 / 152 |
| Both orders averaged (`ask_both_orders`) | 23.2, sure picks 100% right | 11.8 (1 depends on order), sure 80% | ×2 |
| Skip a run-off one sure group settles | 23.0 | 11.2 | −1% / 0 |
| An argument all options repeat said once (`it`) | 21.2 | — | −13% time each |

Both orders steadies the answer for twice the time: kept as an option
(`ovis_eval --both-orders`), off. The run-off skip almost never applies,
and `it` for the shared argument costs right picks: neither kept.

## Model daemon measured

`ovis run`, 4 CPUs, weights in the OS file cache, median of 5 (3 for 7B),
each build asking again:

| Program | 3B: no daemon / starts it / loaded | 7B: no daemon / starts it / loaded |
| --- | --- | --- |
| no question | 0.24 / 0.23 / 0.23 s | 0.20 / 0.21 / 0.22 s |
| 1 question | 3.21 / 3.73 / 2.52 s | 6.06 / 8.00 / 5.25 s |
| 3 questions | 7.67 / 8.27 / 6.98 s | 16.57 / 18.71 / 15.93 s |

The daemon saves 0.7–0.8 s a build (4–21%); starting it costs 0.5 s (3B)
or 2 s (7B) once. Loading from the file cache is cheap (mmap); answering
is the cost: about 2.3 s (3B) and 5.3 s (7B) per question on 4 CPUs,
against ~0.23 s for 1.5B on a Mac GPU (M0.5).

Where a question's time goes (3B, 4 CPUs): reading the prompt, about 25 ms
a token, 70–136 new tokens a question (the system message comes from the
cache). Repacking the weights for the CPU (`use_extra_bufts`) read 10–15%
faster but adds 5–7 s to each load: left off. Run-off questions now reuse
their shared header (the line, names and question, ~38 tokens), so their
later questions read about 40% fewer tokens. Same machine, hard set: 2.23
→ 2.00 s a question on average, answers identical; easy set 22.7 of 26
over 6 orders as before.

Decided (owner): GPU first, and cheaper questions. With a GPU the default
model is 7B; on the CPU 3B (`default_model_for`).

## Rewrites held-out set

`ovis_eval rewrites eval/rewrite-questions.txt` (15 lines no reading fits,
judged by what the program prints; `expect: none` is a refusal), 3B on 4 CPUs,
2026-10-19: **8 of 15 right**, 7 lines rewritten, the one refusal kept, and
every miss a refusal (no wrong rewrite was kept). Right: `double`, `has a
birthday`, `is a teenager`, `how old winnie is`, `gets one year older`, `is
older than 10`, `the age of winnie plus 1`. Refused: `in months`, `turns 13`,
`the names of`, `increase winnie's age`, `ages of people`, `the oldest
person`, `add one to`. A case takes 2–47 s (median ~13 s). Judge a prompt
change against this, not a few lines.

## Phrases in sight measured

Done (S1–S5): the measurements are in
[proposals/phrase-scope.md](proposals/phrase-scope.md) and
[dream/state](dream/state/INDEX.md); CI keeps the near-100 floors.

## Edit candidates measured

`eval/wording-questions.txt`: 28 near misses (a word left out, an extra
joining word, a word spelt like the phrase's, built-in forms, fields).
3B, one order, before → after edit candidates: right 14 → 22 of 28 (every
confident pick right); `questions.txt` 24 → 24 of 26; `hard-questions.txt`
12 → 12 of 20. Ranking (`--no-model`): near misses first 13 → 21, shown
15 → 23. Tried and dropped: replacing a word by any phrase word (`median`
became `average` confidently), a single question of 9 (cut right readings
the run-off kept), dropping words with meaning (`w's age in months` read
as `w's age` at 86%). Two edits (a verb put first, the joining words
before the value made `to`/`by`): wording ranking first 21 → 23, shown
23 → 25 of 28 (`w's age is now 13`, `goes to 14`); the other sets
unchanged; CI floor raised. Left to rewrites: word order, `sort out`,
`the order of`.

## Rewrites measured

`ovis run` through the daemon, 4 CPUs, 3B (7B in brackets). A rewrite
takes 2.2–3.1 s (5.3–7 s); the first read of its system message (~1500
tokens, the phrase list) takes ~20 s (47 s) once, then its state is
loaded from the prompt cache (32 MB). Lines no reading fitted (the
records probes): 3B rewrote right `winnie's age becomes 13`, `make
winnie's age 14`, `double winnie's age`, `the_car's owner turns 13` (set
its owner's age), `give the car 2 more wheels`; refused, so reported,
`remove winnie from people`, `in months`, `is a teenager`, `names of
people`, `the oldest person`. 7B also got `is a teenager` (age between 13
and 19), `the oldest person` and `the number of wheels the_car has`; its
one wrong rewrite (`turns 13` as `increase … by 1`) was refused. No wrong
rewrite was kept by either. Asked again with the refusal's reason, 3B
wrote the same in 4 of 5, so there is one try.

## Phrase front end: programs measured

`ovis_eval programs eval/programs` (readings meant in `# expect` lines):
the M1.14–M2 runs are archived in [dream/state](dream/state/INDEX.md).
2026-10-20, 3B on 4 CPU cores, after the numbers work: 25 programs, 21
built, questions per program median 0 (most 3); shortcut 3 (3 right), model
10 (9 right), left to the author 4 (2 meant to be). The new
`checksum.ovis` asks 2 (`total as hex`, `total moved right by 4 bits`),
both right; `masked with` became an alias after 3B read it as `with bit 15
set`. The one ranking miss (`number of names` read as `number of items
in names`) is the same before the numbers work. The CI floors still hold (questions: right reading first in 11 of
24, shown in 24; wording 23 and 25).

## Manual measured

`ovis_eval manual eval/manual-queries.txt` (34 queries far from the
phrases' words), 4 CPUs, 2026-10-25: where the entry meant comes, first /
in the first 5, and the time a query once the entries are cached (each
model embeds the 171 once, in 1–20 s).

| Embeddings | Joined, words 3× | Alone | Per query |
| --- | --- | --- | --- |
| None (the words) | 13 / 21 | — | — |
| 3B (Qwen2.5-Coder, last token; L4) | 13 / 21 | — | 0.5 s |
| bge-small-en-v1.5 (37 MB) | 17 / 22 | 18 / 24 | 56 ms |
| nomic-embed-text-v1.5 (146 MB) | 16 / 23 | 15 / 26 | 0.2 s |
| EmbeddingGemma-300M (334 MB) | 19 / 24 | 23 / 33 | 50 ms |
| Qwen3-Embedding-0.6B (639 MB) | 18 / 24 | 24 / 31 | ~0.2 s |

Each with the prefixes its authors give (Qwen3 an instruction).
EmbeddingGemma joined with the words counting 0.25 or 0.5: **25 / 33**
(46 ms); at 1, 19 / 31. A model made for search reaches what the words
miss (`trimmed`, `reversed`, `rounded down`, `a copy of`, `in hex`); 3B's
vectors did not. So the words now count half and `ovis manual` uses
EmbeddingGemma when it is pulled. The weight was picked on these 34
queries.

Held out (`eval/manual-held-out.txt`, 30 queries written after the weight
was chosen, about entries the first set doesn't cover), first / in the
first 5, no prefixes but EmbeddingGemma's:

| Embeddings | Joined, words ½ | Alone |
| --- | --- | --- |
| None (the words) | 1 / 10 | — |
| bge-small-en-v1.5 | 10 / 19 | 13 / 20 |
| nomic-embed-text-v1.5 | 9 / 21 | 12 / 21 |
| EmbeddingGemma-300M | 12 / 24 (words ¼: 13 / 24; 1: 10 / 19; 3: 5 / 15) | 15 / 24 |
| Qwen3-Embedding-0.6B | 15 / 24 | 19 / 25 |

So embeddings hold up far from the words, and the weight was partly fitted:
here the words cost 3 firsts, on the first set they gave 2. It stays ½
(over both sets ½ gives 37 of 64 first, ¼ and alone 38): not tuned on
these. Qwen3 reads better but is twice the size (639 MB) and ~0.2 s a
query on 4 CPUs once cached (EmbeddingGemma ~60 ms; the first search
embeds the 171 entries, 20 s against 6 s). Each model drops another's
vectors from the cache file (one size to a file). Decided (owner,
2026-10-25): EmbeddingGemma ships; Qwen3 is the switch if its licence or
reading matters more than 3× the time.

L5, the chat model choosing among the first 5 (`ovis_eval manual
--choose-model`, 3B on 4 CPUs, after EmbeddingGemma joined with the words),
first / in the first 5:

| | 34 queries | Held out (30) |
| --- | --- | --- |
| The search (EmbeddingGemma and words) | 25 / 33 | 12 / 24 |
| Its pick always first | 21 / 33 | 12 / 24 |
| Its pick first only when sure (60%, 30 points ahead) | **28 / 33** | **16 / 24** |

3B is seldom sure (most picks 25–50%), and its unsure picks undo the
search's right firsts as often as they fix them. Its sure picks (7 of 64,
64–90%) were all right and none moved a right first entry down: `in
capitals`, `average of`, `as text`, `squared`, `last item of`, `remove
item`, `number of entries in`. "None" was likeliest once. ~2.9 s a query
once loaded (the daemon keeps it; ~6 s with the start). So `ovis manual`
asks the model when there is one and moves only a sure pick.

## Not verified / known gaps

- The prompt cache's states (~3 MB each) are never cleaned up.
- `ovis_eval` results come from 26 questions and 20 programs written by the
  developer: real authors will write differently.
- The front end writes `@[note: …]` only for `at the end` in a loop,
  branch or block: the elaborator's notes are otherwise dropped.
- Builds and passes tests on macOS arm64 with Homebrew LLVM 22 (owner's
  run, before M1); the C-library case looks for headers in `/usr/include`,
  which macOS keeps in the SDK. Windows is not supported.
- Release packages: only the Linux x86-64 one was made and tried here
  (Ubuntu 24.04 container; Debian and Fedora mirrors are not reachable
  from the sandbox). The macOS packaging (`otool`, `install_name_tool`,
  re-signing) and the arm64 Linux runner wait for the workflow's first
  run. A macOS package bundles Homebrew's OpenSSL if httplib finds it,
  whose certificate path may not exist on the user's Mac (`--endpoint
  https://…`).

## Environment notes (cloud sessions)

- Install `llvm-18-dev` and `libclang-18-dev`
  (`apt-get install -y llvm-18-dev libclang-18-dev`). The base image has
  the runtime libraries but not the headers. Without libclang, `use-c` is
  disabled and its tests skip.
- zlib and SQLite headers and `pkg-config` are already in the image; the
  C-library tests use them.
- Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4`.
  Debug builds of llama.cpp are about 10× slower.
- `huggingface.co` and its CDN (`*.hf.co`) are allowed now, so
  `ovis model pull` works. GitHub and PyPI are reachable. The 1.5B, 3B and
  7B models were already in `~/.cache/ovis/models` in the 2026-10-18
  session.
- The tiny test model:
  - `pip install numpy gguf`
  - `ggml-vocab-qwen2.gguf` is in `build/_deps/llama-src/models/`
  - `python tools/make_tiny_model.py <vocab> tiny.gguf`

## Pinned dependencies

| Dependency | Version | Notes |
| --- | --- | --- |
| llama.cpp | `b11200` | 2026-09-26; uses `llama_model_load_from_file_ptr`, `llama_memory_*` |
| Catch2 | v3.8.1 | |
| nlohmann/json | v3.11.3 | |
| cpp-httplib | v0.18.3 | picks up OpenSSL if present |
| LLVM | system 18 | also builds with 22; API differences guarded by `LLVM_VERSION_MAJOR` |

## Decisions made with the project owner

| When | Decision |
| --- | --- |
| Plan 3 | Tracking docs under 500 lines; dreaming archives inactive memory into `docs/dream/`. |
| Plan 4 | Modules: a folder with `main.ovis` is the program; helper files only define; subfolders are nested modules. |
| Plan 4 | Any C library through `use-c` + libclang; dynamic linking by default. |
| Plan 5 | **Pivot:** a deterministic English-like phrase grammar; the model only picks between type-checked readings, with a time budget. Records, then traits. Design in [proposals/](proposals/phrase-grammar.md), plan M0–M4 in remaining-tasks.md. |
| Superseded | The paragraph pipeline's decisions (Session 1, Plans 1–2): [dream/state](dream/state/INDEX.md). Still in force from them: the lockfile, the endpoint backend, `/` as real division. |
| M0 | Default model: Qwen2.5-Coder-3B for the phrase front end, switched in M1.13 (the old pipeline was tuned on 1.5B). A model counts by the questions it settles, not only by how often its confident picks are right. |
| 2026-10-19 | Memory management is explicit and strict: no garbage collector or reference counting; every new value is made where a statement stores it; `free` (shallow), `with new memory:` arenas, `keep`; escapes are compile errors; checked by default (`--fast`), leak report opt-in. [proposals/memory.md](proposals/memory.md), plan E1–E7. |
| 2026-10-19 | What the compiler adds to a source is a **sigil above the line**, at its indentation, never indented under it (that looked like a block): `@[meaning: LINE]`, the whole line as read, and `@[note: …]`. The lockfile records the line each meaning was for, so an edited line is read again; a meaning it doesn't know is the author's. Hiding them in the lockfile behind future tooling was weighed and not chosen. |
| 2026-10-19 | The model may also **rewrite** an unclear line in words the compiler reads (chosen by the owner now the daemon keeps it loaded), under a grammar of the program's words, kept only if the line reads and nothing named is lost. Choosing comes first. |
| 2026-10-19 | **Traits:** records only for now; numbers and text are refused with an error suggesting a record that wraps the value; boxing them with a tag waits for memory E2–E3. A trait phrase need not have `(it)`: the first parameter of the trait's type picks the impl, and two such values must be the same record (checked when it runs), so operators and `(a) comes before (b)` are allowed. Traits that require traits: yes, later. Generic traits: no (Core has no generics). Lists of records sharing several traits: keep the run-time check, and add a build-time one where the list's records are known. |
| 2026-10-19 | `x = nothing` / `x != nothing` read as `is nothing` / `is not nothing`. A meaning the lockfile doesn't know warns when it loses a name or number the line has (the rewrites' check), it doesn't fail. Determinism across machines is documented, not fingerprinted per entry: the committed lockfile makes builds the same. llama.cpp: `GGML_NATIVE` for source builds; release builds and CI pin AVX2/FMA/F16C (NEON on ARM). |
| 2026-10-19 | **Copying is explicit, as in Rust:** text, lists and records are shared by `=`, arguments and items, never copied behind the author's back; `a copy of x` is the one copy, and deep (shares nothing; cycles survive as with `keep`). No moves or borrowing. [proposals/references.md](proposals/references.md) §10. |
| 2026-10-19 | **References as in C:** `ref x` of a variable, field or item is a typed pointer, an ordinary value (giving back a local's address is a build error). No marker for changed parameters: a phrase changes a number through a pointer; dropping a phrase's result gets a warning. references.md §6, §9. |
| 2026-10-19 | **Pointers as Zig's kinds:** `pointer to a T`, `pointer to many T`, `array of T` (a slice), text, `T or nothing`, `C pointer to T` (headers'), `pointer` (`void *`). Null only in `or nothing` types and C pointers: records stop being null by default (a migration). references.md §3–§4. |
| 2026-10-19 | references.md accepted: C structs imported as C records; structs by value through a C wrapper per function; raw pointer phrases everywhere; Ovis phrases as C callbacks (no captures); headers: the library's and the types its functions reach. Plan R1–R8. |
| 2026-10-19 | A phrase as a value is named with `_` for its slots (`compare _ and _`) or, without slots, `the phrase say goodbye` (or `the action`, `the function`, `the method`, `the procedure`, `the routine`); bare words are always a call. Phrase values are typed `phrase (…) giving …` (Ovis's convention) and `C phrase (…) giving …` (headers'), called with `call f with …`; the type's word takes the same synonyms (`action (…) giving …`). references.md §7b. |
| 2026-10-19 | **CMake:** Ovis reads C headers with the CMake target's flags (a response file), writes an object, and CMake links it with the target (static or shared is CMake's); builds under CMake are `--locked` (no model, no writes). [proposals/cmake.md](proposals/cmake.md), K1–K6. Linking by itself stays for `ovis run`/`build`. |
| 2026-10-19 | **Memory, as built:** the owner chose option (a), the proposal as written: the author stores every new value first. `ovis` enforces it unless `--lax-memory`; the library API's `strict_memory` is off for the tests. Core `free` is one builtin form that picks the runtime call by type, so a program that `use`s C's `free` still works. Programs are checked by default (`--fast` turns it off; CMake's `OVIS_FAST`). Strict analysis messages cite the source line; checked-mode messages name lines, not variables. |
| 2026-10-19 | **CMake, as built:** a locked build also needs the lockfile that records the author's `@[meaning: …]` lines and the C headers' hashes (one `ovis run` writes it). The runtime is C source (`ovis runtime`) that CMake builds with the project's flags as `ovis::runtime`. |
| 2026-10-20 | **Sizes of numbers** (numbers.md B1–B7): narrowing at a store implicit and checked; `uint32 + int32` a 64-bit whole number; bits from 0; English size words incl. `short`, `long`, `unsigned`, `N-bit`; the record tag stays 8 bytes; packed records now; a variable takes the size of its values. |
| 2026-10-20 | **Generics** ([proposals/generics.md](proposals/generics.md) G1–G8): copies per type (monomorphised), checked once against bounds; `any NAME` introduces a type variable (`any comparable thing`), `a stack of (item) has:`; untyped parameters stay one type; numbers and text implement traits statically; typed memory for type variables; C function-like macros become generic C phrases copied by the arguments' real C types; replacing the built-in list and map decided after measuring. |
| 2026-10-21 | **Projects** ([proposals/projects.md](proposals/projects.md)), every recommendation: `ovis.toml` (a TOML subset) stays the one source of truth, C dependencies included; `cc` when no dependency needs CMake, otherwise a CMake project generated into `build/` each build, never edited; the author's own CMake file with its targets listed in `ovis.toml`; git dependencies pinned in the project's `ovis.lock`. |
| 2026-10-21 | **Packages** ([proposals/packages.md](proposals/packages.md)), every recommendation: a package is a git repository, versions `vX.Y.Z` tags, the URL its name; minimal version selection, one version per major line (0.x: per minor); lines coexist as separate packages with distinct types; one version of a C library per program; the orphan rule for impls, the root included; only direct dependencies' phrases in scope, `NAME's phrase` to say which; dependencies built `--locked`, as source with a Core cache; exported unless under `internal/`. |
| 2026-10-21 | **Phrases in sight** ([proposals/phrase-scope.md](proposals/phrase-scope.md)), every recommendation: the ranking shortcut takes a reading adding at most one content word; a file sees a package's phrases only with `use NAME`; a dependency's phrase is offered only when it shares a word with the span; nested `internal/` folders scope a program's (and a package's) phrases to their parent folder; a near-100 sweep in CI. |
| 2026-10-25 | **The manual's embedding model**: EmbeddingGemma-300M (`ovis model pull embedding`), the best measured for its time and size, under the Gemma terms; Qwen3-Embedding-0.6B (Apache 2.0) reads a held-out set better (19 of 30 first alone, against 15) but takes 3× the time a query and twice the room. The words count half. |
| 2026-10-23 | **Manual and editor** ([proposals/manual-and-editor.md](proposals/manual-and-editor.md)), every recommendation: the model orders real manual entries and never writes an explanation; the library's documentation is `about:`/`example:` lines in the `.phr` files, checked by tests; the program's, packages' and C's own words are documented by their comments and their uses; in the editor the model's reading is a hint with a quick fix, and only a build writes sigils by itself; no completion while typing; a line is checked when the cursor leaves it or Enter ends it. |

## Lessons learned

- Round-trip what Ovis writes back into the source. A wrapped line starting
  with "30.1." was once read back as step 30 and corrupted a file on the
  second build. Tests now build every example repeatedly and require
  byte-identical output.
- Build llama.cpp in Release. The Debug build was about 10× slower and made
  inference look broken.
- `pkill -f <pattern>` in the sandbox can match and kill the shell running
  it. Kill by PID instead. (`pgrep -f` has the same trap.)
- A stray process spinning one core made the model 10–40× slower: llama.cpp's
  threads wait on each other, so one busy core stalls them all. Before
  timing the model, check nothing else is running (`ps -eo pid,pcpu,comm`).

- The session moved to another CPU after a worker restart, and the
  llama.cpp built with `GGML_NATIVE` died with "Illegal instruction".
  `-DGGML_NATIVE=OFF` alone turns AVX2 off too and made questions 20×
  slower. Build with `-DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=ON
  -DGGML_FMA=ON -DGGML_F16C=ON` in cloud sessions.
  A `build/` kept from an earlier session's CPU dies the same way
  (2026-10-22: AVX512-FP16 in `ggml_vec_dot_f16`); reconfigure it.

- Set `CMAKE_CXX_STANDARD` before `FetchContent_MakeAvailable`. Set after
  it, Catch2 built as C++14 on Apple clang without `string_view` support
  and the tests failed to link.
- libc++ (macOS) doesn't bring in `<algorithm>` through other standard
  headers as libstdc++ does: `std::ranges::move` in `format.cpp` built on
  Linux and failed on macOS. Include `<algorithm>` (and `<iterator>` for
  `back_inserter`) where they are used.
- The macOS job takes Homebrew's newest LLVM, which moves on its own (18
  on Linux, 23 on macOS on 2026-10-19). LLVM 23's `getTerminator()`
  assumes the block has one and gives back its last instruction, so
  every block looked finished and none got a branch or return. Test the
  last instruction's `isTerminator()` instead.

## History

| Commit | Summary |
| --- | --- |
| (P2) | **L5, the model choosing in `ovis manual`**: `manual_choice_probabilities` asks the chat model (one `choose`) which of the first 5 entries found (pattern and `about`) does what the query asks, or "none of these"; `manual_choose` moves its pick first only when sure (60%, 30 points ahead), else the order stands; `ovis manual` uses it with the daemon's model when there are words, not `--offline` or `--html`. `ovis_eval manual --choose-model FILE` measures it: first 28 of 34 against 25, held out 16 of 30 against 12, no right first lost, ~2.9 s a query. `manual_test`; test cases 256 (253 pass, 3 skipped), 3544 assertions |
| (P2) | **The manual's held-out queries and model decided**: `eval/manual-held-out.txt` (30 queries written after the weight was chosen): words 1 of 30 first, EmbeddingGemma 12 joined, 15 alone, Qwen3-Embedding 15 and 19; the weight stays ½ (partly fitted, above); EmbeddingGemma ships (decisions); no code changed |
| (P2) | **L4 with a model made for search** (what blocked L5): `builtin_model::embed` pools as the GGUF says (`ARCH.pooling_type`) with its special tokens, so BERT-like and Gemma/Qwen3 embedding models work (a chat model still by its last token); `ovis model pull embedding` (EmbeddingGemma-300M, `default_embedding_model`), which `ovis manual` uses when found (`find_embedding_model`, `--embedding-model`, `--embeddings` fails without it), never the chat model; query and entry prefixes (`manual_embeddings`); the words count half (`lexical_weight`). Fixed: `manual-embeddings.bin` written with one vector size for vectors of two models, after which every read failed and the search fell back to the words. `ovis_eval manual` reports embeddings alone too, `--lexical-weight`, `--query-prefix`, `--entry-prefix`, `--no-prefixes`. Measured (above): first 25 of 34 against 13. `manual_test`; test cases 255 (252 pass, 3 skipped), 3539 assertions |
| (P2) | **L6, `ovis manual --html DIR`**: `write_manual_html` writes `index.html` (every entry under its topic) and one page per topic (per package and topic), an anchor per entry with its sentence, other ways of saying it and examples; with words, only the entries found. The links are checked in the files written (a page and an id must be there), problems go to stderr and exit 1; `manual_test`. Test cases 254 (251 pass, 3 skipped) |
| (P2) | **R1, version 0.1.0**: `CMakeLists.txt` and `tools/ovisup` say 0.1.0 (they said 0.0.1, the version of the first tag), and a test (`ovisup_test`) fails if the two differ; deployment.md already named 0.1.0. Done at the owner's word before L5, L6 and L7's rest; the tag `v0.1.0` (the first `stable`) is not made yet. Test cases 253 (250 pass, 3 skipped) |
| (P2) | **L10, C's entries in `ovis manual`**: `c_header_templates` reads the headers a file's `use "x.h"` lines name (all the program's files when `--in` is not a file) with `import_c_library` and makes an entry for each function and function-like macro (generic ones too), constant and struct: found by its name split into words (`deflateInit` as `deflate init`) and its parameters' names (`c_function::parameter_names`, new), shown as its signature in Ovis's types (`phrase_template::title`: `ovt_add(a: a whole number, b: a whole number) gives a whole number`), the header's first comment sentence, `From x.h.`, and up to two lines of the program that use the name; the ones the program calls come first. Entries are embedded with `--embeddings` by their text, so a header is embedded once. Test cases 252 (249 pass, 3 skipped), 3505 assertions (the test library; zlib and SQLite skip without headers) |
| (P2) | **L9, a package's `# example:` lines**: a comment `# example: LINE` above a definition is an example, shown first by `ovis manual` (`program_phrase_templates`, not part of `about`); `check_package_examples` compiles them with no model as a `main` probe whose lines keep their line numbers in the package file, so a problem cites `FILE (examples), line N`, and above a `to …:` header the example must use the header's words; `ovis release` runs it after the package reads (`read_package`), and `ovis check` in a package with no `main.ovis` does the same without tagging. Test cases 250 (247 pass, 3 skipped), 3488 assertions |
| (P2) | **L7 rest**: `ovis manual` has records (fields, traits), traits (needs, defaults, implementers) and a package's top-level constants as entries, and a package's entries end with its full version (`phrase_file::package_version`); one scan with the `to …:` headers (`program_phrase_templates`). Test cases 255 (252 pass, 3 skipped) |
| … | Earlier history: [dream/state](dream/state/INDEX.md) (to deployment, projects J1–J8, packages H1–H9 and the manual's L1–L4, L7, L8; phrase scope S1–S5 and the manual planned) |

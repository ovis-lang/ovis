# Remaining tasks

Ordered by priority. Current status is in [state.md](state.md), and the
design these tasks change is in [architecture-spec.md](architecture-spec.md).
Move a task to state.md's history when it is done. Finished and dropped
tasks: [dream/remaining-tasks](dream/remaining-tasks/INDEX.md).

## P0: the phrase pivot

The design is in [proposals/phrase-grammar.md](proposals/phrase-grammar.md)
and [proposals/types.md](proposals/types.md) (accepted). A deterministic,
English-like parser reads the program; the model picks between
readings that type-check, or rewrites a line in phrases (checked). Milestones M0–M4 below follow the proposal's
phases.

M0 (model scoring) and M1 (the phrase front end, no records, with
follow-ups M1.15 and M1.16) are done: state.md history, and
[dream/remaining-tasks](dream/remaining-tasks/INDEX.md) for the tasks as
written.

### M2 and M3: records and built-in traits

Done, with their leftovers (types.md §1–§2): `nothing` for records and `is
nothing`, defaults of later records, a field given once, what to write when
a record lacks `comes before` / `is the same as`, `position of` and `count
of` for any equatable type, lists of records with `text of`, and `a book is
comparable:` (also `equatable`, `showable`). See state.md history,
architecture-spec §4–§5 and
[dream/remaining-tasks](dream/remaining-tasks/INDEX.md). New, small:

### M4: user traits and impls

Done (types.md §2): M4.1–M4.4, and one-line phrase bodies (M5). See state.md history, architecture-spec §5–§6
and [dream/remaining-tasks](dream/remaining-tasks/INDEX.md). Left from it
(designs decided 2026-10-19, state.md decisions):

### Defer: `at the end`

Done (D1 Ovis Core `(defer …)`, D2 surface with the `Compiler:` note, D3
`examples/notes.ovis` and `eval/programs/tidy.ovis`): state.md history,
architecture-spec §5–§6 and [proposals/defer.md](proposals/defer.md).

## P1: correctness and robustness

- [ ] Ranking: the right reading is first in 11 of 24 (ties in library
  order). An embedding tie-break raised that to 21 but not the model's
  right picks (state.md, "questions measured"; code in `8ffe5c3`, which
  `git revert` of its revert restores). Measure ranking by right picks,
  not rank, over several orders (`ovis_eval questions --orders 6`: 3B
  22–23 of 26). Next: try asking in two orders and averaging, against the
  order bias and the confident "none" misses (done: steadier, +0.2/+0.6,
  twice the time, an option; state.md); and revisit choosing the 9
  shown by embedding (a third fewer asks). Embeddings alone (0.5B) come
  within 3 cases of 3B asked but never say "none" (state.md): decide
  whether the model stays a matchmaker or gets a generative job
  (composing readings, proposing `to …:` phrases), checked by the types
  and the lockfile. Owner: no generative job; GPU first (7B with a GPU,
  3B without) and cheaper questions (done: run-off header reuse).
- [ ] Measure 7B on a GPU (the owner's Mac): time per question and the
  hard set with `--orders 4`, to confirm GPU-first. Then CUDA/Vulkan
  builds in CI, if a runner has a GPU.

## P2: language features

### Sized numbers and bits

Done (N1–N7, decisions B1–B7): state.md history and
[proposals/numbers.md](proposals/numbers.md). Left from it, small:

- [ ] A 4-byte record tag (B5), if a program's records show it matters.

### Generics

Done (G1–G8, accepted decisions in
[proposals/generics.md](proposals/generics.md)): state.md history and
[dream/remaining-tasks](dream/remaining-tasks/INDEX.md). The built-in list
and map stay until `item` is inlined and the table is packed (state.md,
"collections measured").

## P2: model and tooling

### A manual and checking a line when it is done

Accepted: [proposals/manual-and-editor.md](proposals/manual-and-editor.md)
(the owner's idea, 2026-10-23; every recommendation taken).
`about:`/`example:` beside each phrase (checked by tests), and `ovis
manual` (lexical, embeddings, then the model choosing among real entries,
never writing). The owner dropped all editor work and the VS Code
extension on 2026-10-24: the smart manual is the help.

- [ ] L7 rest (records, traits, top-level values and C functions as entries,
  the package's version), L10,
  L5–L6, then R1, version 0.1.0, in the proposal's order. L1–L4, L8
  (the names in scope at a line, `--for`) and L9 (a package's `# example:`
  lines, checked by `check` and `release`) are done
  and L7's `to …:` phrases (with `#` comments, uses, package)
  (`ovis manual` is lexical plus embeddings over the library and a
  `phrases/` folder).
- [ ] L4 measured (state.md): 3B embeddings add nothing over the words
  (21 of 34 in the first 5 both ways), so `--embeddings` is opt-in. Try a
  real embedding model, or an `about:` rewritten toward authors' words, before
  L5 (`choose`), which is worth building only if it beats the words.

### Projects without CMake

Built: [proposals/projects.md](proposals/projects.md) (the owner's
idea, 2026-10-21): `ovis.toml` stays the one place that says what a
program needs, C dependencies included (pkg-config, git, a path, or the
author's own CMake file); Ovis builds with `cc` when it can and otherwise
generates the CMake project into `build/` each build. `ovis new`, `ovis
build`/`run` with no arguments.

- [ ] `check`/`fix` of a CMake-built project before its first build
  don't know its targets' include folders (read from the last configure).
- [ ] CI: `examples/project` on macOS (pkg-config's zlib there).

### Packages hosted on git

Built: [proposals/packages.md](proposals/packages.md) (the owner's
idea, 2026-10-21): a package is a git repository, versions are its tags,
its URL is its name; minimal version selection, one version per major
line, different lines as separate packages with distinct types; C
libraries one per program; overrides in the root; dependencies always
built `--locked`, as source; `ovis add`/`update`/`vendor`/`release`.

- [ ] H10: the Core cache per dependency, when a graph is big enough to
  need it (133 generic lines: ~60 ms of a ~700 ms build).
- [ ] `ovis update` saying which lines a package's new version changes
  (`check-phrases --dependencies` compares with and without the package,
  not two versions; `ovis add` prints it).
- [ ] A package's `phrases/` folder scoped to the files that see it.
- [ ] Exported top-level values (constants) from packages.
- [ ] A package in a subfolder of a repository (`sub/vX.Y.Z` tags).
- [ ] `ovis fix --line` with packages (they read the
  program's own files only).
- [ ] A phrase nobody calls reads an untyped parameter as a whole number
  (`to note (t):` with `an entry with words t` fails): libraries must type
  their parameters until the body says the type.

- [ ] Windows support and testing, then add it to CI (and an ovisup for
  it, as rustup-init.exe).

### Deployment

Done: ovisup, relocatable packages, the release workflow
([deployment.md](deployment.md), state.md history). Next:

- [ ] Merge to main so the workflow's first run publishes `nightly`; fix
  what the macOS and arm64 Linux packaging find there.
- [ ] Version 0.1.0 at the end of the manual plan (R1 in
  [proposals/manual-and-editor.md](proposals/manual-and-editor.md)): the
  versions in CMakeLists.txt and ovisup made 0.1.0 and kept
  one, then the tag `v0.1.0`, the first `stable`; nightly should install
  everywhere first.
- [ ] macOS x86-64 packages, if anyone needs them (a `macos-15-intel`
  runner); musl or older-glibc Linux packages (build in an older container)
  if glibc 2.39 proves too new.
- [ ] Smaller packages: libLLVM is 118 MB of the 220 MB unpacked, and
  libclang needs it as a shared library; strip the bundled libraries, or
  build LLVM with only the targets and libclang parts Ovis uses.

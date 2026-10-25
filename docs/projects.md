# Projects and packages: ovis.toml

How a project and its packages are built, as built. The design and its reasons are in
[proposals/projects.md](proposals/projects.md) (J1–J8, accepted) and
[proposals/packages.md](proposals/packages.md) (H1–H10); the rest of the
compiler is in [architecture-spec.md](architecture-spec.md).

## The file

A folder with `ovis.toml` and a `main.ovis` is a project. `ovis.toml` is
read by `src/ovis/project/toml.cpp` (a TOML subset: tables, bare and
quoted keys, `"…"` and `'…'` texts, whole numbers, booleans, lists over
several lines, inline tables; no dates, floats, multi-line texts or
`[[…]]`) and checked by `project.cpp`, every error `ovis.toml, line N: …`.

| Table | Keys |
| --- | --- |
| `[project]` | `name` (the executable; default the folder's), `toolchain` (the ovisup shim's choice) |
| `[build]` | `include`, `defines`, `phrases` (folders, relative to the project), `c` (patterns of the project's C files: `*`, `?`, `**`) |
| `[c.NAME]` | one of `pkg-config` (a module, maybe `zlib >= 1.2.11`), `git` (with `tag` or `commit`), `path` (a folder with a CMakeLists.txt), `cmake` (the author's file); `targets` for all but pkg-config |
| `[package]` | `name`, `version`, `ovis` (packages.md §1) |
| `[dependencies]` | `NAME = { git = URL, version = "1.2" }` or `{ path = DIR }`; `=1.2.3` and `commit` only outside a package |
| `[overrides]` | by a dependency's name, URL or `"c.NAME"`: `git`, `path`, `tag`, `version`, `commit` |

`find_project` walks up from the current folder to the first `ovis.toml`.

## Commands (`src/main.cpp`)

- `run`, `build`, `ir`, `check`, `fix`, `fmt`, `check-phrases` with no
  program, or with a project's folder, build the project (`apply_project`):
  its include folders, defines and phrases go before the command line's
  flags; `build` writes `build/NAME` unless `-o`. A single `.ovis` file
  named builds as before, whatever project it is in.
- `ovis new NAME` / `ovis init`: `ovis.toml`, `main.ovis` (hello),
  `.gitignore` (`/build/`), `git init`; init keeps files that exist.
- `ovis cmake [--print]`: the generated CMake project, written or printed.
- `ovis update [c NAME]`: git C dependencies' tags looked up again.
- `ovis check-phrases --dependencies` (phrase-scope.md S5): the program
  read by the ranking alone (no model; `program_result::core_lines`),
  as it is and with each direct dependency out of its files' sight (its
  name taken from their `aliases`); a line of the program's own that
  reads, asks or is offered readings differently is listed (exit 1). A
  line that reads only with the package, or asked without it and reads
  with its phrases (its Core names end `package_core_suffix`), is a use,
  counted. `ovis add` prints it for the package added.

## Building

| The project has | Built by |
| --- | --- |
| only pkg-config tables and `c = [...]` | `cc`: `native.cpp` compiles each C file into `build/c/<path with _>.o` with `-MMD`, again only when the file, a header in its `.d` or the flags changed; objects then pkg-config's `--libs` go before the program's own libraries on the link line |
| any `git`, `path` or `cmake` table | the generated CMake project (`cmake_project.cpp`) |

**The generated project** is written into `build/cmake/` at every build
(`CMakeLists.txt`, the embedded `OvisMacros.cmake`, the runtime's C),
each file only when its text changes; configured into
`build/cmake-build/` and built with `cmake --build`. When a file changed
it is configured and built again from clean (`--clean-first`), not left
to make's file times, which macOS's make compares in whole seconds. Its order: the runtime; pkg-config, git
(FetchContent at the locked commit) and path tables; the author's
`cmake` files last, so they may use the others; a check per promised
target, citing ovis.toml's line; `c = [...]` as one static library with
the `[build]` flags; then `ovis_add_executable(NAME ROOT LINK … USE_LINKS)`
with `OVIS_UNLOCKED` on unless `--locked` and the command line's options
in `OVIS_FLAGS`. `USE_LINKS` passes `--link-flags FILE` instead of
`--link-by-caller`, and links with `@FILE`, so a `use` line's own library
(`from "m"`) is linked as by `ovis build`. CMake's output is shown only
when it fails (the log is `build/cmake.log`).

`check` and `fix` never run CMake: they read headers with `[build]`'s and
pkg-config's flags and, after a build configured the project, the
targets' flags from `build/cmake-build/NAME.ovis.rsp`.

## ovis.lock

The project's lockfile (`lock.cpp`), beside `ovis.toml` and committed;
not a file's `<file>.lock`. JSON, sorted, replaced whole and only when
it changes:

```json
{"ovis_project_lock_version": 1,
 "c": {"cjson": {"git": "https://…", "tag": "v1.7.18", "commit": "…"}},
 "packages": []}
```

A git table's tag is resolved once (`git ls-remote`, the peeled commit of
an annotated tag) and kept while `ovis.toml` asks for the same tag; a tag
moved upstream doesn't change the build until `ovis update`, which says
`v1.0.0 moved from A to B`. Under `--locked` a lock that would change is
an error.

## The toolchain line

The ovisup shim (deployment.md) and `ovisup show`/`which` take
`toolchain` from the `[project]` table of the nearest `ovis.toml`, after
`+T` and `OVIS_TOOLCHAIN`, before the default; a toolchain not installed
is named with where it came from.

## Packages

A project with a `[package]` table is a package: a git repository whose
`vX.Y.Z` tags are its versions and whose URL is its name. As built
(`src/ovis/project/packages.cpp`, H1–H9 of the accepted proposal):

- **Choosing versions** (`resolve_packages`): per identity (normalized URL,
  or a path) and line (the major; before 1.0 the minor) the least tag that
  satisfies the highest minimum asked, in rounds until nothing rises.
  `=1.2.3` and `commit` are the root's only; `[overrides]` (by name, URL or
  `"c.NAME"`) replace a package everywhere. Tags come from `git ls-remote`
  (an annotated tag's commit), only when ovis.lock no longer satisfies
  ovis.toml or `ovis update` asks.
- **Fetching** (`fetch_package`): a bare repository per URL and one
  read-only tree per content in the cache (`$OVIS_PACKAGE_CACHE`, else
  `~/.cache/ovis/packages`), checked against the lock's `tree`
  (sha256 over sorted paths and contents). `vendor/NAME-LINE` is used
  instead when its tree is the lock's.
- **Reading** (`load_program_files`, `load_package_files`): packages'
  files before the program's, as modules `ID.MODULE` (`greet@1.hello`),
  locked (their author's meanings settle them; the model is never asked,
  nothing is written). Each file sees (`phrase_file::sees`,
  `vocabulary::sees`) its own package's phrases and records and Ovis's,
  and a direct dependency's only with a top-level `use NAME` line
  (phrase-scope.md S2; the reader's constructor adds them). What a file
  under `X/internal/` defines is seen by the files under `X/` alone, in a
  package or a folder program (S4, Go's rule): its `package` is
  `ID#X/internal` (the program's `#X/internal`, `.#X/internal` to the
  vocabulary) and each file's `sees` lists the internal/ folders above it
  (`own_sees`); a program reads scoped once it has one below its top. A
  line only another folder's internal/ reads is an `internal` problem,
  never a question. A line that reads only with a dependency
  the file doesn't use is a `needs_use` problem, never a question; `ovis
  fix` writes the `use` lines and reads again. A question offers a
  dependency's phrase only if it shares a word of meaning with the unclear
  words (phrase-scope.md S3, `vocabulary::is_dependency`): a package's
  synonyms are its aliases, not the model's guess.
  A package's phrases, records and traits have Core names ending
  `__p_ID` (`package_core_suffix`), said and printed as written
  (`record_descriptions`, `record_print_names`); its top-level values
  stay in it (`modules.cpp`). With two versions in a program its records
  are said "a log 1.1.0 entry" and a failed build says who uses which.
- **Whose phrase:** where two packages a file sees have a phrase with the
  same words, a dependency's reads `alpha's say hi`; a line (or `x = …`)
  starting with a dependency's name and `'s` reads with that package's
  phrases alone, so a meaning written for it stays settled.
- **The orphan rule:** with packages, a file gives a type a trait only if
  its package declares the trait or the type.
- **C:** a package's include folders, defines, pkg-config tables and C
  files are built into the program's `build/packages/`; every git, path
  and cmake table of the graph is combined (`combined_c_dependencies`):
  one of each C library, two tags of one an error unless the root's
  `"c.NAME"` override chooses.
- **Commands:** `ovis add NAME --git URL [--version V] | --path DIR`
  (and `use NAME` into `main.ovis`, which `remove` takes out), `ovis
  add c NAME …`, `ovis remove [c] NAME`, `ovis update [NAME]`, `ovis tree`,
  `ovis fetch`, `ovis vendor`, `ovis release X.Y.Z` (version, committed, a
  new tag, no path dependencies, reads and checks as its users would).
  A `# example: LINE` comment above a definition is an example the
  manual shows; `ovis release`, and `ovis check` in a package (no
  `main.ovis`), read each with no model in its file's package and fail a
  line that doesn't read, or (above a `to …:` header) doesn't use the
  header's words.

ovis.lock's `packages` hold `name`, `git` or `path`, `line`, `version`,
`commit`, `tree` and `dependencies` (`name@line`). Measured (4 CPUs,
2026-10-21): a 133-line generic package adds ~60 ms of reading to a
~700 ms build, so the Core cache (proposal §7) waits for bigger graphs.

## Tests

`project_test` (`[project]`: the TOML subset, tables, mistakes, versions,
lookup), `project_build_test` (new/init, no program, settings, C files
rebuilt by need, pkg-config, `examples/project`), `project_cmake_test`
(`[cmake]`: a path dependency and an author's file, a git dependency
pinned and updated, `--locked`), `ovisup_test` (the toolchain line),
`packages_test` (`[packages]`: tags and tree hashes, version selection,
pins, paths and overrides, locked package files, two lines of a package,
records across lines, `internal/`, `use NAME` and `fix`, whose phrase, the orphan rule, a
package's C, one C library per program, add/remove/fetch/vendor, release).

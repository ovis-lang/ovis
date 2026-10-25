#ifndef OVIS_PHRASE_FRONT_END_HPP
#define OVIS_PHRASE_FRONT_END_HPP

#include <filesystem>
#include <functional>
#include <set>
#include <vector>

#include <ovis/core/ast.hpp>
#include <ovis/core/c_import.hpp>
#include <ovis/english/language_model.hpp>
#include <ovis/phrase/lockfile.hpp>
#include <ovis/phrase/templates.hpp>

// The phrase front end, end to end: English-like lines to a checked Ovis
// Core program. How it works: docs/phrase-front-end.md.
namespace ovis::phrase {

// Where a build is, for a progress display: what it is doing, in which
// file and line, and about which words.
struct build_progress {
  enum class stage_type {
    // Reading a file's lines: `line` of `lines`.
    reading,
    // Asking the model which reading the words have; rewriting a line.
    asking,
    rewriting,
    // Elaborating and checking the whole program.
    checking
  };
  stage_type stage = stage_type::reading;
  // As reports name it, or the file's name for a program of one file.
  string_type file;
  std::size_t line = 0;
  std::size_t lines = 0;
  string_type words;
};

struct front_end_options {
  // Questions still open when the model has used this much time go to the
  // author, so a build never waits long on the model. Loading the model
  // (during the first question) isn't counted.
  double model_budget_seconds = 10;
  std::function<void(string_view_type)> log;
  // For `compile_phrases`: the program's lockfile from the last build,
  // whose readings are used before asking the model.
  const lockfile *previous = nullptr;
  // Where `use "header.h"` looks for headers and libraries.
  core::c_import_options c_imports;
  // Folders of phrase files (`.phr`, `.core`, `.generic`) read for this
  // build, besides the `phrases/` folder next to a folder program's
  // `main.ovis` (`--phrases DIR`).
  std::vector<std::filesystem::path> phrase_directories;
  // Experiments, measured with `ovis_eval questions` (state.md): readings
  // the ranking ties are ordered by the model's embeddings, and with
  // `choose_by_embedding` the first is taken without asking. Off: neither
  // raised the model's right picks.
  bool order_ties_by_embedding = false;
  bool choose_by_embedding = false;
  // Ask each question twice, the readings reversed the second time, and
  // average what each reading got: against the model's bias to the first
  // and last options. Twice the time; measured in state.md.
  bool ask_both_orders = false;
  // When no reading offered is taken, the model may write the unclear line
  // again in words the compiler reads (checked before it is kept).
  bool rewrite = true;
  // Told where the build is: often (every line), so it must be cheap.
  std::function<void(const build_progress &)> progress;
  // The caller writes `sources` back, so reports cite the lines of the file
  // as written (with its new sigils); else the lines as read.
  bool sources_written = true;
  // Also check the strict memory rules (docs/proposals/memory.md §2, §4):
  // no new value made where nothing can free it, none lost, none used after
  // the `with new memory:` that made it. Off until the library, examples
  // and tests follow them.
  bool strict_memory = false;
  // Keep the names in scope before each line of each file, with their types
  // (`program_result::variables`): for `ovis manual --line` (L8).
  bool variables_recorded = false;
  // Find the phrases with a slot for this name in scope, or this type in
  // words, before line `phrases_for_line` of the file `phrases_for_path`
  // (line 0: at its end): `program_result::phrases_taking` (L8, `--for`).
  string_type phrases_for;
  std::filesystem::path phrases_for_path;
  std::size_t phrases_for_line = 0;
};

// A name in scope at a line: its type as an author says it ("a list of
// whole numbers", empty when the line that sets it was not understood), and
// the line that first set it (0: set before this file, by an earlier one).
struct scope_variable {
  string_type name;
  string_type type;
  std::size_t line = 0;
};

// Where the time went, for judging the approach.
struct front_end_timing {
  double lex_ms = 0;
  // Parsing and building questions, without the model.
  double parse_ms = 0;
  double model_ms = 0;
  // The first question also loads the model.
  double first_question_ms = 0;
  double check_ms = 0;
  std::size_t lines = 0;
  // Unclear spans found, and how many of those the model was asked about
  // (the others were settled by `Read as:` lines).
  std::size_t questions = 0;
  std::size_t asked = 0;
  // How the unclear spans were settled: by `Read as:` lines, by a
  // deterministic rule (the one fitting name), by the ranking shortcut, or
  // by the model. The rest went to the author.
  std::size_t by_source = 0;
  std::size_t by_lockfile = 0;
  std::size_t by_rule = 0;
  std::size_t by_ranking = 0;
  std::size_t by_model = 0;
  // Settled by the model's rewrite of the line.
  std::size_t by_rewrite = 0;
};

// A fix the author may take for a problem: what the unclear words would
// read as, and the whole line then, written as `@[meaning: …]` above the
// line when taken (`ovis fix`, the editor).
struct quick_fix {
  string_type reading;
  string_type meaning;

  friend auto operator==(const quick_fix &, const quick_fix &) -> bool = default;
};

// A problem as tools see it. `problems` are their text.
struct diagnostic {
  // Index of the file in the files given (0 for `compile_phrases`).
  std::size_t file = 0;
  // In the file as written after the build (with its new sigils); 0 when
  // the problem is in no line (the prelude).
  std::size_t line = 0;
  // The unclear words or the error, from 1, end excluded; 0 if not known.
  std::size_t column = 0;
  std::size_t end_column = 0;
  // `unclear` (fixes are its readings), `no_phrase`, `unknown_name`,
  // `no_field`, `needs_use`, `internal`, `syntax` (its text names the column), or
  // `error`.
  string_type kind = "error";
  // For `needs_use`: the dependencies whose `use` lines make the line read.
  std::vector<string_type> packages;
  // Without the file and line.
  string_type message;
  // The unclear words, if any.
  string_type span;
  std::vector<quick_fix> fixes;
};

struct front_end_result {
  core::program program;
  // "line N: …", in source order. The build stops if there are any.
  std::vector<string_type> problems;
  // The same problems, for tools.
  std::vector<diagnostic> diagnostics;
  // Worth telling the author, but not stopping the build.
  std::vector<string_type> warnings;
  // The source with the `@[meaning: …]` lines of new decisions written in.
  string_type source;
  // The decisions this build made (not read from the source), in order:
  // the unclear span and its reading.
  std::vector<std::pair<string_type, string_type>> decisions;
  // The readings of this build that came from the model or the previous
  // lockfile: the lockfile to save.
  lockfile lock;
  front_end_timing timing;
  // The templates the program's lines were read with, as "lists.phr:12".
  std::set<string_type> templates_used;

  auto succeeded() const -> bool { return problems.empty(); }
};

// One file of a program in phrases.
struct phrase_file {
  std::filesystem::path path;
  // How reports name it, e.g. `pricing/tax.ovis`; empty for a single-file
  // program.
  string_type name;
  // Its module, e.g. `pricing.tax` or `main`; empty for a single-file
  // program.
  string_type module;
  string_type text;
  // Its lockfile from the last build.
  lockfile lock;
  // A dependency's file (proposals/packages.md §6): its lines are settled
  // by its own meanings and lockfile; nothing about it is asked of the
  // model, and the caller never writes it. Its package's id (`log@1`).
  bool locked = false;
  string_type package;
  // The packages its lines may use: its own (`.` for the program's) and
  // its direct dependencies' ids. Empty when the program uses none.
  std::vector<string_type> sees;
  // When two versions of its package are in the program, how messages tell
  // its records from the other's: "log 1.1.0" ("a log 1.1.0 entry").
  string_type package_label;
  // Its package's full version (`1.2.0`), for the manual; empty if unknown.
  string_type package_version;
  // The names its project gives its direct dependencies, to their ids: a
  // line may start `log's …` to read with that package's phrases alone.
  std::map<string_type, string_type> aliases;
};

// The files of the program `p_path` names:
// - a folder, or a file called `main.ovis`: every `.ovis` file in that folder
//   and its subfolders (`pricing/tax.ovis` is the module `pricing.tax`),
//   helpers first and `main.ovis` last. A subfolder with its own `main.ovis`
//   is a separate program and is left out;
// - any other `.ovis` file: a program on its own.
// Lockfiles are loaded next to each file. A folder program with an
// internal/ folder below its top is read scoped: `pricing/internal/` files
// have `package` `#pricing/internal`, and every file `sees` `.` and the
// internal/ folders above it (phrase-scope.md S4). Throws `error`.
auto load_phrase_program(const std::filesystem::path &p_path)
    -> std::vector<phrase_file>;

// The files of the package `p_id` in `p_root` (packages.md §1): every
// `.ovis` file but a main.ovis, as modules `ID.MODULE`, named in reports
// `ID/path.ovis`, locked. Folders that are programs of their own, hidden
// ones and the top's build/ and vendor/ are left out. What files under
// `X/internal/` define is seen by the files under `X/` alone (phrase-scope.md
// S4): their `package` is `ID#X/internal` (`ID#internal` at the top), and
// each file's `sees` is its package and the internal/ folders above it.
auto load_package_files(const std::filesystem::path &p_root,
                        const string_type &p_id) -> std::vector<phrase_file>;

struct program_result {
  core::program program;
  // "pricing/tax.ovis, line N: …" (no file name for a single file).
  std::vector<string_type> problems;
  // The same problems, for tools, in the same order.
  std::vector<diagnostic> diagnostics;
  std::vector<string_type> warnings;
  // Per file, in the order given: its source with new `@[meaning: …]`
  // lines, the lockfile to save, and the decisions this build made.
  std::vector<string_type> sources;
  std::vector<lockfile> locks;
  std::vector<std::vector<std::pair<string_type, string_type>>> decisions;
  // Per file, the Ovis Core read from each of its lines (by the line as
  // read): two readings of a program compared line by line (`ovis
  // check-phrases --dependencies`).
  std::vector<std::map<std::size_t, string_type>> core_lines;
  // Per file, the names in scope before each of its lines (by the line as
  // read), when `variables_recorded`.
  std::vector<std::map<std::size_t, std::vector<scope_variable>>> variables;
  // `phrases_for`: what it is as an author says it (empty: no such name or
  // type) and the phrases that take it.
  string_type phrases_for_type;
  std::vector<phrase_template> phrases_taking;
  front_end_timing timing;
  std::set<string_type> templates_used;

  auto succeeded() const -> bool { return problems.empty(); }
};

// The ending of a package's Ovis Core names: `greet@1` → `__p_greet_1`,
// so `greet` of two packages, or two lines of one, are two functions.
auto package_core_suffix(string_view_type p_package) -> string_type;

// Reads a program of several files. Phrases defined in any file work in
// all of them; top-level variables of helpers are visible to the files
// after them; helpers may only define (phrases, `use`, and constants).
auto compile_phrase_program(const std::vector<phrase_file> &p_files,
                            english::language_model *p_model,
                            const front_end_options &p_options = {})
    -> program_result;

// The source with what the compiler wrote about its lines put in order,
// changing nothing else and building nothing (`ovis fmt`): each line's
// sigils above it at its indentation, spaced as builds write them; old
// `Read as:` and `Compiler:` lines under a line as its `@[meaning: …]` and
// `@[note: …]`; a meaning the same as its line dropped.
auto formatted_source(string_view_type p_source) -> string_type;

// The built-in phrases as a person reads them (`average of LIST`): those
// whose words are most like `p_words`, best first (at most `p_limit`), or
// every one, by its file (`numbers.phr`), when `p_words` is empty.
// `ovis phrases`.
auto phrases_like(const std::vector<string_type> &p_words, std::size_t p_limit)
    -> std::vector<std::pair<string_type, string_type>>;

// One phrase as the manual shows it (`ovis manual`): every word is the
// library's, none is written by a model.
struct manual_entry {
  // The topic: the `.phr` file's name without its extension (`text`).
  string_type topic;
  // The package whose phrases they are; empty for Ovis's own.
  string_type package;
  // The pattern as a person reads it, and its other ways of saying it.
  string_type pattern;
  std::vector<string_type> aliases;
  string_type about;
  std::vector<string_type> examples;
};

// What finds a phrase by its meaning, not its words (L4): the model embeds the
// query and each entry's ways of saying it and its `about:` sentence. Entries
// are embedded once: the vectors are kept in `cache`, a file keyed by the
// model and the entry's text, so an edit embeds only what changed.
struct manual_embeddings {
  // Null: the search is lexical alone, as without a model.
  english::language_model *model = nullptr;
  // Empty: nothing is kept between searches.
  std::filesystem::path cache;
  // What a model made for search wants before a query and before an entry
  // (`search_query: `, `search_document: `).
  string_type query_prefix, entry_prefix;
  // How many times the lexical place counts against the embeddings'; 0 ranks
  // by the embeddings alone, for measuring them. Half: EmbeddingGemma's
  // order is the better one, and the words still lift what they name
  // (eval/manual-queries.txt: first 25 of 34, against 23 alone and 19 at 1).
  double lexical_weight = 0.5;
};

// One template as the manual shows it.
auto manual_entry_of(const phrase_template &p_item) -> manual_entry;

// The `to …:` phrases a program's and its packages' files define, as
// manual entries (L7): the header gives the pattern, the `#` lines above it
// the `about`, and up to two lines of the program that read to it the
// examples ("show tax on 5   (main.ovis, line 3)"). Found by scanning the
// headers, not by compiling, so they show even while some lines have
// problems. A package's say where they are from, and when `p_from` (the
// file the author is writing) doesn't `use` that package, that too.
auto program_phrase_templates(const std::vector<phrase_file> &p_files,
                              const std::filesystem::path &p_from = {})
    -> std::vector<phrase_template>;

// What a file's `use "x.h"` lines import, as manual entries (L10): each
// function (and function-like macro), constant and struct, found by its name
// split into words (`deflateInit` as `deflate init`) and its parameters'
// names, shown with its C signature in Ovis's types, the header's first
// comment sentence and up to two lines of the program that use it; the ones
// the program already calls come first. Only the headers of the file
// `p_from` (of every file of the program when `p_from` is not one of them);
// a header that can't be read is left out. Headers are read once (and kept
// by their contents) by `import_c_library`, and an entry's vector is kept
// by its text, so a header is embedded once.
auto c_header_templates(const std::vector<phrase_file> &p_files,
                        const std::filesystem::path &p_from,
                        const core::c_import_options &p_imports)
    -> std::vector<phrase_template>;

// A package's `# example: LINE` comments (L9), above a `to …:` header or any
// definition: each line must read with no model, in its own file's package
// (the files from `p_first` on are the package's; those before are its
// dependencies'), and above a header it must use the header's words. The
// problems, cited by file and the comment's line; none when all are right.
auto check_package_examples(const std::vector<phrase_file> &p_files, std::size_t p_first,
                            const front_end_options &p_options = {})
    -> std::vector<string_type>;

// The built-in phrases and `p_more` (a program's own `phrases/` folders)
// that a query is about, best first, at most `p_limit` (0: all); with no
// words, every one by its file. With `p_embeddings` the lexical order and the
// order by meaning are joined by rank, so a synonym finds its phrase and a
// shared word still counts; a model that can't embed leaves it lexical.
auto manual_search(const std::vector<string_type> &p_words,
                   const std::vector<phrase_template> &p_more,
                   std::size_t p_limit,
                   const manual_embeddings &p_embeddings = {})
    -> std::vector<manual_entry>;

// How many of a search's first entries the model chooses among (L5): the
// first 5 hold the entry meant for 33 of 34 queries (state.md, "manual
// measured"), and each more makes the question longer.
inline constexpr std::size_t manual_choice_count = 5;

// L5: how likely the chat model finds each of `p_entries` (at most
// `english::max_choice_options - 1`) to do what `p_query` asks, then "none of
// these", summing to 1. The model only reads the entries' own words. Throws
// `error`.
auto manual_choice_probabilities(const string_type &p_query,
                                 const std::vector<manual_entry> &p_entries,
                                 english::language_model &p_model)
    -> std::vector<double>;

// The search's entries with the model's pick first (L5): the first
// `manual_choice_count` are shown to the model, and its pick moves to the
// front only when it is sure (60%, and 30 points ahead of the next, "none of
// these" included), as a question's answer is taken. Everything else keeps
// the search's order, and a model that fails leaves it as it was.
auto manual_choose(const string_type &p_query, std::vector<manual_entry> p_entries,
                   english::language_model &p_model) -> std::vector<manual_entry>;

// The manual as web pages in `p_directory` (L6, `ovis manual --html DIR`):
// `index.html` lists every entry under its topic, and one page per topic (per
// package and topic) has each entry with its sentence, other ways of saying
// it and examples. Links are checked once the files are written: the problems
// (a link to a page or an anchor that isn't there), none when all are right.
auto write_manual_html(const std::vector<manual_entry> &p_entries,
                       const std::filesystem::path &p_directory)
    -> std::vector<string_type>;

// Reads a program in phrases. Unclear spans are settled by their `Read as:`
// line, else by `p_model` choosing between readings that type-check (written
// back as a `Read as:` line), else reported to the author. Without a model
// (`p_model == nullptr`), unsettled spans are reported.
auto compile_phrases(string_view_type p_source, english::language_model *p_model,
                     const front_end_options &p_options = {})
    -> front_end_result;

} // namespace ovis::phrase

#endif // OVIS_PHRASE_FRONT_END_HPP

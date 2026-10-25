#if !defined(_WIN32)
#include <sys/ioctl.h>
#endif
#ifdef __linux__
#include <cerrno>
#include <csignal>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#elif !defined(_WIN32)
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/Program.h>

#include <ovis/codegen/llvm_codegen.hpp>
#include <ovis/codegen/runtime.hpp>
#include <ovis/core/c_import.hpp>
#include <ovis/english/model_daemon.hpp>
#include <ovis/phrase/front_end.hpp>
#include <ovis/phrase/phrase_check.hpp>
#include <ovis/phrase/templates.hpp>
#include <ovis/project/cmake_project.hpp>
#include <ovis/project/lock.hpp>
#include <ovis/project/native.hpp>
#include <ovis/project/packages.hpp>
#include <ovis/project/project.hpp>
#include <ovis/project/toml.hpp>

namespace {

constexpr const char *usage = R"usage(Ovis: programs written in plain English, compiled with LLVM.

Usage: ovis <command> <file or folder> [options]
       ovis <command> [options]     in a project: the nearest ovis.toml's program
       ovis new NAME | init         make a project (ovis.toml, main.ovis)
       ovis --version
       ovis model <path | pull [3b|7b] | embed MODEL -o OUTPUT | stop>
       ovis runtime [-o FILE]

Commands:
  run        compile and run the program
  build      compile to an executable
  ir         print the generated LLVM IR
  check      report problems without building, asking the model or
             writing any file (--json: as JSON, for editors)
  fix        offer the readings of each unclear line; the one taken is
             written above it as @[meaning: …] (--line [FILE:]N with
             --apply K, or --meaning TEXT, takes one without asking)
  fmt        put the @[…] lines in order, and turn old Read as: and
             Compiler: lines into them; builds nothing (--check: only
             say which files would change)
  phrases    the built-in phrases with these words (ovis phrases round
             decimals), or every one with no words
  manual     the phrases a question is about, with what each does and an
             example (ovis manual make every letter big); --in FILE adds
             that program's phrases/ folders, --line N searches with the
             words of line N of FILE and says which names are set there;
             --for NAME|TYPE lists the phrases that take a name (set before
             --line N, else at the end of FILE) or a type; no words lists
             every phrase by topic;
             --embeddings also compares meanings with the model (no better yet)
  check-phrases
             whether this program's own phrases (its `to …:` blocks and
             helper files) make the questions Ovis asks about other lines
             worse; no model (exit 1 if they do). --dependencies:
             which of its lines its packages' phrases change
  new NAME   make the project NAME/: ovis.toml, a main.ovis saying hello,
             .gitignore, git init (init: the same in this folder)
  update     move the project's packages to the newest version on their
             line, and its git C dependencies to what their tags point to
             now, in ovis.lock (update NAME, update c NAME: one)
  tree       the packages the project uses, and why each version
  add NAME --git URL [--version V] | --path DIR
             depend on a package (by default its newest version); add c NAME
             --pkg-config P | --git URL --tag T | --path DIR [--targets A,B]
             a C library; remove [c] NAME the other way
  fetch      download what ovis.lock names, for building --offline later
  vendor     copy every package into vendor/, which builds then use
  release X.Y.Z
             in a package: check it builds as its users will, then tag vX.Y.Z
  cmake      write the CMake project generated from ovis.toml into
             build/cmake and say where (--print: print it instead)

  runtime      print the C source of the runtime a program is linked with
               (CMake builds it as ovis::runtime)

  model path   show which built-in model would be used
  model pull   download the default model into the cache directory: 7B
               where a GPU runs it, else 3B (or name one: 3b, 7b)
  model embed  write a copy of ovis with MODEL built into the executable
  model stop   stop the model daemon for these options, if one is running

Options:
  -o <path>           output executable for `build` (default: the program's name)
  -I <dir>, -L <dir>  where to look for C headers and libraries of `use` lines
  -D <name[=value]>   define a macro while reading C headers
  --phrases <dir>     also read the phrase files (.phr, .core, .generic) in
                      <dir>; a program folder's own phrases/ is read always
  @FILE               more arguments, one or more per line (CMake response file)
  --offline           never ask the model; unclear lines are reported
  --model-budget <s>  seconds the model may spend on questions (default: 10)
  --keep-source       don't write `@[meaning: …]` lines into the source
  --no-prompt         don't offer fixes when a build in a terminal fails
  --quiet             don't print progress

Checking the program as it runs:
  --fast              no checks: freed values are not recognised, runtime
                      errors don't name the line (the default checks)
  --report-leaks      at exit, list the values the program never freed
  --lax-memory        don't fail a build that makes a new value where nothing
                      can free it, loses one, or uses one after its memory
                      (these are errors by default; docs/proposals/memory.md)

Under CMake (see ovis_add_executable in README.md):
  --object <file>     `build` stops at an object file; the caller links it with
                      the runtime and the C libraries
  --link-by-caller    leave the C libraries to the caller: no pkg-config or -l
  --locked            never ask the model or change a file: fail, naming the
                      line, if the build would (settle it with `ovis run`)
  --depfile <file>    write the files the build read, in Make syntax
  --link-flags <file> write the libraries the `use` lines need (pkg-config's,
                      -l), one per line, for the caller's linker (@file)
  --verbose           show llama.cpp's log

Built-in model (the default):
  --model-path <gguf> model file to use (default: $OVIS_MODEL_PATH, the model
                      embedded in ovis, or a default model next to ovis or in
                      the cache directory: 7B first with a GPU, else 3B first)
  --cpu               run the model on the CPU even if there is a GPU
  --threads <n>       CPU threads (default: the number of performance cores)
  --no-daemon         load the model in this process, not in the daemon that
                      keeps it loaded between builds (also $OVIS_NO_DAEMON=1)
  --daemon-idle <m>   minutes the daemon stays without questions (default: 15)

Model server instead of the built-in model:
  --endpoint <url>    OpenAI-compatible API, e.g. http://127.0.0.1:8080/v1
                      (default: $OVIS_ENDPOINT if set)
  --model <name>      model name to request (default: $OVIS_MODEL or "local")
  --api-key <key>     sent as a bearer token (default: $OVIS_API_KEY)
  --no-grammar        don't send the GBNF grammar (for servers that reject it)

A program is one .ovis file, or a folder with a main.ovis: then every .ovis
file in the folder and its subfolders is a module of the program. main.ovis
says what the program does; the other files only define things.

Ovis reads every line as phrases it knows. Where a line could mean several
things, or uses words it doesn't know, the built-in model picks between the
readings that make sense (or writes the line again in words Ovis reads), and
Ovis writes the line as it reads it above the line:
    @[meaning: show average of numbers]
    show mean of numbers
Edit or write such lines yourself to decide. What the model chose is also
kept in <file>.lock, with the line each meaning is for: commit it, so builds
need no model and a meaning whose line you edit is read again.

Loading the model takes seconds, so the first build that needs it starts
`ovis model serve` in the background, which keeps it loaded for the builds
after it and quits once unused for a while.
)usage";

struct arguments {
  std::string command;
  std::vector<std::string> positional;
  std::filesystem::path output;
  ovis::english::endpoint_options endpoint;
  bool use_endpoint = false;
  std::optional<std::filesystem::path> model_path;
  int threads = 0;
  bool use_gpu = true;
  bool verbose = false;
  ovis::core::c_import_options c_imports;
  bool offline = false;
  // `manual --embeddings`: also find phrases by meaning (off: measured no better).
  bool manual_embeddings = false;
  bool quiet = false;
  bool keep_source = false;
  double model_budget = 10;
  // `check --json`; `fix --line [FILE:]N --apply K | --meaning TEXT`.
  bool json = false;
  // `fmt --check`.
  bool check_only = false;
  // `check-phrases --dependencies`.
  bool dependencies = false;
  // `cmake --print`.
  bool print = false;
  bool prompt = true;
  std::string fix_line;
  // `manual --in FILE`: the phrases in sight of this program.
  std::filesystem::path manual_in;
  // `manual --for NAME|TYPE`: the phrases that take it.
  std::string manual_for;
  std::size_t fix_apply = 0;
  std::string fix_meaning;
  // For builds under CMake.
  std::filesystem::path object;
  std::filesystem::path depfile;
  std::filesystem::path link_flags;
  bool link_by_caller = false;
  bool locked = false;
  // Programs are checked (use after free, lines in errors) unless `--fast`.
  bool fast = false;
  std::vector<std::filesystem::path> phrase_directories;
  bool report_leaks = false;
  // The strict memory rules are on; `--lax-memory` turns them off.
  bool strict_memory = true;
  bool use_daemon = true;
  int daemon_idle_minutes = 15;
  // `model serve`: where to listen, and the pipe to say it is ready on.
  std::filesystem::path socket;
  int ready_fd = -1;
  // `model serve`: weights inside a file (an ovis with them embedded).
  std::optional<std::uint64_t> model_offset;
  std::uint64_t model_size = 0;
  // From ovis.toml: what the program is linked with besides its own `use`
  // lines (the project's C objects, pkg-config's libraries).
  std::vector<std::string> project_link_inputs;
  // The packages' C objects and libraries (packages.md §4).
  std::vector<std::string> package_link_inputs;
  // The project the program is in, and whether `run`/`build` go through
  // the CMake project generated from it (proposals/projects.md §4).
  std::optional<ovis::project::project> project;
  bool cmake_build = false;
  // `ovis update`: the dependencies to look up again (none: all of them).
  std::vector<std::string> update_names;
  // `ovis add`: where the dependency is and which version.
  std::string git_url;
  std::string package_version;
  std::string tag;
  std::string commit;
  std::string path;
  std::string pkg_config;
  std::vector<std::string> targets;
  // The packages the program uses (proposals/packages.md), read before it,
  // and the project's ovis.lock as this build left it.
  ovis::project::package_graph packages;
  ovis::project::project_lock lock;
  bool lock_changed = false;
  // The options as given (not -o), for the ovis that CMake runs.
  std::vector<std::string> options;
};

auto environment(const char *p_name, std::string p_fallback) -> std::string {
  const char *const value = std::getenv(p_name);
  return value != nullptr && *value != '\0' ? value : std::move(p_fallback);
}

// The arguments, with each `@FILE` replaced by the words in FILE (split at
// white space; "double quotes" keep spaces), as CMake writes response files.
auto expand_response_files(int p_count, char **p_values)
    -> std::vector<std::string> {
  std::vector<std::string> result;
  for (int index = 0; index < p_count; ++index) {
    const std::string argument = p_values[index];
    if (index == 0 || !argument.starts_with("@") || argument.size() == 1) {
      result.push_back(argument);
      continue;
    }
    std::ifstream file(argument.substr(1));
    if (!file)
      throw ovis::error("Cannot read " + argument.substr(1) + ".");
    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto text = buffer.str();
    std::string word;
    bool quoted = false;
    bool started = false;
    for (std::size_t at = 0; at < text.size(); ++at) {
      const char character = text[at];
      if (character == '"') {
        quoted = !quoted;
        started = true;
      } else if (character == '\\' && at + 1 < text.size() && quoted) {
        word.push_back(text[++at]);
      } else if (std::isspace(static_cast<unsigned char>(character)) && !quoted) {
        if (started)
          result.push_back(std::move(word));
        word.clear();
        started = false;
      } else {
        word.push_back(character);
        started = true;
      }
    }
    if (started)
      result.push_back(std::move(word));
  }
  return result;
}

// The commands about a project's dependencies (packages.md §9): they name
// dependencies, not a program.
auto is_dependency_command(const std::string &p_command) -> bool {
  return p_command == "add" || p_command == "remove" || p_command == "vendor" ||
         p_command == "fetch" || p_command == "release";
}

// The commands that build or read a program, which in a project need
// none named (proposals/projects.md §2).
auto takes_project(const std::string &p_command) -> bool {
  return p_command == "run" || p_command == "build" || p_command == "ir" ||
         p_command == "check" || p_command == "fix" || p_command == "fmt" ||
         p_command == "check-phrases" || p_command == "init" ||
         p_command == "cmake" || p_command == "update" || p_command == "tree" ||
         is_dependency_command(p_command);
}

auto parse_arguments(int p_count, char **p_values) -> arguments {
  // `ovis --version`: what ovisup checks an installed toolchain runs with.
  if (p_count == 2 && (std::string_view(p_values[1]) == "--version" ||
                       std::string_view(p_values[1]) == "-V"))
    return {.command = "version"};
  // `ovis runtime [-o FILE]` prints the runtime's C source.
  if (p_count >= 2 && std::string_view(p_values[1]) == "runtime") {
    arguments result{.command = "runtime"};
    if (p_count == 4 && std::string_view(p_values[2]) == "-o")
      result.output = p_values[3];
    else if (p_count != 2)
      throw ovis::error("Usage: ovis runtime [-o FILE]");
    return result;
  }
  // `ovis phrases [WORDS…]`: words, not a program.
  if (p_count >= 2 && std::string_view(p_values[1]) == "phrases") {
    arguments result{.command = "phrases"};
    for (int index = 2; index < p_count; ++index)
      result.positional.emplace_back(p_values[index]);
    return result;
  }
  if (p_count < 2)
    throw ovis::error(usage);

  arguments result;
  result.command = p_values[1];
  const auto endpoint = environment("OVIS_ENDPOINT", "");
  result.use_endpoint = !endpoint.empty();
  if (result.use_endpoint)
    result.endpoint.endpoint = endpoint;
  result.endpoint.model = environment("OVIS_MODEL", result.endpoint.model);
  result.endpoint.api_key = environment("OVIS_API_KEY", "");
  result.use_daemon = environment("OVIS_NO_DAEMON", "0") == "0";

  for (int index = 2; index < p_count; ++index) {
    const std::string option = p_values[index];
    const auto value = [&]() -> std::string {
      if (index + 1 >= p_count)
        throw ovis::error(option + " needs a value.");
      return p_values[++index];
    };
    const int first = index;
    if (!option.starts_with("-"))
      result.positional.push_back(option);
    else if (option == "-o")
      result.output = value();
    else if (option == "--endpoint") {
      result.endpoint.endpoint = value();
      result.use_endpoint = true;
    } else if (option == "--model")
      result.endpoint.model = value();
    else if (option == "--api-key")
      result.endpoint.api_key = value();
    else if (option == "--no-grammar")
      result.endpoint.use_grammar = false;
    else if (option == "--model-path")
      result.model_path = value();
    else if (option == "-I")
      result.c_imports.include_directories.emplace_back(value());
    else if (option == "-L")
      result.c_imports.library_directories.emplace_back(value());
    else if (option == "-D")
      result.c_imports.compiler_flags.push_back("-D" + value());
    // As CMake writes them: -I/dir, -L/dir, -DNAME=1 with no space.
    else if (option.starts_with("-I") && option.size() > 2)
      result.c_imports.include_directories.emplace_back(option.substr(2));
    else if (option.starts_with("-L") && option.size() > 2)
      result.c_imports.library_directories.emplace_back(option.substr(2));
    else if (option.starts_with("-D") && option.size() > 2)
      result.c_imports.compiler_flags.push_back(option);
    else if (option == "--phrases")
      result.phrase_directories.emplace_back(value());
    else if (option == "--fast")
      result.fast = true;
    else if (option == "--report-leaks")
      result.report_leaks = true;
    else if (option == "--strict-memory")
      result.strict_memory = true;
    else if (option == "--lax-memory")
      result.strict_memory = false;
    else if (option == "--object")
      result.object = value();
    else if (option == "--depfile")
      result.depfile = value();
    else if (option == "--link-flags")
      result.link_flags = value();
    else if (option == "--link-by-caller") {
      result.link_by_caller = true;
      result.c_imports.link_by_caller = true;
    } else if (option == "--locked")
      result.locked = true;
    else if (option == "--threads")
      result.threads = std::stoi(value());
    else if (option == "--cpu")
      result.use_gpu = false;
    else if (option == "--verbose")
      result.verbose = true;
    else if (option == "--offline")
      result.offline = true;
    else if (option == "--embeddings")
      result.manual_embeddings = true;
    else if (option == "--quiet")
      result.quiet = true;
    else if (option == "--keep-source")
      result.keep_source = true;
    else if (option == "--json")
      result.json = true;
    else if (option == "--check")
      result.check_only = true;
    else if (option == "--dependencies")
      result.dependencies = true;
    else if (option == "--print")
      result.print = true;
    else if (option == "--git")
      result.git_url = value();
    else if (option == "--version")
      result.package_version = value();
    else if (option == "--tag")
      result.tag = value();
    else if (option == "--commit")
      result.commit = value();
    else if (option == "--path")
      result.path = value();
    else if (option == "--pkg-config")
      result.pkg_config = value();
    else if (option == "--targets") {
      std::stringstream list(value());
      for (std::string target; std::getline(list, target, ',');)
        if (!target.empty())
          result.targets.push_back(target);
    }
    else if (option == "--no-prompt")
      result.prompt = false;
    else if (option == "--in")
      result.manual_in = value();
    else if (option == "--line")
      result.fix_line = value();
    else if (option == "--for")
      result.manual_for = value();
    else if (option == "--apply")
      result.fix_apply = std::stoul(value());
    else if (option == "--meaning")
      result.fix_meaning = value();
    else if (option == "--model-budget")
      result.model_budget = std::stod(value());
    else if (option == "--no-daemon")
      result.use_daemon = false;
    else if (option == "--daemon-idle")
      result.daemon_idle_minutes = std::stoi(value());
    else if (option == "--socket")
      result.socket = value();
    else if (option == "--ready-fd")
      result.ready_fd = std::stoi(value());
    else if (option == "--model-offset")
      result.model_offset = std::stoull(value());
    else if (option == "--model-size")
      result.model_size = std::stoull(value());
    else
      throw ovis::error("Unknown option " + option + ".\n\n" + usage);
    // The option and its value as given, for the ovis CMake runs.
    if (option.starts_with("-") && option != "-o")
      for (int at = first; at <= index; ++at)
        result.options.emplace_back(p_values[at]);
  }
  // In a project the program is the nearest ovis.toml's (apply_project).
  if (result.positional.empty() && !takes_project(result.command) &&
      result.command != "manual")
    throw ovis::error(usage);
  return result;
}

// `ovis new NAME` and `ovis init`: a project that builds and says hello.
// init keeps every file that is already there.
auto make_project(const arguments &p_arguments) -> int {
  const bool fresh = p_arguments.command == "new";
  if (fresh && p_arguments.positional.size() != 1)
    throw ovis::error("Usage: ovis new NAME");
  if (!fresh && !p_arguments.positional.empty())
    throw ovis::error("Usage: ovis init (in the folder to make a project of)");
  const auto root = fresh ? std::filesystem::path(p_arguments.positional[0])
                          : std::filesystem::current_path();
  std::error_code ignored;
  if (fresh && std::filesystem::exists(root) &&
      !std::filesystem::is_empty(root, ignored))
    throw ovis::error(root.string() + " already exists; use `ovis init` in it.");
  const auto name =
      std::filesystem::absolute(root).lexically_normal().filename().string();
  std::filesystem::create_directories(root);
  const auto write = [&](const char *p_file, const std::string &p_text) {
    if (std::filesystem::exists(root / p_file))
      return;
    std::ofstream(root / p_file) << p_text;
  };
  if (std::filesystem::exists(root / ovis::project::project_file_name))
    throw ovis::error((root / ovis::project::project_file_name).string() +
                      " already exists.");
  write(ovis::project::project_file_name,
        "[project]\nname = " + ovis::project::toml_string(name) + "\n");
  write("main.ovis", "# Says hello.\nshow \"Hello, world!\"\n");
  write(".gitignore", "/build/\n");
  if (!std::filesystem::exists(root / ".git"))
    if (const auto git = llvm::sys::findProgramByName("git")) {
      const std::optional<llvm::StringRef> quiet[] = {std::nullopt, llvm::StringRef(),
                                                      llvm::StringRef()};
      llvm::sys::ExecuteAndWait(*git, {*git, "-C", root.string(), "init", "-q"},
                                std::nullopt, quiet);
    }
  if (!p_arguments.quiet)
    std::cerr << "ovis: made the project " << name << " in " << root.string()
              << "\n      run it with `" << (fresh ? "cd " + root.string() + " && " : "")
              << "ovis run`\n";
  return 0;
}

auto lock_project(arguments &p_arguments, const std::vector<std::string> &p_update)
    -> ovis::project::project_lock;
auto write_source(const std::filesystem::path &p_path, std::string_view p_text) -> void;
auto add_use_line(const std::filesystem::path &p_path, const std::string &p_name) -> bool;
auto remove_use_line(const std::filesystem::path &p_path, const std::string &p_name) -> bool;

// `ovis add NAME --git URL [--version 1.2]` (or `--path DIR`), `ovis add c
// NAME --pkg-config P | --git URL --tag T | --path DIR | --cmake FILE
// --targets A,B`, `ovis remove [c] NAME`: ovis.toml edited as text, so the
// author's comments and order stay; checked by reading it again.
auto edit_dependencies(arguments &p_arguments, const std::filesystem::path &p_file)
    -> void {
  const auto words = p_arguments.update_names;
  const bool c = !words.empty() && words.front() == "c";
  if (words.size() != (c ? 2u : 1u))
    throw ovis::error(p_arguments.command == "add"
                          ? "Usage: ovis add NAME --git URL [--version V] | --path DIR\n"
                            "       ovis add c NAME --pkg-config P | --git URL --tag T "
                            "--targets A,B | --path DIR --targets A,B"
                          : "Usage: ovis remove NAME | ovis remove c NAME");
  const auto name = words.back();
  std::ifstream input(p_file);
  std::vector<std::string> lines;
  for (std::string line; std::getline(input, line);)
    lines.push_back(line);
  const auto project = ovis::project::load_project(p_file);
  const auto header = c ? "[c." + name + "]" : std::string("[dependencies]");
  const auto trimmed = [](std::string p_text) {
    while (!p_text.empty() && (p_text.back() == ' ' || p_text.back() == '\r'))
      p_text.pop_back();
    while (!p_text.empty() && p_text.front() == ' ')
      p_text.erase(0, 1);
    return p_text;
  };
  if (p_arguments.command == "remove") {
    if (c) {
      const auto found = std::ranges::find_if(
          project.c_dependencies, [&](const auto &p_c) { return p_c.name == name; });
      if (found == project.c_dependencies.end())
        throw ovis::error("ovis.toml has no [c." + name + "].");
      // The table, from its header to the next one.
      auto first = found->line - 1;
      while (first < lines.size() && trimmed(lines[first]) != header)
        --first;
      auto last = first + 1;
      while (last < lines.size() && !trimmed(lines[last]).starts_with("["))
        ++last;
      lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(first),
                  lines.begin() + static_cast<std::ptrdiff_t>(last));
    } else {
      const auto found = std::ranges::find_if(
          project.dependencies, [&](const auto &p_d) { return p_d.name == name; });
      if (found == project.dependencies.end())
        throw ovis::error("ovis.toml has no dependency `" + name + "`.");
      if (!trimmed(lines[found->line - 1]).starts_with(ovis::project::toml_key(name)))
        throw ovis::error("ovis.toml, line " + std::to_string(found->line) +
                          ": remove `" + name + "` by hand (it is not on one line).");
      lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(found->line - 1));
    }
  } else if (c) {
    if (std::ranges::any_of(project.c_dependencies,
                            [&](const auto &p_c) { return p_c.name == name; }))
      throw ovis::error("ovis.toml already has [c." + name + "].");
    std::vector<std::string> table = {"", header};
    if (!p_arguments.pkg_config.empty())
      table.push_back("pkg-config = " + ovis::project::toml_string(p_arguments.pkg_config));
    else if (!p_arguments.git_url.empty()) {
      table.push_back("git = " + ovis::project::toml_string(p_arguments.git_url));
      table.push_back(p_arguments.commit.empty()
                          ? "tag = " + ovis::project::toml_string(p_arguments.tag)
                          : "commit = " + ovis::project::toml_string(p_arguments.commit));
    } else if (!p_arguments.path.empty())
      table.push_back("path = " + ovis::project::toml_string(p_arguments.path));
    else
      throw ovis::error("Say where [c." + name + "] is: --pkg-config, --git (with --tag) "
                        "or --path.");
    if (!p_arguments.targets.empty()) {
      std::string list;
      for (const auto &target : p_arguments.targets)
        list += (list.empty() ? "" : ", ") + ovis::project::toml_string(target);
      table.push_back("targets = [" + list + "]");
    }
    lines.insert(lines.end(), table.begin(), table.end());
  } else {
    if (std::ranges::any_of(project.dependencies,
                            [&](const auto &p_d) { return p_d.name == name; }))
      throw ovis::error("ovis.toml already depends on `" + name + "`.");
    std::string entry;
    if (!p_arguments.path.empty())
      entry = "path = " + ovis::project::toml_string(p_arguments.path);
    else if (!p_arguments.git_url.empty()) {
      auto wanted = p_arguments.package_version;
      // The newest version, as the oldest this project works with.
      if (wanted.empty() && p_arguments.commit.empty()) {
        const auto tags = ovis::project::list_version_tags(p_arguments.git_url);
        if (tags.empty())
          throw ovis::error(p_arguments.git_url + " has no version tags (vX.Y.Z).");
        wanted = tags.rbegin()->first.text();
      }
      entry = "git = " + ovis::project::toml_string(p_arguments.git_url) +
              (p_arguments.commit.empty()
                   ? ", version = " + ovis::project::toml_string(wanted)
                   : ", commit = " + ovis::project::toml_string(p_arguments.commit));
    } else
      throw ovis::error("Say where `" + name + "` is: --git URL or --path DIR.");
    const auto line = ovis::project::toml_key(name) + " = { " + entry + " }";
    const auto at = std::ranges::find_if(lines, [&](const auto &p_line) {
      return trimmed(p_line) == header;
    });
    if (at == lines.end()) {
      lines.push_back("");
      lines.push_back(header);
      lines.push_back(line);
    } else {
      // After the table's last key, before the blank lines that end it.
      auto end = at + 1;
      while (end != lines.end() && !trimmed(*end).starts_with("["))
        ++end;
      while (end != at + 1 && trimmed(*(end - 1)).empty())
        --end;
      lines.insert(end, line);
    }
  }
  std::string text;
  for (const auto &line : lines)
    text += line + "\n";
  // Never left broken: read again before it is written.
  ovis::project::parse_project(text, p_file);
  write_source(p_file, text);
  if (!p_arguments.quiet)
    std::cerr << "ovis: " << (p_arguments.command == "add" ? "added " : "removed ")
              << (c ? "[c." + name + "]" : name) << " in " << p_file.string() << '\n';
  // A file reads with a package only where it says `use NAME`: the
  // program's main.ovis says it (phrase-scope.md S2).
  if (c)
    return;
  const auto main_file = p_file.parent_path() / "main.ovis";
  const bool exists = std::filesystem::exists(main_file);
  const bool changed = exists && (p_arguments.command == "add"
                                      ? add_use_line(main_file, name)
                                      : remove_use_line(main_file, name));
  if (p_arguments.quiet)
    return;
  if (changed)
    std::cerr << "ovis: " << (p_arguments.command == "add" ? "wrote `use " : "took out `use ")
              << name << "` " << (p_arguments.command == "add" ? "into " : "from ")
              << main_file.string() << '\n';
  else if (!exists && p_arguments.command == "add")
    std::cerr << "ovis: write `use " << name << "` at the top of the files that use it\n";
}

// A program in a project (proposals/projects.md §2–§4): with no program
// named, or a project's folder named, the nearest ovis.toml says what to
// build and how; flags on the command line add to its settings. A single
// file named builds as before, ignoring any project.
auto apply_project(arguments &p_arguments) -> void {
  const auto package_only = [](const arguments &p_checked, const ovis::project::project &p_project) {
    return p_checked.command == "check" && p_project.package &&
           !std::filesystem::exists(p_project.root / "main.ovis");
  };
  if (!takes_project(p_arguments.command) || p_arguments.command == "init")
    return;
  std::optional<std::filesystem::path> file;
  // `ovis update [c NAME | c.NAME …]` names dependencies, not a program;
  // `ovis tree` none; add, remove and the rest theirs.
  if (is_dependency_command(p_arguments.command)) {
    file = ovis::project::find_project(std::filesystem::current_path());
    p_arguments.update_names = p_arguments.positional;
    p_arguments.positional.clear();
    if (!file)
      throw ovis::error("`ovis " + p_arguments.command +
                        "` is for a project: no ovis.toml here or above.");
    if (p_arguments.command == "add" || p_arguments.command == "remove")
      edit_dependencies(p_arguments, *file);
  } else if (p_arguments.command == "update" || p_arguments.command == "tree") {
    file = ovis::project::find_project(std::filesystem::current_path());
    for (std::size_t index = 0; index < p_arguments.positional.size(); ++index) {
      const auto &word = p_arguments.positional[index];
      if (word == "c" && index + 1 < p_arguments.positional.size())
        p_arguments.update_names.push_back(p_arguments.positional[++index]);
      else
        p_arguments.update_names.push_back(word.starts_with("c.") ? word.substr(2)
                                                                   : word);
    }
    p_arguments.positional.clear();
  } else if (p_arguments.positional.empty()) {
    file = ovis::project::find_project(std::filesystem::current_path());
    if (!file)
      throw ovis::error("No program named, and no " +
                        std::string(ovis::project::project_file_name) +
                        " here or in a folder above: name one (`ovis " +
                        p_arguments.command + " FILE`), or make a project "
                        "with `ovis new NAME`. `ovis --help` lists the "
                        "commands.");
  } else if (std::filesystem::is_directory(p_arguments.positional[0]) &&
             std::filesystem::exists(
                 std::filesystem::path(p_arguments.positional[0]) /
                 ovis::project::project_file_name)) {
    file = std::filesystem::path(p_arguments.positional[0]) /
           ovis::project::project_file_name;
  }
  if (!file && (p_arguments.command == "cmake" || p_arguments.command == "update" ||
                p_arguments.command == "tree"))
    throw ovis::error("`ovis " + p_arguments.command +
                      "` is for a project: no ovis.toml here or above.");
  if (!file)
    return;
  const auto project = ovis::project::load_project(*file);
  if (p_arguments.command != "update" && p_arguments.command != "tree" &&
      !is_dependency_command(p_arguments.command) && !package_only(p_arguments, project) &&
      !std::filesystem::exists(project.root / "main.ovis"))
    throw ovis::error(project.root.string() + " has no main.ovis: a project's "
                                              "program starts there.");
  p_arguments.positional.insert(p_arguments.positional.begin(),
                                project.root.string());
  if (p_arguments.positional.size() > 1)
    p_arguments.positional.erase(p_arguments.positional.begin() + 1);
  const auto build_directory = project.root / "build";
  p_arguments.project = project;
  // ovis.lock: git C pins and the package graph, for every command that
  // builds or reads the program, and for update.
  if (p_arguments.command != "fmt" &&
      (p_arguments.command != "check-phrases" || p_arguments.dependencies)) {
    std::vector<std::string> update;
    if (p_arguments.command == "update")
      update = p_arguments.update_names.empty() ? std::vector<std::string>{"*"}
                                                : p_arguments.update_names;
    p_arguments.lock = lock_project(p_arguments, update);
  }
  // With CMake, CMake builds the C (the packages' git, path and cmake
  // tables too); check, fix and fmt read headers with the [build] and
  // pkg-config flags alone.
  const bool cmake = ovis::project::needs_cmake(*p_arguments.project);
  if (cmake && p_arguments.command == "ir")
    throw ovis::error(file->string() + ": `ovis ir` isn't available for a project "
                                       "built through CMake; name its main.ovis.");
  p_arguments.cmake_build =
      cmake && (p_arguments.command == "run" || p_arguments.command == "build") &&
      p_arguments.object.empty();
  if (p_arguments.command == "cmake" || p_arguments.command == "update" ||
      p_arguments.command == "tree" || is_dependency_command(p_arguments.command) ||
      package_only(p_arguments, project))
    return;
  // check and fix read the program without CMake: the include folders and
  // definitions its targets give come from the flags file the last build's
  // CMake wrote (ovis_add_executable), when there is one.
  if (cmake && !p_arguments.cmake_build) {
    const auto flags = project.root / "build" / "cmake-build" /
                       (project.program_name() + ".ovis.rsp");
    if (std::filesystem::exists(flags)) {
      std::string response = "@" + flags.string();
      std::string program = "ovis";
      char *values[] = {program.data(), response.data()};
      const auto words = expand_response_files(2, values);
      for (std::size_t index = 1; index < words.size(); ++index) {
        if (words[index].starts_with("-I"))
          p_arguments.c_imports.include_directories.emplace_back(words[index].substr(2));
        else if (words[index].starts_with("-D"))
          p_arguments.c_imports.compiler_flags.push_back(words[index]);
      }
    }
  }
  const bool compiles = !cmake && (p_arguments.command == "run" ||
                                   p_arguments.command == "build" ||
                                   p_arguments.command == "ir");
  const auto parts = ovis::project::build_native_parts(project, build_directory,
                                                       compiles);
  // The file's settings first, the command line's added after them.
  auto &imports = p_arguments.c_imports;
  imports.include_directories.insert(imports.include_directories.begin(),
                                     parts.include_directories.begin(),
                                     parts.include_directories.end());
  imports.compiler_flags.insert(imports.compiler_flags.begin(),
                                parts.compiler_flags.begin(),
                                parts.compiler_flags.end());
  std::vector<std::filesystem::path> phrases;
  for (const auto &directory : project.phrases)
    phrases.push_back(project.root / directory);
  // A package's phrase files come with it, as its own program's do.
  for (const auto &package : p_arguments.packages.packages) {
    for (const auto &directory : package.manifest.phrases)
      phrases.push_back(package.root / directory);
    if (std::filesystem::is_directory(package.root / "phrases"))
      phrases.push_back(package.root / "phrases");
  }
  p_arguments.phrase_directories.insert(p_arguments.phrase_directories.begin(),
                                        phrases.begin(), phrases.end());
  p_arguments.project_link_inputs = parts.link_inputs;
  // Each package's own headers, defines, pkg-config flags and C files, its
  // objects in the program's build/packages/ (the cache stays read-only).
  const bool compiles_packages = p_arguments.command == "run" ||
                                 p_arguments.command == "build" ||
                                 p_arguments.command == "ir";
  for (const auto &package : p_arguments.packages.packages) {
    auto folder = package.id;
    std::ranges::replace(folder, '@', '-');
    auto manifest = package.manifest;
    // Its git, path and cmake C are in the program's CMake project.
    std::erase_if(manifest.c_dependencies, [](const auto &p_c) {
      return p_c.kind != ovis::project::c_dependency::kind_type::pkg_config;
    });
    const auto package_parts = ovis::project::build_native_parts(
        manifest, build_directory / "packages" / folder, compiles_packages);
    imports.include_directories.insert(imports.include_directories.end(),
                                       package_parts.include_directories.begin(),
                                       package_parts.include_directories.end());
    imports.compiler_flags.insert(imports.compiler_flags.end(),
                                  package_parts.compiler_flags.begin(),
                                  package_parts.compiler_flags.end());
    p_arguments.package_link_inputs.insert(p_arguments.package_link_inputs.end(),
                                           package_parts.link_inputs.begin(),
                                           package_parts.link_inputs.end());
  }
  if (!cmake)
    p_arguments.project_link_inputs.insert(p_arguments.project_link_inputs.end(),
                                           p_arguments.package_link_inputs.begin(),
                                           p_arguments.package_link_inputs.end());
  if (p_arguments.command == "build" && p_arguments.output.empty() &&
      p_arguments.object.empty()) {
    std::filesystem::create_directories(build_directory);
    p_arguments.output = build_directory / project.program_name();
  }
}

auto executable_path(const char *p_argument_zero) -> std::filesystem::path {
  static int anchor;
  return llvm::sys::fs::getMainExecutable(p_argument_zero, &anchor);
}

// Stands in for the built-in model when no weights are installed, so that
// building from `@[meaning: …]` lines and the lockfile still works, and an unclear
// line explains what to do.
class missing_model final : public ovis::english::language_model {
public:
  auto name() const -> std::string override { return "none"; }
  auto chat(const std::vector<ovis::english::chat_message> &,
            std::string_view) -> std::string override {
    throw ovis::error(
        "No model is installed, so unclear lines can't be settled.\n"
        "Run `ovis model pull` to download the default model (" +
        std::string(
            ovis::english::default_model_for(ovis::english::has_gpu())
                .file_name) +
        "), pass --model-path <file.gguf>, or use --endpoint <url> for a "
        "model server.");
  }
};

// Runs the built program and gives back its exit status. On Linux the
// program dies with ovis: killed by a timeout, ovis would otherwise leave a
// program that loops forever running on its own.
auto run_program(const std::string &p_path) -> int {
#ifdef __linux__
  const auto parent = getpid();
  const auto child = fork();
  if (child == 0) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent)
      _exit(127);
    execl(p_path.c_str(), p_path.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  if (child > 0) {
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  }
#endif
  return llvm::sys::ExecuteAndWait(p_path, {p_path});
}

// The live line at the bottom of a terminal while a program is read and
// built: a spinner, what is being done, and for how long. Messages print
// above it; it steps aside for questions and the program's own output.
class status_line {
  std::mutex mutex;
  std::condition_variable wake;
  std::string text;
  std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
  // Shown, and drawn on the screen now.
  bool active = true;
  bool drawn = false;
  bool stopping = false;
  // Whether this stage asks the model before anything else has: then its
  // time is mostly loading it.
  bool first_use = false;
  bool model_used = false;
  std::thread painter;

  auto erase() -> void {
    if (drawn)
      std::cerr << "\r\x1b[2K" << std::flush;
    drawn = false;
  }

  auto columns() const -> std::size_t {
#if !defined(_WIN32)
    winsize size{};
    if (ioctl(2, TIOCGWINSZ, &size) == 0 && size.ws_col > 10)
      return size.ws_col;
#endif
    return 80;
  }

  auto paint() -> void {
    static constexpr std::string_view frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                                  "⠴", "⠦", "⠧", "⠇", "⠏"};
    std::size_t frame = 0;
    std::unique_lock lock(mutex);
    while (!stopping) {
      if (active && !text.empty()) {
        const auto seconds = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - since)
                                 .count();
        auto line = text;
        if (seconds >= 1) {
          char elapsed[32];
          std::snprintf(elapsed, sizeof(elapsed), " · %.1f s", seconds);
          line += elapsed;
        }
        if (first_use && seconds >= 2)
          line += " (loading the model)";
        // Never wider than the terminal: a wrapped line can't be redrawn.
        const auto room = columns() - 3;
        if (line.size() > room)
          line = line.substr(0, room - 1) + "…";
        std::cerr << "\r\x1b[2K\x1b[2m" << frames[frame++ % 10] << "\x1b[0m "
                  << line << std::flush;
        drawn = true;
      }
      wake.wait_for(lock, std::chrono::milliseconds(100));
    }
    erase();
  }

public:
  status_line() : painter([this] { paint(); }) {}
  ~status_line() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    wake.notify_all();
    painter.join();
  }

  // What is being done now; the time counts from when it changed.
  auto set(std::string p_text, bool p_uses_model = false) -> void {
    std::lock_guard lock(mutex);
    if (p_text == text)
      return;
    text = std::move(p_text);
    since = std::chrono::steady_clock::now();
    first_use = p_uses_model && !model_used;
    model_used = model_used || p_uses_model;
  }

  // A line that stays, printed above the live one.
  auto message(std::string_view p_message) -> void {
    std::lock_guard lock(mutex);
    erase();
    // Faint: the summary and the program's output are what matter.
    std::cerr << "\x1b[2movis: " << p_message << "\x1b[0m\n" << std::flush;
  }

  // Off the screen until `resume`: for questions and the program's output.
  auto pause() -> void {
    std::lock_guard lock(mutex);
    active = false;
    erase();
  }
  auto resume() -> void {
    std::lock_guard lock(mutex);
    active = true;
  }
};

// The live line of this build, if it has one.
status_line *live = nullptr;

auto progress_log(const arguments &p_arguments)
    -> std::function<void(ovis::string_view_type)> {
  if (p_arguments.quiet)
    return {};
  return [](ovis::string_view_type p_message) {
    if (live != nullptr)
      live->message(p_message);
    else
      std::cerr << "ovis: " << p_message << '\n';
  };
}

// A build's summary in a terminal: only what happened, in words.
auto terminal_summary(const ovis::phrase::front_end_timing &p_timing,
                      bool p_succeeded) -> std::string {
  const auto seconds = [](double p_milliseconds) {
    char text[32];
    std::snprintf(text, sizeof(text), p_milliseconds < 1000 ? "%.0f ms" : "%.1f s",
                  p_milliseconds < 1000 ? p_milliseconds : p_milliseconds / 1000);
    return std::string(text);
  };
  std::string summary = (p_succeeded ? "\x1b[32m✓\x1b[0m " : "\x1b[31m✗\x1b[0m ") +
                        std::to_string(p_timing.lines) + " lines read in " +
                        seconds(p_timing.lex_ms + p_timing.parse_ms);
  std::vector<std::string> settled;
  const auto add = [&](std::size_t p_count, const std::string &p_how) {
    if (p_count > 0)
      settled.push_back(std::to_string(p_count) + " " + p_how);
  };
  add(p_timing.by_rule, "by a rule");
  add(p_timing.by_ranking, "by the words alone");
  add(p_timing.by_lockfile, "from the lockfile");
  add(p_timing.by_model, "chosen by the model");
  add(p_timing.by_rewrite, "rewritten by the model");
  const auto settled_count = p_timing.by_rule + p_timing.by_ranking +
                             p_timing.by_lockfile + p_timing.by_model +
                             p_timing.by_rewrite;
  if (p_timing.questions > settled_count)
    settled.push_back(std::to_string(p_timing.questions - settled_count) +
                      " left for you");
  if (!settled.empty()) {
    summary += " · " + std::to_string(p_timing.questions) + " unclear: ";
    for (std::size_t index = 0; index < settled.size(); ++index)
      summary += (index > 0 ? ", " : "") + settled[index];
  }
  if (p_timing.asked > 0)
    summary += " · the model took " + seconds(p_timing.model_ms) +
               (p_timing.first_question_ms > 2000
                    ? " (" + seconds(p_timing.first_question_ms) + " with loading)"
                    : "");
  return "\x1b[2m" + summary + "\x1b[0m";
}

// What the live line says for where the front end is.
auto progress_text(const ovis::phrase::build_progress &p_progress) -> std::string {
  using enum ovis::phrase::build_progress::stage_type;
  const auto where = p_progress.file + ", line " + std::to_string(p_progress.line);
  switch (p_progress.stage) {
  case reading:
    return "reading " + where + " of " + std::to_string(p_progress.lines);
  case asking:
    return "asking the model what \"" + p_progress.words + "\" means (" + where + ")";
  case rewriting:
    return "asking the model to rewrite " + where;
  case checking:
    return "checking the program";
  }
  return {};
}

// Whether the built-in model will run on a GPU: then the larger default
// model is preferred.
auto uses_gpu(const arguments &p_arguments) -> bool {
  return p_arguments.use_gpu &&
         ovis::english::has_gpu(p_arguments.verbose);
}

auto builtin_options_of(const arguments &p_arguments,
                        const ovis::english::model_location &p_location)
    -> ovis::english::builtin_options {
  return {.location = p_location,
          .use_gpu = p_arguments.use_gpu,
          .threads = p_arguments.threads,
          .verbose = p_arguments.verbose,
          .log = progress_log(p_arguments)};
}

// What `model serve` needs to load exactly the model this process would.
auto serve_arguments(const arguments &p_arguments,
                     const ovis::english::model_location &p_location)
    -> std::vector<std::string> {
  std::vector<std::string> result = {
      "--model-path", p_location.file.string(), "--threads",
      std::to_string(p_arguments.threads), "--daemon-idle",
      std::to_string(p_arguments.daemon_idle_minutes)};
  if (p_location.is_embedded)
    for (const auto &item :
         {std::string("--model-offset"), std::to_string(p_location.offset),
          std::string("--model-size"), std::to_string(p_location.size)})
      result.push_back(item);
  if (!p_arguments.use_gpu)
    result.emplace_back("--cpu");
  if (p_arguments.verbose)
    result.emplace_back("--verbose");
  return result;
}

auto make_model(const arguments &p_arguments, const char *p_argument_zero)
    -> std::unique_ptr<ovis::english::language_model> {
  if (p_arguments.offline)
    return nullptr;
  if (p_arguments.use_endpoint)
    return std::make_unique<ovis::english::endpoint_model>(
        p_arguments.endpoint);
  if (!ovis::english::has_builtin_model_support())
    throw ovis::error("This build of Ovis has no built-in model; use "
                      "--endpoint <url> or --offline.");
  const auto executable = executable_path(p_argument_zero);
  const auto location = ovis::english::find_builtin_model(
      p_arguments.model_path, executable, uses_gpu(p_arguments));
  if (!location)
    return std::make_unique<missing_model>();
  auto options = builtin_options_of(p_arguments, *location);
  const auto socket =
      p_arguments.use_daemon
          ? ovis::english::daemon_socket_path(options, executable)
          : std::nullopt;
  if (!socket)
    return std::make_unique<ovis::english::builtin_model>(options);

  // Name and fingerprint read the file, not the model, so the lockfile is
  // the same whichever process answers.
  ovis::english::builtin_model unloaded(options);
  return std::make_unique<ovis::english::daemon_model>(
      ovis::english::daemon_options{
          .socket = *socket,
          .executable = executable,
          .serve_arguments = serve_arguments(p_arguments, *location),
          .fallback =
              [options] {
                return std::make_unique<ovis::english::builtin_model>(options);
              },
          .log = progress_log(p_arguments)},
      unloaded.name(), unloaded.fingerprint());
}

auto run_model_command(const arguments &p_arguments,
                       const char *p_argument_zero) -> int {
  const auto &subcommand = p_arguments.positional[0];
  const auto executable = executable_path(p_argument_zero);

  const bool gpu = uses_gpu(p_arguments);
  if (subcommand == "path") {
    const auto location =
        ovis::english::find_builtin_model(p_arguments.model_path, executable, gpu);
    if (!location) {
      std::cout << "No model found. Run `ovis model pull`, or put "
                << ovis::english::default_model_for(gpu).file_name << " in "
                << ovis::english::model_cache_directory().string() << ".\n";
      return 1;
    }
    std::cout << location->file.string();
    if (location->is_embedded)
      std::cout << " (embedded, " << location->size << " bytes at offset "
                << location->offset << ")";
    std::cout << '\n';
    return 0;
  }

  if (subcommand == "pull") {
    // The larger model where a GPU runs it, unless asked for one.
    const auto *model = &ovis::english::default_model_for(gpu);
    if (p_arguments.positional.size() > 1) {
      const auto found = std::ranges::find(ovis::english::default_models(),
                                           p_arguments.positional[1],
                                           &ovis::english::default_model::size);
      if (found == ovis::english::default_models().end())
        throw ovis::error("Unknown model size " + p_arguments.positional[1] +
                          "; use 3b or 7b.");
      model = &*found;
    }
    const auto directory = ovis::english::model_cache_directory();
    std::filesystem::create_directories(directory);
    const auto target = directory / model->file_name;
    const auto partial = target.string() + ".part";
    const std::string url(model->url);
    std::cerr << "ovis: downloading " << url << "\n      to " << target.string()
              << '\n';
    // curl and wget handle proxies, redirects and resuming better than an
    // embedded HTTP client would.
    int status = -1;
    if (auto curl = llvm::sys::findProgramByName("curl")) {
      status = llvm::sys::ExecuteAndWait(
          *curl, {*curl, "-fL", "-C", "-", "--progress-bar", "-o", partial, url});
    } else if (auto wget = llvm::sys::findProgramByName("wget")) {
      status =
          llvm::sys::ExecuteAndWait(*wget, {*wget, "-c", "-O", partial, url});
    } else {
      throw ovis::error("Downloading needs curl or wget. Download " + url +
                        " yourself and put it in " + directory.string() + ".");
    }
    if (status != 0)
      throw ovis::error("The download failed.");
    std::filesystem::rename(partial, target);
    std::cerr << "ovis: done.\n";
    return 0;
  }

  if (subcommand == "embed") {
    if (p_arguments.positional.size() != 2 || p_arguments.output.empty())
      throw ovis::error("Usage: ovis model embed MODEL.gguf -o OUTPUT");
    ovis::english::embed_model(executable, p_arguments.positional[1],
                               p_arguments.output);
    return 0;
  }

  if (subcommand == "serve" || subcommand == "stop") {
    std::optional<ovis::english::model_location> location;
    if (p_arguments.model_offset)
      location = ovis::english::model_location{
          .file = *p_arguments.model_path,
          .offset = *p_arguments.model_offset,
          .size = p_arguments.model_size,
          .is_embedded = true};
    else
      location = ovis::english::find_builtin_model(p_arguments.model_path,
                                                   executable, gpu);
    if (!location)
      throw ovis::error("No model found. Run `ovis model pull`.");
    auto options = builtin_options_of(p_arguments, *location);
    options.log = {};
    const auto socket = p_arguments.socket.empty()
                            ? ovis::english::daemon_socket_path(options, executable)
                            : std::optional(p_arguments.socket);
    if (!socket)
      throw ovis::error("There is no private directory for the model daemon.");
    if (subcommand == "stop") {
      std::cout << (ovis::english::stop_daemon(*socket)
                        ? "Stopped the model daemon.\n"
                        : "No model daemon is running for these options.\n");
      return 0;
    }
    // Tells the build that started this daemon how loading went.
    const auto say = [&](const std::string &p_text) {
#ifndef _WIN32
      if (p_arguments.ready_fd >= 0) {
        [[maybe_unused]] const auto written =
            write(p_arguments.ready_fd, p_text.data(), p_text.size());
        close(p_arguments.ready_fd);
      }
#else
      (void)p_text;
#endif
    };
    ovis::english::builtin_model model(options);
    try {
      // Loads the weights now, so the first question doesn't wait.
      model.choose({{"user", "1. yes\n2. no\nAnswer:"}}, 2);
    } catch (const std::exception &p_error) {
      std::string message = p_error.what();
      std::ranges::replace(message, '\n', ' ');
      say(message + "\n");
      throw;
    }
    ovis::english::serve_model(
        model, *socket, std::chrono::minutes(p_arguments.daemon_idle_minutes),
        [&] { say("ready\n"); });
    return 0;
  }

  throw ovis::error("Unknown model command " + subcommand + ".\n\n" + usage);
}

// Replaces the file only if needed, and never half-written.
auto write_source(const std::filesystem::path &p_path, std::string_view p_text)
    -> void {
  auto temporary = p_path;
  temporary += ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << p_text;
    if (!output)
      throw ovis::error("Cannot write " + temporary.string() + ".");
  }
  std::filesystem::rename(temporary, p_path);
}

auto generate(const arguments &p_arguments, const ovis::core::program &p_program,
              const std::string &program_name,
              const std::filesystem::path &p_source) -> int;

// Whether a person is at a terminal, to be offered fixes.
auto is_interactive() -> bool {
#ifdef _WIN32
  return false;
#else
  return isatty(0) != 0 && isatty(2) != 0;
#endif
}

// The lines of a text, each without its `\n`.
auto lines_of(std::string_view p_text) -> std::vector<std::string> {
  std::vector<std::string> result;
  for (std::size_t begin = 0; begin <= p_text.size();) {
    auto end = p_text.find('\n', begin);
    if (end == std::string_view::npos)
      end = p_text.size();
    result.emplace_back(p_text.substr(begin, end - begin));
    begin = end + 1;
  }
  return result;
}

// `use NAME` (phrase-scope.md S2) written at the top of a file, after
// its first comments and `use` lines, unless it is there. Whether it was
// written.
auto add_use_line(const std::filesystem::path &p_path, const std::string &p_name) -> bool {
  std::ifstream input(p_path, std::ios::binary);
  std::stringstream read;
  read << input.rdbuf();
  auto lines = lines_of(read.str());
  const auto line = "use " + p_name;
  if (std::ranges::find(lines, line) != lines.end())
    return false;
  std::size_t at = 0;
  while (at < lines.size() && (lines[at].starts_with("#") || lines[at].starts_with("use ")))
    ++at;
  lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), line);
  std::string text;
  for (std::size_t index = 0; index < lines.size(); ++index)
    text += lines[index] + (index + 1 < lines.size() ? "\n" : "");
  write_source(p_path, text);
  return true;
}

// The file's `use NAME` lines taken out; whether there was one.
auto remove_use_line(const std::filesystem::path &p_path, const std::string &p_name) -> bool {
  std::ifstream input(p_path, std::ios::binary);
  std::stringstream read;
  read << input.rdbuf();
  auto lines = lines_of(read.str());
  if (std::erase(lines, "use " + p_name) == 0)
    return false;
  std::string text;
  for (std::size_t index = 0; index < lines.size(); ++index)
    text += lines[index] + (index + 1 < lines.size() ? "\n" : "");
  write_source(p_path, text);
  return true;
}

// What the author decided a line means: its file, its line, the meaning.
struct taken_fix {
  std::size_t file = 0;
  std::size_t line = 0;
  std::string meaning;
};

// Problems the author settles by saying what the line means.
auto is_fixable(const ovis::phrase::diagnostic &p_problem) -> bool {
  return p_problem.line > 0 && !p_problem.span.empty() &&
         p_problem.kind != "syntax" && p_problem.kind != "error" &&
         p_problem.kind != "needs_use" && p_problem.kind != "internal";
}

// Where a problem is, as reports name it.
auto where_of(const std::vector<ovis::phrase::phrase_file> &p_files,
              const std::filesystem::path &p_program,
              const ovis::phrase::diagnostic &p_problem) -> std::string {
  const auto &file = p_files[p_problem.file];
  return (file.name.empty() ? p_program.string() : file.name) + ", line " +
         std::to_string(p_problem.line);
}

// Asks about each unclear line which reading it has; the ones taken.
auto ask_fixes(const std::vector<ovis::phrase::phrase_file> &p_files,
               const std::filesystem::path &p_program,
               const ovis::phrase::program_result &p_result)
    -> std::vector<taken_fix> {
  std::vector<taken_fix> taken;
  for (const auto &problem : p_result.diagnostics) {
    if (!is_fixable(problem))
      continue;
    const auto lines = lines_of(p_result.sources[problem.file]);
    const auto &written = lines.at(problem.line - 1);
    const auto first = written.find_first_not_of(' ');
    std::cerr << "\n" << where_of(p_files, p_program, problem) << ": "
              << written.substr(first == std::string::npos ? 0 : first) << "\n"
              << "What does \"" << problem.span << "\" mean here?\n";
    std::size_t option = 0;
    for (const auto &fix : problem.fixes)
      std::cerr << "  " << ++option << ". " << fix.reading << "\n";
    std::cerr << "  " << ++option << ". write what the line means\n"
              << "  " << ++option << ". skip\n";
    // Asked again until the answer is one of the options.
    std::size_t chosen = 0;
    std::string answer;
    while (chosen < 1 || chosen > option) {
      std::cerr << "Choose 1-" << option << " (q to stop): " << std::flush;
      if (!std::getline(std::cin, answer) || answer == "q")
        return taken;
      try {
        chosen = std::stoul(answer);
      } catch (const std::exception &) {
        chosen = 0;
      }
    }
    if (chosen >= 1 && chosen <= problem.fixes.size()) {
      taken.push_back(
          {problem.file, problem.line, problem.fixes[chosen - 1].meaning});
    } else if (chosen == problem.fixes.size() + 1) {
      std::cerr << "The line as Ovis should read it: " << std::flush;
      std::string meaning;
      if (std::getline(std::cin, meaning) &&
          meaning.find_first_not_of(' ') != std::string::npos)
        taken.push_back({problem.file, problem.line, meaning});
    }
  }
  return taken;
}

// The fix `fix --line [FILE:]N` names: `--meaning`, or reading `--apply`.
auto argument_fix(const arguments &p_arguments,
                  const std::vector<ovis::phrase::phrase_file> &p_files,
                  const ovis::phrase::program_result &p_result) -> taken_fix {
  const auto colon = p_arguments.fix_line.rfind(':');
  const auto file_name = colon == std::string::npos
                             ? std::string()
                             : p_arguments.fix_line.substr(0, colon);
  const auto line = std::stoul(p_arguments.fix_line.substr(
      colon == std::string::npos ? 0 : colon + 1));
  for (const auto &problem : p_result.diagnostics) {
    const auto &file = p_files[problem.file];
    if (problem.line != line || !is_fixable(problem) ||
        (!file_name.empty() && file.name != file_name &&
         file.path.filename() != file_name))
      continue;
    if (!p_arguments.fix_meaning.empty())
      return {problem.file, line, p_arguments.fix_meaning};
    if (p_arguments.fix_apply < 1 || p_arguments.fix_apply > problem.fixes.size())
      throw ovis::error("Line " + std::to_string(line) + " has " +
                        std::to_string(problem.fixes.size()) +
                        " readings: pass --apply 1 to --apply " +
                        std::to_string(problem.fixes.size()) +
                        ", or --meaning TEXT.");
    return {problem.file, line, problem.fixes[p_arguments.fix_apply - 1].meaning};
  }
  throw ovis::error("Nothing on line " + p_arguments.fix_line +
                    " is unclear.");
}

// Writes each fix as `@[meaning: …]` above its line, at its indentation,
// in place of a meaning already there.
auto write_fixes(const std::vector<ovis::phrase::phrase_file> &p_files,
                 const std::vector<std::string> &p_texts,
                 std::vector<taken_fix> p_fixes) -> void {
  // From the bottom, so the lines above keep their numbers.
  std::ranges::sort(p_fixes, std::ranges::greater{}, &taken_fix::line);
  std::map<std::size_t, std::vector<std::string>> texts;
  for (const auto &fix : p_fixes) {
    if (!texts.contains(fix.file))
      texts[fix.file] = lines_of(p_texts[fix.file]);
    auto &lines = texts[fix.file];
    const auto &line = lines.at(fix.line - 1);
    const auto indent = line.substr(0, line.find_first_not_of(' '));
    const auto sigil = indent + "@[meaning: " + fix.meaning + "]";
    const auto above = fix.line >= 2 ? lines[fix.line - 2] : std::string();
    if (above.find_first_not_of(' ') != std::string::npos &&
        above.substr(above.find_first_not_of(' ')).starts_with("@[meaning:"))
      lines[fix.line - 2] = sigil;
    else
      lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(fix.line - 1),
                   sigil);
  }
  for (const auto &[file, lines] : texts) {
    std::string text;
    for (std::size_t index = 0; index < lines.size(); ++index)
      text += lines[index] + (index + 1 < lines.size() ? "\n" : "");
    write_source(p_files[file].path, text);
    std::cerr << "ovis: wrote what you chose into " << p_files[file].path.string()
              << "\n";
  }
}

// `check --json`: the problems, for editors.
auto diagnostics_json(const std::vector<ovis::phrase::phrase_file> &p_files,
                      const ovis::phrase::program_result &p_result)
    -> std::string {
  nlohmann::ordered_json document;
  document["diagnostics"] = nlohmann::ordered_json::array();
  for (const auto &problem : p_result.diagnostics) {
    nlohmann::ordered_json fixes = nlohmann::ordered_json::array();
    for (const auto &fix : problem.fixes)
      fixes.push_back({{"reading", fix.reading}, {"meaning", fix.meaning}});
    document["diagnostics"].push_back(
        {{"file", p_files[problem.file].path.string()},
         {"line", problem.line},
         {"column", problem.column},
         {"end_column", problem.end_column},
         {"kind", problem.kind},
         {"message", problem.message},
         {"span", problem.span},
         {"fixes", fixes}});
  }
  return document.dump(2) + "\n";
}

// Reads the program, settling unclear lines, and runs, builds or prints it,
// with where the time went. A failed build in a terminal offers the
// readings of its unclear lines, then builds again; `fix` does that on
// purpose, and `check` only reports.
// A Make-style dependency file: the object (or executable) built, after the
// files that went into it, so CMake runs the build again when one changes.
auto write_depfile(const arguments &p_arguments,
                   const std::vector<ovis::phrase::phrase_file> &p_files)
    -> void {
  std::set<std::filesystem::path> dependencies;
  for (const auto &file : p_files) {
    dependencies.insert(file.path);
    const auto lock = ovis::phrase::lockfile::path_for(file.path);
    if (std::filesystem::exists(lock))
      dependencies.insert(lock);
  }
  for (const auto &header : ovis::core::imported_dependencies())
    dependencies.insert(header);
  const auto target = !p_arguments.object.empty() ? p_arguments.object
                                                  : p_arguments.output;
  const auto escaped = [](const std::filesystem::path &p_path) {
    std::string result;
    for (const char character : p_path.string()) {
      if (character == ' ' || character == '#')
        result.push_back('\\');
      if (character == '$')
        result.push_back('$');
      result.push_back(character);
    }
    return result;
  };
  std::ofstream output(p_arguments.depfile);
  if (!output)
    throw ovis::error("Cannot write " + p_arguments.depfile.string() + ".");
  output << escaped(target) << ":";
  for (const auto &dependency : dependencies)
    output << " \\\n  " << escaped(dependency);
  output << '\n';
}

// What a locked build would have to change, or nothing: a line settled now
// that no meaning in the source or the lockfile settles.
auto locked_change(const std::vector<ovis::phrase::phrase_file> &p_files,
                   const ovis::phrase::program_result &p_result)
    -> std::string {
  for (std::size_t index = 0; index < p_files.size(); ++index) {
    const auto &file = p_files[index];
    const auto where = file.path.string();
    if (file.locked)
      continue;
    if (p_result.sources[index] != file.text) {
      const auto before = lines_of(file.text);
      const auto after = lines_of(p_result.sources[index]);
      std::size_t line = 0;
      while (line < before.size() && line < after.size() &&
             before[line] == after[line])
        ++line;
      // The sigil is written above the line it is for.
      return where + ", line " + std::to_string(line + 1) +
             " is not settled in the source: " +
             (line < after.size() ? after[line] : std::string()) +
             " would be written there. Run `ovis run " + where +
             "` or `ovis fix " + where + "` and commit the result.";
    }
    if (p_result.locks[index] != file.lock)
      return where + ": its lockfile would change. Run `ovis run " + where +
             "` and commit the lockfile.";
  }
  return {};
}

// When a package is in the program twice, on two lines: which uses which,
// for a build that failed (its records and phrases are different ones,
// packages.md §4). Empty otherwise.
auto two_copies_note(const ovis::project::package_graph &p_graph) -> std::string {
  std::map<std::string, std::vector<const ovis::project::resolved_package *>> by_name;
  for (const auto &package : p_graph.packages)
    by_name[package.name].push_back(&package);
  std::string note;
  for (const auto &[name, copies] : by_name) {
    if (copies.size() < 2)
      continue;
    note += "ovis: the program has " + std::to_string(copies.size()) + " versions of " +
            name + ", whose records and phrases are not the same:";
    for (const auto *copy : copies) {
      note += "\n      " + copy->package_version.text() + " for";
      std::string users;
      for (const auto &other : p_graph.packages)
        for (const auto &[used, id] : other.dependencies)
          if (id == copy->id)
            users += (users.empty() ? " " : ", ") + other.name;
      for (const auto &[used, id] : p_graph.roots)
        if (id == copy->id)
          users += (users.empty() ? " " : ", ") + std::string("this program");
      note += users;
    }
    note += "\n      Make them use one version (an [overrides] entry, or a newer "
            "release of one), or keep each's values to its own side.\n";
  }
  return note;
}

// The program's files, after the files of the packages it uses (as
// modules `log@1.writer`, locked), and with their phrases/ folders.
auto load_program_files(const arguments &p_arguments,
                        const std::filesystem::path &p_program)
    -> std::vector<ovis::phrase::phrase_file> {
  std::vector<ovis::phrase::phrase_file> files;
  // Each file reads with its own package's phrases and records, and a
  // direct dependency's where it says `use NAME` (packages.md §5).
  for (const auto &package : p_arguments.packages.packages) {
    auto loaded = ovis::phrase::load_package_files(package.root, package.id);
    const auto copies = std::ranges::count(p_arguments.packages.packages, package.name,
                                           &ovis::project::resolved_package::name);
    for (auto &file : loaded) {
      file.aliases = package.dependencies;
      if (copies > 1)
        file.package_label = package.name + " " + package.package_version.text();
    }
    std::ranges::move(loaded, std::back_inserter(files));
  }
  auto own = ovis::phrase::load_phrase_program(p_program);
  if (!files.empty()) {
    // A single-file program has no module; with packages it is main.
    if (own.size() == 1 && own.front().module.empty())
      own.front().module = "main";
    for (auto &file : own) {
      // Its internal/ folders' already, if it has any below its top.
      if (file.sees.empty())
        file.sees = {"."};
      file.aliases = p_arguments.packages.roots;
    }
  }
  std::ranges::move(own, std::back_inserter(files));
  return files;
}

auto run_program_command(const arguments &p_arguments,
                         const char *p_argument_zero) -> int {
  const std::filesystem::path program_path = p_arguments.positional[0];
  const auto &command = p_arguments.command;
  const bool checking = command == "check";
  const bool fixing = command == "fix";
  const bool locked = p_arguments.locked;
  // `check` is for editors: fast, and it changes nothing. A locked build
  // (under CMake) asks nothing either.
  const auto model = checking || locked
                         ? nullptr
                         : make_model(p_arguments, p_argument_zero);
  const bool offers = (fixing && p_arguments.fix_line.empty()) ||
                      (p_arguments.prompt && (command == "run" || command == "build") &&
                       is_interactive());
  // `fix --line`, from an editor: the line as `check` cited it, in the file
  // as it is, with no model and nothing else written.
  if (fixing && !p_arguments.fix_line.empty()) {
    const auto files = load_program_files(p_arguments, program_path);
    const auto result = ovis::phrase::compile_phrase_program(
        files, nullptr,
        {.c_imports = p_arguments.c_imports, .sources_written = false});
    std::vector<std::string> texts;
    for (const auto &file : files)
      texts.push_back(file.text);
    write_fixes(files, texts, {argument_fix(p_arguments, files, result)});
    return 0;
  }
  const auto start = std::chrono::steady_clock::now();
  // In a terminal, a live line shows where the build is. Not in a pipe or
  // CI, nor with --verbose, whose llama.cpp log would tear it.
  std::optional<status_line> status;
  if (!checking && !p_arguments.quiet && !p_arguments.verbose && is_interactive())
    status.emplace();
  live = status ? &*status : nullptr;
  struct forget_live {
    ~forget_live() { live = nullptr; }
  } forget;
  for (int round = 0;; ++round) {
    if (live != nullptr) {
      live->resume();
      live->set("reading " + program_path.filename().string());
    }
    const auto files = load_program_files(p_arguments, program_path);
    auto result = ovis::phrase::compile_phrase_program(
        files, model.get(),
        {.model_budget_seconds = p_arguments.model_budget,
         .log = checking ? nullptr : progress_log(p_arguments),
         .c_imports = p_arguments.c_imports,
         .phrase_directories = p_arguments.phrase_directories,
         .progress = live == nullptr
                         ? std::function<void(const ovis::phrase::build_progress &)>()
                         : [](const ovis::phrase::build_progress &p_progress) {
                             using enum ovis::phrase::build_progress::stage_type;
                             live->set(progress_text(p_progress),
                                       p_progress.stage == asking ||
                                           p_progress.stage == rewriting);
                           },
         .sources_written = !checking && !locked && !p_arguments.keep_source,
         .strict_memory = p_arguments.strict_memory});
    // What follows prints lines of its own.
    if (live != nullptr)
      live->pause();
    if (checking) {
      if (p_arguments.json)
        std::cout << diagnostics_json(files, result);
      else
        for (const auto &problem : result.problems)
          std::cerr << (files[0].name.empty() ? program_path.string() + ", "
                                               : std::string())
                    << problem << '\n';
      return result.succeeded() ? 0 : 1;
    }
    if (locked) {
      if (const auto changed = locked_change(files, result); !changed.empty()) {
        for (const auto &problem : result.problems)
          std::cerr << problem << '\n';
        std::cerr << "ovis: --locked: " << changed << '\n';
        return 1;
      }
      if (!result.succeeded()) {
        for (const auto &problem : result.problems)
          std::cerr << problem << '\n';
        std::cerr << "ovis: --locked: the program has problems; settle them "
                     "with `ovis run` or `ovis fix`, then build again.\n";
        return 1;
      }
    }
    for (std::size_t index = 0; index < files.size() && !locked; ++index) {
      const auto &file = files[index];
      // A dependency's files are its author's (packages.md §6).
      if (file.locked)
        continue;
      // A failed build keeps the old readings unless the model made new ones.
      if (result.locks[index] != file.lock &&
          (result.succeeded() || result.timing.by_model > 0 ||
           result.timing.by_rewrite > 0))
        result.locks[index].save(ovis::phrase::lockfile::path_for(file.path));
      if (!p_arguments.keep_source && result.sources[index] != file.text) {
        write_source(file.path, result.sources[index]);
        if (!p_arguments.quiet)
          std::cerr << "ovis: wrote the readings into " << file.path.string()
                    << '\n';
      }
    }
    for (const auto &warning : result.warnings)
      std::cerr << "ovis: " << warning << '\n';
    const auto &timing = result.timing;
    if (!p_arguments.quiet && live != nullptr)
      std::cerr << terminal_summary(timing, result.succeeded()) << '\n';
    else if (!p_arguments.quiet)
      std::cerr << "ovis: " << timing.lines << " lines: read in "
                << int(timing.lex_ms + timing.parse_ms) << " ms, "
                << timing.questions << " unclear (" << timing.by_rule
                << " by rules, " << timing.by_ranking << " by ranking, "
                << timing.by_lockfile << " from the lockfile"
                << (timing.by_rewrite > 0
                        ? ", " + std::to_string(timing.by_rewrite) + " rewritten"
                        : "")
                << "), " << timing.asked
                << " asked of the model in " << int(timing.model_ms) << " ms"
                << (timing.asked > 0 ? " (first, with loading: " +
                                           std::to_string(int(timing.first_question_ms)) +
                                           " ms)"
                                     : "")
                << ", checked in " << int(timing.check_ms) << " ms\n";
    if (!result.succeeded() && fixing && round < 3) {
      // Lines that read with a dependency the file doesn't use: `fix` writes
      // its `use` line and reads again (phrase-scope.md S2).
      bool wrote = false;
      for (const auto &problem : result.diagnostics)
        for (const auto &name : problem.packages)
          if (problem.kind == "needs_use" && !files[problem.file].locked &&
              add_use_line(files[problem.file].path, name)) {
            wrote = true;
            std::cerr << "ovis: wrote `use " << name << "` into "
                      << files[problem.file].path.string() << '\n';
          }
      if (wrote)
        continue;
    }
    if (!result.succeeded()) {
      // Problems name the file only in a program of several.
      const auto single =
          files[0].name.empty() ? program_path.string() + ", " : std::string();
      const bool asks = offers && round < 3 &&
                        std::ranges::any_of(result.diagnostics, is_fixable);
      // A problem the questions below cover isn't reported twice.
      for (std::size_t index = 0; index < result.problems.size(); ++index)
        if (!asks || !is_fixable(result.diagnostics[index]))
          std::cerr << single << result.problems[index] << '\n';
      std::cerr << two_copies_note(p_arguments.packages);
      std::vector<taken_fix> taken;
      if (asks)
        taken = ask_fixes(files, program_path, result);
      if (taken.empty()) {
        // Nothing taken: what was asked about is reported after all.
        if (asks)
          for (std::size_t index = 0; index < result.problems.size(); ++index)
            if (is_fixable(result.diagnostics[index]))
              std::cerr << single << result.problems[index] << '\n';
        return 1;
      }
      // The lines are the file's as written: with this build's sigils.
      write_fixes(files, result.sources, std::move(taken));
      continue;
    }
    if (fixing) {
      std::cerr << "ovis: every line reads.\n";
      return 0;
    }
    const auto program_name =
        files.size() > 1 || files[0].module == "main"
            ? std::filesystem::absolute(files.back().path)
                  .parent_path()
                  .filename()
                  .string()
            : files[0].path.stem().string();
    if (live != nullptr)
      live->resume();
    const auto exit_status = generate(p_arguments, result.program, program_name,
                                      files.back().path);
    if (!p_arguments.depfile.empty())
      write_depfile(p_arguments, files);
    if (live != nullptr)
      live->pause();
    if (!p_arguments.quiet && command != "run")
      std::cerr << "ovis: done in "
                << int(std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - start)
                           .count())
                << " ms\n";
    return exit_status;
  }
}

// `ovis run`/`build` of a project whose C needs CMake: the generated
// project builds the C and runs this ovis on the program (settling it as
// `ovis run` does unless --locked), then the executable is run or copied.
// The project's ovis.lock brought up to date with ovis.toml (git C
// dependencies pinned to commits), saved when it changed. Under --locked a
// change is an error, as for a program's lockfile.
auto lock_project(arguments &p_arguments, const std::vector<std::string> &p_update)
    -> ovis::project::project_lock {
  const auto project = *p_arguments.project;
  const auto path = project.root / ovis::project::project_lock_name;
  const auto before = ovis::project::project_lock::load(path);
  auto lock = before;
  // Ovis packages: the lock's versions while they still do, else chosen
  // again (packages.md §3, §8).
  p_arguments.packages = ovis::project::resolve_packages(
      project, {.lock = &before,
                .update = p_update,
                .vendor = project.root / "vendor",
                .log = p_arguments.quiet ? std::function<void(const std::string &)>()
                                         : [](const std::string &p_text) {
                                             std::cerr << "ovis: " << p_text << '\n';
                                           }});
  lock.packages = ovis::project::lock_packages(p_arguments.packages);
  // The C of the program and its packages, one of each library, pinned
  // together.
  if (!p_arguments.packages.packages.empty())
    p_arguments.project->c_dependencies =
        ovis::project::combined_c_dependencies(project, p_arguments.packages);
  auto messages = ovis::project::lock_c_dependencies(*p_arguments.project, lock, p_update);
  for (const auto &package : lock.packages) {
    const auto old = std::ranges::find_if(before.packages, [&](const auto &p_old) {
      return p_old.name == package.name && p_old.line == package.line &&
             p_old.git == package.git && p_old.path == package.path;
    });
    if (old == before.packages.end())
      messages.push_back(package.name + " " + package.version + " (" +
                         (package.git.empty() ? package.path : package.git) + ")");
    else if (old->version != package.version || old->commit != package.commit)
      messages.push_back(package.name + " " + old->version + " -> " + package.version);
  }
  p_arguments.lock_changed = lock != before;
  // check writes nothing.
  if (lock != before && p_arguments.command == "check")
    return lock;
  if (lock != before) {
    if (p_arguments.locked)
      throw ovis::error(path.string() + " would change (" +
                        (messages.empty() ? std::string("entries dropped")
                                          : messages.front()) +
                        "): run `ovis build` and commit it.");
    lock.save(path);
  }
  if (!p_arguments.quiet)
    for (const auto &message : messages)
      std::cerr << "ovis: " << message << '\n';
  return lock;
}

// `ovis vendor`: every package's tree copied into vendor/NAME-LINE, which
// builds then use (with the lock's tree hash) instead of the cache.
auto vendor_packages(const arguments &p_arguments) -> int {
  const auto vendor = p_arguments.project->root / "vendor";
  std::filesystem::create_directories(vendor);
  std::set<std::string> kept;
  for (const auto &package : p_arguments.packages.packages) {
    if (package.git.empty())
      continue;
    const auto name = ovis::project::vendor_folder_name(package);
    kept.insert(name);
    const auto folder = vendor / name;
    if (folder == package.root)
      continue;
    std::filesystem::remove_all(folder);
    std::filesystem::copy(package.root, folder, std::filesystem::copy_options::recursive);
    // The cache's files are read-only; the vendored ones are the project's.
    for (const auto &entry : std::filesystem::recursive_directory_iterator(folder))
      if (entry.is_regular_file())
        std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add);
  }
  // Folders of packages no longer used go.
  for (const auto &entry : std::filesystem::directory_iterator(vendor))
    if (entry.is_directory() && !kept.contains(entry.path().filename().string()))
      std::filesystem::remove_all(entry.path());
  if (!p_arguments.quiet)
    std::cerr << "ovis: " << kept.size() << " packages in " << vendor.string()
              << "; commit it to build with no network\n";
  return 0;
}

// The package in `p_arguments.project`, read as its users' programs would
// read it, with its dependencies, no model and nothing written; its problems
// and those of its `# example:` lines are printed (packages.md §6, L9).
struct package_reading {
  std::vector<ovis::phrase::phrase_file> files;
  std::size_t first_own = 0;
  ovis::phrase::program_result result;
  // It reads, and its examples do.
  bool ok = false;
};

auto read_package(const arguments &p_arguments, const std::string &p_line) -> package_reading {
  const auto &project = *p_arguments.project;
  package_reading reading;
  for (const auto &package : p_arguments.packages.packages) {
    auto loaded = ovis::phrase::load_package_files(package.root, package.id);
    for (auto &file : loaded) {
      file.aliases = package.dependencies;
    }
    std::ranges::move(loaded, std::back_inserter(reading.files));
  }
  const auto id = project.package->name + "@" + p_line;
  auto own = ovis::phrase::load_package_files(project.root, id);
  if (own.empty())
    throw ovis::error(project.root.string() + " has no .ovis files.");
  for (auto &file : own) {
    file.aliases = p_arguments.packages.roots;
  }
  reading.first_own = reading.files.size();
  std::ranges::move(own, std::back_inserter(reading.files));
  const ovis::phrase::front_end_options options{
      .c_imports = p_arguments.c_imports,
      .phrase_directories = p_arguments.phrase_directories,
      .sources_written = false,
      .strict_memory = p_arguments.strict_memory};
  reading.result = ovis::phrase::compile_phrase_program(reading.files, nullptr, options);
  for (const auto &problem : reading.result.problems)
    std::cerr << problem << '\n';
  reading.ok = reading.result.succeeded();
  // Only a package that reads has examples worth running.
  if (reading.ok)
    for (const auto &problem :
         ovis::phrase::check_package_examples(reading.files, reading.first_own, options)) {
      std::cerr << problem << '\n';
      reading.ok = false;
    }
  return reading;
}

// `ovis release X.Y.Z` in a package: checks it can be released, then tags
// it vX.Y.Z (the author pushes the tag): the version is ovis.toml's, the
// work is committed, the tag is new, and every file reads and checks with
// no model and nothing written (packages.md §6).
auto release_package(const arguments &p_arguments) -> int {
  const auto &project = *p_arguments.project;
  if (!project.package)
    throw ovis::error("ovis.toml has no [package] table: only a package is released.");
  if (p_arguments.update_names.size() != 1)
    throw ovis::error("Usage: ovis release X.Y.Z");
  const auto wanted = ovis::project::parse_version(p_arguments.update_names.front());
  if (!wanted)
    throw ovis::error(p_arguments.update_names.front() + " is not a version such as 1.4.0.");
  if (*wanted != project.package->package_version)
    throw ovis::error("ovis.toml says version " + project.package->package_version.text() +
                      ": change `version` to " + wanted->text() +
                      " and commit it first, so the tag and the file agree.");
  for (const auto &dependency : project.dependencies)
    if (!dependency.path.empty())
      throw ovis::error("ovis.toml, line " + std::to_string(dependency.line) + ": `" +
                        dependency.name + "` is a folder on this machine; a released "
                        "package's dependencies are git repositories.");
  const auto root = project.root.string();
  const auto git = [&](std::vector<std::string> p_arguments) {
    p_arguments.insert(p_arguments.begin(), {"-C", root});
    return ovis::project::run_command("git", p_arguments);
  };
  const auto status = git({"status", "--porcelain"});
  if (status.status != 0)
    throw ovis::error(root + " is not a git repository: " + status.errors);
  if (!status.output.empty())
    throw ovis::error("Commit or stash the changes first; a tag names a commit:\n" +
                      status.output);
  const auto tag = "v" + wanted->text();
  if (git({"rev-parse", "-q", "--verify", "refs/tags/" + tag}).status == 0)
    throw ovis::error("The tag " + tag + " already exists: a version is released once.");

  const auto read = read_package(p_arguments, wanted->line());
  if (!read.ok)
    throw ovis::error("The package doesn't build as its users would build it; fix the "
                      "problems above (`ovis fix` settles unclear lines).");
  for (auto index = read.first_own; index < read.files.size(); ++index)
    if (read.result.locks[index] != read.files[index].lock)
      throw ovis::error(read.files[index].name + ": its lockfile would change; build it and "
                        "commit the lockfile first.");
  const auto tagged = git({"tag", "-a", tag, "-m", project.package->name + " " + wanted->text()});
  if (tagged.status != 0)
    throw ovis::error("git tag failed: " + tagged.errors);
  std::cerr << "ovis: tagged " << tag << "; publish it with `git push origin " << tag
            << "`\n";
  return 0;
}

auto is_package_only(const arguments &p_arguments) -> bool {
  return p_arguments.command == "check" && p_arguments.project && p_arguments.project->package &&
         !std::filesystem::exists(p_arguments.project->root / "main.ovis");
}

// `ovis check` in a package, which has no main.ovis: the release's reading,
// with no tag.
auto check_package(const arguments &p_arguments) -> int {
  const auto read = read_package(p_arguments, p_arguments.project->package->package_version.line());
  return read.ok ? 0 : 1;
}

// `ovis update [NAME…]`: what apply_project's lock_project moved. A name
// is a [c.NAME] table or a package, as a dependency line names it or by its
// own name.
auto update_project(const arguments &p_arguments) -> int {
  for (const auto &name : p_arguments.update_names) {
    const bool known =
        std::ranges::any_of(p_arguments.project->c_dependencies,
                            [&](const auto &p_c) { return p_c.name == name; }) ||
        std::ranges::any_of(p_arguments.packages.packages,
                            [&](const auto &p_package) {
                              return p_package.name == name ||
                                     p_package.dependencies.contains(name);
                            }) ||
        p_arguments.packages.roots.contains(name);
    if (!known)
      throw ovis::error("ovis.toml has no [c." + name + "], and no package " + name +
                        " is in the program.");
  }
  if (!p_arguments.lock_changed && !p_arguments.quiet)
    std::cerr << "ovis: every dependency is where ovis.lock has it\n";
  return 0;
}

auto cmake_options_for(const arguments &p_arguments, const char *p_argument_zero)
    -> ovis::project::cmake_options {
  ovis::project::cmake_options options{
      .ovis_executable = executable_path(p_argument_zero),
      .unlocked = !p_arguments.locked,
      .fast = p_arguments.fast};
  for (const auto &option : p_arguments.options)
    if (option != "--locked" && option != "--fast" && option != "--print" &&
        option != "--verbose")
      options.ovis_flags.push_back(option);
  return options;
}

// `ovis cmake [--print]`: the project generated from ovis.toml, written
// where builds put it (for an IDE, or to see why CMake failed) or printed.
auto print_cmake_project(const arguments &p_arguments, const char *p_argument_zero)
    -> int {
  const auto &project = *p_arguments.project;
  auto options = cmake_options_for(p_arguments, p_argument_zero);
  for (const auto &entry : p_arguments.lock.c)
    options.commits[entry.name] = entry.commit;
  if (p_arguments.print) {
    std::cout << ovis::project::cmake_project_text(project, options);
    return 0;
  }
  const auto written =
      ovis::project::write_cmake_project(project, options, project.root / "build");
  std::cout << written.string() << '\n';
  if (!p_arguments.quiet)
    std::cerr << "ovis: configure it with `cmake -S " << written.string()
              << " -B BUILD`; it is written again at every build from "
              << project.file.filename().string() << ", so change that instead\n";
  return 0;
}

auto run_cmake_project(const arguments &p_arguments, const char *p_argument_zero)
    -> int {
  const auto &project = *p_arguments.project;
  auto options = cmake_options_for(p_arguments, p_argument_zero);
  for (const auto &entry : p_arguments.lock.c)
    options.commits[entry.name] = entry.commit;
  options.link_inputs = p_arguments.package_link_inputs;
  const auto build_directory = project.root / "build";
  if (!p_arguments.quiet)
    std::cerr << "ovis: building with CMake ("
              << (build_directory / "cmake").string() << ")\n";
  const auto executable = ovis::project::build_with_cmake(
      project, options, build_directory, p_arguments.verbose);
  if (p_arguments.command == "build") {
    const auto output = p_arguments.output.empty()
                            ? build_directory / project.program_name()
                            : p_arguments.output;
    std::filesystem::copy_file(executable, output,
                               std::filesystem::copy_options::overwrite_existing);
    return 0;
  }
  return run_program(executable.string());
}

// The words of meaning of line N of a program (`manual --line`): what is
// not a sigil, a comment, a quoted text or a number.
auto line_words(const std::filesystem::path &p_file, std::size_t p_line)
    -> std::vector<std::string> {
  std::ifstream input(p_file);
  if (!input)
    throw ovis::error("Cannot read " + p_file.string() + ".");
  std::string text;
  for (std::size_t number = 1; std::getline(input, text); ++number)
    if (number == p_line)
      break;
    else
      text.clear();
  const auto first = text.find_first_not_of(" \t");
  if (first == std::string::npos || text[first] == '#' ||
      text.compare(first, 2, "@[") == 0)
    throw ovis::error("Line " + std::to_string(p_line) + " of " + p_file.string() +
                      " has no phrase to look up.");
  std::vector<std::string> words;
  std::string current;
  bool quoted = false;
  const auto finish = [&] {
    if (!current.empty() && std::isalpha(static_cast<unsigned char>(current.front())))
      words.push_back(current);
    current.clear();
  };
  for (const auto letter : text + ' ') {
    if (letter == '"')
      quoted = !quoted;
    else if (!quoted && (std::isalnum(static_cast<unsigned char>(letter)) || letter == '_'))
      current += static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    else
      finish();
  }
  return words;
}

// What is set before line N of FILE (`manual --line`), and with `--for` the
// phrases that take a name or a type there (line 0: at the end of the file)
// (manual-and-editor.md §2.4). Compiled without the model, so a line the
// parser cannot settle leaves its names without a type. A status when
// `--for` is the whole answer.
auto manual_variables(const arguments &p_arguments,
                      const std::filesystem::path &p_in, std::size_t p_line)
    -> std::optional<int> {
  const auto files = load_program_files(p_arguments, p_in);
  ovis::phrase::front_end_options options{.c_imports = p_arguments.c_imports,
                                          .phrase_directories = p_arguments.phrase_directories,
                                          .sources_written = false,
                                          .variables_recorded = p_arguments.manual_for.empty(),
                                          .phrases_for = p_arguments.manual_for,
                                          .phrases_for_path = p_in,
                                          .phrases_for_line = p_line};
  const auto program = ovis::phrase::compile_phrase_program(files, nullptr, options);
  if (!p_arguments.manual_for.empty()) {
    if (program.phrases_for_type.empty()) {
      std::cout << "'" << p_arguments.manual_for
                << "' is no name set " << (p_line ? "before that line" : "in that file")
                << " and no type Ovis knows.\n";
      return 1;
    }
    std::cout << p_arguments.manual_for << " is " << program.phrases_for_type
              << ". Phrases that take it:\n";
    for (const auto &item : program.phrases_taking) {
      const auto entry = ovis::phrase::manual_entry_of(item);
      std::cout << "\n  " << entry.pattern << "   (" << entry.topic
                << (entry.package.empty() ? "" : ", from " + entry.package) << ")\n";
      if (!entry.about.empty())
        std::cout << "      " << entry.about << '\n';
      if (!entry.examples.empty())
        std::cout << "      " << entry.examples.front() << '\n';
    }
    return program.phrases_taking.empty() ? 1 : 0;
  }
  for (std::size_t index = 0; index < files.size(); ++index) {
    if (files[index].path.lexically_normal() != p_in.lexically_normal() ||
        index >= program.variables.size())
      continue;
    // The record of the nearest line at or before the one asked for.
    const auto &lines = program.variables[index];
    auto found = lines.upper_bound(p_line);
    if (found == lines.begin())
      break;
    --found;
    if (found->second.empty()) {
      std::cout << "Nothing is set before line " << p_line << ".\n\n";
      break;
    }
    std::cout << "Set before line " << p_line << ":\n";
    for (const auto &variable : found->second) {
      std::cout << "  " << variable.name << ": "
                << (variable.type.empty() ? "type not known" : variable.type);
      std::cout << (variable.line == 0 ? "" : ", set on line " + std::to_string(variable.line))
                << '\n';
    }
    std::cout << '\n';
  }
  return std::nullopt;
}

// `ovis manual [WORDS…] [--in FILE] [--line N]`: the phrases a query is
// about, with what each does and an example (manual-and-editor.md §2.2).
auto manual_command(const arguments &p_arguments, const char *p_argument_zero)
    -> int {
  std::vector<std::filesystem::path> directories = p_arguments.phrase_directories;
  std::vector<std::string> words = p_arguments.positional;
  std::vector<ovis::phrase::phrase_template> own_phrases;
  if (!p_arguments.manual_in.empty()) {
    const auto &in = p_arguments.manual_in;
    const auto folder = std::filesystem::is_directory(in) ? in : in.parent_path();
    if (std::filesystem::is_directory(folder / "phrases"))
      directories.push_back(folder / "phrases");
    if (const auto project = ovis::project::find_project(
            std::filesystem::absolute(folder.empty() ? "." : folder))) {
      const auto loaded = ovis::project::load_project(*project);
      for (const auto &directory : loaded.phrases)
        directories.push_back(loaded.root / directory);
    }
    // The program's own and its packages' phrases (L7).
    try {
      const auto files = load_program_files(p_arguments, in);
      auto own = ovis::phrase::program_phrase_templates(files, in);
      std::ranges::move(own, std::back_inserter(own_phrases));
      // What its `use "x.h"` lines import (L10).
      auto c_entries =
          ovis::phrase::c_header_templates(files, in, p_arguments.c_imports);
      std::ranges::move(c_entries, std::back_inserter(own_phrases));
    } catch (const ovis::error &) {
    }
    if (!p_arguments.fix_line.empty()) {
      const auto colon = p_arguments.fix_line.rfind(':');
      const auto number = p_arguments.fix_line.substr(colon == std::string::npos ? 0 : colon + 1);
      const auto line = std::stoul(number);
      if (p_arguments.manual_for.empty()) {
        const auto lines = line_words(in, line);
        words.insert(words.end(), lines.begin(), lines.end());
      }
      // The names set before the line, and the phrases for one (L8).
      if (const auto status = manual_variables(p_arguments, in, line))
        return *status;
    } else if (!p_arguments.manual_for.empty()) {
      if (const auto status = manual_variables(p_arguments, in, 0))
        return *status;
    }
  } else if (!p_arguments.fix_line.empty() || !p_arguments.manual_for.empty())
    throw ovis::error("`ovis manual --line` and `--for` need the program: --in FILE.");
  std::vector<ovis::phrase::phrase_template> more = std::move(own_phrases);
  for (const auto &directory : directories) {
    auto library = ovis::phrase::load_phrase_library(directory);
    std::ranges::move(library.templates, std::back_inserter(more));
  }
  // Meaning is found by the model only when asked (L4, `--embeddings`):
  // measured with 3B it finds nothing the words miss, state.md.
  std::unique_ptr<ovis::english::language_model> model;
  if (!words.empty() && p_arguments.manual_embeddings) {
    try {
      model = make_model(p_arguments, p_argument_zero);
    } catch (const ovis::error &) {
    }
  }
  const auto entries = ovis::phrase::manual_search(
      words, more, 6,
      {.model = model.get(),
       .cache = ovis::english::model_cache_directory() / "manual-embeddings.bin"});
  if (entries.empty()) {
    std::cout << "No phrase is about those words. `ovis manual` lists them all.\n";
    return 1;
  }
  std::string topic;
  for (const auto &entry : entries) {
    // Every phrase is listed under its topic; a search, best first.
    if (words.empty()) {
      if (entry.topic != topic)
        std::cout << (topic.empty() ? "" : "\n") << (topic = entry.topic) << ":\n";
    } else
      std::cout << '\n';
    std::cout << (words.empty() ? "  " : "") << entry.pattern;
    if (!words.empty())
      std::cout << "   (" << entry.topic
                << (entry.package.empty() ? "" : ", from " + entry.package) << ')';
    std::cout << '\n';
    const std::string indent = words.empty() ? "      " : "    ";
    if (!entry.about.empty())
      std::cout << indent << entry.about << '\n';
    if (!words.empty()) {
      for (const auto &example : entry.examples)
        std::cout << indent << example << '\n';
      if (!entry.aliases.empty()) {
        std::cout << indent << "Also: ";
        for (std::size_t index = 0; index < entry.aliases.size(); ++index)
          std::cout << (index ? ", " : "") << entry.aliases[index];
        std::cout << '\n';
      }
    }
  }
  return 0;
}

auto run(const arguments &p_arguments, const char *p_argument_zero) -> int {
  const auto &command = p_arguments.command;
  if (command == "cmake")
    return print_cmake_project(p_arguments, p_argument_zero);
  if (command == "update")
    return update_project(p_arguments);
  if (command == "add" && p_arguments.update_names.size() == 1 &&
      !p_arguments.quiet && p_arguments.project &&
      std::filesystem::exists(p_arguments.project->root / "main.ovis")) {
    // What the new package's phrases do to the program's lines, now that
    // main.ovis uses it (phrase-scope.md S5); `ovis remove` undoes it.
    try {
      ovis::phrase::check_dependency_phrases(
          load_program_files(p_arguments, p_arguments.project->root),
          p_arguments.update_names, {.c_imports = p_arguments.c_imports},
          std::cerr);
    } catch (const ovis::error &p_error) {
      std::cerr << "ovis: the program's lines weren't checked against "
                << p_arguments.update_names.front() << "'s phrases: "
                << p_error.what() << '\n';
    }
    return 0;
  }
  if (command == "add" || command == "remove" || command == "fetch") {
    if (!p_arguments.quiet && command == "fetch")
      std::cerr << "ovis: " << p_arguments.packages.packages.size()
                << " packages are in the cache or vendor/\n";
    return 0;
  }
  if (command == "vendor")
    return vendor_packages(p_arguments);
  if (command == "release")
    return release_package(p_arguments);
  if (command == "check" && is_package_only(p_arguments))
    return check_package(p_arguments);
  if (command == "tree") {
    if (p_arguments.packages.packages.empty())
      std::cerr << "ovis: the project uses no packages\n";
    std::cout << ovis::project::describe_graph(p_arguments.packages);
    return 0;
  }
  if (p_arguments.cmake_build)
    return run_cmake_project(p_arguments, p_argument_zero);
  if (command == "model")
    return run_model_command(p_arguments, p_argument_zero);
  if (command == "fmt") {
    int status = 0;
    for (const auto &file :
         ovis::phrase::load_phrase_program(p_arguments.positional[0])) {
      const auto formatted = ovis::phrase::formatted_source(file.text);
      if (formatted == file.text)
        continue;
      if (p_arguments.check_only) {
        std::cerr << "ovis: " << file.path.string() << " would change\n";
        status = 1;
      } else {
        write_source(file.path, formatted);
        std::cerr << "ovis: formatted " << file.path.string() << '\n';
      }
    }
    return status;
  }
  if (command == "phrases") {
    std::string file;
    // The first example comes with each phrase: its words are the manual's.
    std::map<std::string, ovis::phrase::manual_entry> described;
    for (auto &entry : ovis::phrase::manual_search({}, {}, 0))
      described.emplace(entry.pattern, std::move(entry));
    for (const auto &[from, spoken] : ovis::phrase::phrases_like(p_arguments.positional, 12)) {
      // Every phrase is listed under its file; a search, best first.
      if (p_arguments.positional.empty() && from != file)
        std::cout << (file.empty() ? "" : "\n") << (file = from) << ":\n";
      std::cout << "  " << spoken << '\n';
      if (const auto found = described.find(spoken); found != described.end()) {
        if (!found->second.about.empty())
          std::cout << "      " << found->second.about << '\n';
        if (!found->second.examples.empty())
          std::cout << "      e.g. " << found->second.examples.front() << '\n';
      }
    }
    return 0;
  }
  if (command == "manual")
    return manual_command(p_arguments, p_argument_zero);
  if (command == "check-phrases" && p_arguments.dependencies)
    return ovis::phrase::check_dependency_phrases(
        load_program_files(p_arguments, p_arguments.positional[0]), {},
        {.c_imports = p_arguments.c_imports,
         .phrase_directories = p_arguments.phrase_directories},
        std::cout);
  if (command == "check-phrases")
    return ovis::phrase::check_phrases(
        ovis::phrase::parse_phrase_cases(
            ovis::phrase::embedded_question_cases_text()),
        ovis::phrase::read_phrase_definitions(p_arguments.positional[0]),
        std::cout);
  if (command == "runtime") {
    if (p_arguments.output.empty())
      std::cout << ovis::codegen::runtime_source();
    else
      std::ofstream(p_arguments.output) << ovis::codegen::runtime_source();
    return 0;
  }
  if (command == "new" || command == "init")
    return make_project(p_arguments);
  if (command == "version") {
    std::cout << "ovis " OVIS_VERSION "\n";
    return 0;
  }
  if (command != "run" && command != "build" && command != "ir" &&
      command != "check" && command != "fix")
    throw ovis::error("Unknown command " + command + ".\n\n" + usage);
  return run_program_command(p_arguments, p_argument_zero);
}

// Native code for a checked program: printed (`ir`), written (`build`) or
// run (`run`).
auto generate(const arguments &p_arguments, const ovis::core::program &p_program,
              const std::string &program_name,
              const std::filesystem::path &p_source) -> int {
  const auto &command = p_arguments.command;
  const auto stage = [](const char *p_text) {
    if (live != nullptr)
      live->set(p_text);
  };
  llvm::LLVMContext context;
  stage("generating code");
  auto module = ovis::codegen::generate_module(p_program, context,
                                               program_name, !p_arguments.fast);
  stage("optimizing");
  ovis::codegen::optimize(*module);
  if (live != nullptr && command == "ir")
    live->pause();
  if (command == "ir") {
    std::cout << ovis::codegen::to_ir_text(*module);
    return 0;
  }

  // The project's C objects first: the libraries after them resolve them.
  auto libraries = p_arguments.project_link_inputs;
  for (auto &flag : ovis::codegen::libraries_of(p_program))
    libraries.push_back(std::move(flag));
  // For a build system that links: the object file and nothing else.
  if (command == "build" && !p_arguments.object.empty()) {
    if (!p_arguments.link_flags.empty()) {
      // A response file: each flag quoted, for gcc's and clang's @FILE.
      std::ofstream file(p_arguments.link_flags);
      for (const auto &flag : libraries) {
        file << '"';
        for (const auto character : flag)
          file << (character == '"' || character == '\\' ? "\\" : "") << character;
        file << "\"\n";
      }
      if (!file)
        throw ovis::error("Cannot write " + p_arguments.link_flags.string() + ".");
    }
    stage("writing the object file");
    ovis::codegen::emit_object(*module, p_arguments.object,
                               ovis::codegen::c_wrappers_of(p_program));
    return 0;
  }
  if (command == "build") {
    auto output = p_arguments.output;
    if (output.empty()) {
      output = p_source.parent_path() / program_name;
      if (output == p_source)
        output += ".out";
    }
    stage("linking");
    ovis::codegen::build_executable(*module, output, libraries,
                                    !p_arguments.fast, p_arguments.report_leaks,
                                    ovis::codegen::c_wrappers_of(p_program));
    return 0;
  }

  const auto executable =
      std::filesystem::temp_directory_path() /
      ("ovis-run-" + std::to_string(llvm::sys::Process::getProcessId()));
  stage("linking");
  ovis::codegen::build_executable(*module, executable, libraries,
                                  !p_arguments.fast, p_arguments.report_leaks,
                                  ovis::codegen::c_wrappers_of(p_program));
  // The program's own output, from here: the live line is gone.
  if (live != nullptr)
    live->pause();
  const auto status = run_program(executable.string());
  std::filesystem::remove(executable);
  return status;
}

} // namespace

int main(int p_count, char **p_values) {
  try {
    auto expanded = expand_response_files(p_count, p_values);
    std::vector<char *> pointers;
    for (auto &argument : expanded)
      pointers.push_back(argument.data());
    auto arguments =
        parse_arguments(static_cast<int>(pointers.size()), pointers.data());
    apply_project(arguments);
    return run(arguments, p_values[0]);
  } catch (const std::exception &p_error) {
    std::cerr << p_error.what() << '\n';
    return 1;
  }
}

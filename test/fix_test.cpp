#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include <ovis/phrase/front_end.hpp>
#include <ovis/phrase/templates.hpp>

// Problems as tools see them (proposals/quick-fix.md): diagnostics with
// their fixes, `ovis check --json` and `ovis fix`.

using Catch::Matchers::ContainsSubstring;

namespace {

// What a shell command prints on standard output and standard error.
auto output_of(const std::string &p_command) -> std::string {
  std::string output;
  auto *const pipe = popen((p_command + " 2>&1").c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buffer[256];
  while (const auto count = std::fread(buffer, 1, sizeof(buffer), pipe))
    output.append(buffer, count);
  pclose(pipe);
  return output;
}

auto read_file(const std::filesystem::path &p_path) -> std::string {
  std::ifstream input(p_path);
  std::stringstream text;
  text << input.rdbuf();
  return text.str();
}

// A program in a fresh folder of its own.
auto program_file(const std::string &p_name, const std::string &p_text)
    -> std::filesystem::path {
  const auto folder = std::filesystem::temp_directory_path() / ("ovis-fix-" + p_name);
  std::filesystem::remove_all(folder);
  std::filesystem::create_directories(folder);
  const auto path = folder / "p.ovis";
  std::ofstream(path) << p_text;
  return path;
}

const std::string unclear = "total = 1\nshow totl\nnumbers = [3, 1, 2]\n"
                            "show mean of numbers\n";

} // namespace

TEST_CASE("Describe problems for tools.", "[phrase][fix]") {
  const auto result = ovis::phrase::compile_phrases(unclear, nullptr);
  REQUIRE(result.diagnostics.size() == 1);
  const auto &problem = result.diagnostics.front();
  // Cited where it is once `totl`'s meaning is written above line 2.
  CHECK(problem.line == 5);
  CHECK(problem.kind == "unclear");
  CHECK(problem.span == "mean of numbers");
  CHECK(problem.column == 6);
  CHECK(problem.end_column == 21);
  // A fix's meaning is the whole line with the reading put in.
  REQUIRE(problem.fixes.size() == 9);
  CHECK(std::ranges::find(problem.fixes,
                          ovis::phrase::quick_fix{"average of numbers",
                                                  "show average of numbers"}) !=
        problem.fixes.end());
  CHECK_THAT(result.problems.front(), ContainsSubstring("line 5: \"mean of"));

  // Without writing the source back, the lines are as read.
  const auto as_read = ovis::phrase::compile_phrases(
      unclear, nullptr, {.sources_written = false});
  CHECK(as_read.diagnostics.front().line == 4);

  // Other kinds: no phrase at all, and the lexer's, with its column.
  const auto none =
      ovis::phrase::compile_phrases("x = 1\nshow x not 3 at all ever\n", nullptr);
  REQUIRE(!none.diagnostics.empty());
  const auto syntax = ovis::phrase::compile_phrases("show (1\n", nullptr);
  REQUIRE(syntax.diagnostics.size() == 1);
  CHECK(syntax.diagnostics.front().kind == "syntax");
  CHECK(syntax.diagnostics.front().column == 6);
  CHECK_THAT(syntax.problems.front(), ContainsSubstring("line 1, column 6:"));
}

TEST_CASE("Check, and take a fix, from the command line.", "[phrase][fix]") {
  const std::string ovis = OVIS_EXECUTABLE;
  const auto path = program_file("check", unclear);

  // `check --json` changes nothing and cites the file as it is.
  const auto json = nlohmann::json::parse(
      output_of(ovis + " check " + path.string() + " --json 2>/dev/null"));
  REQUIRE(json["diagnostics"].size() == 1);
  CHECK(json["diagnostics"][0]["line"] == 4);
  CHECK(json["diagnostics"][0]["kind"] == "unclear");
  CHECK(read_file(path) == unclear);

  // `fix --line N --apply K` writes the reading's meaning above that line.
  const auto fixes = json["diagnostics"][0]["fixes"];
  std::size_t average = 0;
  for (std::size_t index = 0; index < fixes.size(); ++index)
    if (fixes[index]["reading"] == "average of numbers")
      average = index + 1;
  REQUIRE(average > 0);
  output_of(ovis + " fix " + path.string() + " --line 4 --apply " +
            std::to_string(average));
  CHECK(read_file(path) == "total = 1\nshow totl\nnumbers = [3, 1, 2]\n"
                           "@[meaning: show average of numbers]\n"
                           "show mean of numbers\n");
  CHECK(output_of(ovis + " run " + path.string() + " --offline --quiet") ==
        "1\n2.0\n");

  // `fix` asks, and a meaning may be written by hand; an answer that isn't
  // an option is asked again.
  const auto asked = program_file("ask", "show zorp blat\n");
  const auto answers = output_of("printf 'x\\n4\\nshow \"zorp\"\\n' | " + ovis +
                                 " fix " + asked.string() + " --offline");
  CHECK_THAT(answers, ContainsSubstring("What does \"zorp blat\" mean here?"));
  CHECK_THAT(answers, ContainsSubstring("every line reads"));
  CHECK(read_file(asked) == "@[meaning: show \"zorp\"]\nshow zorp blat\n");

  // A build with no terminal never asks.
  const auto piped = program_file("piped", "show zorp blat\n");
  CHECK_THAT(output_of("echo 1 | " + ovis + " run " + piped.string() +
                       " --offline"),
             !ContainsSubstring("What does"));
}

TEST_CASE("Offer phrases with a line's words when nothing fits.", "[phrase][fix]") {
  // The words of meaning, not the values: `TEXT in capitals`.
  const auto result = ovis::phrase::compile_phrases(
      "show make capitals out of \"hi\" for me now\n", nullptr);
  REQUIRE(!result.succeeded());
  CHECK_THAT(result.problems.front(),
             ContainsSubstring("Phrases with these words:\n    TEXT in capitals"));

  // `ovis phrases`: a search, best first, or every phrase by its file.
  const auto rounding = ovis::phrase::phrases_like({"round", "decimals"}, 3);
  REQUIRE(!rounding.empty());
  CHECK(rounding.front().second == "NUMBER rounded to NUMBER decimal");
  const auto every = ovis::phrase::phrases_like({}, 0);
  CHECK(every.size() == ovis::phrase::builtin_templates().size());
  CHECK(every.front().first == "core.phr");
}

TEST_CASE("Put what the compiler wrote in order.", "[phrase][fix]") {
  // Old lines under a line become sigils above it; sigils line up with
  // their line; a meaning the same as its line goes.
  CHECK(ovis::phrase::formatted_source(
            "x = mean of numbers\n"
            "    Read as: \"mean of numbers\" is average of numbers\n"
            "for each n in numbers:\n"
            "    at the end, show n\n"
            "        Compiler: runs at the end of every pass of the loop.\n"
            "@[meaning:   show x  ]\n"
            "    show y\n"
            "@[meaning: show z]\n"
            "show z\n") ==
        "@[meaning: x = average of numbers]\n"
        "x = mean of numbers\n"
        "for each n in numbers:\n"
        "    @[note: runs at the end of every pass of the loop.]\n"
        "    at the end, show n\n"
        "    @[meaning: show x]\n"
        "    show y\n"
        "show z\n");
  // Formatted text stays as it is; a stranded sigil is left for the build.
  const std::string done = "@[meaning: show 1]\nshow one\n";
  CHECK(ovis::phrase::formatted_source(done) == done);
  CHECK(ovis::phrase::formatted_source("@[note: x]\n\nshow 1\n") ==
        "@[note: x]\n\nshow 1\n");
}

TEST_CASE("Check that a program's phrases don't worsen the questions.",
          "[phrase][fix]") {
  const std::string ovis = OVIS_EXECUTABLE;
  const auto output = output_of(ovis + " check-phrases " + OVIS_EXAMPLES_DIR + "/shop");
  REQUIRE_THAT(output, ContainsSubstring("0 cases worse"));
  // A phrase that reads like a built-in on other lines makes one worse.
  const auto path = program_file("check-phrases",
                                 "to mean of (xs: list of whole numbers):\n"
                                 "    give back sum of xs\n"
                                 "show mean of [1, 2]\n");
  const auto worse = output_of(ovis + " check-phrases " + path.string());
  REQUIRE_THAT(worse, ContainsSubstring("WORSE"));
}

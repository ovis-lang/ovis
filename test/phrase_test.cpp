#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ovis/phrase/lexer.hpp>
#include <ovis/phrase/templates.hpp>

using ovis::phrase::token;
using Catch::Matchers::ContainsSubstring;

namespace {

// Tokens written as kind letters and text: w:show t:Hello n:2 s:,
auto describe(const ovis::phrase::source_line &p_line) -> std::string {
  std::string result;
  for (const auto &item : p_line.tokens) {
    if (!result.empty())
      result += ' ';
    switch (item.kind) {
    case token::kind_type::word:
      result += "w:";
      break;
    case token::kind_type::number:
      result += "n:";
      break;
    case token::kind_type::text:
      result += "t:";
      break;
    case token::kind_type::symbol:
      result += "s:";
      break;
    }
    result += item.text;
  }
  return result;
}

auto lex_failure(std::string_view p_source) -> std::string {
  try {
    ovis::phrase::split_lines(p_source);
  } catch (const ovis::phrase::lex_error &p_error) {
    return p_error.what();
  }
  FAIL("No error for: " << p_source);
  return {};
}

auto template_failure(std::string_view p_text) -> std::string {
  try {
    ovis::phrase::read_templates(p_text, "test.phr");
  } catch (const ovis::error &p_error) {
    return p_error.what();
  }
  FAIL("No error for: " << p_text);
  return {};
}

} // namespace

TEST_CASE("Split a program into lines of tokens.", "[phrase][lexer]") {
  const auto lines = ovis::phrase::split_lines(
      "# Scores\n"
      "scores = [21.5, 23,\n"
      "    19.8]   # continued inside brackets\n"
      "\n"
      "average = mean of scores\n"
      "    Read as: average of scores\n"
      "for each score in scores:\n"
      "    if score > 20:\n"
      "        show \"Warm: \", score, 'degrees'\n"
      "            Compiler:   score is a decimal  \n"
      "show the book's title, b.pages, don't\n"
      "x != 2nd\n");
  REQUIRE(lines.size() == 7);

  REQUIRE(lines[0].line == 2);
  REQUIRE(describe(lines[0]) ==
          "w:scores s:= s:[ n:21.5 s:, n:23 s:, n:19.8 s:]");
  REQUIRE(lines[1].annotations.size() == 1);
  REQUIRE(lines[1].annotations[0].kind ==
          ovis::phrase::annotation::kind_type::read_as);
  REQUIRE(lines[1].annotations[0].text == "average of scores");
  REQUIRE(lines[1].annotations[0].line == 6);

  REQUIRE(lines[2].opens_block());
  REQUIRE(lines[2].depth == 0);
  REQUIRE(lines[3].depth == 1);
  REQUIRE(lines[4].depth == 2);
  REQUIRE(describe(lines[4]) ==
          "w:show t:Warm:  s:, w:score s:, t:degrees");
  // A note under a deeper block belongs to the line just above it.
  REQUIRE(lines[4].annotations.at(0).text == "score is a decimal");

  REQUIRE(lines[5].depth == 0);
  REQUIRE(describe(lines[5]) ==
          "w:show w:the w:book s:'s w:title s:, w:b s:. w:pages s:, w:don't");
  REQUIRE(lines[5].tokens[0].column == 1);
  REQUIRE(lines[5].tokens[0].is_word("SHOW"));
  REQUIRE(describe(lines[6]) == "w:x s:!= w:2nd");
  REQUIRE(lines[6].tokens[2].column == 6);
}

TEST_CASE("Read typographic quotes and apostrophes.", "[phrase][lexer]") {
  const auto lines = ovis::phrase::split_lines(
      "show \u201cIt\u2019s warm\u201d, the week\u2019s days\n");
  REQUIRE(describe(lines[0]) ==
          "w:show t:It\u2019s warm s:, w:the w:week s:'s w:days");
}

TEST_CASE("Number words, operators and the last line.", "[phrase][lexer]") {
  const auto lines = ovis::phrase::split_lines("x != 2nd <= 3.5 - 1\ny = 2.");
  REQUIRE(describe(lines[0]) == "w:x s:!= w:2nd s:<= n:3.5 s:- n:1");
  // "2." is the number 2 and a full stop; the parser decides about the stop.
  REQUIRE(describe(lines[1]) == "w:y s:= n:2 s:.");
}

TEST_CASE("Report layout mistakes with their place.", "[phrase][lexer]") {
  REQUIRE_THAT(lex_failure("if x:\n\tshow x\n"),
               ContainsSubstring("line 2, column 1: Indent with spaces"));
  REQUIRE_THAT(lex_failure("  show x\n"),
               ContainsSubstring("first line can't be indented"));
  REQUIRE_THAT(lex_failure("show x\n    show y\n"),
               ContainsSubstring("line 1 doesn't end with ':'"));
  REQUIRE_THAT(lex_failure("if x:\nshow y\n"),
               ContainsSubstring("Indent the block under line 1"));
  REQUIRE_THAT(lex_failure("if x:\n    show y\n  show z\n"),
               ContainsSubstring("line 3, column 3: This line doesn't line up"));
  REQUIRE_THAT(lex_failure("show x\nif y:\n"),
               ContainsSubstring("nothing is indented under it"));
  REQUIRE_THAT(lex_failure("show \"hi\n"),
               ContainsSubstring("column 6: This text has no closing quote"));
  REQUIRE_THAT(lex_failure("show (x\n\nshow y\n"),
               ContainsSubstring("line 1, column 6: This '(' is never closed"));
  REQUIRE_THAT(lex_failure("show x)\n"),
               ContainsSubstring("This ')' closes nothing"));
  REQUIRE_THAT(lex_failure("show x & y\n"),
               ContainsSubstring("'&' means nothing"));
  REQUIRE_THAT(lex_failure("show \"a\\qb\"\n"),
               ContainsSubstring("Unknown escape \\q"));
  REQUIRE_THAT(lex_failure("Read as: x\n"),
               ContainsSubstring("must be indented under the line"));
  REQUIRE_THAT(lex_failure("show x\nCompiler: x\n"),
               ContainsSubstring("must be indented under the line"));
  // A sigil goes right above its line, lined up with it.
  REQUIRE_THAT(lex_failure("@[note: x]\n\nshow x\n"),
               ContainsSubstring("line 1, column 1: A '@[…]' line goes right above"));
  REQUIRE_THAT(lex_failure("show x\n@[note: x]\n"),
               ContainsSubstring("goes right above"));
  REQUIRE_THAT(lex_failure("if true:\n@[note: x]\n    show x\n"),
               ContainsSubstring("Line this '@[…]' up with the line under it"));
  REQUIRE_THAT(lex_failure("@[later: x]\nshow x\n"),
               ContainsSubstring("Write '@[meaning: LINE]' or '@[note: TEXT]'"));
  REQUIRE_THAT(lex_failure("@[note: x] y\nshow x\n"),
               ContainsSubstring("Write '@[meaning: LINE]'"));
}

TEST_CASE("Read sigils above lines.", "[phrase][lexer]") {
  using kind = ovis::phrase::annotation::kind_type;
  // The `]` that ends one is outside texts; sigils don't open blocks.
  const auto lines = ovis::phrase::split_lines(
      "if true:\n"
      "    @[meaning: show \"[a]\", 1]\n"
      "    @[note:  runs first ]\n"
      "    show \"[a]\", one\n"
      "show 2\n");
  REQUIRE(lines.size() == 3);
  REQUIRE(lines[1].annotations.size() == 2);
  CHECK(lines[1].annotations[0].kind == kind::meaning);
  CHECK(lines[1].annotations[0].text == "show \"[a]\", 1");
  CHECK(lines[1].annotations[0].is_sigil);
  CHECK(lines[1].annotations[1].kind == kind::compiler);
  CHECK(lines[1].annotations[1].text == "runs first");
  CHECK(lines[2].depth == 0);
}

TEST_CASE("A template keeps its about and example lines.",
          "[phrase][templates]") {
  const auto templates = ovis::phrase::read_templates(
      "expr  {s: str} in capitals : str => (prelude_upper $s)\n"
      "      about: The text with every letter a capital: \"Ada\" is \"ADA\".\n"
      "      alias: {s} in upper case\n"
      "      example: show \"Ada\" in capitals\n"
      "      example: shout = message in capitals\n"
      "stmt  end the program => (exit 0)\n",
      "text.phr");
  REQUIRE(templates.size() == 2);
  CHECK(templates[0].about ==
        "The text with every letter a capital: \"Ada\" is \"ADA\".");
  CHECK(templates[0].patterns.size() == 2);
  REQUIRE(templates[0].examples.size() == 2);
  CHECK(templates[0].examples[1] == "shout = message in capitals");
  CHECK(templates[1].about.empty());
  CHECK(templates[1].examples.empty());
  CHECK_THROWS_WITH(ovis::phrase::read_templates(
                        "stmt  end the program => (exit 0)\n"
                        "      about: One.\n      about: Two.\n",
                        "x.phr"),
                    ContainsSubstring("one 'about:' line"));
  CHECK_THROWS_WITH(ovis::phrase::read_templates(
                        "stmt  end the program => (exit 0)\n      example:\n",
                        "x.phr"),
                    ContainsSubstring("'example:' needs a line"));
}

TEST_CASE("Read phrase templates.", "[phrase][templates]") {
  const auto templates = ovis::phrase::read_templates(
      "# lists\n"
      "expr  average of {xs: num[]}  : float  => (/ (float (sum $xs)) (float "
      "(len $xs)))\n"
      "      alias: mean of {xs}\n"
      "\n"
      "expr  empty list of {t: type} : t[] => (array $t 0)\n"
      "expr  {x: float} with {n: int} decimal[s] : str => (fixed $x $n)\n"
      "stmt  (show|print out) [the] {items: any, ...} => (print $items...)\n"
      "stmt  keep doing {n: int} times: BLOCK => (for _ 1 $n BLOCK)\n",
      "lists.phr");
  REQUIRE(templates.size() == 5);

  const auto &average = templates[0];
  REQUIRE(average.kind == ovis::phrase::phrase_template::kind_type::expression);
  REQUIRE(average.file == "lists.phr");
  REQUIRE(average.line == 2);
  REQUIRE(average.patterns.size() == 2);
  REQUIRE(average.slots.size() == 1);
  REQUIRE(average.slots[0].type.to_string() == "num[]");
  REQUIRE(average.result->to_string() == "float");
  REQUIRE(average.core == "(/ (float (sum $xs)) (float (len $xs)))");
  REQUIRE(to_string(average, average.patterns[0]) == "average of {xs: num[]}");
  REQUIRE(to_string(average, average.patterns[1]) == "mean of {xs: num[]}");

  REQUIRE(templates[1].result->kind ==
          ovis::phrase::type_name::kind_type::type_slot);
  REQUIRE(to_string(templates[2], templates[2].patterns[0]) ==
          "{x: float} with {n: int} decimal[s]");
  REQUIRE(to_string(templates[3], templates[3].patterns[0]) ==
          "(show|print out) [the] {items: any, ...}");
  REQUIRE(templates[3].slots[0].is_variadic);
  REQUIRE(!templates[3].result);
  REQUIRE(templates[4].takes_block);
  REQUIRE(templates[4].slots[0].type.name == "int");
}

TEST_CASE("Report mistakes in phrase templates.", "[phrase][templates]") {
  const auto failure = [](std::string_view p_text) {
    return template_failure(p_text);
  };
  REQUIRE_THAT(failure("\n\nexpr size of {xs: T[]} => (len $xs)"),
               ContainsSubstring("test.phr:3: An expr needs a result type"));
  REQUIRE_THAT(failure("stmt sort {xs: T[]} : int => (sort $xs)"),
               ContainsSubstring("A stmt gives back nothing"));
  REQUIRE_THAT(failure("expr loop {n: int}: BLOCK => (for _ 1 $n BLOCK)"),
               ContainsSubstring("Only a stmt can take a block"));
  REQUIRE_THAT(failure("expr size of {xs} : int => (len $xs)"),
               ContainsSubstring("Give the slot 'xs' a type"));
  REQUIRE_THAT(failure("expr size of {xs: list} : int => (len $xs)"),
               ContainsSubstring("Unknown type 'list'"));
  REQUIRE_THAT(failure("expr pick {a: T} or {b: T} : U => (max $a $b)"),
               ContainsSubstring("U doesn't appear in any slot"));
  REQUIRE_THAT(failure("expr size of {xs: T[]} : int => (len $ys)"),
               ContainsSubstring("uses $ys, but the pattern has no such slot"));
  REQUIRE_THAT(failure("expr pick {a: int} or {b: int} : int => $a"),
               ContainsSubstring("'b' is never used"));
  REQUIRE_THAT(failure("expr size of {xs: T[]} : int => (len $xs) (len $xs)"),
               ContainsSubstring("must be one form"));
  REQUIRE_THAT(failure("stmt show {xs: any, ...} => (print $xs)"),
               ContainsSubstring("write $xs..."));
  REQUIRE_THAT(failure("stmt show {xs: any} => (print $xs...)"),
               ContainsSubstring("Only a variadic slot can be spliced"));
  REQUIRE_THAT(failure("stmt loop {n: int} => (for _ 1 $n BLOCK)"),
               ContainsSubstring("doesn't end with ': BLOCK'"));
  REQUIRE_THAT(failure("stmt loop {n: int}: BLOCK => (for _ 1 $n)"),
               ContainsSubstring("never uses BLOCK"));
  REQUIRE_THAT(failure("expr x {a: int} {b: int} : int => (+ $a $b)"),
               ContainsSubstring("Put a word between two slots"));
  REQUIRE_THAT(failure("expr {a: int} : int => $a"),
               ContainsSubstring("at least one word"));
  REQUIRE_THAT(failure("expr x {a: int} y {a: int} : int => $a"),
               ContainsSubstring("Two slots are called 'a'"));
  REQUIRE_THAT(failure("expr size of {xs: T[]} : int => (len $xs)\n"
                       "  alias: count {ys}"),
               ContainsSubstring("test.phr:2: The alias has a slot 'ys'"));
  REQUIRE_THAT(failure("expr pick {a: int} or {b: int} : int => (max $a $b)\n"
                       "  alias: choose {a}"),
               ContainsSubstring("The alias leaves out the slot 'b'"));
  REQUIRE_THAT(failure("expr size of {xs: T[]} : int => (len $xs)\n"
                       "  alias: count {xs: T[]}"),
               ContainsSubstring("Types go on the main pattern"));
  REQUIRE_THAT(failure("  alias: count {xs}"),
               ContainsSubstring("Only 'alias:', 'about:', 'example:' and 'for records:' lines are indented"));
  REQUIRE_THAT(failure("fn size of {xs: T[]} : int => (len $xs)"),
               ContainsSubstring("starts with 'expr' or 'stmt'"));
  REQUIRE_THAT(failure("expr size (of) {xs: T[]} : int => (len $xs)"),
               ContainsSubstring("has one choice"));
  REQUIRE_THAT(failure("expr size of {xs: T[]} : int => (len $xs"),
               ContainsSubstring("doesn't read"));
}

TEST_CASE("The built-in phrase library reads.", "[phrase][templates]") {
  REQUIRE(!ovis::phrase::builtin_phrase_files().empty());
  const auto &templates = ovis::phrase::builtin_templates();
  REQUIRE(templates.size() >= 10);
  for (const auto &item : templates) {
    INFO(item.file << ":" << item.line);
    REQUIRE((!item.core.empty() || item.records_only));
  }
}

#include <ovis/phrase/front_end.hpp>

namespace {

// Answers every question with the same probabilities, and remembers what it
// was asked.
class fixed_chooser final : public ovis::english::language_model {
public:
  std::vector<double> answer;
  std::vector<std::string> questions;

  auto name() const -> std::string override { return "fixed"; }
  auto chat(const std::vector<ovis::english::chat_message> &,
            std::string_view) -> std::string override {
    FAIL("Only choose is used.");
    return {};
  }
  auto choose(const std::vector<ovis::english::chat_message> &p_messages,
              std::size_t p_options) -> std::vector<double> override {
    questions.push_back(p_messages.back().content);
    auto result = answer;
    result.resize(p_options, 0);
    return result;
  }
};

auto problems_of(const ovis::phrase::front_end_result &p_result)
    -> std::string {
  std::string text;
  for (const auto &problem : p_result.problems)
    text += problem + "\n";
  return text;
}

} // namespace

TEST_CASE("Read a program written in phrases.", "[phrase][front-end]") {
  const auto result = ovis::phrase::compile_phrases(
      "numbers = [12, 7, 30]\n"
      "show \"Smallest:\", smallest of numbers, \"Sum:\", sum of numbers\n"
      "total = 0\n"
      "for each n in numbers:\n"
      "    if n is even:\n"
      "        total = total + n\n"
      "    otherwise if n > 10:\n"
      "        stop\n"
      "    otherwise:\n"
      "        big = n * 2\n"
      "repeat 2 times:\n"
      "    add 1 to numbers\n"
      "show total, big, length of numbers, square root of 16 + 1\n",
      nullptr);
  INFO(problems_of(result));
  // `square root of 16 + 1` reads two ways: a question nobody can answer
  // offline.
  REQUIRE(result.problems.size() == 1);
  REQUIRE_THAT(result.problems[0],
               ContainsSubstring("line 13: \"show total, big"));
  REQUIRE_THAT(result.problems[0], ContainsSubstring("(square root of 16) + 1"));
  REQUIRE_THAT(result.problems[0], ContainsSubstring("square root of (16 + 1)"));
}

TEST_CASE("Report what phrases can't read.", "[phrase][front-end]") {
  const auto failure = [](std::string_view p_source) {
    return problems_of(ovis::phrase::compile_phrases(p_source, nullptr));
  };
  // `totl` is read as `total`; `nmbr` is no near spelling, so the names
  // that fit are offered. The line is cited where it is after the build:
  // the meaning of `totl` went above it.
  REQUIRE_THAT(failure("total = 1\nshow totl + nmbr\n"),
               ContainsSubstring("line 3: \"nmbr\" is unclear"));
  REQUIRE_THAT(failure("words = [\"a\"]\nshow 1 + nmbr\n"),
               ContainsSubstring("line 2: Nothing called 'nmbr' is known here."));
  REQUIRE_THAT(failure("if 1 > 0\n"), ContainsSubstring("End the line with ':'"));
  REQUIRE_THAT(failure("x = [1, 2]\nx = 3 +\n"),
               ContainsSubstring("line 2:"));
  REQUIRE_THAT(failure("show (1\n"), ContainsSubstring("never closed"));
  REQUIRE_THAT(failure("n = 3\nfor each x in n:\n    show x\n"),
               ContainsSubstring("is a whole number, not a list"));
  // A failed line doesn't make every later use of its name fail too.
  REQUIRE(ovis::phrase::compile_phrases(
              "numbers = [1, 2]\naverage = mean of numbers\nshow average\n",
              nullptr)
              .problems.size() == 1);
}

TEST_CASE("Settle unclear phrases with the model, and remember it.",
          "[phrase][front-end]") {
  const std::string source = "numbers = [1, 2, 6]\n"
                             "result = mean of numbers\n"
                             "show result\n";
  fixed_chooser model;
  // The first reading, sure enough to be taken.
  model.answer = {0.9, 0.05, 0.05};
  const auto first = ovis::phrase::compile_phrases(source, &model);
  INFO(problems_of(first));
  REQUIRE(first.succeeded());
  // A list's 9 one-list readings fit in one question, with "none of these"
  // as 0.
  REQUIRE(model.questions.size() == 1);
  REQUIRE_THAT(model.questions.front(), ContainsSubstring("0. none of these"));
  REQUIRE_THAT(model.questions.front(),
               ContainsSubstring("Known: numbers is a list of whole numbers."));
  REQUIRE_THAT(model.questions.front(),
               ContainsSubstring("\"mean of numbers\""));
  REQUIRE_THAT(first.source, ContainsSubstring("@[meaning: result = "));
  REQUIRE_THAT(first.source, ContainsSubstring("]\nresult = mean of numbers\n"));
  REQUIRE(first.timing.questions == 1);

  // The `@[meaning: …]` line settles it from now on: no model needed, and the
  // source stays as it is.
  const auto second = ovis::phrase::compile_phrases(first.source, nullptr);
  INFO(problems_of(second));
  REQUIRE(second.succeeded());
  REQUIRE(second.source == first.source);
  REQUIRE(second.timing.asked == 0);

  // Once the line is edited, the lockfile shows the meaning was for the
  // old line: it is dropped and the line read afresh.
  auto edited = first.source;
  edited.replace(edited.find("mean of numbers"), 15, "sum of numbers");
  const auto third =
      ovis::phrase::compile_phrases(edited, nullptr, {.previous = &first.lock});
  REQUIRE(third.succeeded());
  REQUIRE(third.source.find("@[meaning") == std::string::npos);
  // Without the lockfile it is the author's, and wins.
  const auto authored = ovis::phrase::compile_phrases(edited, nullptr);
  REQUIRE(authored.succeeded());
  REQUIRE(authored.source == edited);
  REQUIRE(authored.timing.by_source == 1);
}

TEST_CASE("Write what the compiler read above the line.", "[phrase][front-end]") {
  // A one-line block's header and body share one meaning, and a rebuild
  // changes nothing.
  const auto block = ovis::phrase::compile_phrases(
      "total = 1\nif total > 0: show totl\n", nullptr);
  INFO(problems_of(block));
  REQUIRE(block.succeeded());
  REQUIRE(block.source == "total = 1\n@[meaning: if total > 0: show total]\n"
                          "if total > 0: show totl\n");
  const auto again =
      ovis::phrase::compile_phrases(block.source, nullptr, {.previous = &block.lock});
  REQUIRE(again.source == block.source);
  REQUIRE(again.lock == block.lock);
  REQUIRE(again.timing.by_source == 1);

  // Old `Read as:` and `Compiler:` lines are read, then written as sigils.
  const auto old = ovis::phrase::compile_phrases(
      "numbers = [1, 2]\nx = mean of numbers\n"
      "    Read as: \"mean of numbers\" is average of numbers\n"
      "for each n in numbers:\n"
      "    at the end, show n\n"
      "        Compiler: runs at the end of every pass of the loop.\n",
      nullptr);
  INFO(problems_of(old));
  REQUIRE(old.succeeded());
  REQUIRE(old.source ==
          "numbers = [1, 2]\n@[meaning: x = average of numbers]\n"
          "x = mean of numbers\nfor each n in numbers:\n"
          "    @[note: runs at the end of every pass of the loop.]\n"
          "    at the end, show n\n");
  REQUIRE(old.lock.meanings ==
          std::vector<ovis::phrase::meaning_entry>{
              {"x = mean of numbers", "x = average of numbers"}});
}

TEST_CASE("Leave unsure answers to the author.", "[phrase][front-end]") {
  fixed_chooser model;
  model.answer = {0.5, 0.4, 0.1};
  const auto result = ovis::phrase::compile_phrases(
      "x = 9\nshow square root of x + 7\n", &model);
  REQUIRE(model.questions.size() == 1);
  REQUIRE_THAT(problems_of(result),
               ContainsSubstring("line 2: \"show square root of x + 7\" is "
                                 "unclear. Did you mean one of these?"));
  REQUIRE(result.source.find("@[meaning") == std::string::npos);
}

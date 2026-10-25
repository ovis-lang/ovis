#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ovis/core/memory_check.hpp>
#include <ovis/phrase/front_end.hpp>

// The strict memory rules (proposals/memory.md §2 and §4), checked when
// `strict_memory` is set.

using Catch::Matchers::ContainsSubstring;

namespace {

// The problems a program has under the strict rules, one string.
auto strict_problems(const std::string &p_source) -> std::string {
  const auto result =
      ovis::phrase::compile_phrases(p_source, nullptr, {.strict_memory = true});
  std::string text;
  for (const auto &problem : result.problems)
    text += problem + "\n";
  return text;
}

} // namespace

TEST_CASE("A value lost on every pass of a loop is an error.", "[memory][strict]") {
  const auto lost = strict_problems("count = 0\n"
                                    "while count < 5:\n"
                                    "    label = \"item \" + count\n"
                                    "    count = count + 1\n"
                                    "show count\n");
  REQUIRE_THAT(lost, ContainsSubstring("line 3:"));
  REQUIRE_THAT(lost, ContainsSubstring("`label` gets new text here, on every "
                                       "pass of the loop,"));
  REQUIRE_THAT(lost, ContainsSubstring("`free label`"));
  // Both fixes of the proposal build.
  REQUIRE(strict_problems("count = 0\n"
                          "while count < 5:\n"
                          "    with new memory:\n"
                          "        label = \"item \" + count\n"
                          "    count = count + 1\n"
                          "show count\n").empty());
  REQUIRE(strict_problems("label = \"\"\n"
                          "count = 0\n"
                          "while count < 5:\n"
                          "    free label\n"
                          "    label = \"item \" + count\n"
                          "    count = count + 1\n"
                          "show count\n").empty());
  // `at the end` in the loop's body runs at the end of every pass, before
  // the next pass gives the variable a new value.
  REQUIRE(strict_problems("count = 0\n"
                          "while count < 5:\n"
                          "    words = \"a b\" split by \" \"\n"
                          "    at the end, free words\n"
                          "    kept = new list of text\n"
                          "    at the end:\n"
                          "        free kept\n"
                          "    for each word in words:\n"
                          "        add word to kept\n"
                          "    count = count + 1\n"
                          "show count\n").empty());
  // Without the strict rules the program builds as it always did.
  const auto lenient = ovis::phrase::compile_phrases(
      "count = 0\nwhile count < 5:\n    label = \"item \" + count\n"
      "    count = count + 1\nshow count\n",
      nullptr);
  REQUIRE(lenient.succeeded());
}

TEST_CASE("A new value needs a place to be stored.", "[memory][strict]") {
  const auto shown = strict_problems("numbers = [3, 1, 2]\n"
                                     "show \"count \" + length of numbers\n");
  REQUIRE_THAT(shown, ContainsSubstring("makes new text inside `show`"));
  REQUIRE_THAT(shown, ContainsSubstring("Store it in a variable first"));
  // One join is one value, however many parts.
  REQUIRE(strict_problems("name = \"Ann\"\nage = 7\n"
                          "line = name + \" is \" + age + \" years\"\n"
                          "show line\nfree line\n").empty());
  // A new value as an argument, or in a condition.
  REQUIRE_THAT(strict_problems("name = \"Ann\"\n"
                               "if length of (name + \"!\") > 3:\n"
                               "    show \"long\"\n"),
               ContainsSubstring("makes new text"));
  // Storing it is the way.
  REQUIRE(strict_problems("names = empty list of text\n"
                          "add \"a\" + \"b\" to names\n"
                          "show names\n").empty());
}

TEST_CASE("A value of a new memory can't outlive it.", "[memory][strict]") {
  const auto escape = strict_problems("names = empty list of text\n"
                                      "with new memory:\n"
                                      "    line = \"ab\" + \"cd\"\n"
                                      "    add line to names\n"
                                      "show line\n");
  REQUIRE_THAT(escape, ContainsSubstring("line 4:"));
  REQUIRE_THAT(escape, ContainsSubstring("new memory of line 2"));
  REQUIRE_THAT(escape, ContainsSubstring("can't be stored in `names`"));
  REQUIRE_THAT(escape, ContainsSubstring("line 5:"));
  REQUIRE_THAT(escape, ContainsSubstring("`line` was made in the new memory"));
  // `keep` makes a copy outside.
  REQUIRE(strict_problems("names = empty list of text\n"
                          "with new memory:\n"
                          "    line = \"ab\" + \"cd\"\n"
                          "    add keep line to names\n"
                          "show names\n").empty());
}

TEST_CASE("Values of a named memory last until it is cleared or freed.",
          "[memory][strict]") {
  const std::string start = "scratch = new memory\n"
                            "at the end, free scratch\n"
                            "with scratch:\n"
                            "    label = \"a\" + 1\n";
  // After the block, still there.
  REQUIRE(strict_problems(start + "show label\n").empty());
  const auto cleared = strict_problems(start + "clear scratch\nshow label\n");
  REQUIRE_THAT(cleared, ContainsSubstring("line 6:"));
  REQUIRE_THAT(cleared, ContainsSubstring("`label` was made in `scratch`, which "
                                          "was cleared on line 5"));
  REQUIRE_THAT(strict_problems(start + "free scratch\nshow label\n"),
               ContainsSubstring("which was freed on line 5"));
  // Given a new value in it again, it is fine.
  REQUIRE(strict_problems(start + "clear scratch\n"
                                  "with scratch:\n"
                                  "    label = \"b\" + 2\n"
                                  "show label\n")
              .empty());
}

TEST_CASE("Items of a list are followed to the memory they were made in.",
          "[memory][strict]") {
  // A list of lists whose rows were made in a block: an item read after it.
  const auto rows = strict_problems("with new memory:\n"
                                    "    row = [1, 2]\n"
                                    "    rows = [row]\n"
                                    "    keep_rows = keep rows\n"
                                    "first = item 1 of keep_rows\n"
                                    "show first\n");
  REQUIRE(rows.empty()); // `keep` copies the list, not the rows: not followed.
  const auto inner = strict_problems("outer = empty list of lists of whole numbers\n"
                                     "scratch = new memory\n"
                                     "with scratch:\n"
                                     "    row = [1, 2]\n"
                                     "    add row to outer\n"
                                     "clear scratch\n"
                                     "first = item 1 of outer\n"
                                     "free scratch\n"
                                     "free outer\n");
  REQUIRE_THAT(inner, ContainsSubstring("line 7:"));
  REQUIRE_THAT(inner, ContainsSubstring("This item was made in `scratch`"));
}

TEST_CASE("A phrase gives back new values or existing ones, not both.",
          "[memory][strict]") {
  const auto mixed = strict_problems("to pick (a: text) or (b: text):\n"
                                     "    if length of a > 2:\n"
                                     "        give back a + b\n"
                                     "    give back b\n"
                                     "x = pick \"abc\" or \"d\"\n"
                                     "show x\n");
  REQUIRE_THAT(mixed, ContainsSubstring("gives back new text in one place and "
                                        "existing text in another"));
  REQUIRE(strict_problems("to join (a: text) with (b: text):\n"
                          "    joined = a + b\n"
                          "    give back joined\n"
                          "x = join \"a\" with \"b\"\n"
                          "show x\nfree x\n").empty());
  // A phrase that gives back new text is a new value where it is called.
  REQUIRE_THAT(strict_problems("to join (a: text) with (b: text):\n"
                               "    joined = a + b\n"
                               "    give back joined\n"
                               "show join \"a\" with \"b\"\n"),
               ContainsSubstring("makes new text inside `show`"));
}

TEST_CASE("The prelude follows the strict rules.", "[memory][strict]") {
  // With the generic functions a record's phrases copy in.
  const auto result = ovis::phrase::compile_phrases(
      "a book has:\n"
      "    title (text)\n"
      "    pages (whole number)\n"
      "to (a: book) comes before (b: book):\n"
      "    give back pages of a < pages of b\n"
      "to (a: book) is the same as (b: book):\n"
      "    give back title of a = title of b\n"
      "to text of (b: book):\n"
      "    give back title of b + \"!\"\n"
      "one = a book with title \"A\", pages 3\n"
      "two = a book with title \"B\", pages 2\n"
      "books = [one, two]\n"
      "sort books\n"
      "shown = books as text\n"
      "show shown, books contains one, position of one in books\n"
      "show count of one in books, total pages of books\n"
      "remove one from books\n"
      "sorted = books sorted by pages\n"
      "show sorted\n",
      nullptr);
  REQUIRE(result.succeeded());
  std::string text;
  for (const auto &problem : ovis::core::check_memory(result.program, 0))
    if (problem.unit_index == 0)
      text += std::to_string(problem.location.line) + ": " + problem.message + "\n";
  CHECK(text == "");
  INFO(result.problems.size());
  CHECK(ovis::phrase::compile_phrases("show 1\n", nullptr).succeeded());
}

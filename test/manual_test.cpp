#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include <ovis/phrase/front_end.hpp>

#include "support.hpp"

using namespace std::literals::string_literals;

TEST_CASE("Find a phrase by the words of what it does.", "[phrase][manual]") {
  // The pattern's words count in full, so the phrase asked for comes first.
  const auto rounding = ovis::phrase::manual_search({"round", "decimals"}, {}, 3);
  REQUIRE(!rounding.empty());
  CHECK(rounding.front().pattern == "NUMBER rounded to NUMBER decimal");
  CHECK(rounding.front().topic == "numbers");
  CHECK(!rounding.front().about.empty());
  CHECK(!rounding.front().examples.empty());

  // A way of saying it other than the first finds it too, and shows the rest.
  const auto upper = ovis::phrase::manual_search({"uppercase"}, {}, 3);
  REQUIRE(!upper.empty());
  CHECK(upper.front().pattern == "TEXT in capitals");
  CHECK(std::ranges::find(upper.front().aliases, "TEXT in uppercase"s) !=
        upper.front().aliases.end());

  // Words only the `about:` sentence has: "letter" is not in `in capitals`.
  const auto by_meaning = ovis::phrase::manual_search({"letter", "capital"}, {}, 6);
  CHECK(std::ranges::any_of(by_meaning, [](const auto &p_entry) {
    return p_entry.pattern == "TEXT in capitals";
  }));
  // ...but a word of the pattern ranks above a word of the sentence.
  CHECK(by_meaning.front().pattern == "TEXT in capitals");

  CHECK(ovis::phrase::manual_search({"zzyzx"}, {}, 6).empty());
}

TEST_CASE("List every phrase by topic when asked for none in particular.",
          "[phrase][manual]") {
  const auto every = ovis::phrase::manual_search({}, {}, 0);
  CHECK(every.size() == ovis::phrase::builtin_templates().size());
  // The library's order keeps a topic's phrases together.
  CHECK(every.front().topic == "core");
  CHECK(std::ranges::all_of(every, [](const auto &p_entry) {
    return !p_entry.about.empty() && !p_entry.pattern.empty();
  }));
}

TEST_CASE("A program's own phrases are searched with the library's.",
          "[phrase][manual]") {
  auto own = ovis::phrase::read_templates(
      "expr  flip of {xs: num[]} : num[] => (prelude_reverse $xs)\n"
      "      about: The numbers back to front.\n",
      "phrases/mine.phr");
  const auto found = ovis::phrase::manual_search({"flip"}, own, 3);
  REQUIRE(!found.empty());
  CHECK(found.front().pattern == "flip of LIST");
  CHECK(found.front().topic == "mine");
}

namespace {

// Embeds by topic: a text about capitals is one direction, a query that says
// "shout" the same, everything else another. Counts the texts it was given.
class topic_model final : public ovis::english::language_model {
public:
  std::size_t embedded = 0;
  auto name() const -> std::string override { return "topic"; }
  auto chat(const std::vector<ovis::english::chat_message> &, std::string_view)
      -> std::string override {
    return {};
  }
  auto embed(const std::vector<std::string> &p_texts)
      -> std::vector<std::vector<float>> override {
    std::vector<std::vector<float>> result;
    for (const auto &text : p_texts) {
      ++embedded;
      const bool capitals = text.find("capitals") != std::string::npos ||
                            text.find("shout") != std::string::npos;
      result.push_back(capitals ? std::vector<float>{1, 0}
                                : std::vector<float>{0, 1});
    }
    return result;
  }
};

} // namespace

TEST_CASE("A model finds a phrase by its meaning where the words miss it.",
          "[phrase][manual]") {
  CHECK(ovis::phrase::manual_search({"shout"}, {}, 3).empty());

  topic_model model;
  const auto cache = std::filesystem::temp_directory_path() / "ovis-manual-test.bin";
  std::filesystem::remove(cache);
  const auto found = ovis::phrase::manual_search(
      {"shout"}, {}, 3, {.model = &model, .cache = cache});
  REQUIRE(!found.empty());
  CHECK(found.front().pattern == "TEXT in capitals");
  CHECK(model.embedded > 100);

  // A second search embeds the query alone: the entries come from the cache.
  model.embedded = 0;
  const auto again = ovis::phrase::manual_search(
      {"shout"}, {}, 3, {.model = &model, .cache = cache});
  CHECK(model.embedded == 1);
  CHECK(again.front().pattern == "TEXT in capitals");
  std::filesystem::remove(cache);

  // A word of the phrase still counts: the words and the meaning are joined.
  const auto both = ovis::phrase::manual_search({"reversed"}, {}, 3, {.model = &model});
  CHECK(std::ranges::any_of(both, [](const auto &p_entry) {
    return p_entry.pattern == "TEXT reversed";
  }));

  // A model that can't embed leaves the search to the words.
  class mute final : public ovis::english::language_model {
  public:
    auto name() const -> std::string override { return "mute"; }
    auto chat(const std::vector<ovis::english::chat_message> &, std::string_view)
        -> std::string override {
      return {};
    }
  } silent;
  CHECK(ovis::phrase::manual_search({"shout"}, {}, 3, {.model = &silent}).empty());
  CHECK(ovis::phrase::manual_search({"uppercase"}, {}, 3, {.model = &silent})
            .front()
            .pattern == "TEXT in capitals");
}

TEST_CASE("A program's and its packages' phrases are entries, with their comments "
          "and uses.",
          "[phrase][manual]") {
  ovis::phrase::phrase_file main_file{
      .path = "main.ovis",
      .text = "total = apply the discount to 50\n"
              "show total\n"};
  main_file.sees = {"."};
  ovis::phrase::phrase_file package_file{
      .path = "pricing/discount.ovis",
      .name = "pricing/discount.ovis",
      .text = "# Orders of 40 or more get 10% off.\n"
              "# Rounded down.\n"
              "to apply the discount to (total: decimal) gives a decimal:\n"
              "    give back total * 0.9\n"
              "to broken (:\n"};
  package_file.package = "pricing@1";
  const auto entries = ovis::phrase::program_phrase_templates(
      {package_file, main_file}, "main.ovis");
  REQUIRE(entries.size() == 1);
  CHECK(entries.front().about.starts_with("Orders of 40 or more get 10% off. Rounded down."));
  // The package isn't used by the file asking: the entry says how to.
  CHECK(entries.front().about.find("write `use pricing`") != std::string::npos);
  REQUIRE(entries.front().examples.size() == 1);
  CHECK(entries.front().examples.front() ==
        "total = apply the discount to 50   (main.ovis, line 1)");

  const auto found = ovis::phrase::manual_search({"discount"}, entries, 3);
  REQUIRE(!found.empty());
  CHECK(found.front().package == "pricing@1");
  CHECK(found.front().pattern == "apply the discount to VALUE");
}

TEST_CASE("The names in scope before a line, and the phrases that take one.",
          "[phrase][manual]") {
  ovis::phrase::phrase_file file{
      .path = "main.ovis",
      .text = "count = 3\n"
              "numbers = [1, 2]\n"
              "for each n in numbers:\n"
              "    total = n + 1\n"
              "show total\n"};
  const auto program = ovis::phrase::compile_phrase_program(
      {file}, nullptr, {.sources_written = false, .variables_recorded = true});
  REQUIRE(program.succeeded());
  const auto &before = program.variables.at(0);
  // Nothing is set before the first line; later lines see what earlier ones set.
  CHECK(before.at(1).empty());
  REQUIRE(before.at(3).size() == 2);
  CHECK(before.at(3)[0].name == "count");
  CHECK(before.at(3)[0].type == "a whole number");
  CHECK(before.at(3)[0].line == 1);
  CHECK(before.at(3)[1].name == "numbers");
  CHECK(before.at(3)[1].type == "a list of whole numbers");
  CHECK(before.at(3)[1].line == 2);
  // A name set inside a block is in scope after it.
  CHECK(std::ranges::any_of(before.at(5), [](const auto &p_variable) {
    return p_variable.name == "total" && p_variable.line == 4;
  }));

  // A list takes `length of`; a whole number does not.
  const auto phrases_for = [&](const std::string &p_what) {
    return ovis::phrase::compile_phrase_program(
        {file}, nullptr,
        {.sources_written = false,
         .phrases_for = p_what,
         .phrases_for_path = "main.ovis",
         .phrases_for_line = 0});
  };
  const auto has = [](const ovis::phrase::program_result &p_result,
                      const std::string &p_pattern) {
    return std::ranges::any_of(p_result.phrases_taking, [&](const auto &p_item) {
      return ovis::phrase::manual_entry_of(p_item).pattern == p_pattern;
    });
  };
  const auto list = phrases_for("numbers");
  CHECK(list.phrases_for_type == "a list of whole numbers");
  CHECK(has(list, "length of LIST"));
  const auto number = phrases_for("count");
  CHECK(number.phrases_for_type == "a whole number");
  CHECK(!has(number, "length of LIST"));
  // A type in words works like a name.
  CHECK(has(phrases_for("text"), "TEXT in capitals"));
  CHECK(phrases_for("zzyzx").phrases_for_type.empty());
}

TEST_CASE("A phrase's # example: lines are its examples, not its sentence.", "[phrase][manual]") {
  ovis::phrase::phrase_file file{
      .path = "tax.ovis",
      .text = "# Adds the tax.\n"
              "# example: add tax to 50\n"
              "to add tax to (total):\n"
              "    give back total\n"
              "show add tax to 5\n"
              "show add tax to 6\n"};
  const auto entries = ovis::phrase::program_phrase_templates({file});
  REQUIRE(entries.size() == 1);
  CHECK(entries.front().about == "Adds the tax.");
  REQUIRE(entries.front().examples.size() == 2);
  CHECK(entries.front().examples.front() == "add tax to 50");
}

namespace {

auto c_entries_of(const std::string &p_program, const std::string &p_words)
    -> std::vector<ovis::phrase::manual_entry> {
  ovis::phrase::phrase_file file;
  file.path = "main.ovis";
  file.text = p_program;
  const auto more = ovis::phrase::c_header_templates(
      {file}, "main.ovis", test_support::test_c_imports());
  std::vector<std::string> words;
  std::istringstream split(p_words);
  for (std::string word; split >> word;)
    words.push_back(word);
  // The library's own phrases (`add 40 to xs`) rank among them: only C's.
  auto found = ovis::phrase::manual_search(words, more, 0);
  std::erase_if(found, [](const auto &p_entry) { return p_entry.topic != "ovistest"; });
  return found;
}

} // namespace

TEST_CASE("The manual has what a header imports: signatures, comments, uses.",
          "[phrase][manual][c-import]") {
  if (!ovis::core::has_c_import_support())
    SKIP("Built without libclang.");
  const auto program = "use \"ovistest.h\" from \"ovistest\"\nshow ovt_add(2, 3)\n"s;

  // A name split into words finds its function; the signature has the
  // parameters' names and Ovis's types, the comment is the header's, and the
  // program's own call is the example.
  const auto adding = c_entries_of(program, "add");
  REQUIRE(!adding.empty());
  CHECK(adding.front().pattern ==
        "ovt_add(a: a whole number, b: a whole number) gives a whole number");
  CHECK(adding.front().about == "Adds two numbers. From ovistest.h.");
  CHECK(adding.front().topic == "ovistest");
  REQUIRE(adding.front().examples.size() == 1);
  CHECK(adding.front().examples.front() == "show ovt_add(2, 3)   (main.ovis, line 2)");

  // Constants, structs and macros are entries too.
  CHECK(c_entries_of(program, "error").front().pattern == "OVT_ERROR = -1");
  const auto boxes = c_entries_of(program, "box");
  const auto box = std::ranges::find_if(boxes, [](const auto &p_entry) {
    return p_entry.pattern.starts_with("ovt_box has corner (");
  });
  REQUIRE(box != boxes.end());
  CHECK(box->about == "A C struct from ovistest.h.");
  CHECK(c_entries_of(program, "clamp").front().pattern ==
        "OVT_CLAMP(x, lo, hi)   a C macro, for any types");

  // A parameter's name finds the function too, and only a `use`d header is
  // read.
  CHECK(c_entries_of(program, "amount").front().pattern.starts_with("ovt_counter_add("));
  CHECK(ovis::phrase::c_header_templates({}, "main.ovis", test_support::test_c_imports())
            .empty());
  CHECK(c_entries_of("show 1\n"s, "add").empty());
}

TEST_CASE("The manual has zlib's and SQLite's entries.", "[phrase][manual][c-import]") {
  const auto has_header = [](const char *p_name) {
    for (const auto &directory : ovis::core::system_include_directories())
      if (std::filesystem::exists(directory / p_name))
        return true;
    return false;
  };
  if (!ovis::core::has_c_import_support() || !has_header("zlib.h") ||
      !has_header("sqlite3.h"))
    SKIP("Needs libclang and the zlib and SQLite development files.");
  ovis::phrase::phrase_file file;
  file.path = "main.ovis";
  file.text = "use \"zlib.h\" from \"z\"\nuse \"sqlite3.h\" from \"sqlite3\"\n";
  const auto more = ovis::phrase::c_header_templates({file}, "main.ovis",
                                                     test_support::test_c_imports());
  const auto has = [&](const std::string &p_word, const std::string &p_start) {
    auto found = ovis::phrase::manual_search({p_word}, more, 0);
    return std::ranges::any_of(found, [&](const auto &p_entry) {
      return p_entry.pattern.starts_with(p_start);
    });
  };
  // `deflateInit` is a macro and `sqlite3_open` a function with a comment.
  CHECK(has("deflate", "deflateInit("));
  CHECK(has("open", "sqlite3_open("));
  CHECK(has("ok", "Z_OK = 0"));
}

TEST_CASE("The manual is written as pages, one entry each, with its links checked.",
          "[phrase][manual]") {
  const auto directory = std::filesystem::temp_directory_path() / "ovis_manual_html_test";
  std::filesystem::remove_all(directory);
  auto every = ovis::phrase::manual_search({}, {}, 0);
  every.push_back({.topic = "tax", .package = "shop", .pattern = "add <tax> to TOTAL",
                   .aliases = {"tax on TOTAL"}, .about = "Adds the tax & more.",
                   .examples = {"add tax to 5"}});
  CHECK(ovis::phrase::write_manual_html(every, directory).empty());

  const auto read = [&](const char *p_name) {
    std::ifstream stream(directory / p_name);
    std::stringstream text;
    text << stream.rdbuf();
    return text.str();
  };
  const auto index = read("index.html");
  // Every entry is on the index, linked to its page.
  std::size_t links = 0;
  for (auto at = index.find("<li><a href="); at != std::string::npos;
       at = index.find("<li><a href=", at + 1))
    ++links;
  CHECK(links == every.size());
  CHECK(std::filesystem::exists(directory / "numbers.html"));
  const auto package = read("shop-tax.html");
  CHECK(package.find("Adds the tax &amp; more.") != std::string::npos);
  CHECK(package.find("add &lt;tax&gt; to TOTAL") != std::string::npos);
  CHECK(package.find("Also: <code>tax on TOTAL</code>") != std::string::npos);
  CHECK(package.find("<pre>add tax to 5\n</pre>") != std::string::npos);

  std::filesystem::remove_all(directory);
}

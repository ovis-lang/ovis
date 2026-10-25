#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <ranges>
#include <set>
#include <sstream>

#include <ovis/core/checker.hpp>
#include <ovis/core/c_import.hpp>
#include <ovis/core/elaborate.hpp>
#include <ovis/core/modules.hpp>
#include <ovis/core/monomorphise.hpp>
#include <ovis/core/sexpr.hpp>
#include <ovis/core/memory_check.hpp>
#include <ovis/core/warnings.hpp>
#include <ovis/phrase/front_end.hpp>
#include <ovis/phrase/lexer.hpp>
#include <ovis/phrase/templates.hpp>

#include "parser.hpp"
#include "questions.hpp"

namespace ovis::phrase {

namespace {

using namespace internal;

// One code line while it is being read: its text from its first token, so
// decisions can be put in and the line read again.
struct working_line {
  const source_line *source = nullptr;
  string_type text;
  // The text as written, before any decision: what a `@[meaning: …]` was
  // written for. Empty for a line over several physical lines, whose
  // meaning is kept only in the lockfile.
  string_type original;
  // The whole physical line from its first token (a one-line block's
  // header and body), where that is, and where this line starts in it: a
  // fix's meaning is that line with this one's text replaced.
  string_type physical;
  std::size_t start_column = 1;
  std::size_t offset = 0;
  std::vector<token> tokens;
  // `"SPAN" is READING`, in the order they apply.
  std::vector<string_type> decisions;
  // The decisions of a `@[meaning: …]` the lockfile doesn't know: trusted
  // as the author's, and checked to keep what the line names.
  std::vector<string_type> author_meanings;
  bool decisions_changed = false;
  // A note the build writes above the line (an `at the end`
  // whose block isn't a function or the top level), and whether the line's
  // notes are the compiler's to keep up to date.
  string_type compiler_note;
  bool writes_compiler_note = false;
};

auto span_text(const working_line &p_line, std::size_t p_begin,
               std::size_t p_end) -> string_type {
  const auto base = p_line.tokens.front().column;
  const auto from = p_line.tokens[p_begin].column - base;
  const auto to = p_line.tokens[p_end - 1].end_column - base;
  return p_line.text.substr(from, to - from);
}

// Puts a decision into the line: the span's text is replaced by the
// reading. False if the span isn't in the line (the line was edited).
auto apply_decision(working_line &p_line, string_view_type p_decision) -> bool {
  if (!p_decision.starts_with('"'))
    return false;
  const auto separator = p_decision.find("\" is ", 1);
  if (separator == string_view_type::npos)
    return false;
  const auto span = p_decision.substr(1, separator - 1);
  const auto reading = p_decision.substr(separator + 5);
  // The span as whole words: "it" isn't the end of "split".
  const auto is_word_character = [](char_type p_character) {
    return std::isalnum(static_cast<unsigned char>(p_character)) ||
           p_character == '_';
  };
  const auto &line = p_line.text;
  auto at = line.find(span);
  while (!span.empty() && at != string_type::npos &&
         ((at > 0 && is_word_character(span.front()) &&
           is_word_character(line[at - 1])) ||
          (at + span.size() < line.size() && is_word_character(span.back()) &&
           is_word_character(line[at + span.size()]))))
    at = line.find(span, at + 1);
  if (span.empty() || at == string_type::npos)
    return false;
  auto text = p_line.text;
  text.replace(at, span.size(), reading);
  try {
    p_line.tokens = tokenize(text, p_line.source->line);
  } catch (const lex_error &) {
    return false;
  }
  p_line.text = std::move(text);
  return true;
}

auto trim(string_view_type p_text) -> string_view_type {
  while (!p_text.empty() && std::isspace(static_cast<unsigned char>(p_text.front())))
    p_text.remove_prefix(1);
  while (!p_text.empty() && std::isspace(static_cast<unsigned char>(p_text.back())))
    p_text.remove_suffix(1);
  return p_text;
}

// The decision `"SPAN" is READING` that turns `p_line` into `p_meaning`:
// what lies between their common start and end, widened to whole words.
// Nothing if they are the same.
auto meaning_decision(string_view_type p_line, string_view_type p_meaning)
    -> std::optional<string_type> {
  if (p_line == p_meaning)
    return std::nullopt;
  std::size_t start = 0;
  while (start < p_line.size() && start < p_meaning.size() &&
         p_line[start] == p_meaning[start])
    ++start;
  std::size_t tail = 0;
  while (tail < p_line.size() - start && tail < p_meaning.size() - start &&
         p_line[p_line.size() - 1 - tail] == p_meaning[p_meaning.size() - 1 - tail])
    ++tail;
  while (start > 0 && p_line[start - 1] != ' ')
    --start;
  while (tail > 0 && p_line[p_line.size() - tail] != ' ')
    --tail;
  auto span = trim(p_line.substr(start, p_line.size() - tail - start));
  auto reading = trim(p_meaning.substr(start, p_meaning.size() - tail - start));
  if (span.empty() || reading.empty())
    return std::nullopt;
  return "\"" + string_type(span) + "\" is " + string_type(reading);
}

// Names in the line that are in scope, with their types, for the model.
auto known_names(const std::vector<token> &p_tokens,
                 const std::map<string_type, value_kind> &p_scope)
    -> string_type {
  string_type result;
  std::set<string_type> seen;
  for (const auto &item : p_tokens) {
    if (item.kind != token::kind_type::word)
      continue;
    const auto found = p_scope.find(item.text);
    if (found == p_scope.end() || !seen.insert(item.text).second)
      continue;
    result += (result.empty() ? "" : " ") + item.text + " is " +
              found->second.english() + ".";
  }
  return result;
}

// A `let` for a variable first set inside a block, before its value is
// known: declared with its type, or `?` for the elaborator to infer.
auto declaration(const string_type &p_name, const value_kind &p_type)
    -> string_type {
  // A pointer the parser knows no more of (`new room for 4 items`) is
  // left for the elaborator: it may be a typed one.
  const bool is_open = !p_type.known() ||
                       (p_type.base == "ptr" && p_type.depth == 0 && p_type.exact.empty());
  return "(let " + core_name(p_name) + " " +
         (is_open ? string_type("?") : p_type.core()) + ")";
}

// A phrase the program defines: `to pad (text) to (width):`.
struct user_function {
  // As called, `pad_to(...)`, and in Ovis Core (different only where the
  // name is taken by Ovis Core).
  string_type name;
  string_type core;
  std::vector<string_type> parameters;
  // Stated types, as in `(width: whole number)`.
  std::vector<std::optional<value_kind>> types;
  std::vector<phrase_template> templates;
  // Set when the phrase implements a built-in trait for a record, as in
  // `to (a: book) comes before (b: book):`.
  std::optional<trait_method> implements;
  // A method of a trait (`area of (it) gives a decimal`) or an impl's
  // version of one; `it`, its first parameter, is the value itself.
  bool is_method = false;
  std::optional<value_kind> result;
  // A generic phrase's type variables (`any comparable item`), in the order
  // they are first named, and their bounds as Ovis Core names them.
  std::vector<std::pair<string_type, std::vector<string_type>>> type_parameters;
  // Whether the trait gives it a body.
  bool has_body = false;
  // In a trait's phrase written without `(it)` (`(a) comes before (b)`),
  // the parameters after the first that are, like it, the value's own
  // record: an impl takes them as its record, and a call checks they are
  // the same record when the program runs.
  std::vector<bool> same;
};

// A trait method's line as the header of a `to` phrase, and the type it
// `gives` after its parameters, if it says one.
struct method_line {
  std::vector<token> header;
  std::optional<std::vector<token>> gives;
  // The line's words as the author wrote them, for reports.
  string_type text;
};

auto method_line_of(const std::vector<token> &p_tokens) -> method_line {
  method_line result;
  const auto has_colon = p_tokens.back().is_symbol(":");
  auto end = p_tokens.size() - (has_colon ? 1 : 0);
  auto gives = end;
  for (std::size_t index = 0; index < end; ++index)
    if (p_tokens[index].is_word("gives")) {
      gives = index;
      break;
    }
  if (gives < end) {
    if (gives + 1 == end)
      throw error("Say the type after 'gives', as in: area of (it) gives a "
                  "decimal.");
    // `gives a decimal`: the article is not part of the type.
    auto begin = gives + 1;
    if (begin + 1 < end && is_article(p_tokens[begin]))
      ++begin;
    result.gives = std::vector<token>(
        p_tokens.begin() + static_cast<std::ptrdiff_t>(begin),
        p_tokens.begin() + static_cast<std::ptrdiff_t>(end));
  }
  token to = p_tokens.front();
  to.kind = token::kind_type::word;
  to.text = "to";
  result.header.push_back(to);
  result.header.insert(result.header.end(), p_tokens.begin(),
                       p_tokens.begin() + static_cast<std::ptrdiff_t>(gives));
  token colon = p_tokens.back();
  colon.kind = token::kind_type::symbol;
  colon.text = ":";
  result.header.push_back(colon);
  return result;
}

// A type written in words after a parameter's ':': `whole number`,
// `list of text`.
auto stated_type(const std::vector<token> &p_tokens, std::size_t p_begin,
                 std::size_t p_end, const vocabulary &p_vocabulary)
    -> std::optional<value_kind> {
  if (p_end - p_begin > 2 && p_tokens[p_begin].is_word("list") &&
      p_tokens[p_begin + 1].is_word("of")) {
    auto element = stated_type(p_tokens, p_begin + 2, p_end, p_vocabulary);
    if (!element)
      return std::nullopt;
    auto list = element->with_depth(element->depth + 1);
    // A list of bytes, or of arrays of them, keeps their size.
    if (element->is_c_number() || element->exact.starts_with("(list "))
      list.exact = "(list " + element->exact + ")";
    return list;
  }
  for (const auto &[length, type] :
       type_words(p_tokens, p_begin, p_end, &p_vocabulary))
    if (p_begin + length == p_end)
      return type;
  return std::nullopt;
}

// A type variable as a template's: `ITEM` for `item`.
auto template_variable(string_view_type p_name) -> string_type {
  string_type result;
  for (const auto character : p_name)
    if (std::isalpha(static_cast<unsigned char>(character)))
      result.push_back(static_cast<char_type>(
          std::toupper(static_cast<unsigned char>(character))));
  return result;
}

// A parameter's type as a template slot's, with a generic record's types
// (`stack of item` is a stack of `ITEM`).
auto template_type_of(const value_kind &p_type) -> type_name {
  type_name result{.list_depth = p_type.depth};
  if (p_type.base == "var") {
    result.kind = type_name::kind_type::variable;
    result.name = template_variable(p_type.record);
    return result;
  }
  if (p_type.base == "record") {
    result.name = "any";
    result.record = p_type.record;
    for (const auto &argument : p_type.arguments)
      result.arguments.push_back(template_type_of(argument));
    if (std::ranges::all_of(result.arguments, [](const type_name &p_argument) {
          return p_argument.kind == type_name::kind_type::builtin &&
                 p_argument.name == "any";
        }))
      result.arguments.clear();
    return result;
  }
  result.name = p_type.known() && p_type.exact.empty() ? p_type.base : "any";
  if (result.name == "?")
    result.name = "any";
  return result;
}

// `any comparable item` in a header's types (generics.md §3): each
// introduces a type variable at its first mention, its bounds the traits
// between `any` and its name. Returns the variables, and the tokens with
// each `any …` left as the name alone, so the types read as later
// mentions do. Throws `error` saying what is wrong.
auto type_variables_of(const std::vector<token> &p_tokens,
                       const vocabulary &p_vocabulary)
    -> std::pair<std::vector<std::pair<string_type, std::vector<string_type>>>,
                 std::vector<token>> {
  std::vector<std::pair<string_type, std::vector<string_type>>> variables;
  std::vector<token> tokens;
  // Only in a parameter's type: after its `:`, before its `)`.
  bool in_type = false;
  for (std::size_t index = 0; index < p_tokens.size(); ++index) {
    if (p_tokens[index].is_symbol(":"))
      in_type = true;
    if (p_tokens[index].is_symbol(")"))
      in_type = false;
    if (!in_type || !p_tokens[index].is_word("any")) {
      tokens.push_back(p_tokens[index]);
      continue;
    }
    std::vector<string_type> bounds;
    auto at = index + 1;
    const auto is_bound = [&](std::size_t p_at) -> std::optional<std::pair<string_type, std::size_t>> {
      if (p_at >= p_tokens.size() || p_tokens[p_at].kind != token::kind_type::word)
        return std::nullopt;
      for (const auto *builtin : {"comparable", "equatable", "showable", "number"})
        if (p_tokens[p_at].is_word(builtin))
          return std::pair{string_type(builtin), std::size_t(1)};
      if (const auto [trait, length] =
              p_vocabulary.record_at(p_tokens, p_at, p_tokens.size());
          trait != nullptr && trait->is_trait)
        return std::pair{trait->name, length};
      return std::nullopt;
    };
    while (at < p_tokens.size()) {
      if (p_tokens[at].is_word("and") && !bounds.empty()) {
        ++at;
        continue;
      }
      const auto bound = is_bound(at);
      if (!bound)
        break;
      bounds.push_back(bound->first);
      at += bound->second;
    }
    string_type name;
    if (at < p_tokens.size() && p_tokens[at].kind == token::kind_type::word) {
      name = lower(p_tokens[at].text);
      ++at;
    } else if (!bounds.empty()) {
      // `any shape`: a value of some type that is one; it isn't named again.
      name = "some_" + bounds.back();
    } else {
      throw error("Name the type after 'any', as in (x: any item) or (xs: list "
                  "of any comparable thing).");
    }
    if (p_vocabulary.records.contains(name))
      throw error("'" + name + "' is a " +
                  (p_vocabulary.records.at(name).is_trait ? "trait" : "record") +
                  "; a type variable needs a name of its own.");
    if (std::ranges::find(variables, name, &std::pair<string_type,
                                                      std::vector<string_type>>::first) !=
        variables.end())
      throw error("'" + name + "' is named with 'any' twice: after the first, "
                  "write '" + name + "' alone.");
    variables.emplace_back(name, std::move(bounds));
    auto named = p_tokens[index];
    named.text = name;
    tokens.push_back(named);
    index = at - 1;
  }
  return {std::move(variables), std::move(tokens)};
}

// Reads `to WORDS (PARAMETER[: TYPE]) WORDS ... [as NAME]:` into a user
// function and its templates (a statement and an expression, so the phrase
// works both ways). Throws `error` saying what is wrong.
auto read_header(const std::vector<token> &p_written,
                 const vocabulary &p_vocabulary, bool p_method = false,
                 const std::optional<value_kind> &p_result = std::nullopt,
                 const std::optional<value_kind> &p_receiver = std::nullopt,
                 const std::map<string_type, value_kind> &p_inferred = {},
                 std::map<string_type, std::vector<string_type>> p_outer = {})
    -> user_function {
  user_function result;
  // Its type variables are types in its header (and its body): those of
  // the generic record an impl is for, and its own.
  auto [variables, p_tokens] = type_variables_of(p_written, p_vocabulary);
  result.type_parameters = std::move(variables);
  auto scope = std::move(p_outer);
  for (const auto &[name, bounds] : result.type_parameters)
    scope[name] = bounds;
  const auto saved = std::exchange(p_vocabulary.type_variables, scope);
  struct restore {
    const vocabulary &words;
    std::map<string_type, std::vector<string_type>> saved;
    ~restore() { words.type_variables = std::move(saved); }
  } restored{p_vocabulary, saved};
  result.is_method = p_method;
  result.result = p_result;
  auto end = p_tokens.size() - 1;
  std::optional<string_type> named;
  if (end >= 3 && p_tokens[end - 2].is_word("as") &&
      p_tokens[end - 1].kind == token::kind_type::word) {
    named = p_tokens[end - 1].text;
    end -= 2;
  }
  string_type pattern;
  std::vector<string_type> words;
  for (std::size_t index = 1; index < end; ++index) {
    const auto &item = p_tokens[index];
    if (item.kind == token::kind_type::word) {
      for (const auto character : item.text)
        if (!std::isalpha(static_cast<unsigned char>(character)))
          throw error("A phrase's words are letters only: '" + item.text +
                      "' isn't one.");
      pattern += " " + lower(item.text);
      if (!is_article(item))
        words.push_back(lower(item.text));
      continue;
    }
    if (item.is_symbol(",")) {
      pattern += " ,";
      continue;
    }
    if (!item.is_symbol("("))
      throw error("A phrase is made of words and (parameters): '" +
                  item.text + "' is neither.");
    auto close = index + 1;
    while (close < end && !p_tokens[close].is_symbol(")"))
      ++close;
    if (close == end || close == index + 1 ||
        p_tokens[index + 1].kind != token::kind_type::word ||
        (close > index + 2 && !p_tokens[index + 2].is_symbol(":")))
      throw error("Write a parameter as (name) or (name: type), for "
                  "example (width: whole number).");
    const auto &name = p_tokens[index + 1].text;
    if (std::ranges::find(result.parameters, name) != result.parameters.end())
      throw error("Two parameters are called '" + name + "'.");
    std::optional<value_kind> type;
    if (close > index + 2) {
      type = stated_type(p_tokens, index + 3, close, p_vocabulary);
      if (!type)
        throw error("'" + name + "' needs a type Ovis knows: a whole number, "
                    "a decimal, text, a truth value, a record or a list of "
                    "one.");
    }
    // In `a book is comparable:` every parameter is a book.
    if (!type && p_receiver)
      type = p_receiver;
    result.parameters.push_back(name);
    result.types.push_back(type);
    // Templates have no record types: a record is `any` to the type filter.
    // A parameter the body does arithmetic on is a number, whatever the
    // caller names it, so the phrase isn't offered for text or lists.
    const auto inferred = p_inferred.find(name);
    // The parser's kind: a typed pointer is `ptr`, an array a list.
    auto coarse = type;
    if (coarse)
      coarse->exact.clear();
    const auto slot_type =
        !type ? (inferred != p_inferred.end() ? inferred->second.core()
                                              : string_type("any"))
        // A type variable is a template's (`ITEM`): the same type in each
        // slot it is in, whatever its bounds (Ovis Core checks those).
        : type->base == "var"
            ? template_variable(type->record) +
                  [&] {
                    string_type lists;
                    for (std::size_t level = 0; level < type->depth; ++level)
                      lists += "[]";
                    return lists;
                  }()
        : type->base == "record"
            ? "any" + string_type(type->depth > 0 ? "[]" : "")
            : coarse->core();
    pattern += " {" + name + ": " + slot_type + "}";
    index = close;
  }
  if (words.empty())
    throw error("A phrase needs at least one word besides its parameters.");
  if (p_method) {
    auto receiver = std::ranges::find(result.parameters, "it");
    result.same.assign(result.parameters.size(), false);
    if (receiver == result.parameters.end()) {
      // `(a) comes before (b)`: every parameter without a type is the
      // value's record, and the first picks the impl.
      for (std::size_t index = 0; index < result.parameters.size(); ++index)
        if (!result.types[index]) {
          if (receiver == result.parameters.end())
            receiver = result.parameters.begin() +
                       static_cast<std::ptrdiff_t>(index);
          else
            result.same[index] = true;
        }
      if (receiver == result.parameters.end())
        throw error("A trait's phrase needs the value itself: a parameter "
                    "called 'it', as in area of (it), or parameters without "
                    "a type, as in (a) comes before (b).");
    }
    const auto at = receiver - result.parameters.begin();
    if (result.types[static_cast<std::size_t>(at)])
      throw error("Leave out the type of 'it': it is the record that "
                  "implements the trait.");
    std::rotate(result.same.begin(), result.same.begin() + at,
                result.same.begin() + at + 1);
    // It is the first parameter in Ovis Core, wherever the phrase has it.
    std::rotate(result.parameters.begin(), receiver, receiver + 1);
    std::rotate(result.types.begin(), result.types.begin() + at,
                result.types.begin() + at + 1);
  }
  if (named) {
    result.name = *named;
  } else {
    for (const auto &word : words)
      result.name += (result.name.empty() ? "" : "_") + word;
  }
  // A name Ovis Core already gives a meaning would call that instead. A
  // package's names end in its own ending (vocabulary::core_suffix).
  result.core = core_name(result.name) + p_vocabulary.core_suffix;
  // A phrase that makes a record comparable, equatable or showable is not
  // a phrase of its own: `<`, `=`, `sort`, `show` ... call it, and each
  // record has its own function.
  if (!named && !p_method)
    if (auto method = trait_method_of(result.name, result.types)) {
      result.core = result.core + "__" + method->record;
      result.name = result.core;
      result.implements = std::move(method);
      return result;
    }

  string_type call = "(" + result.core;
  for (const auto &parameter : result.parameters)
    call += " $" + parameter;
  call += ")";
  try {
    // The parser's kind: a number of a size is a whole number or a decimal.
    auto coarse = p_result;
    if (coarse && coarse->is_c_number())
      coarse->exact.clear();
    const auto result_type =
        !coarse || coarse->base == "record" || coarse->base == "void"
            ? string_type("any")
            : coarse->core();
    result.templates = read_templates("expr" + pattern + " : " + result_type +
                                          " => " + call +
                                          "\nstmt" + pattern + " => " + call +
                                          "\n",
                                      "");
    for (auto &each : result.templates)
      each.package = p_vocabulary.package;
    // A generic record of type variables (`stack of item`): the record,
    // and its types as the template's variables.
    for (auto &each : result.templates)
      for (auto &slot : each.slots) {
        const auto at = std::ranges::find(result.parameters, slot.name);
        if (at == result.parameters.end())
          continue;
        const auto &type = result.types[static_cast<std::size_t>(at - result.parameters.begin())];
        if (type && type->base == "record" && !type->arguments.empty())
          slot.type = template_type_of(*type);
      }
  } catch (const error &p_error) {
    // "LINE: message" from the template reader.
    const string_type message = p_error.what();
    const auto colon = message.find(": ");
    throw error(colon == string_type::npos ? message : message.substr(colon + 2));
  }
  return result;
}

// A `to …:` header's words with `_` for each parameter, articles left out:
// how the phrase is named as a value (`compare _ and _`).
auto phrase_value_words(const std::vector<token> &p_header) -> string_type {
  string_type result;
  for (std::size_t index = 1; index < p_header.size(); ++index) {
    const auto &item = p_header[index];
    if (item.is_word("as") || item.is_symbol(":"))
      break;
    if (item.is_symbol("(")) {
      while (index < p_header.size() && !p_header[index].is_symbol(")"))
        ++index;
      result += result.empty() ? "_" : " _";
      continue;
    }
    if (item.kind == token::kind_type::word && !is_article(item))
      result += (result.empty() ? "" : " ") + lower(item.text);
  }
  return result;
}

// Ovis Core is written with a comment line naming the source line each
// statement comes from, so problems the checker finds point at that line.
} // namespace

auto package_core_suffix(string_view_type p_package) -> string_type {
  if (p_package.empty())
    return {};
  string_type suffix = "__p_";
  for (const auto character : p_package)
    suffix.push_back(std::isalnum(static_cast<unsigned char>(character)) ? character : '_');
  return suffix;
}

namespace {

auto line_marker(std::size_t p_line) -> string_type {
  return "\n;@" + std::to_string(p_line) + "\n";
}

// For each line of a unit's Ovis Core, the source line it comes from.
auto source_lines(string_view_type p_core, std::size_t p_first)
    -> std::vector<std::size_t> {
  std::vector<std::size_t> result = {p_first};
  auto current = p_first;
  for (std::size_t begin = 0; begin <= p_core.size();) {
    auto end = p_core.find('\n', begin);
    if (end == string_view_type::npos)
      end = p_core.size();
    const auto text = p_core.substr(begin, end - begin);
    if (text.starts_with(";@"))
      current = std::stoul(string_type(text.substr(2)));
    result.push_back(current);
    begin = end + 1;
  }
  return result;
}

// Reads the program line by line, keeping the scope, and lowers it to Ovis
// Core: one unit per top-level line.
// What the files of a program share while they are read.
struct program_context {
  english::language_model *model;
  const front_end_options &options;
  front_end_timing timing;
  question_asker asker;
  vocabulary words;
  // Top-level variables of the files read so far, visible to the next
  // files of the same package (a package's values are its own).
  std::map<string_type, std::map<string_type, value_kind>> globals;
  // The templates lines were read with, as "lists.phr:12".
  std::set<string_type> templates_used;
  // The program's phrases by their Ovis Core name, as their headers say
  // them, for reports.
  std::map<string_type, string_type> phrase_names;
  // Phrase names, with where each is defined ("in tax.ovis on line 2").
  std::map<string_type, string_type> defined;
  // Records' names, the same way (traits share them: both are types).
  std::map<string_type, string_type> records_defined;
  // A trait's phrases: trait → Ovis Core name of each → its header.
  std::map<string_type, std::map<string_type, user_function>> trait_methods;
  // `is a thing` in `a shape can:`: shape → thing.
  std::map<string_type, std::set<string_type>> trait_requires;

  // The phrase files read at run time (`phrases/` of the program).
  phrase_library extra;

  program_context(english::language_model *p_model,
                  const front_end_options &p_options,
                  phrase_library p_extra = {})
      : model(p_model), options(p_options), asker(p_model, p_options, timing),
        extra(std::move(p_extra)) {
    words.templates = builtin_templates();
    std::ranges::copy(extra.templates, std::back_inserter(words.templates));
    add_libc(words);
  }
};

class program_reader {
  std::vector<source_line> lines;
  std::vector<working_line> working;
  program_context &shared;
  english::language_model *model;
  const front_end_options &options;
  // The file's lockfile from the last build.
  const lockfile &previous;
  // For this file: its problems, source and lockfile.
  front_end_result &result;
  front_end_timing &timing;
  question_asker &asker;
  vocabulary &words;
  // How reports name the file: "pricing/tax.ovis", and as the start of a
  // report, "pricing/tax.ovis, "; both empty for a single file.
  string_type file_name;
  // A dependency's file: never asked about (phrase_file::locked).
  bool locked_file = false;
  // Its package's id ("" for the program's own files), what its lines may
  // use (phrase_file::sees), and its Ovis Core names' ending.
  string_type package;
  std::set<string_type> visible;
  bool scoped = false;
  // phrase_file::aliases, the other way round (id → name), and the words
  // of phrases two packages it sees both have.
  std::map<string_type, string_type> aliases;
  std::map<string_type, string_type> qualifiers;
  std::set<string_type> shared_words;
  // A line that said whose phrases it reads with: those alone.
  std::set<string_type> narrowed;
  string_type core_suffix;
  string_type package_label;

  // Points the shared vocabulary at this file's packages before it reads.
  auto enter() const -> void {
    words.visible = scoped ? &visible : nullptr;
    words.qualifiers = scoped ? &qualifiers : nullptr;
    words.shared_words = scoped ? &shared_words : nullptr;
    // The program's own package is `.` to the vocabulary (its internal/
    // folders `.#pricing/internal`).
    words.package = !scoped                   ? string_type()
                    : package.empty()         ? string_type(".")
                    : package.starts_with('#') ? "." + package
                                              : package;
    words.core_suffix = core_suffix;
  }

  // The Ovis Core name of a record or trait this file declares.
  auto own_record(const string_type &p_name) const -> string_type {
    return p_name + core_suffix;
  }

  // Definitions are kept apart per package: two may each have a `greet`.
  auto defined_key(const string_type &p_name) const -> string_type {
    return whole_package() + "\n" + p_name;
  }

  // The package, its internal/ files included (`log@1#internal` is log@1,
  // the program's `#pricing/internal` the program).
  auto whole_package() const -> string_type {
    return package.substr(0, package.find('#'));
  }

  // Whether this file's package declares the record or trait. Without
  // packages everything is the program's own.
  auto owns(const record_info &p_type) const -> bool {
    const auto mine = words.package.substr(0, words.package.find('#'));
    return !scoped || p_type.package.substr(0, p_type.package.find('#')) == mine;
  }

  // The orphan rule (packages.md §4): a package gives a type a trait only
  // if it declares one of them, so two packages can't both, and adding a
  // dependency can't break an impl.
  auto orphan(const string_type &p_what) const -> string_type {
    return "This gives " + p_what + " a trait, but neither is declared in " +
           (whole_package().empty() ? string_type("this program") : whole_package()) +
           ": a package may only give its own types a trait, or its own trait to a "
           "type (packages.md §4). Wrap the value in a record of your own.";
  }
  // How progress names the file: its report name, else its file name.
  string_type shown_name;
  string_type file_prefix;
  std::map<string_type, value_kind> scope;
  // Variables of other files: assigning one makes this file's own.
  std::set<string_type> foreign;
  // C headers this file uses, with their hashes.
  std::map<string_type, string_type> headers;
  // `let`s for variables first set inside a block, put before the current
  // top-level line so the variable lives on after the block.
  std::vector<string_type> hoisted;
  std::size_t counters = 0;
  // The user functions, by the line of their header.
  std::map<std::size_t, user_function> functions;
  // While reading a function's body: its depth, and its parameters and
  // locals (an assignment to any other name makes a local, as in Python).
  bool in_function = false;
  // When each name was last set, for `it` and left-out arguments.
  std::map<string_type, std::size_t> set_at;
  std::size_t sets = 0;

public:
  // The names in scope before each line (`variables_recorded`).
  std::map<std::size_t, std::vector<scope_variable>> variables_here;

private:
  std::map<string_type, std::size_t> first_set_line;
  std::size_t previous_line = 0;

public:
  // The phrases with a slot for `options.phrases_for`, and what that is.
  std::vector<phrase_template> phrases_found;
  string_type phrases_found_type;
  std::filesystem::path source_path;

private:
  // `options.phrases_for`: a name in scope now, else a type in words.
  auto find_phrases_for() -> void {
    std::optional<value_kind> kind;
    if (const auto found = scope.find(options.phrases_for);
        found != scope.end() && found->second.known())
      kind = found->second;
    else {
      const auto tokens = tokenize(options.phrases_for);
      for (const auto &[length, type] : type_words(tokens, 0, tokens.size(), &words))
        if (length == tokens.size())
          kind = type;
    }
    if (!kind)
      return;
    phrases_found_type = kind->english();
    for (const auto *item : phrases_taking(words, *kind))
      phrases_found.push_back(*item);
  }

  auto here_for_phrases(std::size_t p_line) const -> bool {
    return !options.phrases_for.empty() && options.phrases_for_line == p_line &&
           !options.phrases_for_path.empty() &&
           source_path.lexically_normal() == options.phrases_for_path.lexically_normal();
  }

  // Called before a statement is read: what is in scope now, and which names
  // the statement before it brought in.
  auto record_scope(std::size_t p_line) -> void {
    std::vector<scope_variable> here;
    for (const auto &[name, type] : scope) {
      first_set_line.try_emplace(name, previous_line);
      here.push_back({name, type.known() ? type.english() : string_type(),
                      first_set_line[name]});
    }
    variables_here[p_line] = std::move(here);
    previous_line = p_line;
    if (here_for_phrases(p_line))
      find_phrases_for();
  }
  std::set<string_type> template_words;
  bool model_used = false;
  std::size_t base_depth = 0;
  // The depth of the block under the innermost `at the end:` being read, or
  // 0: names first set at that depth stay in it (nothing runs after it in
  // that block to use them), not hoisted out. A `do:` block is the same.
  std::size_t defer_body_depth = 0;
  // The `do:` blocks being read, innermost last: each one's label, the
  // variable it gives a value to (if any) and the type of the first value
  // a `leave with` gives.
  struct do_block_info {
    string_type label;
    string_type gives;
    value_kind type;
  };
  std::vector<do_block_info> do_blocks;
  std::set<string_type> locals;
  // The lines that declare records (`a book has:`), and each record's Ovis
  // Core by the line it is declared on.
  std::vector<std::size_t> record_lines;
  // `a whole number is hashable:`: the line, and its type in Ovis Core.
  std::map<std::size_t, string_type> plain_impls;
  std::map<std::size_t, string_type> record_forms;
  // The lines that declare traits (`a shape can:`), and the Ovis Core of the
  // phrases they require, by the line each is on.
  std::vector<std::size_t> trait_lines;
  std::map<std::size_t, string_type> trait_needs;
  // A requirement line's built-in phrases (`is comparable and showable`),
  // after the first, which is in `trait_needs`.
  std::multimap<std::size_t, string_type> more_trait_needs;

  auto trait_needs_of(std::size_t p_line) const -> std::vector<string_type> {
    std::vector<string_type> result;
    if (const auto first = trait_needs.find(p_line); first != trait_needs.end())
      result.push_back(first->second);
    const auto [begin, end] = more_trait_needs.equal_range(p_line);
    for (auto at = begin; at != end; ++at)
      result.push_back(at->second);
    return result;
  }
  // `is a thing` in a trait: the traits it requires, by line.
  std::map<std::size_t, std::vector<string_type>> trait_requires;

  // "in tax.ovis on line 2", for reports about a second definition.
  auto where(const source_line &p_line) const -> string_type {
    return (file_name.empty() ? "" : "in " + file_name + " ") + "on line " +
           std::to_string(p_line.line);
  }

  // A field's line: `title`, `title (text)`, `price (decimal, 0)` or
  // `price (0)`. Throws `error` saying what is wrong.
  auto read_field(const std::vector<token> &p_tokens) -> record_field {
    const auto malformed = [] {
      return error("Write a field as its name, then in brackets its type, a "
                   "default, or both: price (decimal, 0).");
    };
    if (p_tokens.empty() || p_tokens[0].kind != token::kind_type::word)
      throw malformed();
    // Its name is the words before the brackets: `number of wheels`.
    std::size_t open = 0;
    std::vector<string_type> name;
    while (open < p_tokens.size() &&
           p_tokens[open].kind == token::kind_type::word)
      name.push_back(p_tokens[open++].text);
    auto field = field_named(name);
    if (open == p_tokens.size())
      return field;
    if (p_tokens.size() < open + 3 || !p_tokens[open].is_symbol("(") ||
        !p_tokens.back().is_symbol(")"))
      throw malformed();
    const auto end = p_tokens.size() - 1;
    auto comma = end;
    int level = 0;
    for (std::size_t index = open + 1; index < end && comma == end; ++index) {
      const auto &item = p_tokens[index];
      level += item.is_symbol("(") || item.is_symbol("[");
      level -= item.is_symbol(")") || item.is_symbol("]");
      if (level == 0 && item.is_symbol(","))
        comma = index;
    }
    auto value_begin = open + 1;
    if (comma < end) {
      const auto type = stated_type(p_tokens, open + 1, comma, words);
      if (!type)
        throw error("'" + field.spoken() + "' needs a type Ovis knows: a whole "
                    "number, a decimal, text, a truth value, a record or a "
                    "list of one.");
      field.type = *type;
      value_begin = comma + 1;
    } else if (const auto type = stated_type(p_tokens, open + 1, end, words)) {
      field.type = *type;
      return field;
    }
    // The default, read on its own: it may use no variables.
    const std::vector<token> value(p_tokens.begin() +
                                       static_cast<std::ptrdiff_t>(value_begin),
                                   p_tokens.begin() +
                                       static_cast<std::ptrdiff_t>(end));
    if (value.empty())
      throw malformed();
    const std::map<string_type, value_kind> no_names;
    line_parser parser(value, no_names, words, false);
    const auto &readings = parser.expressions(0, value.size());
    if (readings.size() != 1)
      throw error(readings.empty()
                      ? "The default of '" + field.spoken() +
                            "' can't be read: write a type or a value such as "
                            "0 or \"none\"."
                      : "The default of '" + field.spoken() +
                            "' reads several ways; put brackets around it.");
    if (!assignable(field.type, readings[0].type))
      throw error("The default of '" + field.spoken() + "' is " +
                  readings[0].type.english() + ", but '" + field.spoken() +
                  "' is " + field.type.english() + ".");
    if (!field.type.known())
      field.type = readings[0].type;
    field.default_core = readings[0].core;
    return field;
  }

  // Problems are diagnostics until the source is written: their text is
  // made then, with the line numbers of the file as written.
  auto problem(const source_line &p_line, const string_type &p_message)
      -> void {
    result.diagnostics.push_back({.line = p_line.line, .message = p_message});
  }

  // The problem just reported is about the words `p_begin`..`p_end` of the
  // line: where they are, and what they may read as.
  auto describe_problem(const working_line &p_line, std::size_t p_begin,
                        std::size_t p_end, string_view_type p_kind,
                        const std::vector<string_type> &p_readings) -> void {
    auto &last = result.diagnostics.back();
    last.kind = string_type(p_kind);
    last.span = span_text(p_line, p_begin, p_end);
    if (const auto at = p_line.physical.find(last.span);
        !last.span.empty() && at != string_type::npos) {
      last.column = p_line.start_column + at;
      last.end_column = last.column + last.span.size();
    }
    for (const auto &reading : p_readings) {
      auto trial = p_line;
      if (p_line.original.empty() ||
          !apply_decision(trial, "\"" + last.span + "\" is " + reading))
        continue;
      last.fixes.push_back(
          {reading, p_line.physical.substr(0, p_line.offset) + trial.text +
                        p_line.physical.substr(std::min(
                            p_line.physical.size(),
                            p_line.offset + p_line.original.size()))});
    }
  }

  // Every word of every phrase, in lower case.
  auto phrase_words() -> const std::set<string_type> & {
    if (template_words.empty())
      for (const auto &item : words.templates)
        for (const auto &pattern : item.patterns)
          for (const auto &element : pattern) {
            template_words.insert(element.word);
            for (const auto &branch : element.branches)
              for (const auto &part : branch)
                template_words.insert(part.word);
          }
    return template_words;
  }

  // Names in scope, the most recently set first.
  auto recent() const -> std::vector<string_type> {
    std::vector<string_type> names;
    for (const auto &[name, type] : scope)
      names.push_back(name);
    std::ranges::stable_sort(names, [&](const string_type &p_left,
                                        const string_type &p_right) {
      const auto left = set_at.find(p_left);
      const auto right = set_at.find(p_right);
      return (left == set_at.end() ? 0 : left->second) >
             (right == set_at.end() ? 0 : right->second);
    });
    return names;
  }

  auto remember(const string_type &p_name) -> void { set_at[p_name] = ++sets; }

  auto nothing_called(const string_type &p_word) const -> string_type {
    std::vector<string_type> names;
    for (const auto &[name, type] : scope)
      names.push_back(name);
    const auto closest = core::closest_name(p_word, names);
    return "Nothing called '" + p_word + "' is known here" +
           (closest ? "; did you mean '" + *closest + "'?" : ".");
  }

  // A record used where a phrase needs it comparable or equatable, without
  // the author having written how: say what to write instead of offering
  // unrelated readings. Empty if that is not why the line is unclear.
  auto missing_phrase_hint(const working_line &p_line,
                           const line_shape &p_shape) const -> string_type {
    // `divide x by 2` of a whole number: the quotient is a decimal, which x
    // cannot hold.
    if (p_line.tokens.size() > 3 && p_line.tokens.front().is_word("divide")) {
      for (std::size_t by = 2; by + 1 < p_line.tokens.size(); ++by) {
        if (!p_line.tokens[by].is_word("by"))
          continue;
        string_type target;
        for (std::size_t index = 1; index < by; ++index)
          target += (target.empty() ? "" : " ") + p_line.tokens[index].text;
        string_type divisor;
        for (auto index = by + 1; index < p_line.tokens.size(); ++index)
          divisor += (divisor.empty() ? "" : " ") + p_line.tokens[index].text;
        if (const auto found = scope.find(target);
            found != scope.end() && found->second.is_number() &&
            found->second.base == "int" && found->second.depth == 0)
          return "\"" + span_text(p_line, p_shape.begin, p_shape.end) +
                 "\" would make " + target + " a decimal (dividing gives "
                 "one), but " + target + " is a whole number. Make " + target +
                 " a decimal (" + target + " = " + target + " as a decimal), "
                 "or keep the whole part: " + target + " = (" + target +
                 " divided by " + divisor + ") rounded down.";
      }
    }
    auto assuming = words;
    assuming.assume_record_traits = true;
    line_parser parser(p_line.tokens, scope, assuming, false);
    const auto readings = readings_of(parser, p_shape);
    if (readings.empty())
      return {};
    // The records the line mentions, by variable or by name.
    std::set<string_type> mentioned;
    for (std::size_t index = 0; index < p_line.tokens.size(); ++index) {
      const auto &item = p_line.tokens[index];
      if (const auto found = scope.find(item.text);
          item.kind == token::kind_type::word && found != scope.end() &&
          found->second.base == "record")
        mentioned.insert(found->second.record);
      else if (const auto [record, length] =
                   words.record_at(p_line.tokens, index, p_line.tokens.size());
               record != nullptr)
        mentioned.insert(record->name);
    }
    for (const auto *used : readings.front().uses)
      for (const auto &slot : used->slots) {
        if (!is_trait_slot(slot.type))
          continue;
        const auto trait = slot.type.kind == type_name::kind_type::variable
                               ? slot.type.bound
                               : slot.type.name;
        if (trait != "comparable" && trait != "equatable")
          continue;
        for (const auto &name : mentioned) {
          const auto record = words.records.at(name).type();
          if (words.records.at(name).is_trait || words.implements(trait, record))
            continue;
          const auto article = record.english();
          string_type spelled;
          for (const auto &word : words.records.at(name).words)
            spelled += (spelled.empty() ? "" : " ") + word;
          const auto phrase = trait == "comparable"
                                  ? "comes before"
                                  : "is the same as";
          return "\"" + span_text(p_line, p_shape.begin, p_shape.end) +
                 "\" needs " + article + " to be " +
                 (trait == "comparable" ? "comparable" : "equatable") +
                 ": write `to (a: " + spelled + ") " + phrase + " (b: " +
                 spelled + "):` and give back true " +
                 (trait == "comparable" ? "when a comes first."
                                        : "when a and b are the same.");
        }
      }
    return {};
  }

  // For an `at the end` that uses a name some later line sets.
  auto set_later_hint(const string_type &p_word, const line_shape &p_shape,
                      std::size_t p_line) const -> string_type {
    if (p_shape.kind != line_shape::kind_type::defer_line)
      return {};
    for (const auto &line : working) {
      const auto shape = shape_of(line.tokens);
      if (shape.kind == line_shape::kind_type::assign && shape.name == p_word &&
          line.source->line > p_line)
        return " '" + p_word + "' is set only after this line; move 'at the "
               "end' below it.";
    }
    return {};
  }

  // Puts readings a small edit gives first: they keep the line's own
  // values and structure, which candidates built from its words alone can't
  // (`list of pant` is `empty list of pant`). They never make the ranking
  // shortcut: the model, or the author, confirms them.
  static auto with_edits(question &p_question, std::vector<candidate> p_edits)
      -> void {
    if (p_edits.empty())
      return;
    double top = 0;
    for (const auto &item : p_question.candidates)
      top = std::max(top, item.score);
    auto &candidates = p_question.candidates;
    std::vector<candidate> merged;
    std::vector<candidate> last;
    for (auto &item : p_edits) {
      // One that leaves only the line's values isn't put first.
      if (item.score <= -10) {
        if (std::ranges::find(candidates, item.reading, &candidate::reading) ==
            candidates.end())
          last.push_back(std::move(item));
        continue;
      }
      // Offered already: it moves up, its words' likeness breaking ties.
      const auto found = std::ranges::find(candidates, item.reading,
                                           &candidate::reading);
      const auto likeness = found == candidates.end() ? 0 : found->score;
      if (found != candidates.end()) {
        item.covers_span = found->covers_span;
        item.added_words = found->added_words;
        candidates.erase(found);
      }
      item.score = top + 1 + item.score + 0.01 * likeness;
      merged.push_back(std::move(item));
    }
    std::ranges::stable_sort(merged, std::ranges::greater{}, &candidate::score);
    std::ranges::move(candidates, std::back_inserter(merged));
    std::ranges::move(last, std::back_inserter(merged));
    candidates = std::move(merged);
  }

  // What is unclear about a line that has `p_readings` (none or several),
  // with its candidates and the answer of a deterministic rule, if any.
  auto question_of(const working_line &p_line, const line_shape &p_shape,
                   const std::vector<parse> &p_readings) -> question {
    question result{.begin = p_shape.begin, .end = p_shape.end};
    if (p_readings.size() > 1) {
      result.kind = question_kind::ambiguous_parse;
      for (const auto &reading : p_readings)
        result.candidates.push_back({reading.reading});
      return result;
    }
    // Find the unclear part: the shortest span nothing reads, in a reading
    // of the rest.
    bool statement = p_shape.is_statement();
    line_parser finder(p_line.tokens, scope, words, true);
    // A hole where a field's name goes wins over the same hole read
    // otherwise: its candidates are the record's fields.
    string_type field_record;
    for (const auto &reading : readings_of(finder, p_shape)) {
      if (reading.holes != 1)
        continue;
      const auto size = reading.hole_end - reading.hole_begin;
      if (size < result.end - result.begin ||
          (size == result.end - result.begin &&
           reading.hole_begin == result.begin && field_record.empty() &&
           !reading.hole_record.empty())) {
        result.begin = reading.hole_begin;
        result.end = reading.hole_end;
        field_record = reading.hole_record;
        statement = false;
      }
    }
    // Readings must fit where the span is: the line has to read with the
    // span replaced (a number where `rounded to 2 decimals` needs one).
    const auto fits_at = [&](std::size_t p_begin, std::size_t p_end,
                             const string_type &p_reading) {
      auto trial = p_line;
      if (!apply_decision(trial, "\"" + span_text(p_line, p_begin, p_end) +
                                     "\" is " + p_reading))
        return false;
      const auto shape = shape_of(trial.tokens);
      line_parser parser(trial.tokens, scope, words, false);
      return shape.problem.empty() && !readings_of(parser, shape).empty();
    };
    if (!field_record.empty()) {
      std::vector<string_type> names;
      for (const auto &field : words.records.at(field_record).fields)
        names.push_back(field.spoken());
      result.kind = question_kind::unknown_name;
      result.record = field_record;
      result.candidates = word_candidates(p_line.tokens, result.begin, p_shape,
                                          scope, names, words);
      names.clear();
      for (const auto &item : result.candidates)
        names.push_back(item.reading);
      if (const auto closest =
              core::closest_name(p_line.tokens[result.begin].text, names))
        result.rule = static_cast<std::size_t>(
            std::ranges::find(names, *closest) - names.begin());
      return result;
    }
    // A record's name where a value goes, `the book`: one of the variables
    // holding that record.
    if (const auto [record, length] =
            words.record_at(p_line.tokens, result.begin, result.end);
        record != nullptr && length == result.end - result.begin) {
      if (result.begin > p_shape.begin &&
          is_article(p_line.tokens[result.begin - 1]))
        --result.begin;
      result.kind = question_kind::reference;
      result.record = record->name;
      for (const auto &name : recent())
        if (scope.at(name) == record->type() &&
            fits_at(result.begin, result.end, name))
          result.candidates.push_back({name});
      if (result.candidates.size() == 1)
        result.rule = 0;
      return result;
    }
    // Several words are unclear: a misspelt name among them is fixed first
    // (`show totl + nmbr`), by the closest name in scope, whatever its type;
    // the next round checks it.
    if (result.end - result.begin > 1)
      for (auto index = result.begin; index < result.end; ++index) {
        const auto &item = p_line.tokens[index];
        if (item.kind != token::kind_type::word || scope.contains(item.text) ||
            phrase_words().contains(lower(item.text)))
          continue;
        const auto names = recent();
        if (const auto closest = core::closest_name(item.text, names))
          return {.kind = question_kind::unknown_name,
                  .begin = index,
                  .end = index + 1,
                  .candidates = {{*closest}},
                  .rule = 0};
      }
    const auto &first = p_line.tokens[result.begin];
    if (result.end - result.begin == 1 && first.kind == token::kind_type::word) {
      result.kind = is_pronoun(first.text) ? question_kind::reference
                                           : question_kind::unknown_name;
      result.candidates = name_candidates(p_line.tokens, result.begin, p_shape,
                                          scope, recent(), words);
      if (result.kind == question_kind::reference) {
        if (result.candidates.size() == 1)
          result.rule = 0;
      } else {
        std::vector<string_type> names;
        for (const auto &item : result.candidates)
          names.push_back(item.reading);
        if (const auto closest = core::closest_name(first.text, names))
          result.rule = static_cast<std::size_t>(
              std::ranges::find(names, *closest) - names.begin());
      }
      return result;
    }
    // Making a record with a word its fields lack (`a person called "W" and
    // aged 12`): the readable start is the record's name and a word or two,
    // so the span widens back to the record, whose `a NAME with …` reading
    // then fits.
    if (!statement)
      for (auto start = result.begin; start-- > p_shape.begin &&
                                      result.begin - start <= 6;) {
        const auto [record, length] =
            words.record_at(p_line.tokens, start, result.begin);
        if (record == nullptr || record->is_trait || start + length > result.begin ||
            result.begin - (start + length) > 2 ||
            std::any_of(p_line.tokens.begin() +
                            static_cast<std::ptrdiff_t>(start + length),
                        p_line.tokens.begin() +
                            static_cast<std::ptrdiff_t>(result.begin),
                        [](const token &p_token) {
                          return p_token.kind != token::kind_type::word;
                        }))
          continue;
        if (start > p_shape.begin && is_article(p_line.tokens[start - 1]))
          --start;
        result.begin = start;
        break;
      }
    const auto fits = [&](const string_type &p_reading) {
      return fits_at(result.begin, result.end, p_reading);
    };
    result.candidates = unknown_candidates(p_line.tokens, result.begin,
                                           result.end, statement,
                                           p_shape.takes_block, scope,
                                           recent(), words, fits);
    with_edits(result,
               edit_candidates(p_line.tokens, result.begin, result.end,
                               result.begin == p_shape.begin &&
                                   result.end == p_shape.end,
                               statement, p_shape.takes_block, scope, words,
                               fits));
    return result;
  }

  // When no reading offered was taken: the model writes the line's whole
  // span again in words the compiler reads, and the rewrite is kept only if
  // the line then reads and nothing named in the span is lost. The decision
  // to apply, or nothing.
  auto rewritten(const working_line &p_line, const line_shape &p_shape)
      -> std::optional<string_type> {
    if (!options.rewrite || p_shape.begin >= p_shape.end)
      return std::nullopt;
    const auto span = span_text(p_line, p_shape.begin, p_shape.end);
    const std::vector<token> span_tokens(
        p_line.tokens.begin() + static_cast<std::ptrdiff_t>(p_shape.begin),
        p_line.tokens.begin() + static_cast<std::ptrdiff_t>(p_shape.end));
    if (!has_anchor(span_tokens, scope, words))
      return std::nullopt;
    // Why the compiler can't use a rewrite, or empty.
    const auto refusal = [&](const string_type &p_reading) -> string_type {
      if (p_reading == span)
        return "it is the same words";
      std::vector<token> tokens;
      try {
        tokens = tokenize(p_reading, p_line.source->line);
      } catch (const lex_error &) {
        return "it isn't one line of words";
      }
      if (auto loss = rewrite_loss(span_tokens, tokens, scope, words);
          !loss.empty())
        return loss;
      auto trial = p_line;
      if (!apply_decision(trial, "\"" + span + "\" is " + p_reading))
        return "it doesn't fit the line";
      const auto shape = shape_of(trial.tokens);
      line_parser parser(trial.tokens, scope, words, false);
      if (!shape.problem.empty() || readings_of(parser, shape).empty())
        return "the compiler can't read it either";
      return {};
    };
    const auto key = question_key(p_line.text, span,
                                  known_names(p_line.tokens, scope),
                                  {"(rewrite)"});
    if (const auto *const locked = previous.find(key);
        locked != nullptr && refusal(locked->reading).empty()) {
      ++timing.by_lockfile;
      if (result.lock.find(key) == nullptr)
        result.lock.readings.push_back(*locked);
      return "\"" + span + "\" is " + locked->reading;
    }
    if (locked_file)
      return std::nullopt;
    const auto request = rewrite_request_of(p_line.text, span, span_tokens,
                                            scope, recent(), words);
    // One try: told why a rewrite was refused, the measured model mostly
    // wrote the same again (greedy decoding), and never a right one.
    report_progress(build_progress::stage_type::rewriting, p_line.source->line,
                    span);
    const auto reply = asker.rewrite(request, {});
    if (!reply)
      return std::nullopt;
    if (const auto reason = refusal(*reply); !reason.empty()) {
      if (options.log)
        options.log("refused the rewrite \"" + *reply + "\": " + reason);
      return std::nullopt;
    }
    ++timing.by_rewrite;
    if (result.lock.find(key) == nullptr)
      result.lock.readings.push_back({key, span, *reply});
    model_used = true;
    return "\"" + span + "\" is " + *reply;
  }

  // Something to try for words no reading fits: the phrases with their
  // words of meaning (not names, values or fields), as a report's ending.
  auto phrases_like(const working_line &p_line, std::size_t p_begin,
                    std::size_t p_end) const -> string_type {
    // The words of a name of several words are the name's too (`line` of
    // `some line` isn't `ask for a line`).
    std::set<std::size_t> in_names;
    for (const auto &[name, type] : scope) {
      if (name.find(' ') == string_type::npos)
        continue;
      std::vector<string_type> parts;
      for (std::size_t begin = 0; begin <= name.size();) {
        const auto end = std::min(name.find(' ', begin), name.size());
        parts.push_back(name.substr(begin, end - begin));
        begin = end + 1;
      }
      for (auto at = p_begin; at + parts.size() <= p_end; ++at) {
        bool matches = true;
        for (std::size_t index = 0; index < parts.size() && matches; ++index)
          matches = p_line.tokens[at + index].is_word(parts[index]);
        if (matches)
          for (std::size_t index = 0; index < parts.size(); ++index)
            in_names.insert(at + index);
      }
    }
    std::vector<string_type> meant;
    for (auto index = p_begin; index < p_end; ++index) {
      const auto &item = p_line.tokens[index];
      if (item.kind == token::kind_type::word && !scope.contains(item.text) &&
          !in_names.contains(index) && !words.is_field_word(item.text))
        meant.push_back(item.text);
    }
    string_type listed;
    std::vector<phrase_template> seen;
    std::ranges::copy_if(words.templates, std::back_inserter(seen),
                         [&](const auto &p_item) { return words.sees(p_item.package); });
    for (const auto &phrase : closest_templates(meant, seen, 4))
      listed += "\n    " + phrase;
    return listed.empty() ? listed : "\nPhrases with these words:" + listed;
  }

  auto report_progress(build_progress::stage_type p_stage, std::size_t p_line,
                       const string_type &p_words = {}) const -> void {
    if (options.progress)
      options.progress({.stage = p_stage,
                        .file = shown_name,
                        .line = p_line,
                        .lines = lines.empty() ? 0 : lines.back().line,
                        .words = p_words});
  }

  // Puts a decision this build made into the line; false if it doesn't fit.
  auto decide(working_line &p_line, const string_type &p_decision) -> bool {
    if (!apply_decision(p_line, p_decision))
      return false;
    p_line.decisions.push_back(p_decision);
    p_line.decisions_changed = true;
    const auto separator = p_decision.find("\" is ", 1);
    result.decisions.emplace_back(p_decision.substr(1, separator - 1),
                                  p_decision.substr(separator + 5));
    return true;
  }

  // Without a committed lockfile a meaning is trusted as the author's, so an
  // edit to the line under one is undone silently. Warn (not fail) when the
  // meaning drops a name or number the line has, as a rewrite would be
  // refused.
  auto warn_of_lost_names(working_line &p_line) -> void {
    for (const auto &decision : std::exchange(p_line.author_meanings, {})) {
      const auto separator = decision.find("\" is ", 1);
      try {
        const auto span = tokenize(decision.substr(1, separator - 1),
                                   p_line.source->line);
        const auto reading =
            tokenize(decision.substr(separator + 5), p_line.source->line);
        if (const auto loss = rewrite_loss(span, reading, scope, words);
            !loss.empty())
          result.warnings.push_back(
              file_prefix + "line " + std::to_string(p_line.source->line) +
              ": its meaning \"" + decision.substr(separator + 5) +
              "\" is not what the line says now: " + loss +
              ". Delete the @[meaning: …] line to have it read again.");
      } catch (const lex_error &) {
      }
    }
  }

  // A line nothing in sight reads, which the direct dependencies the file
  // has no `use` line for would: reported, and never asked about, so the
  // model can't settle it as something else. Whether it was.
  auto unused_package_reads(const working_line &p_line, const line_shape &p_shape)
      -> bool {
    if (!scoped || words.visible != &visible)
      return false;
    const auto reads_with = [&](const std::set<string_type> &p_more) {
      auto widened = visible;
      widened.insert(p_more.begin(), p_more.end());
      words.visible = &widened;
      line_parser parser(p_line.tokens, scope, words, false);
      const bool reads = !readings_of(parser, p_shape).empty();
      words.visible = &visible;
      return reads;
    };
    std::set<string_type> unused;
    for (const auto &[name, id] : aliases)
      if (!visible.contains(id))
        unused.insert(id);
    if (unused.empty() || !reads_with(unused))
      return false;
    // The ones it can't read without; the first, if any one will do.
    std::vector<string_type> needed;
    for (const auto &[name, id] : aliases) {
      if (!unused.contains(id))
        continue;
      auto others = unused;
      others.erase(id);
      if (!reads_with(others))
        needed.push_back(name);
    }
    for (const auto &[name, id] : aliases)
      if (needed.empty() && unused.contains(id) && reads_with({id}))
        needed.push_back(name);
    if (needed.empty())
      for (const auto &[name, id] : aliases)
        if (unused.contains(id))
          needed.push_back(name);
    string_type names;
    string_type lines;
    for (std::size_t index = 0; index < needed.size(); ++index) {
      names += (index == 0 ? "" : index + 1 == needed.size() ? " and " : ", ") + needed[index];
      lines += (index == 0 ? "'" : "' and '") + string_type("use ") + needed[index];
    }
    problem(*p_line.source, "This line reads with the phrases of " + names + ", which this "
                                "file doesn't use: write " + lines + "' at its top" +
                                (locked_file ? " (the package's author must)."
                                             : " (`ovis fix` does)."));
    describe_problem(p_line, p_shape.begin, p_shape.end, "needs_use", {});
    result.diagnostics.back().packages = std::move(needed);
    return true;
  }

  // A line nothing in sight reads, which another folder's internal/ phrases
  // or records of this file's own package would (phrase-scope.md S4):
  // reported, never asked about. Whether it was.
  auto internal_reads(const working_line &p_line, const line_shape &p_shape) -> bool {
    if (!scoped || words.visible != &visible)
      return false;
    const auto whole = [](const string_type &p_name) {
      return p_name.substr(0, p_name.find('#'));
    };
    std::set<string_type> hidden;
    const auto consider = [&](const string_type &p_package) {
      if (p_package.find('#') != string_type::npos && !visible.contains(p_package) &&
          whole(p_package) == whole(words.package))
        hidden.insert(p_package);
    };
    for (const auto &item : words.templates)
      consider(item.package);
    for (const auto &[name, record] : words.records)
      consider(record.package);
    for (const auto &folder : hidden) {
      auto widened = visible;
      widened.insert(folder);
      words.visible = &widened;
      line_parser parser(p_line.tokens, scope, words, false);
      const bool reads = !readings_of(parser, p_shape).empty();
      words.visible = &visible;
      if (!reads)
        continue;
      const auto path = folder.substr(folder.find('#') + 1);
      problem(*p_line.source, "This line reads with what " + path + "/ defines, which only "
                              "the files under " + path.substr(0, path.rfind('/')) +
                              "/ see: move the line there, or the definition out of "
                              "internal/.");
      describe_problem(p_line, p_shape.begin, p_shape.end, "internal", {});
      return true;
    }
    return false;
  }

  // The one reading of the line's span, settling unclear spans with the
  // model. Nothing if it couldn't be settled (reported).
  auto settle(working_line &p_line, line_shape &p_shape)
      -> std::optional<parse> {
    const auto &source = *p_line.source;
    report_progress(build_progress::stage_type::reading, source.line);
    warn_of_lost_names(p_line);
    // `log's write "hi"` (or `x = log's …`): the line says whose phrases it
    // reads with, those of a package its project names, when no variable
    // has that name (packages.md §5).
    struct widen {
      const program_reader &reader;
      ~widen() { reader.enter(); }
    } widened{*this};
    if (scoped) {
      auto &tokens = p_line.tokens;
      std::size_t at = 0;
      if (tokens.size() > 3 && tokens[1].is_symbol("="))
        at = 2;
      if (at + 2 < tokens.size() && tokens[at].kind == token::kind_type::word &&
          tokens[at + 1].is_symbol("'s") && !scope.contains(tokens[at].text))
        if (const auto alias = aliases.find(lower(tokens[at].text)); alias != aliases.end()) {
          narrowed = {alias->second};
          tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(at),
                       tokens.begin() + static_cast<std::ptrdiff_t>(at + 2));
          words.visible = &narrowed;
        }
    }
    for (int round = 0; round < 4; ++round) {
      p_shape = shape_of(p_line.tokens);
      // `show x = 3` is a statement, not a variable called `show x`.
      if (p_shape.kind == line_shape::kind_type::assign &&
          p_shape.name.find(' ') != string_type::npos) {
        line_parser whole(p_line.tokens, scope, words, false);
        if (!whole.statements(0, p_line.tokens.size(), false).empty())
          p_shape = {.kind = line_shape::kind_type::phrase,
                     .begin = 0,
                     .end = p_line.tokens.size()};
      }
      if (!p_shape.problem.empty()) {
        problem(source, p_shape.problem);
        return std::nullopt;
      }
      if (p_shape.kind == line_shape::kind_type::else_ ||
          p_shape.kind == line_shape::kind_type::stop ||
          p_shape.kind == line_shape::kind_type::skip ||
          p_shape.kind == line_shape::kind_type::define ||
          p_shape.kind == line_shape::kind_type::defer_block ||
          p_shape.kind == line_shape::kind_type::do_block ||
          (p_shape.kind == line_shape::kind_type::leave_block &&
           p_shape.begin == p_shape.end) ||
          p_shape.kind == line_shape::kind_type::use_c ||
          p_shape.kind == line_shape::kind_type::use_package ||
          p_shape.kind == line_shape::kind_type::record ||
          p_shape.kind == line_shape::kind_type::trait_decl ||
          p_shape.kind == line_shape::kind_type::impl_decl ||
          (p_shape.kind == line_shape::kind_type::give_back &&
           p_shape.begin == p_shape.end))
        return parse{};

      line_parser parser(p_line.tokens, scope, words, false);
      auto readings = readings_of(parser, p_shape);
      if (readings.size() == 1)
        return readings.front();
      if (readings.empty() &&
          (unused_package_reads(p_line, p_shape) || internal_reads(p_line, p_shape)))
        return std::nullopt;
      if (readings.empty())
        if (const auto hint = missing_phrase_hint(p_line, p_shape);
            !hint.empty()) {
          problem(source, hint);
          return std::nullopt;
        }

      ++timing.questions;
      auto asked = question_of(p_line, p_shape, readings);
      // Nothing to offer for the part found unclear: a small edit of the
      // whole line may read (`c's count not 0`, `b title`).
      if (asked.candidates.empty() && asked.kind != question_kind::reference &&
          readings.empty()) {
        const auto whole = [&](const string_type &p_reading) {
          auto trial = p_line;
          if (!apply_decision(trial, "\"" + span_text(p_line, p_shape.begin,
                                                        p_shape.end) +
                                         "\" is " + p_reading))
            return false;
          const auto shape = shape_of(trial.tokens);
          line_parser parser(trial.tokens, scope, words, false);
          return shape.problem.empty() && !readings_of(parser, shape).empty();
        };
        if (auto edits = edit_candidates(p_line.tokens, p_shape.begin,
                                         p_shape.end, true,
                                         p_shape.is_statement(),
                                         p_shape.takes_block, scope, words,
                                         whole);
            !edits.empty())
          asked = {.kind = question_kind::unknown_phrase,
                   .begin = p_shape.begin,
                   .end = p_shape.end,
                   .candidates = std::move(edits)};
      }
      const auto span = span_text(p_line, asked.begin, asked.end);
      if (asked.candidates.empty()) {
        if (!asked.record.empty()) {
          const auto type = words.records.at(asked.record).type();
          if (asked.kind == question_kind::reference) {
            problem(source, "No variable here holds " + type.english() +
                                ", so \"" + span + "\" means nothing yet.");
            describe_problem(p_line, asked.begin, asked.end, "unknown_name", {});
          } else {
            string_type fields;
            for (const auto &field : words.records.at(asked.record).fields)
              fields += (fields.empty() ? "" : ", ") + field.spoken();
            auto owner = type.english();
            owner[0] = static_cast<char_type>(std::toupper(owner[0]));
            problem(source, owner + " has no field called '" + span +
                                "'. Its fields are: " + fields + ".");
            describe_problem(p_line, asked.begin, asked.end, "no_field", {});
          }
        } else if (asked.kind == question_kind::unknown_name ||
                   asked.kind == question_kind::reference) {
          problem(source, nothing_called(p_line.tokens[asked.begin].text) +
                              set_later_hint(p_line.tokens[asked.begin].text,
                                             p_shape, source.line));
          describe_problem(p_line, asked.begin, asked.end, "unknown_name", {});
        } else if (const auto decision = rewritten(p_line, p_shape)) {
          if (!decide(p_line, *decision))
            break;
          continue;
        } else {
          problem(source, "No phrase Ovis knows reads \"" + span + "\"." +
                              phrases_like(p_line, asked.begin, asked.end));
          describe_problem(p_line, asked.begin, asked.end, "no_phrase", {});
        }
        return std::nullopt;
      }
      std::optional<string_type> chosen;
      std::vector<string_type> readings_offered;
      for (const auto &item : asked.candidates)
        readings_offered.push_back(item.reading);
      // What a report offers the author: no reading the model turned down
      // (if it found none good, its next best wouldn't be either).
      auto offered = readings_offered;
      const auto known = known_names(p_line.tokens, scope);
      const auto key = question_key(p_line.text, span, known, readings_offered);
      const auto *const locked = previous.find(key);
      if (locked != nullptr &&
          std::ranges::find(readings_offered, locked->reading) !=
              readings_offered.end()) {
        chosen = locked->reading;
        ++timing.by_lockfile;
        if (result.lock.find(key) == nullptr)
          result.lock.readings.push_back(*locked);
      } else if (asked.rule) {
        chosen = asked.candidates[*asked.rule].reading;
        ++timing.by_rule;
      } else if (const auto shortcut = ranking_shortcut(asked.candidates)) {
        chosen = asked.candidates[*shortcut].reading;
        ++timing.by_ranking;
      } else if (locked_file) {
        // A dependency settles its own lines; nobody else's model may.
      } else {
        // Only now, when the model is asked anyway: builds the lockfile,
        // a rule or the shortcut settles never load it.
        const bool by_embedding = options.choose_by_embedding &&
                                  asked.kind == question_kind::unknown_phrase;
        if (by_embedding || (options.order_ties_by_embedding &&
                             asked.kind == question_kind::unknown_phrase))
          asker.order_ties(span, asked.candidates);
        if (by_embedding)
          chosen = asker.unavailable.empty()
                       ? std::optional(asked.candidates.front().reading)
                       : std::nullopt;
        else
        {
          report_progress(build_progress::stage_type::asking, source.line, span);
          chosen = asker.choose(p_line.text, known, span, asked.candidates);
          if (!chosen)
            offered = asker.not_rejected;
        }
        if (chosen) {
          ++timing.by_model;
          if (result.lock.find(key) == nullptr)
            result.lock.readings.push_back({key, span, *chosen});
          model_used = true;
        }
      }
      if (!chosen && asked.kind == question_kind::unknown_phrase)
        if (const auto decision = rewritten(p_line, p_shape)) {
          if (!decide(p_line, *decision))
            break;
          continue;
        }
      if (!chosen) {
        string_type listed;
        for (std::size_t index = 0; index < offered.size() && index < 4; ++index)
          listed += "\n    " + offered[index];
        const auto turned_down = offered.size() < readings_offered.size();
        problem(source,
                "\"" + span + "\" is unclear" +
                    (locked_file ? " in the package " + package +
                                       ", whose author must settle it (its "
                                       "meanings and lockfile don't)"
                     : asker.unavailable.empty()
                         ? ""
                         : " and couldn't be asked about: " + asker.unavailable) +
                    (listed.empty() && turned_down
                         ? ". The model found no reading that means the same; "
                           "write it in other words."
                         : ". Did you mean one of these? Write it in the source:" +
                               listed) +
                    phrases_like(p_line, asked.begin, asked.end));
        describe_problem(p_line, asked.begin, asked.end, "unclear", offered);
        return std::nullopt;
      }
      if (!decide(p_line, "\"" + span + "\" is " + *chosen)) {
        problem(source, "Internal compiler error: the reading \"" + *chosen +
                            "\" doesn't fit the line.");
        return std::nullopt;
      }
    }
    problem(source, "This line stays unclear after several readings; "
                    "rewrite it more simply.");
    return std::nullopt;
  }

  // The Ovis Core of the lines from `p_index` at `p_depth`, up to the end of
  // that block. Moves `p_index` past them.
  auto read_block(std::size_t &p_index, std::size_t p_depth)
      -> std::vector<string_type> {
    std::vector<string_type> forms;
    while (p_index < lines.size() && lines[p_index].depth == p_depth) {
      const auto line = lines[p_index].line;
      auto form = read_statement(p_index, p_depth);
      if (!form.empty())
        forms.push_back(line_marker(line) + form);
    }
    return forms;
  }

  auto joined(const std::vector<string_type> &p_forms) -> string_type {
    string_type result;
    for (const auto &form : p_forms)
      result += " " + form;
    return result;
  }

  // The body under a line that opens a block (always there: the lexer
  // makes sure of it).
  auto read_body(std::size_t &p_index, std::size_t p_depth)
      -> std::vector<string_type> {
    return read_block(p_index, p_depth + 1);
  }

  auto read_statement(std::size_t &p_index, std::size_t p_depth)
      -> string_type {
    using enum line_shape::kind_type;
    auto &line = working[p_index];
    const auto &source = lines[p_index];
    if (options.variables_recorded || here_for_phrases(source.line))
      record_scope(source.line);
    ++p_index;
    line_shape shape;
    const auto value = settle(line, shape);
    if (value)
      for (const auto *used : value->uses)
        shared.templates_used.insert(used->file + ":" +
                                     std::to_string(used->line));
    const auto skip_body = [&] {
      while (p_index < lines.size() && lines[p_index].depth > p_depth)
        ++p_index;
    };
    if (!value) {
      // Known from here on, so one unclear line isn't reported again on
      // every line using what it sets.
      if (shape.kind == assign || shape.kind == each)
        scope.try_emplace(shape.name);
      skip_body();
      return {};
    }

    switch (shape.kind) {
    case assign: {
      remember(shape.name);
      auto existing = scope.find(shape.name);
      if ((in_function && locals.insert(shape.name).second) ||
          (!in_function && foreign.erase(shape.name) > 0))
        existing = scope.end();
      if (existing == scope.end()) {
        scope[shape.name] = value->type;
        if (p_depth > base_depth &&
            !(defer_body_depth != 0 && p_depth <= defer_body_depth)) {
          hoisted.push_back(declaration(shape.name, value->type));
          return "(set " + core_name(shape.name) + " " + value->core + ")";
        }
        return "(let " + core_name(shape.name) + " " + value->core + ")";
      }
      if (!existing->second.known())
        existing->second = value->type;
      return "(set " + core_name(shape.name) + " " + value->core + ")";
    }
    case phrase: {
      auto core = value->core;
      if (shape.takes_block) {
        const auto body = joined(read_body(p_index, p_depth));
        const auto at = core.find("BLOCK");
        core.replace(at, 5, body.empty() ? "(pass)" : body.substr(1));
      }
      return core;
    }
    case if_: {
      auto result_form = "(if " + value->core + " (then" +
                         joined(read_body(p_index, p_depth)) + ")";
      // otherwise if … / otherwise: nest as else branches.
      std::string closing = ")";
      while (p_index < lines.size() && lines[p_index].depth == p_depth) {
        const auto next = shape_of(working[p_index].tokens);
        if (next.kind == else_if) {
          auto &next_line = working[p_index];
          ++p_index;
          line_shape next_shape;
          const auto condition = settle(next_line, next_shape);
          const auto body = joined(read_body(p_index, p_depth));
          if (!condition)
            continue;
          result_form += " (else (if " + condition->core + " (then" + body + ")";
          closing += "))";
        } else if (next.kind == else_) {
          ++p_index;
          result_form +=
              " (else" + joined(read_body(p_index, p_depth)) + ")";
          break;
        } else {
          break;
        }
      }
      return result_form + closing;
    }
    case else_if:
    case else_:
      problem(source, "'otherwise' must follow an 'if' at the same depth.");
      skip_body();
      return {};
    case while_:
      return "(while " + value->core + joined(read_body(p_index, p_depth)) +
             ")";
    case each: {
      if (value->type.known() && value->type.depth == 0 &&
          value->type.base != "map") {
        problem(source, "'" + span_text(line, shape.begin, shape.end) +
                            "' is " + value->type.english() +
                            ", not a list to go through.");
        skip_body();
        return {};
      }
      // The loop's variable lives only in the loop, as in Ovis Core: after
      // it the name means what it meant before, if anything.
      const auto outer = scope.find(shape.name) == scope.end()
                             ? std::nullopt
                             : std::optional(scope[shape.name]);
      // A map is gone through by its keys.
      scope[shape.name] =
          value->type.base == "map"
              ? (value->type.exact.empty()
                     ? value_kind{}
                     : kind_of(core::value_type::parse(
                                   core::read_sexprs(value->type.exact).front())
                                   ->map_key()))
          : value->type.known() ? value->type.element()
                                : value_kind{};
      remember(shape.name);
      // `for each i in the numbers from 1 to n` counts: no list is made, so
      // there is none to free.
      string_type head = "(each " + core_name(shape.name) + " " + value->core;
      try {
        const auto forms = core::read_sexprs(value->core);
        if (forms.size() == 1 && forms[0].is_list() &&
            forms[0].items.size() == 3 &&
            (forms[0].head() == "prelude_range" ||
             forms[0].head() == "prelude_range_down"))
          head = string_type(forms[0].head() == "prelude_range" ? "(for "
                                                                 : "(for-down ") +
                 core_name(shape.name) + " " + forms[0].items[1].to_string() +
                 " " + forms[0].items[2].to_string();
      } catch (const error &) {
      }
      auto form = head + joined(read_body(p_index, p_depth)) + ")";
      if (outer)
        scope[shape.name] = *outer;
      else
        scope.erase(shape.name);
      return form;
    }
    case repeat: {
      const auto counter = "repeat_" + std::to_string(++counters);
      return "(for " + counter + " 1 " + value->core +
             joined(read_body(p_index, p_depth)) + ")";
    }
    case defer_line:
    case defer_block: {
      note_defer(line, p_index - 1, p_depth);
      if (shape.kind == defer_line)
        return "(defer " + value->core + ")";
      // Names set inside are the block's own.
      const auto outer_scope = scope;
      const auto outer_locals = locals;
      const auto outer_depth = std::exchange(defer_body_depth, p_depth + 1);
      // Names first set in a block inside it are declared at its top,
      // not hoisted out of it.
      auto outer_hoisted = std::exchange(hoisted, {});
      auto body = joined(read_body(p_index, p_depth));
      string_type lets;
      for (const auto &let : hoisted)
        lets += " " + let;
      if (!lets.empty())
        body = lets + body;
      hoisted = std::move(outer_hoisted);
      defer_body_depth = outer_depth;
      std::erase_if(scope, [&](const auto &p_entry) {
        return !outer_scope.contains(p_entry.first);
      });
      locals = outer_locals;
      return body.empty() ? string_type("(pass)") : "(defer" + body + ")";
    }
    case do_block:
      return read_do_block(p_index, p_depth, shape);
    case leave_block: {
      if (do_blocks.empty()) {
        problem(source, "'leave' only works inside a 'do:' block.");
        return {};
      }
      auto &target = do_blocks.back();
      if (value->core.empty())
        return "(leave :" + target.label + ")";
      if (target.gives.empty()) {
        problem(source, "This 'do:' block gives no value: write 'NAME = do:' "
                        "for one, or just 'leave'.");
        return {};
      }
      if (!target.type.known())
        target.type = value->type;
      return "(leave :" + target.label + " " + value->core + ")";
    }
    case stop:
      return "(break)";
    case skip:
      return "(continue)";
    case define:
      return read_function(source, p_index, p_depth);
    case record: {
      skip_body();
      if (p_depth > 0 || in_function) {
        problem(source, "Declare records ('a NAME has:') at the top level, "
                        "not inside a block.");
        return {};
      }
      const auto found = record_forms.find(source.line);
      return found == record_forms.end() ? string_type() : found->second;
    }
    case trait_decl:
      return read_trait(source, p_index, p_depth, shape);
    case impl_decl:
      return read_impl(source, p_index, p_depth, shape);
    case use_c:
      if (p_depth > 0 || in_function) {
        problem(source, "Write 'use' lines at the top level.");
        return {};
      }
      return "(use-c " + quote(shape.name) + " " + quote(shape.library) + ")";
    case use_package:
      if (p_depth > 0 || in_function)
        problem(source, "Write 'use' lines at the top level.");
      else if (!aliases.contains(shape.name))
        problem(source, "There is no package called '" + shape.name +
                            "' in ovis.toml's [dependencies]" +
                            (package.empty() ? ": `ovis add " + shape.name +
                                                   " --git URL` adds one."
                                             : "."));
      return {};
    case give_back:
      if (!in_function) {
        problem(source, "'give back' only works inside a 'to …:' phrase.");
        return {};
      }
      return value->core.empty() ? "(return)" : "(return " + value->core + ")";
    }
    return {};
  }

  // `do:` (or `NAME = do:`) and its body: a block of its own, whose names
  // stay in it, as Ovis Core's `(block :LABEL …)`.
  auto read_do_block(std::size_t &p_index, std::size_t p_depth,
                     const line_shape &p_shape) -> string_type {
    const auto label = "block_" + std::to_string(++counters);
    do_blocks.push_back({label, p_shape.name, {}});
    const auto outer_scope = scope;
    const auto outer_locals = locals;
    const auto outer_depth = std::exchange(defer_body_depth, p_depth + 1);
    // Names first set deeper inside are declared at its top, not hoisted
    // out of it.
    auto outer_hoisted = std::exchange(hoisted, {});
    auto body = joined(read_body(p_index, p_depth));
    string_type lets;
    for (const auto &let : hoisted)
      lets += " " + let;
    hoisted = std::move(outer_hoisted);
    defer_body_depth = outer_depth;
    std::erase_if(scope, [&](const auto &p_entry) {
      return !outer_scope.contains(p_entry.first);
    });
    locals = outer_locals;
    const auto block = do_blocks.back();
    do_blocks.pop_back();
    const auto form = "(block :" + label + lets + body + ")";
    if (p_shape.name.empty())
      return form;
    // The variable it gives, as an assignment would set it.
    remember(p_shape.name);
    auto existing = scope.find(p_shape.name);
    if ((in_function && locals.insert(p_shape.name).second) ||
        (!in_function && foreign.erase(p_shape.name) > 0))
      existing = scope.end();
    const auto name = core_name(p_shape.name);
    if (existing != scope.end()) {
      if (!existing->second.known())
        existing->second = block.type;
      return "(set " + name + " " + form + ")";
    }
    scope[p_shape.name] = block.type;
    if (p_depth > base_depth &&
        !(defer_body_depth != 0 && p_depth <= defer_body_depth)) {
      hoisted.push_back(declaration(p_shape.name, block.type));
      return "(set " + name + " " + form + ")";
    }
    return "(let " + name + " " + form + ")";
  }

  // `at the end` runs at the end of its own block, which isn't what "the
  // end" suggests anywhere but a function or the top level: say so under it.
  auto note_defer(working_line &p_line, std::size_t p_line_index,
                  std::size_t p_depth) -> void {
    p_line.writes_compiler_note = true;
    if (p_depth == 0)
      return;
    auto opener = p_line_index;
    while (opener > 0 && lines[opener - 1].depth >= p_depth)
      --opener;
    if (opener == 0)
      return;
    using enum line_shape::kind_type;
    switch (shape_of(working[opener - 1].tokens).kind) {
    case define:
      return;
    case each:
    case while_:
    case repeat:
      p_line.compiler_note = "runs at the end of every pass of the loop.";
      return;
    case if_:
    case else_if:
    case else_:
      p_line.compiler_note = "runs at the end of this branch.";
      return;
    default:
      p_line.compiler_note = "runs at the end of this block.";
    }
  }

  // A `to …:` line and its body, as an Ovis Core `fn`.
  auto read_function(const source_line &p_source, std::size_t &p_index,
                     std::size_t p_depth, bool p_in_trait = false)
      -> string_type {
    const auto skip_body = [&] {
      while (p_index < lines.size() && lines[p_index].depth > p_depth)
        ++p_index;
    };
    if ((p_depth > 0 && !p_in_trait) || in_function) {
      problem(p_source, "Define phrases ('to …:') at the top level, not "
                        "inside a block.");
      skip_body();
      return {};
    }
    const auto found = functions.find(p_source.line);
    if (found == functions.end()) {
      // Its header couldn't be read (reported).
      skip_body();
      return {};
    }
    const auto &function = found->second;
    const auto saved_scope = scope;
    auto saved_hoisted = std::exchange(hoisted, {});
    in_function = true;
    base_depth = p_depth + 1;
    locals.clear();
    string_type parameters;
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
      const auto &name = function.parameters[index];
      const auto &type = function.types[index];
      scope[name] = type.value_or(value_kind{});
      locals.insert(name);
      remember(name);
      const auto same = index < function.same.size() && function.same[index];
      parameters += (index > 0 ? " " : "") +
                    (same   ? "(" + core_name(name) + " same)"
                     : type ? "(" + core_name(name) + " " + type->core() + ")"
                            : core_name(name));
    }
    // A generic phrase's type variables are types in its body.
    std::map<string_type, std::vector<string_type>> variables;
    for (const auto &[name, bounds] : function.type_parameters)
      variables[name] = bounds;
    const auto body = [&] {
      const type_variable_scope generic(words, std::move(variables));
      return joined(read_body(p_index, p_depth));
    }();
    string_type form = "(fn " + function.core + " " +
                       forall_of(function.type_parameters) + "(" + parameters + ")";
    if (function.result)
      form += " " + function.result->core();
    for (const auto &let : hoisted)
      form += " " + let;
    form += body + ")";
    scope = saved_scope;
    hoisted = std::move(saved_hoisted);
    in_function = false;
    base_depth = 0;
    return form;
  }

  // `a shape can:` and its body in Ovis Core: the required phrases, and the
  // default ones with their bodies.
  auto read_trait(const source_line &p_source, std::size_t &p_index,
                  std::size_t p_depth, const line_shape &p_shape)
      -> string_type {
    string_type form = "(trait " + own_record(p_shape.name);
    const auto top = p_depth == 0 && !in_function;
    if (!top)
      problem(p_source, "Declare traits ('a NAME can:') at the top level, not "
                        "inside a block.");
    while (p_index < lines.size() && lines[p_index].depth > p_depth) {
      const auto &child = lines[p_index];
      ++p_index;
      if (child.depth > p_depth + 1)
        continue;
      if (child.tokens.front().is_word("is")) {
        // `is comparable and a thing`: phrases, and traits required.
        for (const auto &need : trait_needs_of(child.line))
          form += " " + need;
        for (const auto &other : trait_requires[child.line])
          form += " (requires " + other + ")";
        continue;
      }
      if (const auto need = trait_needs.find(child.line);
          need != trait_needs.end()) {
        form += " " + need->second;
        continue;
      }
      form += " " + read_function(child, p_index, p_depth + 1, true);
    }
    if (!top)
      return {};
    form += ")";
    // `comes_before__shape`: the built-in trait's phrase on a value of the
    // trait, for `<`, `sort` and the like, dispatched by the method.
    for (const auto &[name, method] : shared.trait_methods[p_shape.name]) {
      if (!builtin_trait_method(method))
        continue;
      const auto type = value_kind{"record", 0, p_shape.name}.core();
      string_type parameters;
      string_type arguments;
      for (std::size_t index = 0; index < method.parameters.size(); ++index) {
        const auto parameter = "p" + std::to_string(index);
        parameters += (index > 0 ? " (" : "(") + parameter + " " +
                      (index == 0 || method.same[index]
                           ? type
                           : method.types[index]->core()) +
                      ")";
        arguments += " " + parameter;
      }
      form += "\n(fn " + name + "__" + p_shape.name + " (" + parameters +
              ") " + method.result->core() + " (" + name + arguments + "))";
    }
    return form;
  }

  // `a circle is a shape:` and its methods in Ovis Core.
  auto read_impl(const source_line &p_source, std::size_t &p_index,
                 std::size_t p_depth, const line_shape &p_shape)
      -> string_type {
    const auto top = p_depth == 0 && !in_function;
    if (!top)
      problem(p_source, "Write 'a NAME is a TRAIT:' at the top level, not "
                        "inside a block.");
    const auto [record, record_length] =
        words.record_at(p_source.tokens, p_shape.begin, p_shape.name_end);
    const auto [trait, trait_length] = words.record_at(
        p_source.tokens, p_shape.trait_begin, p_shape.trait_end);
    if (!builtin_trait_of(p_shape).empty()) {
      // The phrases are plain functions of the record.
      string_type forms;
      while (p_index < lines.size() && lines[p_index].depth > p_depth) {
        const auto &child = lines[p_index];
        ++p_index;
        if (child.depth > p_depth + 1)
          continue;
        const auto method = read_function(child, p_index, p_depth + 1, true);
        if (top && !method.empty())
          forms += (forms.empty() ? "" : "\n") + method;
      }
      return forms;
    }
    const auto plain = plain_impls.find(p_source.line);
    const bool known = top && trait != nullptr && trait->is_trait &&
                       trait_length == p_shape.trait_end - p_shape.trait_begin &&
                       (plain != plain_impls.end() ||
                        (record != nullptr && !record->is_trait &&
                         record_length == p_shape.name_end - p_shape.begin));
    string_type form;
    if (known) {
      form = "(impl " + trait->name + " " +
             (plain != plain_impls.end() ? plain->second : record->name);
      if (const auto forall = forall_of(generic_impl_variables(p_shape));
          !forall.empty())
        form += " " + forall.substr(0, forall.size() - 1);
    }
    while (p_index < lines.size() && lines[p_index].depth > p_depth) {
      const auto &child = lines[p_index];
      ++p_index;
      if (child.depth > p_depth + 1)
        continue;
      auto method = read_function(child, p_index, p_depth + 1, true);
      if (known && !method.empty())
        form += " " + method;
    }
    return known ? form + ")" : string_type();
  }

public:
  program_reader(const phrase_file &p_file, program_context &p_shared,
                 front_end_result &p_result)
      : shared(p_shared), model(p_shared.model), options(p_shared.options),
        previous(p_file.lock), result(p_result), timing(p_shared.timing),
        asker(p_shared.asker), words(p_shared.words),
        file_name(p_file.name), locked_file(p_file.locked), package(p_file.package),
        visible(p_file.sees.begin(), p_file.sees.end()), scoped(!p_file.sees.empty()),
        aliases(p_file.aliases),
        core_suffix(package_core_suffix(p_file.package.substr(0, p_file.package.find('#')))),
        package_label(p_file.package_label),
        shown_name(p_file.name.empty() ? p_file.path.filename().string()
                                       : p_file.name),
        file_prefix(p_file.name.empty() ? string_type() : p_file.name + ", "),
        source_path(p_file.path) {
    enter();
    const string_view_type p_source = p_file.text;
    const auto start = clock_type::now();
    lines = split_lines(p_source);
    timing.lex_ms += milliseconds_since(start);
    timing.lines += lines.size();

    // Each line's text from its first token, for putting decisions in.
    std::vector<string_view_type> physical;
    for (std::size_t begin = 0; begin <= p_source.size();) {
      auto end = p_source.find('\n', begin);
      if (end == string_view_type::npos)
        end = p_source.size();
      physical.push_back(p_source.substr(begin, end - begin));
      begin = end + 1;
    }
    for (std::size_t index = 0; index < lines.size(); ++index) {
      const auto &line = lines[index];
      working_line item{.source = &line, .tokens = line.tokens};
      const auto &text = physical[line.line - 1];
      const auto single_line = line.tokens.back().line == line.line;
      // Up to its last token: the header of `if x: show x` isn't its body.
      item.text = string_type(
          single_line ? text.substr(line.tokens.front().column - 1,
                                    line.tokens.back().end_column -
                                        line.tokens.front().column)
                      : text.substr(line.tokens.front().column - 1));
      if (item.text.ends_with('\r'))
        item.text.pop_back();
      if (single_line) {
        item.original = item.text;
        item.start_column = index > 0 && lines[index - 1].line == line.line
                                ? lines[index - 1].tokens.front().column
                                : line.tokens.front().column;
        item.physical = string_type(trim(text.substr(item.start_column - 1)));
        item.offset = line.tokens.front().column - item.start_column;
      }
      for (const auto &note : line.annotations) {
        // Old `Read as:` and `Compiler:` lines are rewritten as sigils.
        if (!note.is_sigil)
          item.decisions_changed = true;
        if (note.kind == annotation::kind_type::compiler)
          continue;
        if (note.kind == annotation::kind_type::meaning) {
          // Of the whole physical line, which a one-line block shares
          // with its header.
          const auto first = index > 0 && lines[index - 1].line == line.line
                                 ? lines[index - 1].tokens.front().column
                                 : line.tokens.front().column;
          const auto written =
              trim(text.substr(first - 1, line.tokens.back().end_column - first));
          const auto *const entry = previous.find_meaning(note.text);
          // Written by the compiler for another line: the line was edited
          // since, so it is read afresh.
          if (!single_line || (entry != nullptr && entry->line != written)) {
            item.decisions_changed = true;
            continue;
          }
          const auto decision = meaning_decision(written, note.text);
          if (!decision)
            continue;
          // A one-line block's body or its header, whichever has the words.
          if (apply_decision(item, *decision) ||
              (index > 0 && lines[index - 1].line == line.line &&
               apply_decision(working.back(), *decision))) {
            auto &changed = item.text != item.original ? item : working.back();
            changed.decisions.push_back(*decision);
            if (entry == nullptr)
              changed.author_meanings.push_back(*decision);
            ++timing.by_source;
          } else {
            item.decisions_changed = true;
          }
          continue;
        }
        // A decision whose words are no longer in the line is dropped: the
        // line was edited, so it is read afresh.
        if (single_line && apply_decision(item, note.text)) {
          item.decisions.push_back(note.text);
          ++timing.by_source;
        }
        else
          item.decisions_changed = true;
      }
      working.push_back(std::move(item));
    }

    // `use csv` lines say which dependencies' phrases and records the file
    // reads with (proposals/phrase-scope.md S2), wherever they are: before
    // any line is read, as `use "x.h"` is.
    for (const auto &line : lines)
      if (const auto shape = shape_of(line.tokens);
          scoped && line.depth == 0 && shape.kind == line_shape::kind_type::use_package)
        if (const auto alias = aliases.find(shape.name); alias != aliases.end())
          visible.insert(alias->second);

    // Records and traits are known in every file, before anything else is
    // read: a phrase's parameter or another record's field may be one.
    for (std::size_t index = 0; index < lines.size(); ++index) {
      const auto &line = lines[index];
      const auto shape = shape_of(line.tokens);
      const bool is_trait = shape.kind == line_shape::kind_type::trait_decl;
      if (line.depth != 0 ||
          (shape.kind != line_shape::kind_type::record && !is_trait) ||
          !shape.problem.empty())
        continue;
      const auto noun = is_trait ? "trait" : "record";
      const auto as_type = type_words(line.tokens, shape.begin, shape.name_end);
      if (core_name(shape.name) != shape.name ||
          std::ranges::any_of(as_type, [&](const auto &p_match) {
            return p_match.first == shape.name_end - shape.begin;
          })) {
        problem(line, "'" + span_text(working[index], shape.begin, shape.name_end) +
                          "' is already a type Ovis has: call the " + noun +
                          " something else.");
        continue;
      }
      // `list of …` and `map of …` are Ovis's own.
      if (!shape.parameters.empty() &&
          (shape.name == "list" || shape.name == "map" ||
           shape.name == "dictionary" || shape.name == "array")) {
        problem(line, "'" + shape.name + "' is Ovis's own; call the record "
                      "something else.");
        continue;
      }
      if (const auto [defined_at, added] =
              shared.records_defined.try_emplace(defined_key(shape.name), where(line));
          !added) {
        problem(line, string_type("A ") + (is_trait ? "trait" : "record") +
                          " called '" +
                          span_text(working[index], shape.begin, shape.end) +
                          "' is already declared " + defined_at->second + ".");
        continue;
      }
      record_info record{.name = own_record(shape.name), .is_trait = is_trait,
                         .parameters = shape.parameters,
                         .header = shape.header,
                         .is_packed = shape.packed,
                         .package = words.package};
      for (auto at = shape.begin; at < shape.name_end; ++at)
        record.words.push_back(lower(line.tokens[at].text));
      // Said and printed as written, whatever its Core name's ending; in
      // messages with its package's version when two are in the program.
      if (!core_suffix.empty()) {
        auto spoken = shape.name;
        std::ranges::replace(spoken, '_', ' ');
        core::record_descriptions()[record.name] =
            package_label.empty() ? spoken : package_label + " " + spoken;
        core::record_print_names()[record.name] =
            (string_view_type("aeiou").contains(spoken.front()) ? "an " : "a ") + spoken;
      }
      words.records[record.name] = std::move(record);
      (is_trait ? trait_lines : record_lines).push_back(index);
    }
  }

  // A trait's phrases are known in every file before any impl or line is
  // read, so an impl may come before its trait, in another file.
  auto declare_traits() -> void {
    enter();
    for (const auto index : trait_lines) {
      const auto &trait = words.records.at(own_record(shape_of(lines[index].tokens).name));
      for (auto at = index + 1; at < lines.size() && lines[at].depth > 0; ++at) {
        if (lines[at].depth > 1)
          continue;
        try {
          if (lines[at].tokens.front().is_word("is")) {
            declare_requirements(lines[at], trait);
            continue;
          }
          declare_trait_phrase(lines[at], working[at], trait);
        } catch (const error &p_error) {
          problem(lines[at], p_error.what());
        }
      }
    }
  }

  // `is comparable`, `is a thing`, `is showable and a thing` in a trait:
  // its records must be those too, and its values may be used as them. A
  // built-in trait is its phrase, which each record writes as for itself
  // (`a circle is comparable:`) or in the impl.
  auto declare_requirements(const source_line &p_line, const record_info &p_trait)
      -> void {
    const auto &tokens = p_line.tokens;
    if (p_line.opens_block())
      throw error("Write what every " + p_trait.words.back() +
                  " also is without ':', as in: is comparable");
    std::vector<std::pair<std::size_t, std::size_t>> names;
    std::size_t begin = 1;
    for (std::size_t at = 1; at <= tokens.size(); ++at)
      if (at == tokens.size() || tokens[at].is_word("and") ||
          tokens[at].is_symbol(",")) {
        auto first = begin;
        if (first < at && is_article(tokens[first]))
          ++first;
        if (first == at)
          throw error("Name a trait after 'is', as in: is comparable");
        names.emplace_back(first, at);
        begin = at + 1;
      }
    for (const auto &[first, end] : names) {
      const auto word = lower(tokens[first].text);
      if (end == first + 1 &&
          (word == "comparable" || word == "equatable" || word == "showable")) {
        const auto header = word == "comparable"
                                ? "(a) comes before (b) gives a truth value"
                            : word == "equatable"
                                ? "(a) is the same as (b) gives a truth value"
                                : "text of (it) gives text";
        auto line = p_line;
        line.tokens = split_lines(header).front().tokens;
        for (auto &token : line.tokens)
          token.line = p_line.line;
        declare_trait_phrase(line, working_line{.text = header}, p_trait);
        continue;
      }
      const auto [other, length] = words.record_at(tokens, first, end);
      if (other == nullptr || !other->is_trait || length != end - first) {
        string_type name;
        for (auto at = first; at < end; ++at)
          name += (at > first ? " " : "") + tokens[at].text;
        throw error("No trait is called '" + name +
                    "'. A trait may require comparable, equatable, showable "
                    "or a trait declared with 'a NAME can:'.");
      }
      if (other->name == p_trait.name)
        throw error("A trait can't require itself.");
      trait_requires[p_line.line].push_back(other->name);
      shared.trait_requires[p_trait.name].insert(other->name);
    }
  }

  auto declare_trait_phrase(const source_line &p_line,
                            const working_line &p_working,
                            const record_info &p_trait) -> void {
    const auto parts = method_line_of(p_line.tokens);
    std::optional<value_kind> gives;
    if (parts.gives) {
      gives = stated_type(*parts.gives, 0, parts.gives->size(), words);
      if (!gives)
        throw error("'gives' needs a type Ovis knows: a whole number, a "
                    "decimal, text, a truth value, a record or a list of one.");
    }
    auto function = read_header(parts.header, words, true, gives);
    function.types[0] = p_trait.type();
    for (std::size_t index = 1; index < function.parameters.size(); ++index)
      if (function.same[index])
        function.types[index] = p_trait.type();
    function.has_body = p_line.opens_block();
    // `(a) comes before (b)`, `(a) is the same as (b)`, `text of (it)`:
    // the library's phrases and operators (`<`, `=`, `show`) are the
    // trait's, through the functions every impl gets
    // (`comes_before__circle`) and one for the trait itself.
    const auto builtin = builtin_trait_method(function);
    if (builtin) {
      const auto wanted = builtin->trait == "showable" ? value_kind{"str"}
                                                       : value_kind{"bool"};
      if (function.result != wanted)
        throw error("This phrase makes every " + p_trait.words.back() + " " +
                    builtin->trait + ": end it with 'gives " +
                    (builtin->trait == "showable" ? "text" : "a truth value") +
                    "'.");
      function.templates.clear();
    }
    if (!function.has_body) {
      for (std::size_t index = 1; index < function.parameters.size(); ++index)
        if (!function.types[index])
          throw error("Say the type of '" + function.parameters[index] +
                      "' in a phrase every " + p_trait.words.back() +
                      " must provide, as in (" + function.parameters[index] +
                      ": whole number).");
    }
    if (const auto [defined_at, added] =
            shared.defined.try_emplace(defined_key(function.name), where(p_line));
        !added)
      throw error("A phrase called '" + function.name + "' is already defined " +
                  defined_at->second + ".");
    std::ranges::copy(function.templates, std::back_inserter(words.templates));
    words.functions[function.name + core_suffix] = {.core = function.core,
                                                    .arity = function.parameters.size()};
    auto text = p_working.text;
    for (const auto &stop : {string_type(" gives "), string_type(":")})
      if (const auto at = text.find(stop); at != string_type::npos)
        text.erase(at);
    while (!text.empty() && text.back() == ' ')
      text.pop_back();
    shared.phrase_names[function.core] = text;

    if (function.has_body) {
      functions.emplace(p_line.line, function);
    } else {
      string_type needs =
          "(" + function.core + " (" + core_name(function.parameters[0]);
      for (std::size_t index = 1; index < function.parameters.size(); ++index)
        needs += " (" + core_name(function.parameters[index]) + " " +
                 (function.same[index] ? string_type("same")
                                       : function.types[index]->core()) +
                 ")";
      needs += ")" + (gives ? " " + gives->core() : string_type()) + ")";
      if (!trait_needs.try_emplace(p_line.line, needs).second)
        more_trait_needs.emplace(p_line.line, needs);
    }
    if (builtin)
      words.impls[builtin->trait][p_trait.name] =
          function.core + "__" + p_trait.name;
    shared.trait_methods[p_trait.name][function.core] = std::move(function);
  }

  // The built-in trait a trait's phrase is the phrase of: `comes before`
  // with two values of the trait, `is the same as`, `text of` with one.
  static auto builtin_trait_method(const user_function &p_function)
      -> std::optional<trait_method> {
    std::vector<std::optional<value_kind>> types;
    for (std::size_t index = 0; index < p_function.parameters.size(); ++index)
      types.push_back(index == 0 || p_function.same[index]
                          ? std::optional(value_kind{"record", 0, "it"})
                          : p_function.types[index]);
    return trait_method_of(p_function.core, types);
  }

  // The methods of `a circle is a shape:`: each is a phrase of the trait,
  // read as a function of the record.
  auto declare_impl(const source_line &p_line, const line_shape &p_shape)
      -> void {
    const auto [record, record_length] =
        words.record_at(p_line.tokens, p_shape.begin, p_shape.name_end);
    const auto [trait, trait_length] = words.record_at(
        p_line.tokens, p_shape.trait_begin, p_shape.trait_end);
    const auto index =
        static_cast<std::size_t>(&p_line - lines.data());
    if (record != nullptr && record->parameters.size() != p_shape.parameters.size()) {
      problem(p_line, record->parameters.empty()
                          ? "'" + record->name + "' holds no other types."
                          : "Write what " + record->type().english() +
                                " holds in brackets, as its header does.");
      return;
    }
    // `a whole number is hashable:` (generics.md §4): numbers and text
    // have a trait statically, for bounds and direct calls.
    const auto plain = record == nullptr
                           ? stated_type(p_line.tokens, p_shape.begin,
                                         p_shape.end, words)
                           : std::nullopt;
    if (plain && (!builtin_trait_of(p_shape).empty() || plain->depth > 0 ||
                  !(plain->is_number() || plain->base == "str" ||
                    plain->base == "bool"))) {
      const auto type = span_text(working[index], p_shape.begin, p_shape.end);
      const auto trait_text = span_text(
          working[index],
          p_shape.trait_begin - (builtin_trait_of(p_shape).empty() ? 1 : 0),
          p_shape.trait_end);
      if (!builtin_trait_of(p_shape).empty() && plain->depth == 0 &&
          (plain->is_number() || plain->base == "str"))
        problem(p_line, type + " is already " + trait_text + ".");
      else
        problem(p_line, "Only records, numbers, text and truth values can be " +
                            trait_text + ", not " + type +
                            ". Make a record that holds the value and give it "
                            "the trait: 'a wrapped value has:' with 'value (" +
                            type + ")', then 'a wrapped value is " + trait_text +
                            ":'.");
      return;
    }
    if (plain && (trait == nullptr || !trait->is_trait ||
                  trait_length != p_shape.trait_end - p_shape.trait_begin)) {
      problem(p_line, "No trait is called '" +
                          span_text(working[index], p_shape.trait_begin,
                                    p_shape.trait_end) +
                          "'. Declare it with 'a NAME can:'.");
      return;
    }
    if (plain && !owns(*trait)) {
      problem(p_line, orphan(span_text(working[index], p_shape.begin, p_shape.end)));
      return;
    }
    if (plain) {
      declare_methods(p_line, index, *trait, *plain, plain->core(), p_shape);
      plain_impls[p_line.line] = plain->core();
      return;
    }
    if (record == nullptr || record->is_trait ||
        record_length != p_shape.name_end - p_shape.begin) {
      problem(p_line, "No record is called '" +
                          span_text(working[index], p_shape.begin, p_shape.end) +
                          "'.");
      return;
    }
    if (!builtin_trait_of(p_shape).empty()) {
      if (!owns(*record)) {
        problem(p_line, orphan(span_text(working[index], p_shape.begin, p_shape.end)));
        return;
      }
      declare_builtin_impl(p_line, p_shape, record);
      return;
    }
    if (trait == nullptr || !trait->is_trait ||
        trait_length != p_shape.trait_end - p_shape.trait_begin) {
      problem(p_line, "No trait is called '" +
                          span_text(working[index], p_shape.trait_begin,
                                    p_shape.trait_end) +
                          "'. Declare it with 'a NAME can:'.");
      return;
    }
    if (!owns(*trait) && !owns(*record)) {
      problem(p_line, orphan(span_text(working[index], p_shape.begin, p_shape.end)));
      return;
    }
    words.trait_impls.emplace(trait->name, record->name);
    declare_methods(p_line, index, *trait, generic_receiver(*record, p_shape),
                    record->name, p_shape);
  }

  // The phrases of `a circle is a shape:` (or `a whole number is
  // hashable:`), each a function of the record or type `p_receiver`.
  auto declare_methods(const source_line &p_line, std::size_t index,
                       const record_info &p_trait, const value_kind &p_receiver,
                       const string_type &p_type, const line_shape &p_shape)
      -> void {
    const auto *const trait = &p_trait;
    const auto &methods = shared.trait_methods[trait->name];
    std::set<string_type> written;
    // The trait's phrases have its package's Core names: an impl's headers
    // are read with them, whoever writes the impl.
    const auto trait_package = trait->package.substr(0, trait->package.find('#'));
    words.core_suffix = trait_package == "." ? string_type()
                                             : package_core_suffix(trait_package);
    struct restore {
      const program_reader &reader;
      ~restore() { reader.enter(); }
    } restored{*this};
    for (auto at = index + 1; at < lines.size() && lines[at].depth > 0; ++at) {
      const auto &line = lines[at];
      if (line.depth > 1)
        continue;
      try {
        if (!line.opens_block())
          throw error("Write a phrase of '" + trait->words.back() +
                      "' as its header ending with ':', and what it does "
                      "indented under it.");
        const auto parts = method_line_of(line.tokens);
        if (parts.gives)
          throw error("Leave out 'gives' here: the trait says what the "
                      "phrase gives.");
        const auto variables = generic_impl_variables(p_shape);
        auto function =
            read_header(parts.header, words, true, std::nullopt, std::nullopt,
                        {}, as_scope(variables));
        function.type_parameters = variables;
        const auto declared = methods.find(function.core);
        if (declared == methods.end()) {
          string_type known;
          for (const auto &[name, method] : methods)
            known += (known.empty() ? "" : ", ") + shared.phrase_names[name];
          throw error("'" + trait->words.back() + "' has no phrase like this " +
                      "one. Its phrases are: " + known + ".");
        }
        if (declared->second.parameters.size() != function.parameters.size())
          throw error("This phrase takes " +
                      std::to_string(function.parameters.size()) +
                      " parameter(s), but the trait's takes " +
                      std::to_string(declared->second.parameters.size()) + ".");
        if (declared->second.same != function.same)
          throw error("Write the values of '" + trait->words.back() +
                      "' in this phrase as the trait does: the same "
                      "parameters with and without a type.");
        function.types[0] = p_receiver;
        for (std::size_t index = 1; index < function.parameters.size(); ++index)
          if (function.same[index])
            function.types[index] = p_receiver;
        function.same.clear();
        written.insert(function.core);
        functions.emplace(line.line, std::move(function));
      } catch (const error &p_error) {
        problem(line, p_error.what());
      }
    }
    // A trait's `comes before`, `is the same as` or `text of` makes its
    // records comparable, equatable or showable: by their own phrase, or
    // the trait's default through the trait's function.
    // A record writing it for itself (`a circle is comparable:`) has its
    // own function already.
    for (const auto &[name, method] : methods)
      if (const auto builtin = builtin_trait_method(method);
          builtin && (written.contains(name) || method.has_body) &&
          p_receiver.base == "record")
        words.impls[builtin->trait][p_type] =
            name + "__" + (written.contains(name) ? p_type : trait->name);
  }

  // The untyped parameters of the `to …:` line at `p_index` that its body
  // uses as numbers: next to a number with `* / % - < > <= >=`
  // (`total >= 40`, `price * 0.9`). `+` is left out: with text it joins.
  auto numeric_parameters(std::size_t p_index) const
      -> std::map<string_type, value_kind> {
    std::map<string_type, value_kind> result;
    std::set<string_type> names;
    const auto &header = lines[p_index].tokens;
    for (std::size_t at = 1; at + 1 < header.size(); ++at)
      if (header[at].is_symbol("(") && header[at + 1].kind == token::kind_type::word &&
          at + 2 < header.size() && header[at + 2].is_symbol(")"))
        names.insert(header[at + 1].text);
    static const std::set<string_view_type> operators = {"*", "/", "%", "-",
                                                         "<", ">", "<=", ">="};
    for (auto index = p_index + 1;
         index < lines.size() && lines[index].depth > 0; ++index) {
      const auto &tokens = lines[index].tokens;
      for (std::size_t at = 0; at + 2 < tokens.size(); ++at) {
        const auto numeric = [&](const token &p_token) {
          return p_token.kind == token::kind_type::number;
        };
        const auto named = [&](const token &p_token) {
          return p_token.kind == token::kind_type::word && names.contains(p_token.text);
        };
        if (tokens[at + 1].kind != token::kind_type::symbol ||
            !operators.contains(tokens[at + 1].text))
          continue;
        if (named(tokens[at]) && numeric(tokens[at + 2]))
          result[tokens[at].text] = value_kind{"num"};
        else if (numeric(tokens[at]) && named(tokens[at + 2]))
          result[tokens[at + 2].text] = value_kind{"num"};
      }
    }
    // Text and lists: a parameter in a place only the library's text
    // phrases have (`p in capitals`, `words of p`; not `+`, which joins a
    // number to text too) or only its
    // list ones (`for each x in p`, `item 2 of p`, `sort p`). A parameter
    // used both ways is left open.
    std::map<string_type, std::set<string_type>> uses;
    for (auto index = p_index + 1;
         index < lines.size() && lines[index].depth > 0; ++index) {
      const auto &tokens = lines[index].tokens;
      for (std::size_t at = 0; at < tokens.size(); ++at) {
        if (tokens[at].kind != token::kind_type::word ||
            !names.contains(tokens[at].text))
          continue;
        if (const auto kind = use_of(tokens, at))
          uses[tokens[at].text].insert(*kind);
      }
    }
    for (const auto &[name, kinds] : uses)
      if (kinds.size() == 1 && !result.contains(name))
        result[name] = *kinds.begin() == "text"   ? value_kind{"str"}
                       : *kinds.begin() == "texts" ? value_kind{"str", 1}
                                                   : value_kind{"any", 1};
    return result;
  }

  // "text", "list" or "texts" (a list of text) when the word at `p_at` is
  // used where only the library's phrases of that type read it.
  static auto use_of(const std::vector<token> &p_tokens, std::size_t p_at)
      -> std::optional<string_type> {
    const auto words_before = [&](std::initializer_list<string_view_type> p_words) {
      if (p_at < p_words.size())
        return false;
      auto at = p_at - p_words.size();
      for (const auto word : p_words)
        if (!p_tokens[at++].is_word(word))
          return false;
      return true;
    };
    const auto words_after = [&](std::initializer_list<string_view_type> p_words) {
      if (p_at + p_words.size() >= p_tokens.size())
        return false;
      auto at = p_at + 1;
      for (const auto word : p_words)
        if (!p_tokens[at++].is_word(word))
          return false;
      return true;
    };
    if (words_after({"in", "capitals"}) || words_after({"in", "small"}) ||
        words_after({"in", "upper"}) || words_after({"in", "lower"}) ||
        words_after({"in", "uppercase"}) || words_after({"in", "lowercase"}) ||
        words_after({"trimmed"}) || words_after({"reversed"}) ||
        words_after({"backwards"}) || words_after({"starts", "with"}) ||
        words_after({"begins", "with"}) || words_after({"ends", "with"}) ||
        words_after({"split", "by"}) || words_before({"words", "of"}) ||
        words_before({"words", "in"}) || words_before({"letters", "in"}) ||
        words_before({"characters", "in"}) ||
        words_before({"word", "count", "of"}) ||
        words_before({"character", "of"}) || words_before({"code", "of"}) ||
        (p_at >= 2 && words_before({"of"}) &&
         p_tokens[p_at - 2].kind == token::kind_type::number &&
         p_at >= 3 && p_tokens[p_at - 3].is_word("character")))
      return "text";
    if (words_after({"joined", "with"}))
      return "texts";
    if ((words_before({"in"}) && p_at >= 4 && p_tokens[0].is_word("for") &&
         p_tokens[1].is_word("each") && p_at + 2 == p_tokens.size() &&
         p_tokens.back().is_symbol(":")) ||
        words_before({"first", "item", "of"}) ||
        words_before({"last", "item", "of"}) ||
        words_before({"items", "in"}) ||
        (p_at == 1 && (p_tokens[0].is_word("sort") ||
                       p_tokens[0].is_word("reverse")) &&
         p_at + 1 == p_tokens.size()) ||
        (p_at >= 3 && words_before({"of"}) &&
         p_tokens[p_at - 2].kind == token::kind_type::number &&
         p_tokens[p_at - 3].is_word("item")))
      return "list";
    return std::nullopt;
  }

  // Phrases the program defines, and C functions it uses, are known on
  // every line, before and after their definition, in every file.
  auto declare() -> void {
    enter();
    std::size_t index = 0;
    for (const auto &line : lines) {
      const auto here = index++;
      if (line.depth != 0)
        continue;
      const auto shape = shape_of(line.tokens);
      if (shape.kind == line_shape::kind_type::use_c && shape.problem.empty())
        import_header(line, shape);
      if (shape.kind == line_shape::kind_type::impl_decl &&
          shape.problem.empty())
        declare_impl(line, shape);
      if (shape.kind != line_shape::kind_type::define)
        continue;
      try {
        const auto [header, result] = stated_result(line.tokens);
        declare_function(line,
                         read_header(header, words, false, result, std::nullopt,
                                     numeric_parameters(here)),
                         true);
      } catch (const error &p_error) {
        problem(line, p_error.what());
      }
    }
  }

  // `to checksum of (data) gives an int32:`: the header without its `gives
  // TYPE`, and the type, as a trait's phrases say it. A `gives` not followed
  // by a whole type to the end (or to `as NAME`) is the header's own word.
  auto stated_result(const std::vector<token> &p_tokens) const
      -> std::pair<std::vector<token>, std::optional<value_kind>> {
    auto end = p_tokens.size() - 1; // the ':'
    if (end >= 2 && p_tokens[end - 2].is_word("as"))
      end -= 2;
    int depth = 0;
    std::optional<std::size_t> gives;
    for (std::size_t index = 1; index < end; ++index) {
      depth += p_tokens[index].is_symbol("(")   ? 1
               : p_tokens[index].is_symbol(")") ? -1
                                                : 0;
      if (depth == 0 && p_tokens[index].is_word("gives"))
        gives = index;
    }
    if (!gives || *gives + 1 >= end)
      return {p_tokens, std::nullopt};
    auto begin = *gives + 1;
    if (begin + 1 < end && is_article(p_tokens[begin]))
      ++begin;
    const auto type = stated_type(p_tokens, begin, end, words);
    if (!type)
      return {p_tokens, std::nullopt};
    std::vector<token> header(p_tokens.begin(),
                              p_tokens.begin() + static_cast<std::ptrdiff_t>(*gives));
    header.insert(header.end(),
                  p_tokens.begin() + static_cast<std::ptrdiff_t>(end),
                  p_tokens.end());
    return {std::move(header), type};
  }

  // Makes a `to …:` phrase (or the phrase of `a book is comparable:`) known
  // everywhere. `p_text_from_line` takes the report's name from the line's
  // words after the first.
  auto declare_function(const source_line &p_line, user_function p_function,
                        bool p_text_from_line) -> void {
    auto &function = p_function;
    if (const auto [defined_at, added] =
            shared.defined.try_emplace(defined_key(function.name), where(p_line));
        !added) {
      problem(p_line, "A phrase called '" + function.name +
                          "' is already defined " + defined_at->second +
                          ". Name one of them with 'as NAME' before the ':'.");
      return;
    }
    if (function.implements)
      if (const auto record = words.records.find(function.implements->record);
          record != words.records.end() && !owns(record->second)) {
        auto spoken = record->second.words.empty() ? record->first : record->second.words.back();
        problem(p_line, orphan("'" + spoken + "'"));
        return;
      }
    std::ranges::copy(function.templates, std::back_inserter(words.templates));
    words.phrase_values.emplace(phrase_value_words(p_line.tokens), function.core);
    if (function.implements)
      words.impls[function.implements->trait][function.implements->record] =
          function.core;
    words.functions[function.name + core_suffix] = {.core = function.core,
                                                    .arity = function.parameters.size()};
    // Reports name it as its header says it: "pad (text) to (width)".
    const auto header = std::ranges::find(working, &p_line, &working_line::source);
    if (header != working.end()) {
      auto text = header->text;
      if (p_text_from_line)
        text = text.substr(text.find_first_of(" \t") + 1);
      while (!text.empty() && (text.back() == ':' || text.back() == ' '))
        text.pop_back();
      shared.phrase_names[function.core] = text;
    }
    functions.emplace(p_line.line, std::move(function));
  }

  // The built-in trait `a book is comparable:` is about, or empty.
  auto builtin_trait_of(const line_shape &p_shape) const -> string_type {
    const auto &name = p_shape.trait_name;
    return (name == "comparable" || name == "equatable" || name == "showable") &&
                   !words.records.contains(name)
               ? name
               : string_type();
  }

  // `a book is comparable:` and its one phrase, `(it) comes before (other):`:
  // the same as `to (a: book) comes before (b: book):`.
  auto declare_builtin_impl(const source_line &p_line, const line_shape &p_shape,
                            const record_info *p_record) -> void {
    const auto index = static_cast<std::size_t>(&p_line - lines.data());
    const auto trait = p_shape.trait_name;
    const auto example = trait == "comparable" ? "(it) comes before (other):"
                         : trait == "equatable" ? "(it) is the same as (other):"
                                                : "text of (it):";
    bool found = false;
    for (auto at = index + 1; at < lines.size() && lines[at].depth > 0; ++at) {
      const auto &line = lines[at];
      if (line.depth > 1)
        continue;
      try {
        if (!line.opens_block())
          throw error("Write the phrase as its header ending with ':', and "
                      "what it does indented under it: " + string_type(example));
        const auto parts = method_line_of(line.tokens);
        if (parts.gives)
          throw error("Leave out 'gives' here: " + trait + " says what the "
                      "phrase gives.");
        const auto variables = generic_impl_variables(p_shape);
        auto function = read_header(parts.header, words, false, std::nullopt,
                                    generic_receiver(*p_record, p_shape), {},
                                    as_scope(variables));
        function.type_parameters = variables;
        if (!function.implements || function.implements->trait != trait)
          throw error("A record that is " + trait + " has one phrase, written "
                      "like this: " + string_type(example));
        found = true;
        declare_function(line, std::move(function), false);
      } catch (const error &p_error) {
        problem(line, p_error.what());
      }
    }
    if (!found)
      problem(p_line, "Say how " + p_record->type().english() + " is " + trait +
                          ": write " + example + " under this line.");
  }

  // Each record's fields, once every file's records are known (a field may
  // hold another record).
  auto read_records() -> void {
    enter();
    // A field whose line can't be read yet is kept by its name and read
    // again once every record's fields are in: its default may be `a point
    // with …` of a record declared further down.
    struct retry {
      record_info *record;
      std::size_t slot;
      std::size_t line;
    };
    std::vector<retry> retries;
    for (const auto index : record_lines) {
      const auto &line = lines[index];
      auto &record = words.records.at(own_record(shape_of(line.tokens).name));
      // A type variable may not be a type already.
      for (const auto &parameter : record.parameters)
        if (words.records.contains(parameter))
          problem(line, "'" + parameter + "' is a " +
                            (words.records.at(parameter).is_trait ? "trait"
                                                                  : "record") +
                            "; a type variable needs a name of its own.");
      const auto variables = generic_scope(record);
      for (auto at = index + 1; at < lines.size() && lines[at].depth > 0; ++at) {
        const auto &field_line = lines[at];
        if (field_line.tokens.back().is_symbol(":")) {
          problem(field_line, "A field's line opens no block: remove the ':'.");
          continue;
        }
        if (field_line.depth > 1)
          continue;
        try {
          auto field = read_field(field_line.tokens);
          if (std::ranges::any_of(record.fields, [&](const record_field &p_other) {
                return lower(p_other.name) == lower(field.name);
              })) {
            problem(field_line, "'" + field.spoken() + "' is listed twice.");
            continue;
          }
          record.fields.push_back(std::move(field));
        } catch (const error &p_error) {
          if (field_line.tokens.size() > 1 &&
              field_line.tokens[0].kind == token::kind_type::word) {
            retries.push_back({&record, record.fields.size(), at});
            std::vector<string_type> name;
            for (const auto &item : field_line.tokens) {
              if (item.kind != token::kind_type::word)
                break;
              name.push_back(item.text);
            }
            record.fields.push_back(field_named(name));
          } else {
            problem(field_line, p_error.what());
          }
        }
      }
    }
    // Back to front, so removing a field that can't be read leaves the
    // places of the others.
    for (const auto &again : std::views::reverse(retries)) {
      try {
        const auto variables = generic_scope(*again.record);
        again.record->fields[again.slot] = read_field(lines[again.line].tokens);
      } catch (const error &p_error) {
        problem(lines[again.line], p_error.what());
        again.record->fields.erase(again.record->fields.begin() +
                                   static_cast<std::ptrdiff_t>(again.slot));
      }
    }
    for (const auto index : record_lines) {
      const auto &record = words.records.at(own_record(shape_of(lines[index].tokens).name));
      string_type form =
          (record.is_packed ? "(packed-record " : "(record ") + record.name;
      if (!record.parameters.empty()) {
        form += " (forall";
        for (const auto &parameter : record.parameters)
          form += " " + parameter;
        form += ")";
      }
      for (const auto &field : record.fields)
        form += " (" + field.name +
                (field.type.known() ? " " + field.type.core() : "") + ")";
      record_forms[lines[index].line] = form + ")";
    }
  }

  // `(forall item (key equatable showable)) `, or nothing for a phrase that
  // isn't generic.
  static auto forall_of(
      const std::vector<std::pair<string_type, std::vector<string_type>>> &p_variables)
      -> string_type {
    if (p_variables.empty())
      return "";
    string_type result = "(forall";
    for (const auto &[name, bounds] : p_variables) {
      if (bounds.empty()) {
        result += " " + name;
        continue;
      }
      result += " (" + name;
      for (const auto &bound : bounds)
        result += " " + bound;
      result += ")";
    }
    return result + ") ";
  }

  // An impl for every use of a generic record (`a stack of (comparable item)
  // is comparable:`): its type variables with their bounds, the record's
  // type of them, and the scope they are types in.
  auto generic_impl_variables(const line_shape &p_shape) const
      -> std::vector<std::pair<string_type, std::vector<string_type>>> {
    std::vector<std::pair<string_type, std::vector<string_type>>> result;
    for (std::size_t index = 0; index < p_shape.parameters.size(); ++index) {
      std::vector<string_type> bounds;
      for (const auto &bound : p_shape.bounds[index])
        bounds.push_back(bound == "numeric" ? "number" : bound);
      result.emplace_back(p_shape.parameters[index], std::move(bounds));
    }
    return result;
  }
  static auto generic_receiver(const record_info &p_record,
                               const line_shape &p_shape) -> value_kind {
    auto type = p_record.type();
    for (const auto &parameter : p_shape.parameters)
      type.arguments.push_back({"var", 0, parameter});
    return type;
  }
  static auto as_scope(
      const std::vector<std::pair<string_type, std::vector<string_type>>> &p_variables)
      -> std::map<string_type, std::vector<string_type>> {
    return {p_variables.begin(), p_variables.end()};
  }

  // While it lives, a generic record's or phrase's type variables are types
  // (`item`, `list of items`), with their bounds.
  struct type_variable_scope {
    vocabulary &words;
    std::map<string_type, std::vector<string_type>> saved;
    type_variable_scope(vocabulary &p_words,
                        std::map<string_type, std::vector<string_type>> p_variables)
        : words(p_words), saved(std::exchange(words.type_variables,
                                              std::move(p_variables))) {}
    type_variable_scope(const type_variable_scope &) = delete;
    auto operator=(const type_variable_scope &) = delete;
    ~type_variable_scope() { words.type_variables = std::move(saved); }
  };
  auto generic_scope(const record_info &p_record) -> type_variable_scope {
    std::map<string_type, std::vector<string_type>> variables;
    for (const auto &parameter : p_record.parameters)
      variables[parameter];
    return {words, std::move(variables)};
  }

  // Makes a header's functions and constants callable everywhere.
  auto import_header(const source_line &p_line, const line_shape &p_shape)
      -> void {
    if (!core::has_c_import_support()) {
      problem(p_line, "This build of Ovis can't read C headers: it was built "
                      "without libclang.");
      return;
    }
    try {
      const auto &library = core::import_c_library(p_shape.name, p_shape.library,
                                                   options.c_imports);
      for (const auto &function : library.functions)
        words.functions.try_emplace(
            function.name,
            callable{.core = function.name,
                     // A generic C macro's depends on the variant a call
                     // takes: Ovis Core works it out.
                     .result = function.is_generic_macro()
                                   ? value_kind{}
                                   : kind_of(core::c_result_type(function)),
                     .arity = function.is_variadic
                                  ? std::nullopt
                                  : std::optional(function.parameters.size()),
                     .roles = [&] {
                       string_type roles;
                       for (const auto role : function.macro_roles)
                         roles.push_back(role == core::c_function::macro_role::type ? 't'
                                         : role == core::c_function::macro_role::field
                                             ? 'f'
                                             : 'v');
                       return roles;
                     }()});
      for (const auto &constant : library.constants)
        words.constants.try_emplace(constant.name, kind_of(constant));
      // Its structs, as records: `t's tm_hour`, `a new z_stream`.
      for (const auto &record : library.records) {
        if (words.records.contains(record.name))
          continue;
        record_info info{.name = record.name, .words = {lower(record.name)}};
        for (const auto &field : record.fields) {
          record_field described = field_named({lower(field.name)});
          described.name = field.name;
          described.type = kind_of(field.type);
          info.fields.push_back(std::move(described));
        }
        words.records.emplace(record.name, std::move(info));
      }
      headers[p_shape.name] = library.header_hash;
      if (const auto found = previous.c_headers.find(p_shape.name);
          found != previous.c_headers.end() &&
          found->second != library.header_hash)
        result.warnings.push_back(
            file_prefix + "line " + std::to_string(p_line.line) + ": " +
            p_shape.name + " changed since the last build; check the program "
                           "still does what it should.");
    } catch (const error &p_error) {
      problem(p_line, p_error.what());
    }
  }

  // Who made the lockfile's readings: this build's model if it answered,
  // else whoever made the previous ones.
  auto finish_lockfile() -> void {
    auto &lock = result.lock;
    lock.c_headers = headers;
    lock.phrase_files = shared.extra.hashes;
    for (const auto &[name, hash] : shared.extra.hashes)
      if (const auto found = previous.phrase_files.find(name);
          found != previous.phrase_files.end() && found->second != hash)
        result.warnings.push_back(file_prefix + name + " changed since the last "
                                  "build; check the program still does what "
                                  "it should.");
    // Each meaning with the line it is for, made afresh (the lock starts as
    // the previous one); the readings it now carries stay, so rebuilding
    // changes neither file.
    lock.meanings.clear();
    for (const auto &[number, group] : physical_lines()) {
      const auto meaning = meaning_of(group);
      if (!meaning)
        continue;
      lock.meanings.push_back(*meaning);
      for (const auto &entry : previous.readings)
        if (meaning->line.find(entry.span) != string_type::npos &&
            meaning->meaning.find(entry.reading) != string_type::npos &&
            lock.find(entry.key) == nullptr)
          lock.readings.push_back(entry);
    }
    if (model_used) {
      lock.model = model->name();
      lock.model_fingerprint = model->fingerprint();
    } else if (!lock.readings.empty()) {
      lock.model = previous.model;
      lock.model_fingerprint = previous.model_fingerprint;
    }
    // In a fixed order, whatever order the questions came in.
    std::ranges::sort(lock.readings, {}, &reading_entry::key);
  }

  // Top-level lines as Ovis Core, with the source line each starts at.
  auto read() -> std::vector<std::pair<std::size_t, string_type>> {
    std::vector<std::pair<std::size_t, string_type>> units;
    enter();
    if (scoped) {
      for (const auto &[name, id] : aliases)
        qualifiers[id] = name;
      // Words two packages this file sees give phrases: a dependency's
      // phrase with them is read with its package's name.
      std::map<string_type, std::set<string_type>> owners;
      for (const auto &item : words.templates)
        if (!item.package.empty() && words.sees(item.package))
          owners[spoken_pattern(item, item.patterns.front())].insert(
              item.package.substr(0, item.package.find('#')));
      for (const auto &[spoken, packages] : owners)
        if (packages.size() > 1)
          shared_words.insert(spoken);
    }
    scope = shared.globals[whole_package()];
    for (const auto &[name, type] : scope)
      foreign.insert(name);
    std::size_t index = 0;
    while (index < lines.size()) {
      const auto line = lines[index].line;
      auto form = read_statement(index, 0);
      string_type text = line_marker(line);
      for (const auto &let : hoisted)
        text += let + "\n";
      hoisted.clear();
      text += form;
      if (!form.empty())
        units.emplace_back(line, std::move(text));
    }
    // Line 0: what is in scope when the file ends.
    if (here_for_phrases(0))
      find_phrases_for();
    shared.globals[whole_package()] = scope;
    return units;
  }

  // Whether the `Compiler:` notes under the line are not the ones this
  // build would write.
  static auto compiler_note_changed(const working_line &p_line) -> bool {
    if (!p_line.writes_compiler_note)
      return false;
    std::vector<string_type> written;
    for (const auto &note : p_line.source->annotations)
      if (note.kind == annotation::kind_type::compiler)
        written.push_back(note.text);
    if (p_line.compiler_note.empty())
      return !written.empty();
    return written != std::vector<string_type>{p_line.compiler_note};
  }

  // The working lines of each physical line: two for a one-line block.
  auto physical_lines() const
      -> std::map<std::size_t, std::vector<const working_line *>> {
    std::map<std::size_t, std::vector<const working_line *>> result;
    for (const auto &line : working)
      result[line.source->line].push_back(&line);
    return result;
  }

  // The physical line as written and as the compiler reads it, if the two
  // differ. Nothing for a line over several physical lines.
  static auto meaning_of(const std::vector<const working_line *> &p_group)
      -> std::optional<meaning_entry> {
    meaning_entry result;
    for (std::size_t index = 0; index < p_group.size(); ++index) {
      const auto &line = *p_group[index];
      if (line.original.empty())
        return std::nullopt;
      // The spaces between a one-line block's header and its body.
      const auto gap =
          index == 0 ? string_type()
                     : string_type(line.source->tokens.front().column -
                                       p_group[index - 1]->source->tokens.back().end_column,
                                   ' ');
      result.line += gap + line.original;
      result.meaning += gap + line.text;
    }
    if (result.line == result.meaning)
      return std::nullopt;
    return result;
  }

  // The source with each changed line's sigils rewritten above it: its
  // `@[meaning: …]`, then its `@[note: …]`. Old `Read as:` and `Compiler:`
  // lines under it go.
  // Each line of the source as read, by number, at its number in the
  // source as written (`written_source`): reports cite the file on disk.
  std::map<std::size_t, std::size_t> line_map;

  auto line_after(std::size_t p_line) const -> std::size_t {
    const auto found = line_map.find(p_line);
    return found == line_map.end() ? p_line : found->second;
  }

  auto written_source(string_view_type p_source) -> string_type {
    std::map<std::size_t, string_type> above;
    std::set<std::size_t> dropped;
    for (const auto &[number, group] : physical_lines()) {
      if (std::ranges::none_of(group, [](const working_line *p_line) {
            return p_line->decisions_changed || compiler_note_changed(*p_line);
          }))
        continue;
      const string_type indent(group.front()->source->tokens.front().column - 1,
                               ' ');
      string_type sigils;
      if (const auto meaning = meaning_of(group))
        sigils += indent + "@[meaning: " + meaning->meaning + "]\n";
      for (const auto *line : group) {
        for (const auto &note : line->source->annotations) {
          dropped.insert(note.line);
          // A note the compiler doesn't keep is the author's: it stays.
          if (note.kind == annotation::kind_type::compiler &&
              !line->writes_compiler_note)
            sigils += indent + "@[note: " + note.text + "]\n";
        }
        if (!line->compiler_note.empty())
          sigils += indent + "@[note: " + line->compiler_note + "]\n";
      }
      above[number] = std::move(sigils);
    }
    if (above.empty())
      return string_type(p_source);

    string_type output;
    std::size_t number = 0;
    std::size_t written = 0;
    for (std::size_t begin = 0; begin <= p_source.size(); ++number) {
      auto end = p_source.find('\n', begin);
      const auto last = end == string_view_type::npos;
      if (last)
        end = p_source.size();
      const auto text = p_source.substr(begin, end - begin);
      begin = end + 1;
      if (const auto found = above.find(number + 1); found != above.end()) {
        output += found->second;
        written += static_cast<std::size_t>(std::ranges::count(found->second, '\n'));
      }
      if (!dropped.contains(number + 1)) {
        output += string_type(text) + (last ? "" : "\n");
        line_map[number + 1] = ++written;
      }
      if (last)
        break;
    }
    return output;
  }
};

} // namespace

namespace {

// The text of a source line, trimmed, to quote in a report.
auto source_line_text(string_view_type p_source, std::size_t p_line)
    -> string_type {
  std::size_t begin = 0;
  for (std::size_t number = 1; number < p_line && begin != string_view_type::npos;
       ++number) {
    begin = p_source.find('\n', begin);
    if (begin != string_view_type::npos)
      ++begin;
  }
  if (begin == string_view_type::npos)
    return {};
  auto line = p_source.substr(begin, p_source.find('\n', begin) - begin);
  while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
    line.remove_prefix(1);
  while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
    line.remove_suffix(1);
  return string_type(line);
}

// A checker problem in the author's words: the names Ovis Core gave to
// phrases, built-in operations and renamed variables, as the program says
// them. Types are already in English.
auto in_authors_words(const string_type &p_message,
                      const std::map<string_type, string_type> &p_phrases)
    -> string_type {
  static const std::map<string_view_type, string_view_type> operations = {
      {"len", "length of"},
      {"get", "item … of"},
      {"append", "add … to"},
      {"to_str", "as text"},
      {"substring", "part of"},
      {"char_at", "character … of"},
      {"read_int", "ask for a whole number"},
      {"read_float", "ask for a decimal"},
      {"read_line", "ask for a line"},
      {"random", "a random number from … to"},
      {"print", "show"}};
  string_type result;
  std::size_t at = 0;
  while (at < p_message.size()) {
    const auto open = p_message.find('\'', at);
    const auto close =
        open == string_type::npos ? open : p_message.find('\'', open + 1);
    if (close == string_type::npos) {
      result += p_message.substr(at);
      break;
    }
    result += p_message.substr(at, open - at);
    auto name = p_message.substr(open + 1, close - open - 1);
    // Only a name in quotes: an apostrophe in "doesn't" pairs with nothing.
    if (name.empty() || !std::ranges::all_of(name, [](char_type p_character) {
          return std::isalnum(static_cast<unsigned char>(p_character)) ||
                 p_character == '_' || p_character == '.';
        })) {
      result += '\'';
      at = open + 1;
      continue;
    }
    if (const auto phrase = p_phrases.find(name); phrase != p_phrases.end())
      name = phrase->second;
    else if (name.starts_with("prelude_"))
      name = "a built-in phrase";
    else if (const auto operation = operations.find(name);
             operation != operations.end())
      name = string_type(operation->second);
    else if (name.size() > 1 && name.ends_with('_'))
      // A variable named like an Ovis Core word, renamed by `core_name`.
      name.pop_back();
    result += name == "a built-in phrase" ? name : "'" + name + "'";
    at = close + 1;
  }
  return result;
}

} // namespace

namespace {

// Nested internal/ folders (phrase-scope.md S4, Go's rule): what a file under
// `X/internal/` defines is seen only by the files under `X/`. The deepest
// internal/ folder above the file at `p_relative` (`pricing/internal`), if
// any: its package's `#pricing/internal` holds what the file defines.
auto internal_folder(const std::filesystem::path &p_relative) -> string_type {
  string_type folder;
  string_type deepest;
  for (const auto &part : p_relative.parent_path()) {
    folder += (folder.empty() ? "" : "/") + part.string();
    if (part == "internal")
      deepest = folder;
  }
  return deepest;
}

// What the file at `p_relative` of package `p_id` (`.` for the program's)
// sees of its own package: the package, and the internal/ folder of every
// folder above it.
auto own_sees(const string_type &p_id, const std::filesystem::path &p_relative)
    -> std::vector<string_type> {
  std::vector<string_type> result = {p_id, p_id + "#internal"};
  string_type folder;
  for (const auto &part : p_relative.parent_path()) {
    folder += (folder.empty() ? "" : "/") + part.string();
    result.push_back(p_id + "#" + folder + "/internal");
  }
  return result;
}

} // namespace

auto load_phrase_program(const std::filesystem::path &p_path)
    -> std::vector<phrase_file> {
  namespace fs = std::filesystem;
  const auto load = [](const fs::path &p_file, string_type p_name,
                       string_type p_module) {
    std::ifstream input(p_file, std::ios::binary);
    if (!input)
      throw error("Cannot read " + p_file.string() + ".");
    std::stringstream text;
    text << input.rdbuf();
    return phrase_file{p_file, std::move(p_name), std::move(p_module),
                       text.str(), lockfile::load(lockfile::path_for(p_file))};
  };
  std::error_code code;
  const bool is_folder = fs::is_directory(p_path, code);
  if (!is_folder && p_path.filename() != "main.ovis")
    return {load(p_path, "", "")};

  const auto root = is_folder ? p_path : p_path.parent_path();
  const auto main = root / "main.ovis";
  if (!fs::exists(main, code))
    throw error("The folder " + root.string() +
                " has no main.ovis, the file that says what the program "
                "does.");
  std::vector<fs::path> helpers;
  // A project's (ovis.toml) build/ and vendor/ are Ovis's, not its modules.
  const bool project = fs::exists(root / "ovis.toml", code);
  for (auto entry = fs::recursive_directory_iterator(root, code);
       entry != fs::recursive_directory_iterator(); entry.increment(code)) {
    const auto &path = entry->path();
    if (entry->is_directory()) {
      // Hidden folders, and other programs (folders with their own
      // main.ovis), are not part of this program.
      const auto relative = path.lexically_relative(root);
      if (path.filename().string().starts_with(".") ||
          fs::exists(path / "main.ovis", code) ||
          (project && (relative == "build" || relative == "vendor")))
        entry.disable_recursion_pending();
      continue;
    }
    if (path.extension() == ".ovis" && path != main)
      helpers.push_back(fs::relative(path, root));
  }
  std::ranges::sort(helpers);
  std::vector<phrase_file> result;
  for (const auto &relative : helpers) {
    auto module = relative;
    module.replace_extension();
    auto name = module.generic_string();
    std::ranges::replace(name, '/', '.');
    result.push_back(load(root / relative, relative.generic_string(), name));
  }
  result.push_back(load(main, "main.ovis", string_type(core::main_module)));
  // A program with an internal/ folder below its top reads scoped, so each
  // file sees only the internal/ folders above it; one at the top hides
  // nothing. The program's own package is `.` to the reader and empty here.
  if (std::ranges::any_of(helpers, [](const fs::path &p_relative) {
        const auto folder = internal_folder(p_relative);
        return !folder.empty() && folder != "internal";
      }))
    for (std::size_t index = 0; index < result.size(); ++index) {
      const auto relative = index < helpers.size() ? helpers[index] : fs::path("main.ovis");
      const auto folder = internal_folder(relative);
      result[index].package = folder.empty() ? string_type() : "#" + folder;
      result[index].sees = own_sees(".", relative);
    }
  return result;
}

auto load_package_files(const std::filesystem::path &p_root,
                        const string_type &p_id) -> std::vector<phrase_file> {
  namespace fs = std::filesystem;
  std::error_code code;
  std::vector<fs::path> modules;
  for (auto entry = fs::recursive_directory_iterator(p_root, code);
       entry != fs::recursive_directory_iterator(); entry.increment(code)) {
    const auto &path = entry->path();
    const auto relative = path.lexically_relative(p_root);
    if (entry->is_directory()) {
      const auto name = path.filename().string();
      if (name.starts_with(".") || fs::exists(path / "main.ovis", code) ||
          (relative == "build" || relative == "vendor"))
        entry.disable_recursion_pending();
      continue;
    }
    if (path.extension() == ".ovis" && relative != "main.ovis")
      modules.push_back(relative);
  }
  std::ranges::sort(modules);
  std::vector<phrase_file> result;
  for (const auto &relative : modules) {
    std::ifstream input(p_root / relative, std::ios::binary);
    if (!input)
      throw error("Cannot read " + (p_root / relative).string() + ".");
    std::stringstream text;
    text << input.rdbuf();
    auto module = relative;
    module.replace_extension();
    auto name = module.generic_string();
    std::ranges::replace(name, '/', '.');
    const auto folder = internal_folder(relative);
    // Its dependencies only where a file says `use NAME` (phrase-scope.md S2).
    result.push_back({.path = p_root / relative,
                      .name = p_id + "/" + relative.generic_string(),
                      .module = p_id + "." + name,
                      .text = text.str(),
                      .lock = lockfile::load(lockfile::path_for(p_root / relative)),
                      .locked = true,
                      .package = folder.empty() ? p_id : p_id + "#" + folder,
                      .sees = own_sees(p_id, relative)});
  }
  return result;
}

auto compile_phrase_program(const std::vector<phrase_file> &p_files,
                            english::language_model *p_model,
                            const front_end_options &p_options)
    -> program_result {
  program_result result;
  const auto start = clock_type::now();
  // The program's own phrase files: `--phrases` folders, and `phrases/` next
  // to a folder program's main.ovis.
  phrase_library extra;
  {
    auto directories = p_options.phrase_directories;
    for (const auto &file : p_files)
      if (file.module == "main" && !file.path.empty()) {
        const auto beside = file.path.parent_path() / "phrases";
        if (std::filesystem::is_directory(beside))
          directories.push_back(beside);
      }
    for (const auto &directory : directories) {
      try {
        auto library = load_phrase_library(directory);
        // A phrase worded exactly as another is a mistake: the reading would
        // be ambiguous on every line that uses it.
        for (const auto &added : library.templates)
          for (const auto &known : {std::cref(builtin_templates()),
                                    std::cref(extra.templates)})
            for (const auto &other : known.get())
              if (other.kind == added.kind && &other != &added &&
                  spoken_pattern(other, other.patterns.front()) ==
                      spoken_pattern(added, added.patterns.front()))
                throw error(added.file + ":" + std::to_string(added.line) +
                            ": '" +
                            spoken_pattern(added, added.patterns.front()) +
                            "' is already a phrase, in " + other.file + ":" +
                            std::to_string(other.line) + ".");
        std::ranges::move(library.templates, std::back_inserter(extra.templates));
        extra.prelude += library.prelude;
        extra.generics.merge(library.generics);
        extra.hashes.merge(library.hashes);
      } catch (const error &p_error) {
        result.problems.push_back(p_error.what());
        return result;
      }
    }
  }
  program_context shared(p_model, p_options, std::move(extra));
  std::vector<front_end_result> files(p_files.size());
  // Every file's phrases are known before any line is read.
  std::vector<std::optional<program_reader>> readers(p_files.size());
  for (std::size_t index = 0; index < p_files.size(); ++index) {
    try {
      readers[index].emplace(p_files[index], shared, files[index]);
    } catch (const lex_error &p_error) {
      const string_type text = p_error.what();
      files[index].diagnostics.push_back(
          {.line = p_error.line,
           .column = p_error.column,
           .kind = "syntax",
           .message = text.substr(text.find(": ") + 2)});
    }
  }
  for (auto &reader : readers)
    if (reader)
      reader->declare_traits();
  for (auto &reader : readers)
    if (reader)
      reader->declare();
  for (auto &reader : readers)
    if (reader)
      reader->read_records();
  // Ovis Core of each file, with the source line each unit starts at.
  std::vector<std::vector<std::pair<std::size_t, string_type>>> units(
      p_files.size());
  result.core_lines.resize(p_files.size());
  result.variables.resize(p_files.size());
  for (std::size_t index = 0; index < p_files.size(); ++index) {
    auto &file = files[index];
    file.source = p_files[index].text;
    file.lock = p_files[index].lock;
    if (!readers[index])
      continue;
    units[index] = readers[index]->read();
    result.variables[index] = std::move(readers[index]->variables_here);
    if (!readers[index]->phrases_found_type.empty()) {
      result.phrases_for_type = readers[index]->phrases_found_type;
      result.phrases_taking = std::move(readers[index]->phrases_found);
    }
    // Each Core line under the line it comes from, as read, even when the
    // build stops on a problem.
    for (const auto &[line, text] : units[index]) {
      const auto from = source_lines(text, line);
      std::istringstream core_text(text);
      std::size_t at = 1;
      for (string_type core_line; std::getline(core_text, core_line); ++at)
        if (!core_line.starts_with(";@"))
          result.core_lines[index][from[at]] += core_line + "\n";
    }
    readers[index]->finish_lockfile();
    file.source = readers[index]->written_source(p_files[index].text);
    if (p_options.sources_written)
      for (auto &item : file.diagnostics)
        item.line = readers[index]->line_after(item.line);
  }
  const auto prefix = [&](std::size_t p_file) {
    return p_files[p_file].name.empty() ? string_type()
                                        : p_files[p_file].name + ", ";
  };
  // A problem's text, as the build prints it.
  const auto add_problem = [&](diagnostic p_problem) {
    result.problems.push_back(
        p_problem.line == 0
            ? "prelude: " + p_problem.message
            : prefix(p_problem.file) + "line " + std::to_string(p_problem.line) +
                  (p_problem.kind == "syntax"
                       ? ", column " + std::to_string(p_problem.column)
                       : string_type()) +
                  ": " + p_problem.message);
    result.diagnostics.push_back(std::move(p_problem));
  };
  for (std::size_t index = 0; index < files.size(); ++index)
    for (auto &item : files[index].diagnostics) {
      item.file = index;
      add_problem(std::move(item));
    }
  for (auto &file : files) {
    std::ranges::move(file.warnings, std::back_inserter(result.warnings));
    result.sources.push_back(std::move(file.source));
    result.locks.push_back(std::move(file.lock));
    result.decisions.push_back(std::move(file.decisions));
  }
  result.timing = shared.timing;
  result.templates_used = shared.templates_used;
  result.timing.parse_ms =
      milliseconds_since(start) - result.timing.lex_ms - result.timing.model_ms;
  if (!result.succeeded())
    return result;

  const auto check_start = clock_type::now();
  if (p_options.progress)
    p_options.progress({.stage = build_progress::stage_type::checking});
  // For each unit: its file (none for the prelude) and source lines.
  struct origin {
    std::optional<std::size_t> file;
    std::vector<std::size_t> lines;
  };
  std::vector<origin> origins = {{}};
  // The `.generic` files' generics, a program's own first: they are used
  // instead of a built-in of the same name.
  string_type generics;
  for (const auto &[name, text] : shared.extra.generics)
    generics += text + "\n";
  for (const auto &[name, text] : builtin_generics())
    if (!shared.extra.generics.contains(name))
      generics += text + "\n";
  result.program.units.push_back(
      core::parse_unit(generics + builtin_prelude() + shared.extra.prelude));
  // In a program of several files every unit has a module, or modules
  // aren't resolved at all.
  if (!p_files.front().module.empty())
    result.program.units.back().module = "prelude";
  // A line of the source as read, at its number in the file as written.
  const auto written_line = [&](std::size_t p_file, std::size_t p_line) {
    return readers[p_file] && p_options.sources_written
               ? readers[p_file]->line_after(p_line)
               : p_line;
  };
  for (std::size_t index = 0; index < p_files.size(); ++index)
    for (const auto &[line, text] : units[index]) {
      // For working on Ovis: each line's Ovis Core as the front end wrote it.
      if (const auto *const dump = std::getenv("OVIS_DUMP_CORE");
          dump != nullptr && *dump != '\0')
        std::cerr << ";; line " << line << "\n" << text << "\n";
      try {
        result.program.units.push_back(core::parse_unit(text));
        result.program.units.back().module = p_files[index].module;
        origins.push_back({index, source_lines(text, line)});
        // For runtime errors: each Core line's line in the file as written.
        auto &mapped = result.program.units.back().source_lines;
        for (const auto &from : origins.back().lines)
          mapped.push_back(written_line(index, from));
      } catch (const error &p_error) {
        add_problem({.file = index,
                     .line = written_line(index, line),
                     .message = "Internal compiler error: " +
                                string_type(p_error.what()) +
                                "\n    Ovis Core: " + text});
      }
    }
  const auto report = [&](const core::diagnostic &p_problem) {
    const auto &where = origins[p_problem.unit_index];
    if (!where.file) {
      add_problem({.message = p_problem.message});
      return;
    }
    const auto &lines = where.lines;
    const auto line = p_problem.location.line < lines.size()
                          ? lines[p_problem.location.line]
                          : lines.front();
    add_problem({.file = *where.file,
                 .line = written_line(*where.file, line),
                 .message = in_authors_words(p_problem.message,
                                             shared.phrase_names) +
                            "\n    " +
                            source_line_text(p_files[*where.file].text, line)});
  };
  if (result.succeeded()) {
    // A field slot's phrase calls a copy for its record and field.
    for (const auto &problem : core::copy_field_generics(result.program))
      report(problem);
    for (const auto &problem : core::resolve_modules(result.program))
      report(problem);
    for (const auto &problem :
         core::resolve_c_imports(result.program, p_options.c_imports))
      report(problem);
  }
  if (result.succeeded()) {
    // What clang said about C macro calls in earlier builds: asked again
    // only for calls they don't have.
    core::c_macro_answers_used().clear();
    for (const auto &file : p_files)
      for (const auto &[key, answer] : file.lock.c_macros)
        core::c_macro_answers().try_emplace(key, answer);
    std::vector<core::diagnostic> unclear;
    auto notes = core::elaborate(result.program, &unclear);
    for (const auto &problem : unclear)
      report(problem);
    const auto problems = core::check(result.program, &notes);
    for (const auto &problem : problems)
      report(problem);
    // The strict memory rules, once the program type-checks (unit 0 is the
    // prelude, which is analysed but not reported).
    if (p_options.strict_memory && problems.empty())
      for (const auto &problem : core::check_memory(result.program, 1))
        report(problem);
    // What builds but is probably not meant, once the program is right.
    if (result.succeeded())
      for (const auto &problem : core::warn(result.program, 1)) {
        const auto &where = origins[problem.unit_index];
        if (!where.file)
          continue;
        const auto line = problem.location.line < where.lines.size()
                              ? where.lines[problem.location.line]
                              : where.lines.front();
        result.warnings.push_back(
            prefix(*where.file) + "line " +
            std::to_string(written_line(*where.file, line)) + ": " +
            in_authors_words(problem.message, shared.phrase_names));
      }
  }
  // Each file's lockfile keeps the answers about the headers it uses.
  for (auto &lock : result.locks) {
    lock.c_macros.clear();
    for (const auto &key : core::c_macro_answers_used())
      for (const auto &[header, hash] : lock.c_headers)
        if (key.starts_with(hash + " "))
          lock.c_macros[key] = core::c_macro_answers().at(key);
  }
  result.timing.check_ms = milliseconds_since(check_start);
  return result;
}

auto compile_phrases(string_view_type p_source, english::language_model *p_model,
                     const front_end_options &p_options) -> front_end_result {
  phrase_file file{.text = string_type(p_source)};
  if (p_options.previous != nullptr)
    file.lock = *p_options.previous;
  auto program = compile_phrase_program({file}, p_model, p_options);
  return {.program = std::move(program.program),
          .problems = std::move(program.problems),
          .diagnostics = std::move(program.diagnostics),
          .warnings = std::move(program.warnings),
          .source = std::move(program.sources.front()),
          .decisions = std::move(program.decisions.front()),
          .lock = std::move(program.locks.front()),
          .timing = program.timing,
          .templates_used = std::move(program.templates_used)};
}

auto phrases_like(const std::vector<string_type> &p_words, std::size_t p_limit)
    -> std::vector<std::pair<string_type, string_type>> {
  std::vector<std::pair<string_type, string_type>> result;
  const auto &templates = builtin_templates();
  if (p_words.empty()) {
    for (const auto &item : templates)
      result.emplace_back(item.file, spoken_pattern(item, item.patterns.front()));
    return result;
  }
  for (const auto &spoken : closest_templates(p_words, templates, p_limit))
    for (const auto &item : templates)
      if (std::ranges::any_of(item.patterns, [&](const pattern &p_pattern) {
            return spoken_pattern(item, p_pattern) == spoken;
          })) {
        result.emplace_back(item.file, spoken);
        break;
      }
  return result;
}

} // namespace ovis::phrase

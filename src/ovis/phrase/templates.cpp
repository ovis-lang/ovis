#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

#include <ovis/core/ast.hpp>
#include <ovis/core/sexpr.hpp>
#include <ovis/phrase/templates.hpp>

namespace ovis::phrase {

namespace {

constexpr string_view_type block_marker = "BLOCK";
const std::set<string_view_type> builtin_type_names = {
    "int", "float", "num", "str", "bool", "any", "type", "size", "record", "memory", "ptr", "map",
    "showable", "comparable", "equatable"};

auto trim(string_view_type p_text) -> string_view_type;

} // namespace

auto is_type_slot_name(string_view_type p_name) -> bool {
  return p_name == "type" || p_name == "size";
}

namespace {

auto trim(string_view_type p_text) -> string_view_type {
  while (!p_text.empty() && std::isspace(static_cast<unsigned char>(p_text.front())))
    p_text.remove_prefix(1);
  while (!p_text.empty() && std::isspace(static_cast<unsigned char>(p_text.back())))
    p_text.remove_suffix(1);
  return p_text;
}

auto lower(string_view_type p_text) -> string_type {
  string_type result(p_text);
  for (auto &character : result)
    character = static_cast<char_type>(std::tolower(static_cast<unsigned char>(character)));
  return result;
}

auto is_identifier(string_view_type p_text) -> bool {
  return !p_text.empty() &&
         (std::isalpha(static_cast<unsigned char>(p_text.front())) ||
          p_text.front() == '_') &&
         std::ranges::all_of(p_text, [](char_type p_character) {
           return std::isalnum(static_cast<unsigned char>(p_character)) ||
                  p_character == '_';
         });
}

// The position of the last `p_character` outside {}, [] and (), or npos.
auto find_outside(string_view_type p_text, char_type p_character, bool p_last)
    -> std::size_t {
  auto result = string_view_type::npos;
  int depth = 0;
  for (std::size_t index = 0; index < p_text.size(); ++index) {
    const auto character = p_text[index];
    if (character == '{' || character == '[' || character == '(')
      ++depth;
    else if (character == '}' || character == ']' || character == ')')
      --depth;
    else if (character == p_character && depth == 0) {
      result = index;
      if (!p_last)
        break;
    }
  }
  return result;
}

// Reads one template line and its aliases; `fail` reports at its line.
class template_reader {
  phrase_template &result;
  std::size_t line;
  // Slot types as written, resolved once every slot is known.
  std::vector<string_type> written_types;
  // While reading the result type, which may name a field slot.
  bool reading_result = false;

public:
  template_reader(phrase_template &p_result, std::size_t p_line)
      : result(p_result), line(p_line) {}

  [[noreturn]] auto fail(const string_type &p_message) const -> void {
    throw error(result.file + ":" + std::to_string(line) + ": " + p_message);
  }

  auto at_line(std::size_t p_line) -> void { line = p_line; }

  auto read_type(string_view_type p_text) const -> type_name {
    type_name type;
    auto text = trim(p_text);
    // `[TYPE] field of RECORD`
    if (const auto at = text.find("field of "); at != string_view_type::npos &&
        (at == 0 || text[at - 1] == ' ')) {
      type.kind = type_name::kind_type::field;
      type.name = at == 0 ? "any" : string_type(trim(text.substr(0, at)));
      type.record = string_type(trim(text.substr(at + 9)));
      if (!builtin_type_names.contains(type.name) || is_type_slot_name(type.name))
        fail("'" + type.name + "' is not a type a field can have.");
      if (!is_identifier(type.record))
        fail("'" + type.record + "' is not a record's type.");
      if (!std::isupper(static_cast<unsigned char>(type.record.front()))) {
        const auto slot = std::ranges::find(result.slots, type.record, &slot::name);
        if (slot == result.slots.end() ||
            trim(written_types[static_cast<std::size_t>(
                slot - result.slots.begin())]) != "type" &&
            trim(written_types[static_cast<std::size_t>(
                slot - result.slots.begin())]) != "size")
          fail("A field's record is a type variable such as T, or a slot of "
               "type `type`: '" + type.record + "' is neither.");
      }
      return type;
    }
    while (text.ends_with("[]")) {
      ++type.list_depth;
      text = trim(text.substr(0, text.size() - 2));
    }
    // `T:comparable`: a variable whose type must implement a trait.
    if (const auto colon = text.find(':'); colon != string_view_type::npos) {
      type.bound = string_type(trim(text.substr(colon + 1)));
      text = trim(text.substr(0, colon));
      if (text.empty() || !std::isupper(static_cast<unsigned char>(text.front())))
        fail("Only a type variable such as T can have a trait: '" +
             string_type(p_text) + "'.");
      if (!is_trait_name(type.bound) || type.bound == "any")
        fail("'" + type.bound + "' is not a trait. Traits are num, showable, "
             "comparable and equatable.");
    }
    type.name = string_type(text);
    if (!is_identifier(text))
      fail("'" + string_type(p_text) + "' is not a type.");
    if (builtin_type_names.contains(text)) {
      type.kind = type_name::kind_type::builtin;
      if (is_type_slot_name(text) && type.list_depth > 0)
        fail("A slot can take a type, but not a list of types.");
    } else if (std::isupper(static_cast<unsigned char>(text.front()))) {
      type.kind = type_name::kind_type::variable;
    } else {
      const auto slot = std::ranges::find(result.slots, type.name, &slot::name);
      const auto written =
          slot == result.slots.end()
              ? string_view_type()
              : trim(written_types[static_cast<std::size_t>(
                    slot - result.slots.begin())]);
      // A field slot's name gives its field's type, as a result.
      const auto is_field = written.ends_with("field of") ||
                            written.find("field of ") != string_view_type::npos;
      if (!is_type_slot_name(written) && !(is_field && reading_result))
        fail("Unknown type '" + type.name +
             "'. Types are int, float, num, str, bool, any, an uppercase "
             "variable such as T, or a slot of type `type`.");
      type.kind = type_name::kind_type::type_slot;
    }
    return type;
  }

  // Words only, for the inside of [] and ().
  auto read_words(string_view_type p_text) const -> pattern {
    pattern words;
    for (std::size_t index = 0; index < p_text.size();) {
      if (std::isspace(static_cast<unsigned char>(p_text[index]))) {
        ++index;
        continue;
      }
      auto end = index;
      while (end < p_text.size() &&
             !std::isspace(static_cast<unsigned char>(p_text[end])))
        ++end;
      const auto word = p_text.substr(index, end - index);
      if (word.find_first_of("{}[]()|") != string_view_type::npos)
        fail("Only words can go inside [] and (): '" + string_type(p_text) +
             "'.");
      words.push_back({.kind = pattern_element::kind_type::word,
                       .word = lower(word)});
      index = end;
    }
    if (words.empty())
      fail("Empty [] or () in the pattern.");
    return words;
  }

  // The closing bracket matching the one at `p_begin`.
  auto closing(string_view_type p_text, std::size_t p_begin) const
      -> std::size_t {
    const auto closer = p_text[p_begin] == '{'   ? '}'
                        : p_text[p_begin] == '[' ? ']'
                                                 : ')';
    const auto end = p_text.find(closer, p_begin);
    if (end == string_view_type::npos)
      fail(string_type("A '") + p_text[p_begin] + "' is never closed.");
    return end;
  }

  // A slot `{name: type}`; in an alias only `{name}`, naming a slot of the
  // main pattern.
  auto read_slot(string_view_type p_body, bool p_is_main,
                 std::vector<bool> &p_used) -> std::size_t {
    auto body = trim(p_body);
    const auto variadic = body.ends_with("...");
    if (variadic) {
      body = trim(body.substr(0, body.size() - 3));
      if (!body.ends_with(','))
        fail("Write a variadic slot as {name: type, ...}.");
      body = trim(body.substr(0, body.size() - 1));
    }
    const auto colon = body.find(':');
    const auto name = string_type(trim(body.substr(0, colon)));
    if (!is_identifier(name) || std::isupper(static_cast<unsigned char>(name[0])))
      fail("'" + name + "' is not a slot name: use lower case letters.");
    if (!p_is_main) {
      if (colon != string_view_type::npos || variadic)
        fail("Types go on the main pattern; write {" + name +
             "} in an alias.");
      const auto found = std::ranges::find(result.slots, name, &slot::name);
      if (found == result.slots.end())
        fail("The alias has a slot '" + name +
             "' that the main pattern doesn't.");
      const auto index =
          static_cast<std::size_t>(found - result.slots.begin());
      if (p_used[index])
        fail("The alias uses the slot '" + name + "' twice.");
      p_used[index] = true;
      return index;
    }
    if (colon == string_view_type::npos)
      fail("Give the slot '" + name + "' a type: {" + name + ": T}.");
    if (std::ranges::contains(result.slots, name, &slot::name))
      fail("Two slots are called '" + name + "'.");
    if (variadic && std::ranges::any_of(result.slots, &slot::is_variadic))
      fail("A pattern can have only one variadic slot.");
    result.slots.push_back({.name = name, .is_variadic = variadic});
    written_types.emplace_back(trim(body.substr(colon + 1)));
    return result.slots.size() - 1;
  }

  auto read_pattern(string_view_type p_text, bool p_is_main) -> pattern {
    pattern elements;
    std::vector<bool> used(result.slots.size(), false);
    for (std::size_t index = 0; index < p_text.size();) {
      const auto character = p_text[index];
      if (std::isspace(static_cast<unsigned char>(character))) {
        ++index;
      } else if (character == '{') {
        const auto end = closing(p_text, index);
        elements.push_back(
            {.kind = pattern_element::kind_type::slot,
             .slot = read_slot(p_text.substr(index + 1, end - index - 1),
                               p_is_main, used)});
        index = end + 1;
      } else if (character == '[' || character == '(') {
        const auto end = closing(p_text, index);
        const auto inside = p_text.substr(index + 1, end - index - 1);
        pattern_element element{.kind = character == '['
                                            ? pattern_element::kind_type::optional
                                            : pattern_element::kind_type::choice};
        if (character == '[') {
          element.branches.push_back(read_words(inside));
        } else {
          for (std::size_t begin = 0; begin <= inside.size();) {
            auto bar = inside.find('|', begin);
            if (bar == string_view_type::npos)
              bar = inside.size();
            element.branches.push_back(
                read_words(inside.substr(begin, bar - begin)));
            begin = bar + 1;
          }
          if (element.branches.size() < 2)
            fail("(" + string_type(inside) +
                 ") has one choice; use [ ] for words that may be left out.");
        }
        elements.push_back(std::move(element));
        index = end + 1;
      } else if (string_view_type("}])|").find(character) !=
                 string_view_type::npos) {
        fail(string_type("This '") + character + "' opens nothing.");
      } else {
        auto end = index;
        while (end < p_text.size() &&
               !std::isspace(static_cast<unsigned char>(p_text[end])) &&
               string_view_type("{}[]()|").find(p_text[end]) ==
                   string_view_type::npos)
          ++end;
        const auto word = lower(p_text.substr(index, end - index));
        index = end;
        // `decimal[s]`: the word, or the word with the suffix.
        if (index < p_text.size() && p_text[index] == '[') {
          const auto close = closing(p_text, index);
          const auto suffix = p_text.substr(index + 1, close - index - 1);
          if (!is_identifier(suffix))
            fail("Write an optional ending as word[ending]: '" + word + "[" +
                 string_type(suffix) + "]'.");
          elements.push_back(
              {.kind = pattern_element::kind_type::choice,
               .branches = {{{.word = word}}, {{.word = word + lower(suffix)}}}});
          index = close + 1;
        } else {
          elements.push_back({.word = word});
        }
      }
    }

    if (std::ranges::none_of(elements, [](const auto &p_element) {
          return p_element.kind == pattern_element::kind_type::word ||
                 p_element.kind == pattern_element::kind_type::choice;
        }))
      fail("A pattern needs at least one word that is always there.");
    // A type is written in words (`3 int32s`), so it may follow a value.
    const auto is_type_slot = [&](const pattern_element &p_element) {
      return p_element.slot < written_types.size() &&
             is_type_slot_name(written_types[p_element.slot]);
    };
    for (std::size_t index = 1; index < elements.size(); ++index)
      if (elements[index - 1].kind == pattern_element::kind_type::slot &&
          elements[index].kind == pattern_element::kind_type::slot &&
          !is_type_slot(elements[index]))
        fail("Put a word between two slots, or the parser can't tell where "
             "one ends.");
    if (!p_is_main && std::ranges::contains(used, false))
      fail("The alias leaves out the slot '" +
           result.slots[static_cast<std::size_t>(
                            std::ranges::find(used, false) - used.begin())]
               .name +
           "'.");
    return elements;
  }

  auto resolve_slot_types() -> void {
    for (std::size_t index = 0; index < result.slots.size(); ++index)
      result.slots[index].type = read_type(written_types[index]);
  }

  // Every `$name` names a slot, spliced with `...` only if variadic; every
  // slot is used; BLOCK appears exactly when the statement takes a block.
  auto check_core() -> void {
    std::vector<core::sexpr> forms;
    try {
      forms = core::read_sexprs(result.core);
    } catch (const core::syntax_error &p_error) {
      fail(string_type("The Ovis Core doesn't read: ") + p_error.what());
    }
    if (forms.size() != 1)
      fail("The Ovis Core must be one form: a template stands for exactly "
           "one.");

    std::vector<bool> used(result.slots.size(), false);
    bool has_block = false;
    std::function<void(const core::sexpr &)> visit =
        [&](const core::sexpr &p_form) {
      for (const auto &item : p_form.items)
        visit(item);
      if (!p_form.is_symbol())
        return;
      if (p_form.text == block_marker) {
        has_block = true;
        return;
      }
      // `$f` may be part of a symbol: `prelude_total_by:$f`.
      const auto dollar = p_form.text.find('$');
      if (dollar == string_type::npos)
        return;
      auto name = string_view_type(p_form.text).substr(dollar + 1);
      const auto spliced = name.ends_with("...");
      if (spliced)
        name.remove_suffix(3);
      const auto slot = std::ranges::find(result.slots, name, &slot::name);
      if (slot == result.slots.end())
        fail("The Ovis Core uses $" + string_type(name) +
             ", but the pattern has no such slot.");
      if (spliced != slot->is_variadic)
        fail(spliced ? "Only a variadic slot can be spliced: write $" +
                           string_type(name) + "."
                     : "The variadic slot " + string_type(name) +
                           " holds several values: write $" +
                           string_type(name) + "...");
      used[static_cast<std::size_t>(slot - result.slots.begin())] = true;
    };
    visit(forms.front());
    // A type slot may only decide other slots' types: `the {t: type} in
    // {xs: t[]}`.
    for (const auto &item : result.slots)
      if (item.type.kind == type_name::kind_type::type_slot ||
          item.type.kind == type_name::kind_type::field)
        for (std::size_t index = 0; index < used.size(); ++index)
          if (result.slots[index].name == item.type.name ||
              result.slots[index].name == item.type.record)
            used[index] = true;

    for (std::size_t index = 0; index < used.size(); ++index)
      if (!used[index])
        fail("The slot '" + result.slots[index].name +
             "' is never used in the Ovis Core.");
    if (has_block && !result.takes_block)
      fail("The Ovis Core uses BLOCK, but the pattern doesn't end with "
           "': BLOCK'.");
    if (!has_block && result.takes_block)
      fail("The pattern takes a block, but the Ovis Core never uses BLOCK.");
  }

  // Type variables in the result, and fields' records, must come from a
  // slot.
  auto check_result() const -> void {
    for (const auto &item : result.slots)
      if (item.type.kind == type_name::kind_type::field &&
          std::isupper(static_cast<unsigned char>(item.type.record.front())) &&
          std::ranges::none_of(result.slots, [&](const slot &p_slot) {
            return p_slot.type.kind == type_name::kind_type::variable &&
                   p_slot.type.name == item.type.record;
          }))
        fail("The field slot '" + item.name + "' is of " + item.type.record +
             ", which no other slot decides.");
    if (!result.result ||
        result.result->kind != type_name::kind_type::variable)
      return;
    if (std::ranges::none_of(result.slots, [&](const slot &p_slot) {
          return p_slot.type.kind == type_name::kind_type::variable &&
                 p_slot.type.name == result.result->name;
        }))
      fail("The result type " + result.result->name +
           " doesn't appear in any slot, so nothing decides it.");
  }

  // `expr PATTERN : TYPE => CORE` or `stmt PATTERN [: BLOCK] => CORE`.
  auto read_main(string_view_type p_text) -> void {
    const auto arrow = p_text.find("=>");
    if (arrow == string_view_type::npos)
      fail("Write the Ovis Core after '=>'.");
    result.core = string_type(trim(p_text.substr(arrow + 2)));
    if (result.core.empty())
      fail("Write the Ovis Core after '=>'.");
    auto left = trim(p_text.substr(0, arrow));

    const auto colon = find_outside(left, ':', true);
    const auto tail = colon == string_view_type::npos
                          ? string_view_type()
                          : trim(left.substr(colon + 1));
    if (colon != string_view_type::npos)
      left = trim(left.substr(0, colon));
    const auto is_expression =
        result.kind == phrase_template::kind_type::expression;
    if (is_expression && colon == string_view_type::npos)
      fail("An expr needs a result type: 'expr PATTERN : TYPE => CORE'.");
    if (is_expression && tail == block_marker)
      fail("Only a stmt can take a block.");
    if (!is_expression && colon != string_view_type::npos &&
        tail != block_marker)
      fail("A stmt gives back nothing; only ': BLOCK' may end its pattern.");
    result.takes_block = !is_expression && colon != string_view_type::npos;

    result.patterns.push_back(read_pattern(left, true));
    resolve_slot_types();
    if (is_expression) {
      reading_result = true;
      result.result = read_type(tail);
      reading_result = false;
      if (is_type_slot_name(result.result->name))
        fail("An expr gives back a value, not a type.");
    }
    check_result();
    if (result.core == "records")
      result.core.clear(), result.records_only = true;
    else
      check_core();
  }

  // `for records: CORE`: the Ovis Core for a record, which may use the same
  // slots.
  auto read_record_core(string_view_type p_text) -> void {
    result.record_core = string_type(trim(p_text));
    core::sexpr form;
    try {
      auto forms = core::read_sexprs(result.record_core);
      if (forms.size() != 1)
        fail("The Ovis Core after 'for records:' must be one form.");
      form = std::move(forms.front());
    } catch (const core::syntax_error &p_error) {
      fail(string_type("The Ovis Core doesn't read: ") + p_error.what());
    }
    if (std::ranges::none_of(result.slots, [](const slot &p_slot) {
          return is_trait_slot(p_slot.type);
        }))
      fail("'for records:' needs a slot typed by a trait, such as "
           "`comparable` or `T:equatable`.");
    std::function<void(const core::sexpr &)> visit =
        [&](const core::sexpr &p_form) {
      for (const auto &item : p_form.items)
        visit(item);
      if (!p_form.is_symbol())
        return;
      const auto dollar = p_form.text.find('$');
      if (dollar == string_type::npos)
        return;
      const auto name = string_view_type(p_form.text).substr(dollar + 1);
      if (std::ranges::find(result.slots, name, &slot::name) == result.slots.end())
        fail("The Ovis Core uses $" + string_type(name) +
             ", but the pattern has no such slot.");
    };
    visit(form);
  }

  auto read_about(string_view_type p_text) -> void {
    if (!result.about.empty())
      fail("A template has one 'about:' line.");
    result.about = string_type(trim(p_text));
    if (result.about.empty())
      fail("'about:' needs a sentence.");
  }

  auto read_example(string_view_type p_text) -> void {
    result.examples.emplace_back(trim(p_text));
    if (result.examples.back().empty())
      fail("'example:' needs a line of a program.");
  }

  auto read_alias(string_view_type p_text) -> void {
    result.patterns.push_back(read_pattern(trim(p_text), false));
  }
};

auto element_to_string(const phrase_template &p_template,
                       const pattern_element &p_element) -> string_type;

auto words_to_string(const phrase_template &p_template,
                     const pattern &p_pattern) -> string_type {
  string_type result;
  for (const auto &element : p_pattern) {
    if (!result.empty())
      result += ' ';
    result += element_to_string(p_template, element);
  }
  return result;
}

auto element_to_string(const phrase_template &p_template,
                       const pattern_element &p_element) -> string_type {
  switch (p_element.kind) {
  case pattern_element::kind_type::word:
    return p_element.word;
  case pattern_element::kind_type::slot: {
    const auto &slot = p_template.slots[p_element.slot];
    return "{" + slot.name + ": " + slot.type.to_string() +
           (slot.is_variadic ? ", ...}" : "}");
  }
  case pattern_element::kind_type::optional:
    return "[" + words_to_string(p_template, p_element.branches[0]) + "]";
  case pattern_element::kind_type::choice: {
    const auto &branches = p_element.branches;
    // `decimal[s]` is read as a choice of decimal and decimals.
    if (branches.size() == 2 && branches[0].size() == 1 &&
        branches[1].size() == 1 &&
        branches[1][0].word.starts_with(branches[0][0].word) &&
        branches[1][0].word.size() > branches[0][0].word.size())
      return branches[0][0].word + "[" +
             branches[1][0].word.substr(branches[0][0].word.size()) + "]";
    string_type result = "(";
    for (const auto &branch : branches)
      result += (result.size() > 1 ? "|" : "") + words_to_string(p_template, branch);
    return result + ")";
  }
  }
  return {};
}

} // namespace

auto is_trait_slot(const type_name &p_type) -> bool {
  return (p_type.kind == type_name::kind_type::builtin &&
          is_record_trait_name(p_type.name)) ||
         (p_type.kind == type_name::kind_type::variable && !p_type.bound.empty());
}

auto is_record_trait_name(string_view_type p_name) -> bool {
  return p_name == "showable" || p_name == "comparable" || p_name == "equatable";
}

auto is_trait_name(string_view_type p_name) -> bool {
  return p_name == "num" || p_name == "any" || is_record_trait_name(p_name);
}

auto type_name::to_string() const -> string_type {
  if (kind == kind_type::field)
    return (name == "any" ? "" : name + " ") + "field of " + record;
  auto result = bound.empty() ? name : name + ":" + bound;
  for (std::size_t depth = 0; depth < list_depth; ++depth)
    result += "[]";
  return result;
}

namespace {

// A slot as the model is shown it: what kind of value goes there.
auto slot_hint(const slot &p_slot) -> string_type {
  const auto &type = p_slot.type;
  string_type hint = "VALUE";
  if (type.kind == type_name::kind_type::field)
    hint = "FIELD";
  else if (is_type_slot_name(type.name))
    hint = "TYPE";
  else if (type.list_depth > 0)
    hint = "LIST";
  else if (type.name == "int" || type.name == "float" || type.name == "num")
    hint = "NUMBER";
  else if (type.name == "str")
    hint = "TEXT";
  else if (type.name == "bool")
    hint = "CONDITION";
  return p_slot.is_variadic ? hint + ", ..." : hint;
}

}  // namespace

auto spoken_pattern(const phrase_template &p_template, const pattern &p_pattern)
    -> string_type {
  string_type result;
  const auto add = [&](const string_type &p_part) {
    result += (result.empty() ? "" : " ") + p_part;
  };
  for (const auto &element : p_pattern) {
    switch (element.kind) {
    case pattern_element::kind_type::word:
      add(element.word);
      break;
    case pattern_element::kind_type::slot:
      add(slot_hint(p_template.slots[element.slot]));
      break;
    case pattern_element::kind_type::optional:
    case pattern_element::kind_type::choice: {
      // The first way of saying it is enough.
      string_type words;
      for (const auto &part : element.branches.front())
        words += (words.empty() ? "" : " ") + part.word;
      add(element.kind == pattern_element::kind_type::optional
              ? "[" + words + "]"
              : words);
      break;
    }
    }
  }
  return result;
}


auto to_string(const phrase_template &p_template, const pattern &p_pattern)
    -> string_type {
  return words_to_string(p_template, p_pattern);
}

auto read_templates(string_view_type p_text, string_view_type p_file)
    -> std::vector<phrase_template> {
  std::vector<phrase_template> result;
  std::optional<template_reader> reader;
  std::size_t number = 0;
  for (std::size_t begin = 0; begin <= p_text.size();) {
    auto end = p_text.find('\n', begin);
    if (end == string_view_type::npos)
      end = p_text.size();
    const auto line = p_text.substr(begin, end - begin);
    begin = end + 1;
    ++number;

    const auto content = trim(line);
    if (content.empty() || content.starts_with('#'))
      continue;
    if (std::isspace(static_cast<unsigned char>(line.front()))) {
      if (!reader || !(content.starts_with("alias:") ||
                       content.starts_with("about:") ||
                       content.starts_with("example:") ||
                       content.starts_with("for records:")))
        throw error(string_type(p_file) + ":" + std::to_string(number) +
                    ": Only 'alias:', 'about:', 'example:' and 'for "
                    "records:' lines are indented, under a template.");
      reader->at_line(number);
      if (content.starts_with("alias:"))
        reader->read_alias(content.substr(6));
      else if (content.starts_with("about:"))
        reader->read_about(content.substr(6));
      else if (content.starts_with("example:"))
        reader->read_example(content.substr(8));
      else
        reader->read_record_core(content.substr(12));
      continue;
    }

    auto &added = result.emplace_back();
    added.file = string_type(p_file);
    added.line = number;
    reader.emplace(added, number);
    const auto kind = content.substr(0, content.find_first_of(" \t"));
    if (kind != "expr" && kind != "stmt")
      reader->fail("A template starts with 'expr' or 'stmt'.");
    added.kind = kind == "expr" ? phrase_template::kind_type::expression
                                : phrase_template::kind_type::statement;
    reader->read_main(content.substr(kind.size()));
  }
  for (const auto &item : result)
    if (item.records_only && item.record_core.empty())
      throw error(item.file + ":" + std::to_string(item.line) +
                  ": '=> records' needs a 'for records:' line under it.");
  return result;
}

auto builtin_templates() -> const std::vector<phrase_template> & {
  static const auto templates = [] {
    std::vector<phrase_template> result;
    for (const auto &[file, text] : builtin_phrase_files())
      if (file.ends_with(".phr"))
        std::ranges::move(read_templates(text, file),
                          std::back_inserter(result));
    return result;
  }();
  return templates;
}

auto builtin_prelude() -> string_type {
  string_type result;
  for (const auto &[file, text] : builtin_phrase_files())
    if (file.ends_with(".core"))
      result += string_type(text) + "\n";
  return result;
}

namespace {

// The generic functions of one `.generic` file, added to `p_into`.
auto read_generics(string_view_type p_file, string_view_type p_text,
                   std::map<string_type, string_type> &p_into) -> void {
  for (const auto &form : core::read_sexprs(p_text)) {
    if (form.head() != "fn" || form.items.size() < 2 ||
        !(form.items[1].text.ends_with(":$T:$F") ||
          form.items[1].text.ends_with(":$T")))
      throw error(string_type(p_file) + ": a generic is (fn NAME:$T:$F "
                                        "(PARAMETERS) BODY...), or NAME:$T.");
    // As an Ovis Core generic (generics.md G6): `NAME:$T:$F` is `NAME
    // (forall T (field F))`, `(record $T)` is `(var T)`, and `$T`, `$F` in
    // the names it calls are those variables.
    const auto &name = form.items[1].text;
    const auto base = name.substr(0, name.find(":$"));
    auto text = form.to_string();
    const auto header = text.find(name);
    text.replace(header, name.size(),
                 base + (name.ends_with(":$F") ? " (forall T (field F))"
                                               : " (forall T)"));
    for (const auto &[from, to] :
         {std::pair<string_type, string_type>{"(record $T)", "(var T)"},
          {"$T", "T"},
          {"$F", "F"}})
      for (auto at = text.find(from); at != string_type::npos;
           at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    p_into[base] = text;
  }
}

auto fnv_hash_of(string_view_type p_text) -> string_type {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const auto character : p_text) {
    hash ^= static_cast<unsigned char>(character);
    hash *= 1099511628211ULL;
  }
  char buffer[17];
  std::snprintf(buffer, sizeof buffer, "%016llx",
                static_cast<unsigned long long>(hash));
  return buffer;
}

} // namespace

auto load_phrase_library(const std::filesystem::path &p_directory)
    -> phrase_library {
  namespace fs = std::filesystem;
  phrase_library result;
  std::vector<fs::path> files;
  for (const auto &entry : fs::directory_iterator(p_directory)) {
    const auto extension = entry.path().extension().string();
    if (entry.is_regular_file() &&
        (extension == ".phr" || extension == ".core" || extension == ".generic"))
      files.push_back(entry.path());
  }
  std::ranges::sort(files);
  for (const auto &path : files) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
      throw error("Cannot read " + path.string() + ".");
    std::stringstream buffer;
    buffer << input.rdbuf();
    const auto text = buffer.str();
    const auto name = p_directory.filename().string() + "/" +
                      path.filename().string();
    result.hashes[name] = fnv_hash_of(text);
    const auto extension = path.extension().string();
    if (extension == ".phr") {
      std::ranges::move(read_templates(text, name),
                        std::back_inserter(result.templates));
    } else if (extension == ".core") {
      try {
        core::parse_unit(text);
      } catch (const error &p_error) {
        throw error(name + ": " + p_error.what());
      }
      result.prelude += text + "\n";
    } else {
      read_generics(name, text, result.generics);
    }
  }
  return result;
}

auto builtin_generics() -> const std::map<string_type, string_type> & {
  static const auto generics = [] {
    std::map<string_type, string_type> result;
    for (const auto &[file, text] : builtin_phrase_files())
      if (file.ends_with(".generic"))
        read_generics(file, text, result);
    return result;
  }();
  return generics;
}

} // namespace ovis::phrase

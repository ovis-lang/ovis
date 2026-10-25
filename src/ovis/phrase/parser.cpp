#include <algorithm>
#include <cctype>
#include <set>
#include <tuple>

#include <ovis/core/checker.hpp>

#include <ovis/core/monomorphise.hpp>

#include "parser.hpp"

namespace ovis::phrase::internal {

auto milliseconds_since(clock_type::time_point p_start) -> double {
  return std::chrono::duration<double, std::milli>(clock_type::now() - p_start)
      .count();
}

auto lower(string_view_type p_text) -> string_type {
  string_type result(p_text);
  for (auto &character : result)
    character = static_cast<char_type>(
        std::tolower(static_cast<unsigned char>(character)));
  return result;
}

// Words are equal ignoring case and a plural ending: "decimal" is
// "Decimals".
auto same_word(string_view_type p_left, string_view_type p_right) -> bool {
  const auto left = lower(p_left);
  const auto right = lower(p_right);
  if (left == right)
    return true;
  // Short words have no plurals here: "as" isn't more than one "a".
  const auto &shorter = left.size() < right.size() ? left : right;
  const auto &longer = left.size() < right.size() ? right : left;
  return shorter.size() >= 3 &&
         (shorter + "s" == longer || shorter + "es" == longer);
}

auto is_article(const token &p_token) -> bool {
  return p_token.is_word("a") || p_token.is_word("an") ||
         p_token.is_word("the");
}

auto is_article_word(string_view_type p_word) -> bool {
  return p_word == "a" || p_word == "an" || p_word == "the";
}

// Text as written in the source and in Ovis Core: quoted, with escapes.
auto quote(string_view_type p_text) -> string_type {
  string_type result = "\"";
  for (const auto character : p_text) {
    if (character == '"' || character == '\\')
      result += '\\';
    if (character == '\n')
      result += "\\n";
    else if (character == '\t')
      result += "\\t";
    else
      result += character;
  }
  return result + "\"";
}

auto value_kind::core() const -> string_type {
  if (!exact.empty())
    return exact;
  // A map whose key and value types aren't known here: Ovis Core works
  // them out.
  if (base == "map" && depth == 0)
    return "?";
  if (base == "record" || base == "var") {
    auto result = "(" + string_type(base) + " " + record;
    for (const auto &argument : arguments)
      result += " " + (argument.known() ? argument.with_depth(argument.depth).core()
                                        : string_type("?"));
    result += ")";
    for (std::size_t level = 0; level < depth; ++level)
      result = "(list " + result + ")";
    return result;
  }
  auto result = base == "?" ? string_type("int") : base;
  for (std::size_t level = 0; level < depth; ++level)
    result += "[]";
  return result;
}

auto assignable(const value_kind &p_wanted, const value_kind &p_value) -> bool {
  return !p_wanted.known() || !p_value.known() || p_wanted == p_value ||
         // An array of C numbers where C keeps a pointer (`next_in`).
         (p_value.is_c_array() && p_wanted.depth == 0 &&
          (p_wanted.base == "ptr" || p_wanted.base == "str")) ||
         (p_wanted == value_kind{"float"} && p_value == value_kind{"int"}) ||
         // `nothing` where a record goes; Ovis Core checks it is `null`.
         (p_wanted.is_record() && p_value == value_kind{"ptr"});
}

auto vocabulary::common_trait(const std::vector<value_kind> &p_types) const
    -> std::optional<string_type> {
  for (const auto &[name, record] : records) {
    if (!record.is_trait)
      continue;
    if (std::ranges::all_of(p_types, [&](const value_kind &p_type) {
          return p_type.is_record() &&
                 (p_type.record == name ||
                  trait_impls.contains({name, p_type.record}));
        }))
      return name;
  }
  return std::nullopt;
}

auto in_use(const value_kind &p_type, const record_info &p_record,
            const value_kind &p_use) -> value_kind {
  if (p_record.parameters.empty())
    return p_type;
  // Its own type variables, inside its own phrases, stay.
  const auto bound = [&](const string_type &p_name) -> std::optional<value_kind> {
    const auto at = std::ranges::find(p_record.parameters, p_name);
    if (at == p_record.parameters.end())
      return std::nullopt;
    const auto index = static_cast<std::size_t>(at - p_record.parameters.begin());
    return index < p_use.arguments.size() ? p_use.arguments[index] : value_kind{};
  };
  if (p_type.base == "var") {
    const auto found = bound(p_type.record);
    if (!found)
      return p_type;
    if (!found->known())
      return {"?", p_type.depth};
    return found->with_depth(found->depth + p_type.depth);
  }
  auto result = p_type;
  for (auto &argument : result.arguments)
    argument = in_use(argument, p_record, p_use);
  // `node of item or nothing`: its Ovis Core, with the use's types.
  if (result.exact.find("(var ") != string_type::npos) {
    core::type_bindings bindings;
    bool all_known = true;
    for (std::size_t index = 0; index < p_record.parameters.size(); ++index) {
      const auto found = bound(p_record.parameters[index]);
      const auto type = found && found->known()
                            ? core::value_type::parse(
                                  core::read_sexprs(found->core()).front())
                            : std::nullopt;
      if (!type) {
        all_known = false;
        break;
      }
      bindings[p_record.parameters[index]] = *type;
    }
    const auto exact = core::value_type::parse(core::read_sexprs(result.exact).front());
    if (all_known && exact)
      result.exact = core::substitute(*exact, bindings).to_string();
    else
      result.exact.clear();
  }
  return result;
}

auto record_field::spoken() const -> string_type {
  string_type result;
  for (const auto &word : words)
    result += (result.empty() ? "" : " ") + word;
  return result.empty() ? name : result;
}

auto field_named(const std::vector<string_type> &p_words) -> record_field {
  record_field result{.words = p_words};
  for (const auto &word : p_words)
    result.name += (result.name.empty() ? "" : "_") + word;
  return result;
}

auto record_info::find(string_view_type p_word) const -> const record_field * {
  std::vector<string_type> written;
  for (std::size_t begin = 0; begin < p_word.size();) {
    auto end = p_word.find(' ', begin);
    if (end == string_view_type::npos)
      end = p_word.size();
    written.emplace_back(p_word.substr(begin, end - begin));
    begin = end + 1;
  }
  for (const auto &field : fields)
    if (same_word(field.name, p_word) ||
        (field.words.size() > 1 && written.size() == field.words.size() &&
         std::ranges::equal(written, field.words, [](const auto &p_left,
                                                      const auto &p_right) {
           return same_word(p_left, p_right);
         })))
      return &field;
  return nullptr;
}

// Whether the tokens at `p_position` are the field's words.
auto is_field_at(const record_field &p_field, const std::vector<token> &p_tokens,
                 std::size_t p_position, std::size_t p_end) -> std::size_t {
  if (p_position < p_end && p_tokens[p_position].kind == token::kind_type::word &&
      same_word(p_tokens[p_position].text, p_field.name))
    return 1;
  const auto length = p_field.words.size();
  if (length < 2 || p_position + length > p_end)
    return 0;
  for (std::size_t index = 0; index < length; ++index)
    if (p_tokens[p_position + index].kind != token::kind_type::word ||
        !same_word(p_tokens[p_position + index].text, p_field.words[index]))
      return 0;
  return length;
}

auto record_info::field_at(const std::vector<token> &p_tokens,
                           std::size_t p_position, std::size_t p_end) const
    -> std::pair<const record_field *, std::size_t> {
  std::pair<const record_field *, std::size_t> best = {nullptr, 0};
  for (const auto &field : fields)
    if (const auto length = is_field_at(field, p_tokens, p_position, p_end);
        length > best.second)
      best = {&field, length};
  return best;
}

auto vocabulary::field_lengths(const std::vector<token> &p_tokens,
                               std::size_t p_position, std::size_t p_end) const
    -> std::vector<std::size_t> {
  std::set<std::size_t, std::greater<>> lengths;
  for (const auto &[name, record] : records)
    for (const auto &field : record.fields)
      if (const auto length = is_field_at(field, p_tokens, p_position, p_end))
        lengths.insert(length);
  return {lengths.begin(), lengths.end()};
}

auto vocabulary::is_field_word(string_view_type p_word) const -> bool {
  static const std::set<string_type> function_words = {
      "of", "the", "a", "an", "in", "on", "to", "for", "by", "with", "and"};
  if (function_words.contains(lower(p_word)))
    return false;
  for (const auto &[name, record] : records)
    for (const auto &field : record.fields)
      for (const auto &word : field.words.empty()
                                  ? std::vector<string_type>{field.name}
                                  : field.words)
        if (same_word(word, p_word))
          return true;
  return false;
}

auto vocabulary::record_at(const std::vector<token> &p_tokens,
                           std::size_t p_position, std::size_t p_end) const
    -> std::pair<const record_info *, std::size_t> {
  std::pair<const record_info *, std::size_t> best = {nullptr, 0};
  for (const auto &[name, record] : records) {
    const auto length = record.words.size();
    if (p_position + length > p_end || length <= best.second || !sees(record.package))
      continue;
    bool matches = true;
    for (std::size_t index = 0; index < length && matches; ++index)
      matches = p_tokens[p_position + index].kind == token::kind_type::word &&
                same_word(p_tokens[p_position + index].text,
                          record.words[index]);
    if (matches)
      best = {&record, length};
  }
  return best;
}

auto vocabulary::is_field(string_view_type p_word) const -> bool {
  return std::ranges::any_of(records, [&](const auto &p_record) {
    return p_record.second.find(p_word) != nullptr;
  });
}

auto vocabulary::implementation(string_view_type p_trait,
                                const value_kind &p_type) const
    -> const string_type * {
  if (!p_type.is_record())
    return nullptr;
  const auto trait = impls.find(string_type(p_trait));
  if (trait == impls.end())
    return nullptr;
  const auto found = trait->second.find(p_type.record);
  return found == trait->second.end() ? nullptr : &found->second;
}

auto vocabulary::implements(string_view_type p_trait,
                            const value_kind &p_type) const -> bool {
  if (!p_type.known() || p_trait == "any")
    return true;
  // A type variable reads wherever a trait goes: Ovis Core checks its
  // bounds once, and says which one is missing.
  if (p_type.is_variable())
    return true;
  if (p_trait == "num")
    return p_type.is_number();
  // Anything but a C pointer can be shown: a record as it is written, or
  // by the `text of` its author gave it, and a list of such values.
  if (p_trait == "showable")
    return p_type.base != "ptr" && p_type.base != "memory";
  if (p_type.depth > 0)
    return false;
  if (p_type.base == "int" || p_type.base == "float" || p_type.base == "str")
    return true;
  if (p_trait == "equatable" && p_type.base == "bool")
    return true;
  if (assume_record_traits && p_type.is_record() &&
      (p_trait == "comparable" || p_trait == "equatable"))
    return true;
  return implementation(p_trait, p_type) != nullptr;
}

auto trait_method_of(string_view_type p_name,
                     const std::vector<std::optional<value_kind>> &p_types)
    -> std::optional<trait_method> {
  static const std::map<string_view_type, std::pair<string_view_type, std::size_t>>
      methods = {{"comes_before", {"comparable", 2}},
                 {"is_same_as", {"equatable", 2}},
                 {"text_of", {"showable", 1}}};
  const auto found = methods.find(p_name);
  if (found == methods.end() || p_types.size() != found->second.second)
    return std::nullopt;
  for (const auto &type : p_types)
    if (!type || !type->is_record() || type->record != p_types.front()->record)
      return std::nullopt;
  return trait_method{string_type(found->second.first), p_types.front()->record};
}

auto value_kind::english() const -> string_type {
  if (depth == 0 && !exact.empty())
    if (const auto type = core::value_type::parse(core::read_sexprs(exact).front()))
      return core::describe_in_english(*type);
  if (base == "record" || base == "var")
    if (const auto type = core::value_type::parse(core::read_sexprs(core()).front()))
      return core::describe_in_english(*type);
  static const std::map<string_type, std::pair<string_type, string_type>>
      names = {{"int", {"a whole number", "whole numbers"}},
               {"float", {"a decimal", "decimals"}},
               {"str", {"text", "texts"}},
               {"bool", {"true or false", "truth values"}},
               {"ptr", {"a C pointer", "C pointers"}},
               {"memory", {"a memory", "memories"}},
               {"map", {"a map", "maps"}},
               {"void", {"nothing", "nothing"}},
               {"?", {"not known yet", "values"}}};
  const auto &[one, many] = names.at(base);
  if (depth == 0)
    return one;
  string_type result;
  for (std::size_t level = 1; level < depth; ++level)
    result += "a list of lists of ";
  return (result.empty() ? "a list of " : result) + many;
}

auto kind_of(const core::value_type &p_type) -> value_kind {
  if (p_type == core::value_type::nothing())
    return {"void"};
  if (!p_type.is_known())
    return {};
  if (p_type.base == core::base_type::record) {
    value_kind result{"record", p_type.is_list ? std::size_t(1) : 0,
                      p_type.record};
    for (const auto &argument : p_type.record_arguments())
      result.arguments.push_back(kind_of(argument));
    return result;
  }
  if (p_type.base == core::base_type::variable)
    return {"var", p_type.is_list ? std::size_t(1) : 0, p_type.record};
  auto name = p_type.base == core::base_type::map
                  ? string_type("map")
                  : core::value_type{p_type.base}.to_string();
  value_kind result{name, p_type.is_list ? std::size_t(1) : 0};
  if (p_type.is_typed_pointer() || p_type.is_c_array() || p_type.is_map())
    result.exact = p_type.to_string();
  return result;
}

auto kind_of(const core::c_constant &p_constant) -> value_kind {
  switch (p_constant.kind) {
  case core::c_constant::kind_type::integer:
    return {"int"};
  case core::c_constant::kind_type::real:
    return {"float"};
  case core::c_constant::kind_type::null:
    return {"ptr"};
  case core::c_constant::kind_type::runtime:
    // stdin, stdout and stderr are FILE pointers; the rest are numbers.
    return {p_constant.name.starts_with("std") ? "ptr" : "int"};
  }
  return {};
}

auto core_name(const string_type &p_written) -> string_type {
  // A name of several words (`fan speed`) is one identifier.
  auto p_name = p_written;
  std::ranges::replace(p_name, ' ', '_');
  static const auto reserved = [] {
    std::set<string_type> names = {
        "int",  "integer", "i64",     "float",   "real", "double", "number",
        "f64",  "bool",    "boolean", "str",     "string", "text", "void",
        "nothing", "none", "ptr",     "pointer", "any",  "true",   "false",
        "main"};
    for (const auto keyword : core::core_keywords())
      names.emplace(keyword);
    for (const auto name : core::builtin_names())
      names.emplace(name);
    for (const auto &function : core::libc_functions())
      names.insert(function.name);
    return names;
  }();
  return reserved.contains(p_name) || p_name.starts_with("prelude_")
             ? p_name + "_"
             : p_name;
}

auto add_libc(vocabulary &p_vocabulary) -> void {
  for (const auto &function : core::libc_functions())
    p_vocabulary.functions.try_emplace(
        function.name,
        callable{.core = function.name,
                 .result = kind_of(core::c_result_type(function)),
                 .arity = function.is_variadic
                              ? std::nullopt
                              : std::optional(function.parameters.size())});
  // Ovis Core's own numeric operations, as C would have them.
  for (const auto &[name, result, arity] :
       std::initializer_list<std::tuple<string_view_type, value_kind, std::size_t>>{
           {"abs", {}, 1},
           {"min", {}, 2},
           {"max", {}, 2},
           {"sqrt", {"float"}, 1},
           {"pow", {"float"}, 2},
           {"floor", {"float"}, 1}})
    p_vocabulary.functions.try_emplace(
        string_type(name),
        callable{.core = string_type(name), .result = result, .arity = arity});
  for (const auto name : {"null", "NULL", "EOF", "RAND_MAX", "CLOCKS_PER_SEC",
                          "stdin", "stdout", "stderr", "pi", "M_PI"})
    if (const auto *constant = core::find_c_constant(name))
      p_vocabulary.constants.try_emplace(name, kind_of(*constant));
}

namespace {

// A child's reading inside its parent's: bracketed where the grouping
// would otherwise be unclear, so readings that differ only in grouping
// read differently.
auto nested_reading(const parse &p_child, bool p_parent_is_phrase)
    -> string_type {
  const auto bracket =
      p_parent_is_phrase ? p_child.shape == parse::shape_type::operation
                         : p_child.shape == parse::shape_type::phrase;
  return bracket ? "(" + p_child.reading + ")" : p_child.reading;
}

struct operator_info {
  int level;
  string_view_type core;
};

auto operator_at(const token &p_token) -> std::optional<operator_info> {
  if (p_token.kind == token::kind_type::word) {
    if (p_token.is_word("or"))
      return operator_info{or_level, "or"};
    if (p_token.is_word("and"))
      return operator_info{and_level, "and"};
    if (p_token.is_word("mod"))
      return operator_info{product_level, "%"};
    return std::nullopt;
  }
  if (p_token.kind != token::kind_type::symbol)
    return std::nullopt;
  static const std::map<string_view_type, operator_info> symbols = {
      {"=", {comparison_level, "="}},  {"==", {comparison_level, "="}},
      {"!=", {comparison_level, "!="}}, {"<", {comparison_level, "<"}},
      {">", {comparison_level, ">"}},   {"<=", {comparison_level, "<="}},
      {">=", {comparison_level, ">="}}, {"+", {sum_level, "+"}},
      {"-", {sum_level, "-"}},          {"*", {product_level, "*"}},
      {"/", {product_level, "/"}},      {"%", {product_level, "%"}}};
  const auto found = symbols.find(p_token.text);
  return found == symbols.end() ? std::nullopt
                                : std::optional(found->second);
}

// A record shown by the `text of` its author gave it, or a list of such
// records shown as `[a, b]`: the Ovis Core that gives the text.
auto shown_by_author(const vocabulary &p_vocabulary, const value_kind &p_type,
                     const string_type &p_core) -> std::optional<string_type> {
  if (const auto *const text = p_vocabulary.implementation("showable", p_type))
    return "(" + *text + " " + p_core + ")";
  if (p_type.depth == 1 && p_type.base == "record" &&
      p_vocabulary.implementation("showable", p_type.element()) != nullptr)
    return "(prelude_list_text " + p_core + ")";
  return std::nullopt;
}

auto operation_type(string_view_type p_operator, const value_kind &p_left,
                    const value_kind &p_right, const vocabulary &p_vocabulary)
    -> std::optional<value_kind> {
  const auto number_like = [](const value_kind &p_type) {
    return p_type.is_number() || !p_type.known();
  };
  if (p_operator == "and" || p_operator == "or") {
    const auto truth = [](const value_kind &p_type) {
      return !p_type.known() || (p_type.base == "bool" && p_type.depth == 0);
    };
    if (truth(p_left) && truth(p_right))
      return value_kind{"bool"};
    return std::nullopt;
  }
  if (p_operator == "=" || p_operator == "!=" || p_operator == "<" ||
      p_operator == ">" || p_operator == "<=" || p_operator == ">=") {
    const auto ordered = p_operator != "=" && p_operator != "!=";
    // Records compare by the phrases their author wrote.
    if (p_left == p_right &&
        p_vocabulary.implementation(ordered ? "comparable" : "equatable", p_left))
      return value_kind{"bool"};
    // Text and C pointers compare with `null`: `while line != null`.
    const auto pointer_like = [](const value_kind &p_type) {
      return p_type.depth == 0 && (p_type.base == "ptr" || p_type.base == "str");
    };
    if (!ordered && pointer_like(p_left) && pointer_like(p_right) &&
        (p_left.base == "ptr" || p_right.base == "ptr"))
      return value_kind{"bool"};
    // `x = nothing` reads as `x is nothing`: a record compares with the
    // `nothing` literal (typed as a pointer) as the phrases do.
    const auto record_or_nothing = [](const value_kind &p_record,
                                      const value_kind &p_other) {
      return p_record.is_record() && p_other.depth == 0 && p_other.base == "ptr";
    };
    if (!ordered && (record_or_nothing(p_left, p_right) ||
                     record_or_nothing(p_right, p_left)))
      return value_kind{"bool"};
    if ((number_like(p_left) && number_like(p_right)) ||
        (p_left == p_right && (!ordered || p_left.base == "str")))
      return value_kind{"bool"};
    // A generic's type variable (generics.md §3): Ovis Core checks it has
    // the trait.
    if ((p_left.is_variable() || p_right.is_variable()) &&
        (p_left == p_right || number_like(p_left) || number_like(p_right)))
      return value_kind{"bool"};
    return std::nullopt;
  }
  // Adding anything to text joins them as text.
  const auto joinable = [&](const value_kind &p_type) {
    return (p_type.depth == 0 &&
            (p_type.base != "record" ||
             p_vocabulary.implementation("showable", p_type) != nullptr)) ||
           shown_by_author(p_vocabulary, p_type, {}).has_value();
  };
  if (p_operator == "+" &&
      ((p_left.base == "str" && p_left.depth == 0) ||
       (p_right.base == "str" && p_right.depth == 0)) &&
      joinable(p_left) && joinable(p_right))
    return value_kind{"str"};
  // Arithmetic on a type variable's values gives one; Ovis Core checks it
  // is a number.
  if (p_left.is_variable() || p_right.is_variable()) {
    const auto &variable = p_left.is_variable() ? p_left : p_right;
    const auto &other = p_left.is_variable() ? p_right : p_left;
    if (other == variable || number_like(other))
      return variable;
    return std::nullopt;
  }
  if (!number_like(p_left) || !number_like(p_right))
    return std::nullopt;
  if (p_operator == "/" || p_left.base == "float" || p_right.base == "float")
    return value_kind{"float"};
  if (p_left.base == "int" && p_right.base == "int")
    return value_kind{"int"};
  return value_kind{};
}

// The Ovis Core of `left OPERATOR right`: the operator, or for records the
// functions their authors wrote (`comes before`, `is the same as`, `text of`).
auto operation_core(string_view_type p_operator, const parse &p_left,
                    const parse &p_right, const vocabulary &p_vocabulary)
    -> string_type {
  const auto call = [](const string_type &p_function, const string_type &p_first,
                       const string_type &p_second) {
    return "(" + p_function + " " + p_first + " " + p_second + ")";
  };
  const auto &left = p_left.core;
  const auto &right = p_right.core;
  if (p_left.type == p_right.type && p_left.type.is_record()) {
    if (p_operator == "=" || p_operator == "!=")
      if (const auto *const same = p_vocabulary.implementation("equatable", p_left.type)) {
        const auto result = call(*same, left, right);
        return p_operator == "=" ? result : "(not " + result + ")";
      }
    if (const auto *const before = p_vocabulary.implementation("comparable", p_left.type)) {
      if (p_operator == "<")
        return call(*before, left, right);
      if (p_operator == ">")
        return call(*before, right, left);
      if (p_operator == "<=")
        return "(not " + call(*before, right, left) + ")";
      if (p_operator == ">=")
        return "(not " + call(*before, left, right) + ")";
    }
  }
  const auto shown = [&](const parse &p_part) {
    if (p_operator == "+")
      if (const auto text =
              shown_by_author(p_vocabulary, p_part.type, p_part.core))
        return *text;
    return p_part.core;
  };
  return call(string_type(p_operator), shown(p_left), shown(p_right));
}

} // namespace

// Type names written in words, for slots of type `type`.
namespace {

auto pointer_type_words(const std::vector<token> &p_tokens,
                        std::size_t p_position, std::size_t p_end,
                        const vocabulary *p_vocabulary)
    -> std::vector<std::pair<std::size_t, value_kind>>;
auto number_type_words(const std::vector<token> &p_tokens,
                       std::size_t p_position, std::size_t p_end)
    -> std::vector<std::pair<std::size_t, value_kind>>;

} // namespace

auto generic_type_words(const record_info &p_record,
                        const std::vector<token> &p_tokens,
                        std::size_t p_position, std::size_t p_end,
                        const vocabulary *p_vocabulary)
    -> std::vector<std::pair<std::size_t, value_kind>> {
  std::vector<std::pair<std::size_t, value_kind>> result;
  if (p_record.parameters.empty())
    return result;
  // Each way the header's words read from here: how far, and the types.
  std::vector<std::pair<std::size_t, std::vector<value_kind>>> ways = {
      {p_position, {}}};
  for (const auto &word : p_record.header) {
    std::vector<std::pair<std::size_t, std::vector<value_kind>>> next;
    for (const auto &[at, types] : ways) {
      if (!word.empty()) {
        if (at < p_end && p_tokens[at].kind == token::kind_type::word &&
            same_word(p_tokens[at].text, word))
          next.emplace_back(at + 1, types);
        continue;
      }
      // An article may come before a type: `a pair of a book and text`.
      auto begin = at;
      if (begin + 1 < p_end && is_article(p_tokens[begin]))
        ++begin;
      for (const auto &[length, type] :
           type_words(p_tokens, begin, p_end, p_vocabulary)) {
        auto more = types;
        more.push_back(type);
        next.emplace_back(begin + length, std::move(more));
      }
    }
    ways = std::move(next);
  }
  for (auto &[at, types] : ways)
    if (types.size() == p_record.parameters.size()) {
      auto type = p_record.type();
      type.arguments = std::move(types);
      result.emplace_back(at - p_position, std::move(type));
    }
  return result;
}

auto type_words(const std::vector<token> &p_tokens, std::size_t p_position,
                std::size_t p_end, const vocabulary *p_vocabulary)
    -> std::vector<std::pair<std::size_t, value_kind>> {
  static const std::vector<std::pair<std::vector<string_view_type>, string_view_type>>
      names = {{{"whole", "number"}, "int"},   {{"integer"}, "int"},
               {{"decimal", "number"}, "float"}, {{"decimal"}, "float"},
               {{"text"}, "str"},               {{"word"}, "str"},
               {{"truth", "value"}, "bool"},    {{"memory"}, "memory"}};
  std::vector<std::pair<std::size_t, value_kind>> result;
  // `list of whole numbers`, `lists of lists of text`: a list of any type.
  if (p_position + 2 < p_end &&
      p_tokens[p_position].kind == token::kind_type::word &&
      same_word(p_tokens[p_position].text, "list") &&
      p_tokens[p_position + 1].is_word("of"))
    for (const auto &[length, element] :
         type_words(p_tokens, p_position + 2, p_end, p_vocabulary)) {
      auto list = element.with_depth(element.depth + 1);
      // A list of bytes, or of arrays of them, keeps their size.
      if (element.is_c_number() || element.exact.starts_with("(list "))
        list.exact = "(list " + element.exact + ")";
      result.emplace_back(length + 2, list);
    }
  for (const auto &[words, base] : names) {
    if (p_position + words.size() > p_end)
      continue;
    bool matches = true;
    for (std::size_t index = 0; index < words.size(); ++index)
      matches = matches && p_tokens[p_position + index].kind ==
                               token::kind_type::word &&
                same_word(p_tokens[p_position + index].text, words[index]);
    if (matches)
      result.emplace_back(words.size(), value_kind{string_type(base)});
  }
  if (p_vocabulary != nullptr) {
    if (const auto [record, length] =
            p_vocabulary->record_at(p_tokens, p_position, p_end);
        record != nullptr) {
      result.emplace_back(length, record->type());
      // `stack of books`, `table from text to whole numbers`: its header's
      // words with a type for each of its type variables.
      for (auto &[header_length, type] :
           generic_type_words(*record, p_tokens, p_position + length, p_end,
                              p_vocabulary))
        result.emplace_back(length + header_length, std::move(type));
    }
    // A type variable of the generic being read: `item`, `items`.
    if (p_position < p_end && p_tokens[p_position].kind == token::kind_type::word)
      for (const auto &[name, bounds] : p_vocabulary->type_variables)
        if (same_word(p_tokens[p_position].text, name))
          result.emplace_back(1, value_kind{"var", 0, name});
  }
  for (auto &number : number_type_words(p_tokens, p_position, p_end))
    result.push_back(std::move(number));
  for (auto &pointer : pointer_type_words(p_tokens, p_position, p_end, p_vocabulary))
    result.push_back(std::move(pointer));
  // `node or nothing`: a record that may be nothing (references.md §3).
  const auto count = result.size();
  for (std::size_t index = 0; index < count; ++index) {
    const auto [length, type] = result[index];
    const auto at = p_position + length;
    if (type.is_record() && type.exact.empty() && at + 1 < p_end &&
        p_tokens[at].is_word("or") && p_tokens[at + 1].is_word("nothing")) {
      auto maybe = type;
      maybe.exact = "(maybe " + type.core() + ")";
      result.emplace_back(length + 2, maybe);
    }
  }
  return result;
}

namespace {

// Types of number written in words (numbers.md §2): `int32`, `byte`,
// `float32`, C's `short` and `long` at Ovis's sizes, `unsigned short`,
// `32-bit whole number`, `unsigned 8-bit integer`, `32-bit decimal`.
auto number_type_words(const std::vector<token> &p_tokens,
                       std::size_t p_position, std::size_t p_end)
    -> std::vector<std::pair<std::size_t, value_kind>> {
  std::vector<std::pair<std::size_t, value_kind>> result;
  const auto word = [&](std::size_t p_at, string_view_type p_word) {
    return p_at < p_end && p_tokens[p_at].kind == token::kind_type::word &&
           same_word(p_tokens[p_at].text, p_word);
  };
  static const std::vector<std::pair<string_view_type, string_view_type>>
      numbers = {{"int8", "i8"},     {"uint8", "u8"},     {"byte", "u8"},
                 {"int16", "i16"},   {"uint16", "u16"},   {"int32", "i32"},
                 {"uint32", "u32"},  {"int64", "int"},    {"uint64", "u64"},
                 {"float32", "f32"}, {"float64", "float"}};
  const auto add_number = [&](std::size_t p_length, string_view_type p_core) {
    const bool is_real = p_core == "f32" || p_core == "float";
    value_kind number{is_real ? "float" : "int"};
    if (p_core != "int" && p_core != "float")
      number.exact = string_type(p_core);
    result.emplace_back(p_length, number);
  };
  for (const auto &[name, core] : numbers)
    if (word(p_position, name))
      add_number(1, core);
  // C's names, at Ovis's sizes (numbers.md B4): `short`, `long`, `unsigned
  // short`, `signed byte`.
  const bool is_unsigned = word(p_position, "unsigned");
  const bool is_signed = word(p_position, "signed");
  const auto named_at = p_position + (is_unsigned || is_signed ? 1 : 0);
  static const std::vector<std::tuple<string_view_type, string_view_type,
                                      string_view_type>>
      c_names = {{"short", "i16", "u16"}, {"long", "int", "u64"},
                 {"byte", "i8", "u8"}};
  for (const auto &[name, signed_core, unsigned_core] : c_names)
    if ((is_unsigned || is_signed || name != "byte") && word(named_at, name))
      add_number(named_at + 1 - p_position,
                 is_unsigned || (name == "byte" && !is_signed) ? unsigned_core
                                                               : signed_core);
  // `32-bit whole number`, `unsigned 8-bit integer`, `32-bit decimal`.
  if (named_at < p_end && p_tokens[named_at].kind == token::kind_type::number) {
    const auto &bits = p_tokens[named_at].text;
    auto at = named_at + 1;
    if (at < p_end && p_tokens[at].is_symbol("-"))
      ++at;
    if (word(at, "bit")) {
      ++at;
      const bool whole = word(at, "whole") && word(at + 1, "number");
      const auto whole_length = whole ? 2 : word(at, "integer") ? 1 : 0;
      static const std::map<string_view_type, std::pair<string_view_type, string_view_type>>
          sizes = {{"8", {"i8", "u8"}},
                   {"16", {"i16", "u16"}},
                   {"32", {"i32", "u32"}},
                   {"64", {"int", "u64"}}};
      if (const auto found = sizes.find(bits);
          whole_length > 0 && found != sizes.end())
        add_number(at + whole_length - p_position,
                   is_unsigned ? found->second.second : found->second.first);
      const auto decimal_length = word(at, "decimal") ? (word(at + 1, "number") ? 2 : 1) : 0;
      if (decimal_length > 0 && !is_unsigned && !is_signed &&
          (bits == "32" || bits == "64")) {
        add_number(at + 1 - p_position, bits == "32" ? "f32" : "float");
        if (decimal_length == 2)
          add_number(at + 2 - p_position, bits == "32" ? "f32" : "float");
      }
    }
  }
  return result;
}

// The C types of references.md as written: C numbers (`int32`, `byte`),
// `pointer`, `pointer to an int32`, `pointer to many bytes`, `C pointer to
// a T`, `array of float32s`, `phrase (whole number, text) giving a truth
// value` (or `action`, `function`, `method`, `procedure`, `routine`), `C
// phrase (int32) giving int32`.
auto pointer_type_words(const std::vector<token> &p_tokens,
                        std::size_t p_position, std::size_t p_end,
                        const vocabulary *p_vocabulary)
    -> std::vector<std::pair<std::size_t, value_kind>> {
  std::vector<std::pair<std::size_t, value_kind>> result;
  if (p_position >= p_end || p_tokens[p_position].kind != token::kind_type::word)
    return result;
  const auto word = [&](std::size_t p_at, string_view_type p_word) {
    return p_at < p_end && p_tokens[p_at].kind == token::kind_type::word &&
           same_word(p_tokens[p_at].text, p_word);
  };
  // What a pointer points at, after `to [a]` or `to many`.
  const auto pointees = [&](std::size_t p_at) {
    std::vector<std::pair<std::size_t, value_kind>> found;
    if (p_at < p_end && is_article(p_tokens[p_at]))
      ++p_at;
    for (auto &[length, type] : type_words(p_tokens, p_at, p_end, p_vocabulary))
      found.emplace_back(p_at + length, type);
    return found;
  };
  // `pointer`, `pointer to (a|many) T`, `C pointer to T`.
  const bool is_c = word(p_position, "c") && word(p_position + 1, "pointer");
  const auto pointer_at = p_position + (is_c ? 1 : 0);
  if (word(pointer_at, "pointer")) {
    if (!is_c)
      result.emplace_back(1, value_kind{"ptr"});
    if (word(pointer_at + 1, "to")) {
      const bool many = word(pointer_at + 2, "many");
      for (const auto &[end, type] : pointees(pointer_at + 2 + (many ? 1 : 0))) {
        if (type.depth > 0 && !type.is_c_number() && type.exact.empty())
          continue;
        value_kind pointer{"ptr"};
        if (type.is_record()) {
          // A record is a pointer to its struct already.
          if (!many) {
            result.emplace_back(end - p_position, type);
            continue;
          }
          pointer.exact = "(many (struct " + type.record + "))";
        } else {
          pointer.exact = string_type(is_c ? "(cptr " : many ? "(many " : "(ptr ") +
                          type.core() + ")";
        }
        result.emplace_back(end - p_position, pointer);
      }
    }
  }
  // `map of text to whole numbers` (or `dictionary`): keys and values.
  if ((word(p_position, "map") || word(p_position, "dictionary")) &&
      word(p_position + 1, "of"))
    for (auto to = p_position + 3; to + 1 < p_end; ++to) {
      if (!word(to, "to"))
        continue;
      std::optional<value_kind> key;
      // Keys and values may be numbers of any size, or text.
      for (const auto &[length, type] :
           type_words(p_tokens, p_position + 2, to, p_vocabulary))
        if (p_position + 2 + length == to && (!key || type.is_c_number()))
          key = type;
      if (!key ||
          (key->base != "int" && key->base != "float" && key->base != "str") ||
          key->depth > 0)
        continue;
      for (const auto &[length, type] :
           type_words(p_tokens, to + 1, p_end, p_vocabulary)) {
        if (type.base == "void")
          continue;
        value_kind map{"map"};
        map.exact = "(map " + key->core() + " " + type.core() + ")";
        result.emplace_back(to + 1 + length - p_position, map);
      }
    }
  // `array of bytes`: a list kept as C numbers.
  if (word(p_position, "array") && word(p_position + 1, "of"))
    for (const auto &[length, type] :
         type_words(p_tokens, p_position + 2, p_end, p_vocabulary))
      if (type.is_c_number()) {
        value_kind array{type.base, 1};
        array.exact = "(list " + type.exact + ")";
        result.emplace_back(length + 2, array);
      }
  // `phrase (whole number, text) giving a truth value`.
  const bool is_c_phrase = word(p_position, "c");
  const auto phrase_at = p_position + (is_c_phrase ? 1 : 0);
  if (phrase_at < p_end &&
      (word(phrase_at, "phrase") || word(phrase_at, "action") ||
       word(phrase_at, "function") || word(phrase_at, "method") ||
       word(phrase_at, "procedure") || word(phrase_at, "routine")) &&
      phrase_at + 1 < p_end && p_tokens[phrase_at + 1].is_symbol("(")) {
    auto close = phrase_at + 2;
    while (close < p_end && !p_tokens[close].is_symbol(")"))
      ++close;
    if (close == p_end)
      return result;
    // The parameters, split at commas, each one whole type.
    std::vector<value_kind> parameters;
    bool readable = true;
    for (auto begin = phrase_at + 2; begin < close && readable;) {
      auto comma = begin;
      while (comma < close && !p_tokens[comma].is_symbol(","))
        ++comma;
      auto at = begin;
      if (at < comma && is_article(p_tokens[at]))
        ++at;
      std::optional<value_kind> whole;
      for (const auto &[length, type] : type_words(p_tokens, at, comma, p_vocabulary))
        if (at + length == comma)
          whole = type;
      if (!whole)
        readable = false;
      else
        parameters.push_back(*whole);
      begin = comma + 1;
    }
    if (!readable)
      return result;
    const auto signature = [&](const value_kind &p_result) {
      if (!is_c_phrase) {
        std::vector<core::value_type> types;
        for (const auto &parameter : parameters)
          types.push_back(*core::value_type::parse(
              core::read_sexprs(parameter.core()).front()));
        return core::phrase_type(
                   types, p_result.base == "void"
                              ? core::value_type::nothing()
                              : *core::value_type::parse(
                                    core::read_sexprs(p_result.core()).front()))
            .to_string();
      }
      const auto c_name = [](const value_kind &p_type) -> string_type {
        if (p_type.is_c_number())
          return p_type.exact;
        return p_type.base == "void" ? "void" : p_type.base;
      };
      string_type text = "(c-phrase (";
      for (std::size_t index = 0; index < parameters.size(); ++index)
        text += (index > 0 ? " " : "") + c_name(parameters[index]);
      return text + ") " + c_name(p_result) + ")";
    };
    try {
      value_kind phrase{"ptr"};
      phrase.exact = signature(value_kind{"void"});
      result.emplace_back(close + 1 - p_position, phrase);
      if (word(close + 1, "giving") || word(close + 1, "gives"))
        for (const auto &[end, type] : pointees(close + 2)) {
          value_kind giving{"ptr"};
          giving.exact = signature(type);
          result.emplace_back(end - p_position, giving);
        }
    } catch (const error &) {
    }
  }
  return result;
}

auto accepts(const type_name &p_wanted, const value_kind &p_value,
             std::map<string_type, value_kind> &p_variables,
             const phrase_template &p_template, const slot_fills &p_fills,
             const vocabulary &p_vocabulary) -> bool {
  if (!p_value.known())
    return true;
  if (p_wanted.kind == type_name::kind_type::variable) {
    if (p_value.depth < p_wanted.list_depth)
      return false;
    const auto element = p_value.with_depth(p_value.depth - p_wanted.list_depth);
    if (!p_wanted.bound.empty() && !p_vocabulary.implements(p_wanted.bound, element))
      return false;
    const auto [bound, added] = p_variables.try_emplace(p_wanted.name, element);
    if (added || !bound->second.known() || bound->second == element)
      return true;
    // A circle where a shape goes (`append c to shapes`): the trait.
    const auto is_one = [&](const value_kind &p_record, const value_kind &p_trait) {
      return p_record.is_record() && p_trait.is_record() &&
             p_record.depth == p_trait.depth &&
             p_vocabulary.trait_impls.contains({p_trait.record, p_record.record});
    };
    if (is_one(element, bound->second))
      return true;
    if (is_one(bound->second, element)) {
      bound->second = element;
      return true;
    }
    return false;
  }
  if (p_wanted.kind == type_name::kind_type::type_slot) {
    const auto index = static_cast<std::size_t>(
        std::ranges::find(p_template.slots, p_wanted.name, &slot::name) -
        p_template.slots.begin());
    const auto &chosen = p_fills.types[index];
    return !chosen ||
           p_value == chosen->with_depth(chosen->depth + p_wanted.list_depth);
  }
  // A generic record of these types (a generic phrase's `stack of item`).
  if (p_wanted.name == "any" && !p_wanted.record.empty() &&
      p_wanted.kind == type_name::kind_type::builtin) {
    if (p_value.depth != p_wanted.list_depth)
      return false;
    const auto element = p_value.with_depth(0);
    if (!element.is_record() || element.record != p_wanted.record)
      return false;
    for (std::size_t index = 0; index < std::min(element.arguments.size(),
                                                 p_wanted.arguments.size());
         ++index)
      if (!accepts(p_wanted.arguments[index], element.arguments[index],
                   p_variables, p_template, p_fills, p_vocabulary))
        return false;
    return true;
  }
  if (p_wanted.name == "any")
    return p_wanted.list_depth == 0 || p_value.depth == p_wanted.list_depth;
  if (p_wanted.name == "showable")
    return p_vocabulary.implements("showable", p_value);
  if (is_record_trait_name(p_wanted.name))
    return p_value.depth == p_wanted.list_depth &&
           p_vocabulary.implements(p_wanted.name, p_value);
  // An array of C numbers goes where a pointer does (C takes its items).
  if (p_value.is_c_array() && p_wanted.list_depth == 0 && p_wanted.name == "ptr")
    return true;
  if (p_value.depth != p_wanted.list_depth)
    return false;
  const auto &base = p_wanted.name;
  // A whole number may stand for a decimal, but a list of whole numbers
  // isn't a list of decimals.
  return base == p_value.base ||
         (base == "num" && (p_value.base == "int" || p_value.base == "float")) ||
         (base == "float" && p_value.base == "int" && p_value.depth == 0);
}

auto result_type(const phrase_template &p_template,
                 const std::map<string_type, value_kind> &p_variables,
                 const slot_fills &p_fills) -> value_kind {
  const auto &wanted = *p_template.result;
  switch (wanted.kind) {
  case type_name::kind_type::builtin:
    if (wanted.name == "num" || wanted.name == "any")
      return {};
    return {wanted.name, wanted.list_depth};
  case type_name::kind_type::variable: {
    const auto bound = p_variables.find(wanted.name);
    if (bound == p_variables.end() || !bound->second.known())
      return {};
    return bound->second.with_depth(bound->second.depth + wanted.list_depth);
  }
  case type_name::kind_type::type_slot: {
    const auto index = static_cast<std::size_t>(
        std::ranges::find(p_template.slots, wanted.name, &slot::name) -
        p_template.slots.begin());
    const auto &chosen = p_fills.types[index];
    if (!chosen)
      return {};
    auto result = chosen->with_depth(chosen->depth + wanted.list_depth);
    // A typed pointer or a phrase stays one; a C number read is a number;
    // a list of C numbers is an array of them.
    if (wanted.list_depth == 0 && !chosen->is_c_number())
      result.exact = chosen->exact;
    if (wanted.list_depth == 1 && chosen->is_c_number())
      result.exact = "(list " + chosen->exact + ")";
    return result;
  }
  case type_name::kind_type::field:
    break;
  }
  return {};
}

// The template's Ovis Core with the slots filled in; for records, its
// `for records:` Core. A trait-typed slot written after a `_` or a `:`
// stands for its type's name, so `contains_$xs` is `contains_int` for
// whole numbers and `comes_before__$a` the function of a record `book`.
auto expand(const phrase_template &p_template, const slot_fills &p_fills,
            bool p_record_mode) -> string_type {
  string_type result;
  const auto &core = p_record_mode ? p_template.record_core : p_template.core;
  for (std::size_t index = 0; index < core.size();) {
    if (core[index] != '$') {
      result += core[index++];
      continue;
    }
    const auto after_name = index > 0 && (core[index - 1] == '_' || core[index - 1] == ':');
    auto end = index + 1;
    while (end < core.size() &&
           (std::isalnum(static_cast<unsigned char>(core[end])) ||
            core[end] == '_'))
      ++end;
    const auto name = core.substr(index + 1, end - index - 1);
    if (core.compare(end, 3, "...") == 0)
      end += 3;
    index = end;
    const auto slot = static_cast<std::size_t>(
        std::ranges::find(p_template.slots, name, &slot::name) -
        p_template.slots.begin());
    if (p_fills.types[slot]) {
      result += p_fills.types[slot]->core();
      continue;
    }
    const auto &type = p_template.slots[slot].type;
    if (after_name && is_trait_slot(type) && !p_fills.values[slot].empty()) {
      const auto &value = p_fills.values[slot].front().type;
      const auto element = value.with_depth(value.depth - type.list_depth);
      result += element.is_record() ? element.record : element.base;
      continue;
    }
    for (std::size_t item = 0; item < p_fills.values[slot].size(); ++item)
      result += (item > 0 ? " " : "") + p_fills.values[slot][item].core;
  }
  return result;
}

// The template's reading: its canonical words (first choices, no optional
// words) with the slots' readings.
auto reading_of(const phrase_template &p_template, const pattern &p_pattern,
                const slot_fills &p_fills) -> string_type {
  string_type result;
  const auto add = [&](const string_type &p_text) {
    result += (result.empty() ? "" : " ") + p_text;
  };
  for (std::size_t index = 0; index < p_pattern.size(); ++index) {
    const auto &element = p_pattern[index];
    switch (element.kind) {
    case pattern_element::kind_type::word:
      add(element.word);
      break;
    case pattern_element::kind_type::choice:
      for (const auto &word : element.branches[0])
        add(word.word);
      break;
    case pattern_element::kind_type::optional:
      break;
    case pattern_element::kind_type::slot: {
      if (const auto &type = p_fills.types[element.slot]) {
        add(type->english().substr(type->english().starts_with("a ") ? 2 : 0));
        break;
      }
      // A phrase with a slot at its edge, in a slot at this one's edge,
      // could also group the other way: `add tax to (x with 2 decimals)`
      // and `(add tax to x) with 2 decimals` must read differently.
      // Commas and the end of a statement already delimit the rest.
      const auto at_edge =
          (index == 0 || index + 1 == p_pattern.size()) &&
          p_template.kind == phrase_template::kind_type::expression &&
          !p_template.slots[element.slot].is_variadic;
      string_type values;
      for (const auto &value : p_fills.values[element.slot])
        values += (values.empty() ? "" : ", ") +
                  ((at_edge && value.shape == parse::shape_type::phrase &&
                    value.open_edge) ||
                           (p_template.slots[element.slot].is_variadic &&
                            value.reading.find(", ") != string_type::npos)
                       ? "(" + value.reading + ")"
                       : nested_reading(value, true));
      add(values);
      break;
    }
    }
  }
  return result;
}

} // namespace

auto phrases_taking(const vocabulary &p_vocabulary, const value_kind &p_value)
    -> std::vector<const phrase_template *> {
  std::vector<const phrase_template *> result;
  for (const auto &item : p_vocabulary.templates) {
    if (item.records_only || !p_vocabulary.sees(item.package))
      continue;
    slot_fills fills;
    fills.types.resize(item.slots.size());
    for (const auto &slot : item.slots) {
      std::map<string_type, value_kind> variables;
      const auto kind = slot.type.kind;
      // A slot of a type written in words, or of a field, takes no value.
      if (kind == type_name::kind_type::type_slot ||
          kind == type_name::kind_type::field || slot.type.name == "type" ||
          slot.type.name == "size")
        continue;
      if (accepts(slot.type, p_value, variables, item, fills, p_vocabulary)) {
        result.push_back(&item);
        break;
      }
    }
  }
  return result;
}

// The parse a template gives with these fills, if their types fit.
auto build_phrase(const phrase_template &p_template, const pattern &p_pattern,
                  const slot_fills &p_fills, const vocabulary &p_vocabulary)
    -> std::optional<parse> {
  std::map<string_type, value_kind> variables;
  for (std::size_t index = 0; index < p_template.slots.size(); ++index)
    for (const auto &value : p_fills.values[index])
      if (p_template.slots[index].type.kind != type_name::kind_type::field &&
          !accepts(p_template.slots[index].type, value.type, variables,
                   p_template, p_fills, p_vocabulary))
        return std::nullopt;

  // Slots typed by a trait that hold records choose the template's record
  // Ovis Core; they must all hold one record, as `a comes before b` does.
  bool record_mode = false;
  {
    std::optional<string_type> chosen;
    bool other = false;
    for (std::size_t index = 0; index < p_template.slots.size(); ++index) {
      const auto &type = p_template.slots[index].type;
      if (!is_trait_slot(type))
        continue;
      for (const auto &value : p_fills.values[index]) {
        if (!value.type.known() || value.type.depth < type.list_depth)
          continue;
        const auto element =
            value.type.with_depth(value.type.depth - type.list_depth);
        if (!element.is_record()) {
          other = other || element.known();
          continue;
        }
        if (type.name == "showable")
          continue;
        if (chosen && *chosen != element.record)
          return std::nullopt;
        chosen = element.record;
      }
    }
    if (chosen && other)
      return std::nullopt;
    // A template with no `for records:` line is for records as it is: its
    // Ovis Core is generic (prelude.core), or compares by the phrases the
    // record's author wrote.
    record_mode = chosen.has_value() && !p_template.record_core.empty();
    if (p_template.records_only && !record_mode)
      return std::nullopt;
  }

  // A field slot holds a word: a field of the record its type names, once
  // the other slots say which record that is. It expands to RECORD:FIELD,
  // for a generic prelude function (phrases/*.generic).
  auto fills = p_fills;
  // A record shown by the `text of` its author gave it is that text.
  for (std::size_t index = 0; index < p_template.slots.size(); ++index)
    if (p_template.slots[index].type.name == "showable")
      for (auto &value : fills.values[index])
        if (const auto text =
                shown_by_author(p_vocabulary, value.type, value.core)) {
          value.core = *text;
          value.type = {"str"};
        }
  std::map<string_type, value_kind> field_types;
  string_type hole_record;
  for (std::size_t index = 0; index < p_template.slots.size(); ++index) {
    const auto &type = p_template.slots[index].type;
    if (type.kind != type_name::kind_type::field || fills.values[index].empty())
      continue;
    std::optional<value_kind> owner;
    if (std::isupper(static_cast<unsigned char>(type.record.front()))) {
      if (const auto bound = variables.find(type.record); bound != variables.end())
        owner = bound->second;
    } else {
      const auto slot = static_cast<std::size_t>(
          std::ranges::find(p_template.slots, type.record, &slot::name) -
          p_template.slots.begin());
      owner = p_fills.types[slot];
    }
    if (!owner || !owner->is_record())
      return std::nullopt;
    const auto &record = p_vocabulary.records.at(owner->record);
    auto &value = fills.values[index].front();
    if (value.holes > 0) {
      hole_record = record.name;
      continue;
    }
    const auto *const field = record.find(value.reading);
    if (field == nullptr)
      return std::nullopt;
    if (type.name != "any" &&
        !accepts({.name = type.name}, field->type, variables, p_template,
                 p_fills, p_vocabulary))
      return std::nullopt;
    value.core = record.name + ":" + field->name;
    field_types[p_template.slots[index].name] = field->type;
  }

  parse result{.shape = parse::shape_type::phrase};
  result.open_edge =
      p_pattern.front().kind == pattern_element::kind_type::slot ||
      p_pattern.back().kind == pattern_element::kind_type::slot;
  result.core = expand(p_template, fills, record_mode);
  result.reading = reading_of(p_template, p_pattern, fills);
  // Whose phrase it is, where two packages have one with these words.
  if (p_vocabulary.qualifiers != nullptr && p_vocabulary.shared_words != nullptr)
    if (const auto name = p_vocabulary.qualifiers->find(p_template.package);
        name != p_vocabulary.qualifiers->end() &&
        p_vocabulary.shared_words->contains(
            spoken_pattern(p_template, p_template.patterns.front())))
      result.reading = name->second + "'s " + result.reading;
  if (p_template.result) {
    const auto field = field_types.find(p_template.result->name);
    result.type = p_template.result->kind == type_name::kind_type::type_slot &&
                          field != field_types.end()
                      ? field->second
                      : result_type(p_template, variables, fills);
  }
  // A phrase that starts or ends with a slot binds like what fills it, so
  // `square root of x + 1` reads both ways and becomes a question.
  const auto edge_level = [&](const pattern_element &p_element) {
    if (p_element.kind != pattern_element::kind_type::slot ||
        p_fills.values[p_element.slot].empty())
      return int(primary_level);
    int level = primary_level;
    for (const auto &value : p_fills.values[p_element.slot])
      level = std::min(level, value.level);
    return level;
  };
  result.level = std::min(edge_level(p_pattern.front()),
                          edge_level(p_pattern.back()));
  result.uses.push_back(&p_template);
  for (const auto &values : p_fills.values)
    for (const auto &value : values)
      result.use_all(value);
  for (const auto &values : p_fills.values)
    for (const auto &value : values)
      result.take_holes(value);
  if (!hole_record.empty())
    result.hole_record = hole_record;
  return result;
}

struct line_parser::state {
  const std::vector<token> &tokens;
  const std::map<string_type, value_kind> &scope;
  const vocabulary &words;
  bool allow_holes;
  // Bracket nesting of each token (brackets count as the outer level), and
  // each bracket's partner.
  std::vector<std::size_t> depth;
  std::vector<std::size_t> partner;
  std::map<std::pair<std::size_t, std::size_t>, std::vector<parse>> chart;

  static constexpr std::size_t max_readings = 12;

  auto balanced(std::size_t p_begin, std::size_t p_end) const -> bool {
    for (auto index = p_begin; index < p_end; ++index)
      if (partner[index] != string_type::npos &&
          (partner[index] < p_begin || partner[index] >= p_end))
        return false;
    return true;
  }

  auto word_at(std::size_t p_position, std::size_t p_end,
               string_view_type p_word) const -> bool {
    if (p_position >= p_end)
      return false;
    // A user phrase's header may have commas: `the biggest of (a), (b) and
    // (c)`.
    if (p_word == ",")
      return tokens[p_position].is_symbol(",");
    return tokens[p_position].kind == token::kind_type::word &&
           same_word(tokens[p_position].text, p_word);
  }

  auto match_words(const pattern &p_words, std::size_t p_position,
                   std::size_t p_end) const -> std::optional<std::size_t> {
    for (const auto &word : p_words) {
      if (!word_at(p_position, p_end, word.word))
        return std::nullopt;
      ++p_position;
    }
    return p_position;
  }

  auto atom(const token &p_token) const -> std::optional<parse> {
    switch (p_token.kind) {
    case token::kind_type::number:
      // `1_000.5`: Ovis Core reads `_` only in whole numbers.
      return parse{.core = p_token.text.find('.') == string_type::npos
                               ? p_token.text
                               : [&] {
                                   auto digits = p_token.text;
                                   std::erase(digits, '_');
                                   return digits;
                                 }(),
                   .reading = p_token.text,
                   .type = {p_token.text.find('.') == string_type::npos
                                ? "int"
                                : "float"}};
    case token::kind_type::text:
      return parse{.core = quote(p_token.text),
                   .reading = quote(p_token.text),
                   .type = {"str"}};
    case token::kind_type::word:
      if (p_token.is_word("true") || p_token.is_word("false"))
        return parse{.core = lower(p_token.text),
                     .reading = lower(p_token.text),
                     .type = {"bool"}};
      if (const auto found = scope.find(p_token.text); found != scope.end())
        return parse{.core = core_name(p_token.text),
                     .reading = p_token.text,
                     .type = found->second};
      // No record: Ovis Core's `null`, which a record's place may hold.
      if (p_token.is_word("nothing"))
        return parse{.core = "null", .reading = "nothing", .type = {"ptr"}};
      if (const auto found = words.constants.find(p_token.text);
          found != words.constants.end())
        return parse{.core = p_token.text,
                     .reading = p_token.text,
                     .type = found->second};
      return std::nullopt;
    case token::kind_type::symbol:
      return std::nullopt;
    }
    return std::nullopt;
  }

  // `name(arguments)`: a call of a user or C function. A function giving
  // nothing back is only a statement.
  auto calls(std::size_t p_begin, std::size_t p_end, bool p_statement,
             std::vector<parse> &p_results) -> void {
    if (p_end - p_begin < 3 || tokens[p_begin].kind != token::kind_type::word ||
        !tokens[p_begin + 1].is_symbol("(") || partner[p_begin + 1] != p_end - 1)
      return;
    const auto found = words.functions.find(tokens[p_begin].text);
    if (found == words.functions.end() ||
        (!p_statement && found->second.result.base == "void"))
      return;
    const auto &function = found->second;
    auto pieces = p_begin + 2 < p_end - 1 ? items(p_begin + 2, p_end - 1)
                                          : std::vector<std::vector<parse>>{};
    // A C macro's type and field arguments are words, not values.
    if (!function.roles.empty()) {
      std::vector<std::pair<std::size_t, std::size_t>> spans;
      auto start = p_begin + 2;
      for (auto index = start; index <= p_end - 1; ++index)
        if (index == p_end - 1 ||
            (tokens[index].is_symbol(",") && depth[index] == depth[p_begin + 2])) {
          spans.emplace_back(start, index);
          start = index + 1;
        }
      for (std::size_t at = 0; at < std::min(spans.size(), function.roles.size());
           ++at) {
        const auto [begin, end] = spans[at];
        string_type text;
        for (auto index = begin; index < end; ++index)
          text += (index > begin ? " " : "") + tokens[index].text;
        if (function.roles[at] == 't') {
          pieces[at].clear();
          auto first = begin;
          if (first + 1 < end && is_article(tokens[first]))
            ++first;
          for (const auto &[length, type] : type_words(tokens, first, end, &words))
            if (first + length == end)
              pieces[at].push_back({.core = "(type " + type.core() + ")",
                                    .reading = text, .type = type});
        } else if (function.roles[at] == 'f' && end == begin + 1 &&
                   tokens[begin].kind == token::kind_type::word) {
          pieces[at] = {{.core = "(member " + tokens[begin].text + ")",
                         .reading = text}};
        }
      }
    }
    const auto choices = !pieces.empty() ? combinations(pieces)
                                         : std::vector<std::vector<parse>>{{}};
    for (const auto &arguments : choices) {
      if (function.arity && arguments.size() != *function.arity)
        continue;
      parse call{.core = "(" + function.core,
                 .reading = tokens[p_begin].text + "(",
                 .type = function.result};
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto &argument = arguments[index];
        call.core += " " + argument.core;
        call.reading += (index > 0 ? ", " : "") + argument.reading;
        call.use_all(argument);
        call.take_holes(argument);
      }
      call.core += ")";
      call.reading += ")";
      if (call.holes <= 1)
        p_results.push_back(std::move(call));
    }
  }

  // The comma-separated items of a span at the span's own nesting level.
  auto items(std::size_t p_begin, std::size_t p_end)
      -> std::vector<std::vector<parse>> {
    std::vector<std::vector<parse>> result;
    auto start = p_begin;
    for (auto index = p_begin; index <= p_end; ++index)
      if (index == p_end ||
          (tokens[index].is_symbol(",") && depth[index] == depth[p_begin])) {
        result.push_back(start < index ? expressions(start, index)
                                       : std::vector<parse>{});
        start = index + 1;
      }
    return result;
  }

  // The items of a variadic slot: split at every comma, and also with
  // neighbouring pieces kept together, as a phrase may have commas itself
  // (`show the biggest of 4, 9 and 2`). Up to 4 commas are tried both ways.
  auto groupings(std::size_t p_begin, std::size_t p_end)
      -> std::vector<std::vector<std::vector<parse>>> {
    std::vector<std::size_t> commas;
    for (auto index = p_begin; index < p_end; ++index)
      if (tokens[index].is_symbol(",") && depth[index] == depth[p_begin])
        commas.push_back(index);
    if (commas.size() > 4) {
      // Too many to try every way: split at every comma, and also keep a
      // record's fields together (`a book with title "A", pages 1, a book
      // with …`): a comma before a field's name after a `with`.
      std::vector<std::vector<parse>> records;
      auto start = p_begin;
      bool after_with = false;
      for (auto index = p_begin; index <= p_end; ++index) {
        if (index < p_end && depth[index] == depth[p_begin] &&
            tokens[index].is_word("with"))
          after_with = true;
        const auto split =
            index == p_end ||
            (tokens[index].is_symbol(",") && depth[index] == depth[p_begin] &&
             !(after_with && index + 1 < p_end &&
               !words.field_lengths(tokens, index + 1, p_end).empty()));
        if (!split)
          continue;
        records.push_back(start < index ? expressions(start, index)
                                        : std::vector<parse>{});
        start = index + 1;
        after_with = false;
      }
      return {items(p_begin, p_end), std::move(records)};
    }
    std::vector<std::vector<std::vector<parse>>> result;
    for (std::size_t mask = (std::size_t(1) << commas.size()); mask-- > 0;) {
      std::vector<std::vector<parse>> pieces;
      auto start = p_begin;
      for (std::size_t index = 0; index <= commas.size(); ++index) {
        if (index < commas.size() && (mask & (std::size_t(1) << index)) == 0)
          continue;
        const auto stop = index < commas.size() ? commas[index] : p_end;
        pieces.push_back(start < stop ? expressions(start, stop)
                                      : std::vector<parse>{});
        start = stop + 1;
      }
      result.push_back(std::move(pieces));
    }
    return result;
  }

  // Every way to pick one reading per item, up to a few.
  static auto combinations(const std::vector<std::vector<parse>> &p_items)
      -> std::vector<std::vector<parse>> {
    std::vector<std::vector<parse>> result = {{}};
    for (const auto &item : p_items) {
      std::vector<std::vector<parse>> next;
      for (const auto &partial : result)
        for (const auto &choice : item) {
          if (next.size() >= 4)
            break;
          next.push_back(partial);
          next.back().push_back(choice);
        }
      result = std::move(next);
    }
    return result;
  }

  // The fewest tokens the elements from `p_from` on can match.
  static auto minimum_length(const pattern &p_pattern, std::size_t p_from)
      -> std::size_t {
    std::size_t total = 0;
    for (auto index = p_from; index < p_pattern.size(); ++index) {
      const auto &element = p_pattern[index];
      switch (element.kind) {
      case pattern_element::kind_type::word:
        total += is_article_word(element.word) ? 0 : 1;
        break;
      case pattern_element::kind_type::slot:
        total += 1;
        break;
      case pattern_element::kind_type::optional:
        break;
      case pattern_element::kind_type::choice: {
        auto shortest = element.branches[0].size();
        for (const auto &branch : element.branches)
          shortest = std::min(shortest, branch.size());
        total += shortest;
        break;
      }
      }
    }
    return total;
  }

  auto match(const phrase_template &p_template, const pattern &p_pattern,
             std::size_t p_element, std::size_t p_position, std::size_t p_end,
             slot_fills &p_fills, std::vector<slot_fills> &p_complete) -> void {
    if (p_complete.size() >= 16)
      return;
    // An article the pattern doesn't ask for may be there anyway.
    if (p_position < p_end && is_article(tokens[p_position]) &&
        !(p_element < p_pattern.size() &&
          p_pattern[p_element].kind == pattern_element::kind_type::word &&
          word_at(p_position, p_end, p_pattern[p_element].word)))
      match(p_template, p_pattern, p_element, p_position + 1, p_end, p_fills,
            p_complete);
    if (p_element == p_pattern.size()) {
      if (p_position == p_end)
        p_complete.push_back(p_fills);
      return;
    }
    const auto &element = p_pattern[p_element];
    const auto next = [&](std::size_t p_after) {
      match(p_template, p_pattern, p_element + 1, p_after, p_end, p_fills,
            p_complete);
    };
    switch (element.kind) {
    case pattern_element::kind_type::word:
      if (word_at(p_position, p_end, element.word))
        next(p_position + 1);
      // And an article the pattern asks for may be left out.
      else if (is_article_word(element.word))
        next(p_position);
      break;
    case pattern_element::kind_type::optional:
      if (const auto after = match_words(element.branches[0], p_position, p_end))
        next(*after);
      next(p_position);
      break;
    case pattern_element::kind_type::choice:
      for (const auto &branch : element.branches)
        if (const auto after = match_words(branch, p_position, p_end))
          next(*after);
      break;
    case pattern_element::kind_type::slot: {
      const auto &slot = p_template.slots[element.slot];
      auto &values = p_fills.values[element.slot];
      const auto holes_so_far = [&] {
        std::size_t count = 0;
        for (const auto &filled : p_fills.values)
          for (const auto &value : filled)
            count += value.holes;
        return count;
      };
      if (is_type_slot_name(slot.type.name) &&
          slot.type.kind == type_name::kind_type::builtin) {
        for (const auto &[length, type] :
             type_words(tokens, p_position, p_end, &words)) {
          // A `size` slot takes only a type of number.
          if (slot.type.name == "size" && !type.is_number())
            continue;
          p_fills.types[element.slot] = type;
          next(p_position + length);
        }
        p_fills.types[element.slot].reset();
        break;
      }
      // A field's name, its words (`number of wheels`), checked against the
      // record once the other slots are filled (`build_phrase`). While
      // looking for what is unclear, a word no record has may be a hole
      // there.
      if (slot.type.kind == type_name::kind_type::field) {
        if (p_position < p_end &&
            tokens[p_position].kind == token::kind_type::word) {
          if (const auto lengths = words.field_lengths(tokens, p_position, p_end);
              !lengths.empty()) {
            for (const auto length : lengths) {
              string_type written;
              for (auto index = p_position; index < p_position + length; ++index)
                written += (written.empty() ? "" : " ") + tokens[index].text;
              values = {parse{.core = written, .reading = written}};
              next(p_position + length);
            }
          } else if (allow_holes && holes_so_far() == 0) {
            values = {hole(p_position, p_position + 1)};
            next(p_position + 1);
          }
          values.clear();
        }
        break;
      }
      // Leave room for the words the rest of the pattern needs.
      const auto needed = minimum_length(p_pattern, p_element + 1);
      for (auto end = p_position + 1; end + needed <= p_end; ++end) {
        if (!balanced(p_position, end))
          continue;
        if (slot.is_variadic) {
          for (const auto &grouping : groupings(p_position, end))
            for (auto &choice : combinations(grouping)) {
              values = std::move(choice);
              if (!values.empty() && holes_so_far() <= 1)
                next(end);
            }
        } else {
          for (const auto &value : expressions(p_position, end)) {
            values = {value};
            if (holes_so_far() <= 1)
              next(end);
          }
        }
        values.clear();
      }
      break;
    }
    }
  }

  auto add_phrases(phrase_template::kind_type p_kind, bool p_block,
                   std::size_t p_begin, std::size_t p_end,
                   std::vector<parse> &p_results) {
    for (const auto &item : words.templates) {
      if (!words.sees(item.package))
        continue;
      if (item.kind != p_kind ||
          (p_kind == phrase_template::kind_type::statement &&
           item.takes_block != p_block))
        continue;
      for (const auto &pattern : item.patterns) {
        slot_fills fills;
        fills.values.resize(item.slots.size());
        fills.types.resize(item.slots.size());
        std::vector<slot_fills> complete;
        match(item, pattern, 0, p_begin, p_end, fills, complete);
        for (const auto &found : complete)
          if (auto built = build_phrase(item, item.patterns[0], found, words))
            p_results.push_back(std::move(*built));
      }
    }
  }

  // Readings that read the same can't be told apart by the author, so the
  // first one (in template order) stands for them.
  static auto unique(std::vector<parse> &p_results) -> void {
    std::set<string_type> cores;
    std::set<string_type> readings;
    std::erase_if(p_results, [&](const parse &p_parse) {
      const auto new_core = cores.insert(p_parse.core).second;
      const auto new_reading = readings.insert(p_parse.reading).second;
      return !new_core || !new_reading;
    });
  }

  state(const std::vector<token> &p_tokens,
        const std::map<string_type, value_kind> &p_scope,
        const vocabulary &p_vocabulary, bool p_allow_holes)
      : tokens(p_tokens), scope(p_scope), words(p_vocabulary),
        allow_holes(p_allow_holes),
        depth(p_tokens.size(), 0), partner(p_tokens.size(), string_type::npos) {
    std::vector<std::size_t> open;
    std::size_t level = 0;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
      if (tokens[index].is_symbol("(") || tokens[index].is_symbol("[")) {
        depth[index] = level++;
        open.push_back(index);
      } else if (tokens[index].is_symbol(")") || tokens[index].is_symbol("]")) {
        depth[index] = --level;
        partner[index] = open.back();
        partner[open.back()] = index;
        open.pop_back();
      } else {
        depth[index] = level;
      }
    }
  }

  static auto hole(std::size_t p_begin, std::size_t p_end) -> parse {
    return {.shape = parse::shape_type::hole,
            .core = "HOLE",
            .reading = "…",
            .holes = 1,
            .hole_begin = p_begin,
            .hole_end = p_end};
  }

  // A field of a record: `price of b`, `b's price`, `b.price`.
  auto field_accesses(std::size_t p_begin, std::size_t p_end,
                      std::vector<parse> &p_results) -> void {
    if (p_end - p_begin < 3)
      return;
    // A field's words, or one word (which may be unclear).
    auto lengths = words.field_lengths(tokens, p_begin, p_end);
    if (std::ranges::find(lengths, std::size_t(1)) == lengths.end())
      lengths.push_back(1);
    for (const auto length : lengths)
      if (tokens[p_begin].kind == token::kind_type::word &&
          p_begin + length + 1 < p_end && tokens[p_begin + length].is_word("of"))
        for (const auto &object : expressions(p_begin + length + 1, p_end))
          add_field(object, p_begin, length, "of", p_results);
    for (std::size_t length = 1; length + 2 <= p_end - p_begin; ++length) {
      const auto at = p_end - length;
      if (!(tokens[at - 1].is_symbol("'s") || tokens[at - 1].is_symbol(".")) ||
          tokens[at].kind != token::kind_type::word)
        continue;
      const auto found = words.field_lengths(tokens, at, p_end);
      if (length > 1 && std::ranges::find(found, length) == found.end())
        continue;
      for (const auto &object : expressions(p_begin, at - 1))
        if (object.level == primary_level)
          add_field(object, at, length, tokens[at - 1].text, p_results);
    }
  }

  auto add_field(const parse &p_object, std::size_t p_at, std::size_t p_length,
                 string_view_type p_style, std::vector<parse> &p_results)
      -> void {
    string_type word;
    for (auto index = p_at; index < p_at + p_length; ++index)
      word += (word.empty() ? "" : " ") + tokens[index].text;
    // The field whose words these are, exactly.
    const auto field_of = [&](const record_info &p_record) -> const record_field * {
      const auto [field, length] = p_record.field_at(tokens, p_at, p_at + p_length);
      return length == p_length ? field : nullptr;
    };
    string_type name = word;
    value_kind type;
    parse result{.level = primary_level};
    if (p_object.type.known()) {
      if (!p_object.type.is_record())
        return;
      const auto &record = words.records.at(p_object.type.record);
      const auto *const field = field_of(record);
      if (field == nullptr) {
        // Not one of its fields: while looking for what is unclear, the
        // word is, and the record's fields are what it may be.
        if (!allow_holes || p_object.holes > 0 || p_length > 1)
          return;
        result.take_holes(hole(p_at, p_at + 1));
        result.hole_record = record.name;
        name = "HOLE";
      } else {
        name = field->name;
        type = in_use(field->type, record, p_object.type);
        word = field->spoken();
      }
    } else {
      // Not known yet (a phrase's parameter): any record with the field,
      // and its type if only one has it.
      std::vector<const record_field *> found;
      for (const auto &[record_name, record] : words.records)
        if (const auto *const field = field_of(record))
          found.push_back(field);
      if (found.empty())
        return;
      name = found.front()->name;
      word = found.front()->spoken();
      // A generic record's field holds what its use holds: not known here.
      if (found.size() == 1 && found.front()->type.base != "var" &&
          found.front()->type.core().find("(var ") == string_type::npos)
        type = found.front()->type;
    }
    result.core = "(field " + p_object.core + " " + name + ")";
    result.type = type;
    if (p_style == "of") {
      // Like a phrase ending in a slot: `price of b + 1` groups both ways.
      result.shape = parse::shape_type::phrase;
      result.open_edge = true;
      result.level = std::min(int(primary_level), p_object.level);
      result.reading = word + " of " + nested_reading(p_object, true);
    } else {
      result.reading = p_object.reading + (p_style == "." ? "." : "'s ") + word;
    }
    result.take_holes(p_object);
    result.use_all(p_object);
    p_results.push_back(std::move(result));
  }

  // A phrase as a value (references.md §7b): its words with `_` for its
  // slots (`compare _ and _`), after `the phrase` (or `the action`, `the
  // function`, `the method`, `the procedure`, `the routine`), which one
  // without slots needs: bare words are a call.
  auto phrase_values(std::size_t p_begin, std::size_t p_end,
                     std::vector<parse> &p_results) -> void {
    if (words.phrase_values.empty())
      return;
    auto at = p_begin;
    if (at < p_end && tokens[at].is_word("the"))
      ++at;
    bool marked = false;
    for (const auto *const marker :
         {"phrase", "action", "function", "method", "procedure", "routine"})
      if (at < p_end && tokens[at].is_word(marker)) {
        marked = true;
        ++at;
        break;
      }
    if (!marked)
      at = p_begin;
    string_type key;
    bool has_slot = false;
    for (auto index = at; index < p_end; ++index) {
      if (tokens[index].is_symbol(","))
        continue;
      if (tokens[index].kind != token::kind_type::word)
        return;
      if (is_article(tokens[index]))
        continue;
      has_slot |= tokens[index].text == "_";
      key += (key.empty() ? "" : " ") + lower(tokens[index].text);
    }
    const auto found = words.phrase_values.find(key);
    if (found == words.phrase_values.end() || (!has_slot && !marked))
      return;
    string_type reading;
    for (auto index = p_begin; index < p_end; ++index)
      reading += (index > p_begin ? " " : "") + tokens[index].text;
    p_results.push_back({.core = "(phrase-of " + found->second + ")",
                         .reading = reading,
                         .type = value_kind{"ptr"}});
  }

  // `a book with title "Dune", pages 412`: a new record. Fields left out
  // get their defaults; one without a default is the checker's to report.
  // Items may be written as English has them too: `with the age of 12`,
  // `and with the pages being 412`, and `named "Winnie"` for a `name` field.
  // How `(new …)` names the record: a generic one with its types, when
  // they are said (`(stack int)`).
  static auto made_name(const record_info &p_record, const value_kind &p_use)
      -> string_type {
    if (p_use.arguments.empty() ||
        std::ranges::any_of(p_use.arguments, [](const value_kind &p_argument) {
          return !p_argument.known();
        }))
      return p_record.name;
    auto type = made_type(p_record, p_use);
    // `(record stack int)` is written `(stack int)` in `new`.
    return "(" + type.substr(8);
  }
  static auto made_type(const record_info &p_record, const value_kind &p_use)
      -> string_type {
    auto use = p_use;
    use.depth = 0;
    use.exact.clear();
    use.base = "record";
    use.record = p_record.name;
    return use.core();
  }
  // A field's default in one use: a generic record's type variables are
  // its types, or left for Ovis Core to work out.
  static auto default_in_use(const record_info &p_record,
                             const record_field &p_field, const value_kind &p_use)
      -> string_type {
    auto core = *p_field.default_core;
    for (std::size_t index = 0; index < p_record.parameters.size(); ++index) {
      const auto variable = "(var " + p_record.parameters[index] + ")";
      const auto type = index < p_use.arguments.size() &&
                                p_use.arguments[index].known()
                            ? p_use.arguments[index].core()
                            : string_type("?");
      for (auto at = core.find(variable); at != string_type::npos;
           at = core.find(variable, at + type.size()))
        core.replace(at, variable.size(), type);
    }
    return core;
  }
  // A generic record's types, as the fields given decide them: `a stack
  // with items [1, 2]` holds whole numbers.
  static auto arguments_given(const record_info &p_record,
                              const std::vector<parse> &p_items)
      -> std::vector<value_kind> {
    if (p_record.parameters.empty())
      return {};
    std::vector<value_kind> result(p_record.parameters.size());
    for (const auto &item : p_items) {
      const auto name = item.core.substr(1, item.core.find(' ') - 1);
      const auto field = std::ranges::find(p_record.fields, name, &record_field::name);
      if (field == p_record.fields.end() || !item.type.known())
        continue;
      const auto &wanted = field->type;
      if (wanted.base != "var" || item.type.depth < wanted.depth)
        continue;
      const auto at = std::ranges::find(p_record.parameters, wanted.record);
      if (at == p_record.parameters.end())
        continue;
      auto &slot = result[static_cast<std::size_t>(at - p_record.parameters.begin())];
      if (!slot.known())
        slot = item.type.with_depth(item.type.depth - wanted.depth);
    }
    if (std::ranges::any_of(result, [](const value_kind &p_kind) {
          return !p_kind.known();
        }))
      return {};
    return result;
  }

  auto new_records(std::size_t p_begin, std::size_t p_end,
                   std::vector<parse> &p_results) -> void {
    // `a new z_stream`: every field its default, or zero for a C struct.
    if (tokens[p_begin].is_word("new")) {
      const auto [made, made_length] = words.record_at(tokens, p_begin + 1, p_end);
      // `a new stack of whole numbers`: a generic one, its types said.
      std::vector<std::pair<std::size_t, value_kind>> uses;
      if (made != nullptr) {
        uses.emplace_back(made_length, made->type());
        for (auto [header, use] : generic_type_words(
                 *made, tokens, p_begin + 1 + made_length, p_end, &words))
          uses.emplace_back(made_length + header, std::move(use));
      }
      for (const auto &[used_length, use] : uses)
      if (made != nullptr && !made->is_trait &&
          p_begin + 1 + used_length == p_end) {
        parse result{.core = "(new " + made_name(*made, use),
                     .reading = "a new " + [&] {
                       string_type text;
                       for (auto at = p_begin + 1; at < p_end; ++at)
                         text += (at > p_begin + 1 ? " " : "") + tokens[at].text;
                       return text;
                     }(),
                     .type = use};
        for (const auto &field : made->fields)
          if (field.default_core)
            result.core += " (" + field.name + " " +
                           default_in_use(*made, field, use) + ")";
        result.core += ")";
        p_results.push_back(std::move(result));
      }
    }
    const auto [record, length] = words.record_at(tokens, p_begin, p_end);
    if (record == nullptr)
      return;
    new_record_with(record, p_begin + length, record->type(), p_begin, p_end,
                    p_results);
    // A generic record with its types said: `a stack of books with …`.
    for (const auto &[header, use] :
         generic_type_words(*record, tokens, p_begin + length, p_end, &words))
      new_record_with(record, p_begin + length + header, use, p_begin, p_end,
                      p_results);
  }

  // `a stack of whole numbers with items [1, 2]`, `a book with …`: the
  // record `p_use` (a generic one's types, if they are said), its items
  // from `p_with`.
  auto new_record_with(const record_info *record, std::size_t with,
                       const value_kind &p_use, std::size_t p_begin,
                       std::size_t p_end, std::vector<parse> &p_results)
      -> void {
    // `a book or nothing`: no book yet, in a place that may hold one.
    if (record != nullptr && !record->is_trait && with + 2 == p_end &&
        tokens[with].is_word("or") && tokens[with + 1].is_word("nothing")) {
      auto type = p_use;
      type.exact = "(maybe " + made_type(*record, p_use) + ")";
      p_results.push_back({.core = "(cast " + type.exact + " null)",
                           .reading = "a " + record->name + " or nothing",
                           .type = type});
      return;
    }
    if (record == nullptr || with + 2 > p_end)
      return;
    const auto *const named = record->find("name");
    const bool is_named = named != nullptr && (tokens[with].is_word("named") ||
                                               tokens[with].is_word("called"));
    if (!is_named && (with + 3 > p_end || !tokens[with].is_word("with")))
      return;
    // Where an item's field is: past `and`, `with` and an article.
    const auto field_at = [&](std::size_t p_at, std::size_t p_until) {
      while (p_at < p_until &&
             (tokens[p_at].is_word("and") || tokens[p_at].is_word("with") ||
              is_article(tokens[p_at])))
        ++p_at;
      return p_at;
    };
    const auto starts_field = [&](std::size_t p_at) {
      return record->field_at(tokens, field_at(p_at, p_end), p_end).first !=
             nullptr;
    };
    // Items are split at commas, and at `and` or `with` before one of its
    // fields.
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    auto start = with + 1;
    for (auto index = start; index <= p_end; ++index)
      if (index == p_end ||
          (depth[index] == depth[p_begin] &&
           (tokens[index].is_symbol(",") ||
            ((tokens[index].is_word("and") || tokens[index].is_word("with")) &&
             field_at(start, index) < index && starts_field(index))))) {
        spans.emplace_back(start, index);
        start = index + 1;
      }
    std::vector<std::vector<parse>> items;
    for (std::size_t span = 0; span < spans.size(); ++span) {
      const auto [begin, end] = spans[span];
      // `named "Winnie"` gives the name; other items name their field.
      const bool is_name = is_named && span == 0;
      const auto at = is_name ? begin : field_at(begin, end);
      if (at >= end ||
          (!is_name && (end - at < 2 || tokens[at].kind != token::kind_type::word)))
        return;
      const auto [found, length] =
          is_name ? std::pair(named, std::size_t(0))
                  : record->field_at(tokens, at, end);
      const auto *const field = found;
      if (field == nullptr && !allow_holes)
        return;
      // `age of 12`, `age being 12`, `age is 12`, `age as 12`, `age = 12`,
      // `people which are …`, `owner who is …`.
      auto value_begin = is_name ? at : at + std::max(length, std::size_t(1));
      if (value_begin >= end)
        return;
      if (!is_name && value_begin + 2 < end &&
          (tokens[value_begin].is_word("which") ||
           tokens[value_begin].is_word("who") ||
           tokens[value_begin].is_word("that")) &&
          (tokens[value_begin + 1].is_word("is") ||
           tokens[value_begin + 1].is_word("are")))
        ++value_begin;
      // `button a set to true`, `age equal to 12`.
      if (!is_name && value_begin + 2 < end &&
          (tokens[value_begin].is_word("set") ||
           tokens[value_begin].is_word("equal")) &&
          tokens[value_begin + 1].is_word("to"))
        value_begin += 2;
      else if (!is_name && value_begin + 1 < end &&
          (tokens[value_begin].is_word("of") ||
           tokens[value_begin].is_word("equals") ||
           tokens[value_begin].is_word("being") ||
           tokens[value_begin].is_word("is") ||
           tokens[value_begin].is_word("are") ||
           tokens[value_begin].is_word("as") ||
           tokens[value_begin].is_symbol("=")))
        ++value_begin;
      auto values = expressions(value_begin, end);
      // A list field's items joined by `and`: `people betty and winnie`; one
      // item is a list of one.
      const auto field_type =
          field != nullptr ? in_use(field->type, *record, p_use) : value_kind{};
      if (field != nullptr && field->type.depth > 0) {
        std::vector<std::pair<std::size_t, std::size_t>> parts;
        auto part = value_begin;
        for (auto index = value_begin; index <= end; ++index)
          if (index == end || (depth[index] == depth[value_begin] &&
                               tokens[index].is_word("and"))) {
            parts.emplace_back(part, index);
            part = index + 1;
          }
        std::vector<std::vector<parse>> items;
        for (const auto [part_begin, part_end] : parts) {
          std::vector<parse> fitting;
          if (part_begin < part_end)
            for (const auto &item : expressions(part_begin, part_end))
              // A list given for a list of what isn't known yet is the
              // list, not one item of a list of lists.
              if (assignable(field_type.element(), item.type) &&
                  (field_type.element().known() || item.type.depth == 0))
                fitting.push_back(item);
          items.push_back(std::move(fitting));
        }
        if (std::ranges::none_of(items, &std::vector<parse>::empty))
          for (const auto &chosen : combinations(items)) {
            parse list{.core = "(list", .reading = "[", .type = field_type};
            for (std::size_t index = 0; index < chosen.size(); ++index) {
              list.core += " " + chosen[index].core;
              list.reading += (index > 0 ? ", " : "") + chosen[index].reading;
              list.take_holes(chosen[index]);
              list.use_all(chosen[index]);
            }
            list.core += ")";
            list.reading += "]";
            values.push_back(std::move(list));
          }
      }
      std::vector<parse> choices;
      for (const auto &value : values) {
        if (field != nullptr && !assignable(field_type, value.type))
          continue;
        const auto bracket = value.reading.find(", ") != string_type::npos;
        parse item{.core = "(" + (field ? field->name : string_type("HOLE")) +
                           " " + value.core + ")",
                   .reading = (field ? field->spoken() : tokens[at].text) + " " +
                              (bracket ? "(" + value.reading + ")"
                                       : nested_reading(value, true)),
                   // What the value is: a generic record's types follow it.
                   .type = field ? (field_type.known() ? field_type : value.type)
                                 : value_kind{},
                   .level = value.level};
        if (field == nullptr) {
          if (value.holes > 0)
            continue;
          item.take_holes(hole(at, at + 1));
          item.hole_record = record->name;
        }
        item.take_holes(value);
        item.use_all(value);
        choices.push_back(std::move(item));
      }
      if (choices.empty())
        return;
      items.push_back(std::move(choices));
    }
    const auto article =
        p_use.arguments.empty()
            ? core::describe_in_english(core::value_type::record_of(record->name))
            : p_use.english();
    for (const auto &chosen : combinations(items)) {
      parse result{.shape = parse::shape_type::phrase,
                   .core = "(new " + made_name(*record, p_use),
                   .reading = article + " with",
                   .type = p_use,
                   .open_edge = true};
      std::set<string_type> given;
      bool is_repeated = false;
      for (std::size_t index = 0; index < chosen.size(); ++index) {
        const auto &item = chosen[index];
        result.core += " " + item.core;
        result.reading += (index > 0 ? ", " : " ") + item.reading;
        result.level = item.level;
        result.take_holes(item);
        result.use_all(item);
        is_repeated |=
            !given.insert(item.core.substr(1, item.core.find(' ') - 1)).second;
      }
      // A field is given once: that also keeps the unknown field of a line
      // from being offered the fields already there.
      if (result.holes > 1 || is_repeated)
        continue;
      for (const auto &field : record->fields)
        if (!given.contains(field.name) && field.default_core)
          result.core += " (" + field.name + " " +
                         default_in_use(*record, field, p_use) + ")";
      result.core += ")";
      if (result.type.arguments.empty())
        result.type.arguments = arguments_given(*record, chosen);
      p_results.push_back(std::move(result));
    }
  }

  // `price of b = 9.5`: sets a field.
  auto set_fields(std::size_t p_begin, std::size_t p_end,
                  std::vector<parse> &p_results) -> void {
    for (auto split = p_begin + 1; split + 1 < p_end; ++split) {
      if (depth[split] != depth[p_begin] || !tokens[split].is_symbol("="))
        continue;
      for (const auto &target : expressions(p_begin, split)) {
        if (!target.core.starts_with("(field "))
          continue;
        for (const auto &value : expressions(split + 1, p_end)) {
          if (!assignable(target.type, value.type) ||
              target.holes + value.holes > 1)
            continue;
          parse result{.core = "(set-field " +
                               target.core.substr(7, target.core.size() - 8) +
                               " " + value.core + ")",
                       .reading = target.reading + " = " + value.reading};
          result.take_holes(target);
          result.take_holes(value);
          result.use_all(target);
          result.use_all(value);
          p_results.push_back(std::move(result));
        }
      }
    }
  }

  // Changing a variable or a field in words: `set b's pages to 5`,
  // `increase count by 1`, `add 1 to b's pages`, `subtract 2 from total`,
  // `multiply x by 3`. The variable must exist (`set x to 5` for a new one
  // is an assignment, `shape_of`).
  auto updates(std::size_t p_begin, std::size_t p_end,
               std::vector<parse> &p_results) -> void {
    struct form {
      string_view_type verb;
      string_view_type between;
      // `add V to T` names the value first.
      bool value_first;
      // The operator applied to the target and the value, or none to set it.
      string_view_type operation;
    };
    static constexpr form forms[] = {
        {"set", "to", false, ""},       {"change", "to", false, ""},
        {"increase", "by", false, "+"}, {"decrease", "by", false, "-"},
        {"multiply", "by", false, "*"}, {"divide", "by", false, "/"},
        {"add", "to", true, "+"},       {"subtract", "from", true, "-"}};
    if (p_end - p_begin < 3)
      return;
    // The verb after the target: `count decrease by 1`, `count increased by
    // 1`. Only for the forms that name the target first.
    for (const auto &item : forms) {
      if (item.value_first || item.operation.empty())
        continue;
      for (auto verb = p_begin + 1; verb + 2 < p_end; ++verb) {
        if (depth[verb] != depth[p_begin] ||
            !(tokens[verb].is_word(item.verb) ||
              tokens[verb].is_word(string_type(item.verb) + "d")) ||
            !tokens[verb + 1].is_word(item.between))
          continue;
        auto moved = tokens;
        moved[verb].text = string_type(item.verb);
        std::rotate(moved.begin() + static_cast<std::ptrdiff_t>(p_begin),
                    moved.begin() + static_cast<std::ptrdiff_t>(verb),
                    moved.begin() + static_cast<std::ptrdiff_t>(verb + 1));
        line_parser again(moved, scope, words, allow_holes);
        for (auto reading : again.statements(p_begin, p_end, false))
          p_results.push_back(std::move(reading));
      }
    }
    // Changes the target to the value (or by it) as the form says.
    const auto change = [&](const form &item, std::size_t target_begin,
                            std::size_t target_end, std::size_t value_begin,
                            std::size_t value_end) {
      // A variable already set (`fan speed`), or a field.
      string_type variable;
      for (auto index = target_begin; index < target_end; ++index)
        variable += (variable.empty() ? "" : " ") + tokens[index].text;
      const auto is_variable = scope.contains(variable);
      for (const auto &target : expressions(target_begin, target_end)) {
        const auto is_field = target.core.starts_with("(field ");
        if (!(is_field || (is_variable && target.holes == 0 &&
                           target.core == core_name(variable))))
          continue;
        for (const auto &value : expressions(value_begin, value_end)) {
          if (target.holes + value.holes > 1)
            continue;
          auto core = value.core;
          if (!item.operation.empty()) {
            // An untyped parameter may be a list: `add x to xs` is then
            // the list phrase's alone.
            if (!target.type.known())
              continue;
            const auto type =
                operation_type(item.operation, target.type, value.type, words);
            if (!type || !assignable(target.type, *type))
              continue;
            core = operation_core(item.operation, target, value, words);
          } else if (!assignable(target.type, value.type)) {
            continue;
          }
          parse result{
              .core = is_field ? "(set-field " +
                                     target.core.substr(7, target.core.size() - 8) +
                                     " " + core + ")"
                               : "(set " + target.core + " " + core + ")",
              .reading = string_type(item.verb) + " " +
                         (item.value_first ? value.reading : target.reading) +
                         " " + string_type(item.between) + " " +
                         (item.value_first ? target.reading : value.reading)};
          result.take_holes(target);
          result.take_holes(value);
          result.use_all(target);
          result.use_all(value);
          p_results.push_back(std::move(result));
        }
      }
    };
    for (const auto &item : forms) {
      if (!tokens[p_begin].is_word(item.verb))
        continue;
      for (auto split = p_begin + 2; split + 1 < p_end; ++split) {
        if (depth[split] != depth[p_begin] || !tokens[split].is_word(item.between))
          continue;
        if (item.value_first)
          change(item, split + 1, p_end, p_begin + 1, split);
        else
          change(item, p_begin + 1, split, split + 1, p_end);
      }
    }
    // The same change without `to`: `w's age becomes 13`, `make w's age 14`.
    // Both read as the first form, `set w's age to 13`.
    const auto &set = forms[0];
    for (auto split = p_begin + 1; split + 1 < p_end; ++split)
      if (depth[split] == depth[p_begin] &&
          (tokens[split].is_word("becomes") || tokens[split].is_word("become")))
        change(set, p_begin, split, split + 1, p_end);
    if (tokens[p_begin].is_word("make"))
      for (auto split = p_begin + 2; split < p_end; ++split)
        if (depth[split] == depth[p_begin])
          change(set, p_begin + 1, split, split, p_end);
  }

  auto expressions(std::size_t p_begin, std::size_t p_end)
      -> const std::vector<parse> & {
    const auto key = std::pair(p_begin, p_end);
    if (const auto found = chart.find(key); found != chart.end())
      return found->second;
    // A span being read doesn't contain itself: an empty entry stands until
    // it is done.
    chart[key];
    std::vector<parse> results;
    if (p_begin >= p_end || !balanced(p_begin, p_end))
      return chart[key] = results;

    if (is_article(tokens[p_begin]) && p_end - p_begin > 1)
      std::ranges::copy(expressions(p_begin + 1, p_end),
                        std::back_inserter(results));
    if (p_end - p_begin == 1)
      if (auto value = atom(tokens[p_begin]))
        results.push_back(std::move(*value));
    // A variable named in several words (`fan speed`), once it is set.
    if (p_end - p_begin > 1 && p_end - p_begin <= 4) {
      string_type name;
      bool words_only = true;
      for (auto index = p_begin; index < p_end; ++index) {
        words_only = words_only && tokens[index].kind == token::kind_type::word;
        name += (name.empty() ? "" : " ") + tokens[index].text;
      }
      if (const auto found = scope.find(name); words_only && found != scope.end())
        results.push_back(
            {.core = core_name(name), .reading = name, .type = found->second});
    }

    // `ref x` passes a variable's address to a C function, for output
    // parameters; only calls take it.
    if (p_end - p_begin == 2 && tokens[p_begin].is_word("ref") &&
        p_begin > 0 && (tokens[p_begin - 1].is_symbol("(") ||
                        tokens[p_begin - 1].is_symbol(","))) {
      if (const auto found = scope.find(tokens[p_begin + 1].text);
          found != scope.end())
        results.push_back({.core = "(ref " + core_name(found->first) + ")",
                           .reading = "ref " + found->first,
                           .type = {"ptr"}});
    }

    // (…) and […]
    if (partner[p_begin] == p_end - 1 && tokens[p_begin].is_symbol("("))
      for (auto inner : expressions(p_begin + 1, p_end - 1)) {
        inner.reading = "(" + inner.reading + ")";
        inner.shape = parse::shape_type::atom;
        inner.level = primary_level;
        results.push_back(std::move(inner));
      }
    if (partner[p_begin] == p_end - 1 && tokens[p_begin].is_symbol("[") &&
        p_end - p_begin > 2)
      // Grouped as a variadic slot's items: an item may have commas (`[a
      // book with title "Emma", pages 474]`).
      for (const auto &grouping : groupings(p_begin + 1, p_end - 1))
      for (const auto &elements : combinations(grouping)) {
        if (elements.empty())
          continue;
        parse list{.core = "(list", .reading = "["};
        auto element = elements[0].type;
        bool fits = true;
        for (std::size_t index = 0; index < elements.size(); ++index) {
          const auto &item = elements[index];
          list.core += " " + item.core;
          list.reading += (index > 0 ? ", " : "") + item.reading;
          list.take_holes(item);
          list.use_all(item);
          if (element.is_number() && item.type.is_number() &&
              item.type.base == "float")
            element = item.type;
          else if (item.type != element && item.type.known() &&
                   !(element.is_number() && item.type.is_number()))
            fits = false;
        }
        list.core += ")";
        list.reading += "]";
        // Different records that are all one trait: a list of that trait.
        if (!fits && element.is_record()) {
          std::vector<value_kind> kinds;
          for (const auto &item : elements)
            kinds.push_back(item.type);
          if (const auto trait = words.common_trait(kinds)) {
            element = words.records.at(*trait).type();
            fits = true;
          }
        }
        list.type = element.known() ? element.with_depth(element.depth + 1)
                                    : value_kind{};
        if (fits)
          results.push_back(std::move(list));
      }

    // not …, - …
    if (tokens[p_begin].is_word("not"))
      for (const auto &operand : expressions(p_begin + 1, p_end))
        if (operand.level >= not_level &&
            operation_type("and", operand.type, operand.type, words))
          results.push_back({.shape = parse::shape_type::operation,
                             .core = "(not " + operand.core + ")",
                             .reading = "not " + nested_reading(operand, false),
                             .type = {"bool"},
                             .level = not_level,
                             .holes = operand.holes,
                             .hole_begin = operand.hole_begin,
                             .hole_end = operand.hole_end,
                             .hole_record = operand.hole_record,
                             .uses = operand.uses});
    if (tokens[p_begin].is_symbol("-"))
      for (const auto &operand : expressions(p_begin + 1, p_end))
        if (operand.level >= negation_level &&
            (operand.type.is_number() || !operand.type.known()))
          results.push_back({.shape = parse::shape_type::operation,
                             .core = "(- " + operand.core + ")",
                             .reading = "-" + nested_reading(operand, false),
                             .type = operand.type,
                             .level = negation_level,
                             .holes = operand.holes,
                             .hole_begin = operand.hole_begin,
                             .hole_end = operand.hole_end,
                             .hole_record = operand.hole_record,
                             .uses = operand.uses});

    // left OPERATOR right, left-associative.
    for (auto split = p_begin + 1; split + 1 < p_end; ++split) {
      if (depth[split] != depth[p_begin])
        continue;
      const auto info = operator_at(tokens[split]);
      if (!info)
        continue;
      for (const auto &left : expressions(p_begin, split)) {
        if (left.level < info->level)
          continue;
        for (const auto &right : expressions(split + 1, p_end)) {
          if (right.level <= info->level || left.holes + right.holes > 1)
            continue;
          const auto type =
              operation_type(info->core, left.type, right.type, words);
          if (!type)
            continue;
          results.push_back(
              {.shape = parse::shape_type::operation,
               .core = operation_core(info->core, left, right, words),
               .reading = nested_reading(left, false) + " " +
                          tokens[split].text + " " +
                          nested_reading(right, false),
               .type = *type,
               .level = info->level});
          results.back().take_holes(right);
          results.back().take_holes(left);
          results.back().use_all(left);
          results.back().use_all(right);
        }
      }
    }

    calls(p_begin, p_end, false, results);
    field_accesses(p_begin, p_end, results);
    new_records(p_begin, p_end, results);
    phrase_values(p_begin, p_end, results);
    add_phrases(phrase_template::kind_type::expression, false, p_begin, p_end,
                results);

    unique(results);
    if (results.size() > max_readings)
      results.resize(max_readings);
    if (results.empty() && allow_holes)
      results.push_back(hole(p_begin, p_end));
    return chart[key] = std::move(results);
  }

  // Readings of the whole span as a statement phrase.
  auto statements(std::size_t p_begin, std::size_t p_end, bool p_block)
      -> std::vector<parse> {
    std::vector<parse> results;
    if (!p_block) {
      calls(p_begin, p_end, true, results);
      set_fields(p_begin, p_end, results);
      updates(p_begin, p_end, results);
    }
    add_phrases(phrase_template::kind_type::statement, p_block, p_begin, p_end,
                results);
    unique(results);
    return results;
  }
};

line_parser::line_parser(const std::vector<token> &p_tokens,
                         const std::map<string_type, value_kind> &p_scope,
                         const vocabulary &p_vocabulary, bool p_allow_holes)
    : impl(std::make_unique<state>(p_tokens, p_scope, p_vocabulary,
                                   p_allow_holes)) {}

line_parser::~line_parser() = default;

auto line_parser::expressions(std::size_t p_begin, std::size_t p_end)
    -> const std::vector<parse> & {
  return impl->expressions(p_begin, p_end);
}

auto line_parser::statements(std::size_t p_begin, std::size_t p_end,
                             bool p_block) -> std::vector<parse> {
  return impl->statements(p_begin, p_end, p_block);
}

auto shape_of(const std::vector<token> &p_tokens) -> line_shape {
  using enum line_shape::kind_type;
  const auto size = p_tokens.size();
  const auto block = size > 0 && p_tokens.back().is_symbol(":");
  const auto word = [&](std::size_t p_index, string_view_type p_word) {
    return p_index < size && p_tokens[p_index].is_word(p_word);
  };
  const auto with_block = [&](line_shape::kind_type p_kind, std::size_t p_begin,
                              string_view_type p_example) {
    line_shape shape{.kind = p_kind, .begin = p_begin, .end = size - 1,
                     .takes_block = true};
    if (!block)
      shape.problem = "End the line with ':' and indent what it covers "
                      "under it, as in: " + string_type(p_example);
    else if (p_begin >= size - 1 && p_kind != else_)
      shape.problem = "Say what this line depends on before the ':'.";
    return shape;
  };

  if (word(0, "if"))
    return with_block(if_, 1, "if x > 0:");
  if (word(0, "elif"))
    return with_block(else_if, 1, "otherwise if x > 0:");
  if ((word(0, "otherwise") || word(0, "else")) && word(1, "if"))
    return with_block(else_if, 2, "otherwise if x > 0:");
  if (word(0, "otherwise") || word(0, "else")) {
    auto shape = with_block(else_, 1, "otherwise:");
    if (block && size != 2)
      shape.problem = "Write 'otherwise if …:' or just 'otherwise:'.";
    return shape;
  }
  if (word(0, "while"))
    return with_block(while_, 1, "while x > 0:");
  if (word(0, "for")) {
    const auto first = word(1, "each") ? std::size_t(2) : std::size_t(1);
    auto shape = with_block(each, first + 2, "for each score in scores:");
    if (first >= size || p_tokens[first].kind != token::kind_type::word ||
        !word(first + 1, "in"))
      shape.problem = "Write a loop as 'for each NAME in LIST:'.";
    else
      shape.name = p_tokens[first].text;
    return shape;
  }
  if (word(0, "repeat")) {
    auto shape = with_block(repeat, 1, "repeat 3 times:");
    if (size < 3 || !word(size - 2, "times"))
      shape.problem = "Write 'repeat N times:'.";
    shape.end = size >= 2 ? size - 2 : 0;
    return shape;
  }
  if (word(0, "to") && block)
    return {.kind = define, .begin = 1, .end = size - 1, .takes_block = true};
  // `a book has:`, `a shape can:`, `a circle is a shape:`
  // `stack of (item)`, `table from (key) to (value)`: a generic record's
  // name (the words before the first function word or bracket), its type
  // variables (the last word in each bracket; words before it are bounds)
  // and its header.
  const auto has_bracket = [&](std::size_t p_begin, std::size_t p_end) {
    for (auto index = p_begin; index < p_end; ++index)
      if (p_tokens[index].is_symbol("("))
        return true;
    return false;
  };
  const auto generic_names = [&](line_shape &p_shape, std::size_t p_begin,
                                 std::size_t p_end) -> string_type {
    static const std::set<string_type> joining = {"of", "from", "for", "with",
                                                  "to", "and", "in", "on", "by"};
    auto name_end = p_begin;
    while (name_end < p_end && p_tokens[name_end].kind == token::kind_type::word &&
           !joining.contains(lower(p_tokens[name_end].text)))
      ++name_end;
    if (name_end == p_begin)
      return "A generic record's name comes first, as in: a stack of (item) has:";
    p_shape.name_end = name_end;
    for (auto index = name_end; index < p_end; ++index) {
      const auto &item = p_tokens[index];
      if (item.kind == token::kind_type::word) {
        p_shape.header.push_back(lower(item.text));
        continue;
      }
      if (!item.is_symbol("("))
        return "Write a type variable in brackets, as in: a stack of (item) has:";
      std::vector<string_type> words;
      auto close = index + 1;
      for (; close < p_end && p_tokens[close].kind == token::kind_type::word;
           ++close)
        if (!p_tokens[close].is_word("and"))
          words.push_back(lower(p_tokens[close].text));
      if (close == p_end || !p_tokens[close].is_symbol(")") || words.empty())
        return "Write a type variable in brackets, as in: a stack of (item) has:";
      p_shape.parameters.push_back(words.back());
      words.pop_back();
      p_shape.bounds.push_back(std::move(words));
      p_shape.header.emplace_back();
      index = close;
    }
    return "";
  };
  const auto names_of = [&](std::size_t p_begin, std::size_t p_end,
                            string_type &p_name, string_view_type p_example) {
    string_type problem;
    for (auto index = p_begin; index < p_end; ++index) {
      const auto &item = p_tokens[index];
      if (item.kind != token::kind_type::word ||
          !std::ranges::all_of(item.text, [](char_type p_character) {
            return std::isalpha(static_cast<unsigned char>(p_character)) != 0;
          }))
        problem = "A name is words of letters, as in: " + string_type(p_example);
      p_name += (p_name.empty() ? "" : "_") + lower(p_tokens[index].text);
    }
    return problem;
  };
  // `a header has, packed:`: its fields with no gaps between them.
  if (block && size >= 6 && (word(0, "a") || word(0, "an")) &&
      word(size - 4, "has") && p_tokens[size - 3].is_symbol(",") &&
      word(size - 2, "packed")) {
    line_shape shape{.kind = record, .begin = 1, .end = size - 4,
                     .takes_block = true};
    shape.packed = true;
    shape.name_end = shape.end;
    shape.problem = names_of(shape.begin, shape.end, shape.name,
                             "a file header has, packed:");
    return shape;
  }
  if (block && size >= 4 && (word(0, "a") || word(0, "an")) &&
      (word(size - 2, "has") || word(size - 2, "can"))) {
    line_shape shape{.kind = word(size - 2, "has") ? record : trait_decl,
                     .begin = 1, .end = size - 2, .takes_block = true};
    shape.name_end = shape.end;
    if (shape.kind == record && has_bracket(shape.begin, shape.end))
      shape.problem = generic_names(shape, shape.begin, shape.end);
    if (shape.problem.empty())
      shape.problem = names_of(shape.begin, shape.name_end, shape.name,
                               shape.kind == record ? "an order item has:"
                                                    : "a shape can:");
    return shape;
  }
  // `a book is comparable:`: the phrases that make a record comparable,
  // equatable or showable.
  if (block && size >= 5 && (word(0, "a") || word(0, "an")))
    for (std::size_t is = 2; is + 2 <= size - 1; ++is)
      if (is + 2 == size - 1 && word(is, "is") && (word(is + 1, "comparable") ||
                             word(is + 1, "equatable") ||
                             word(is + 1, "showable"))) {
        line_shape shape{.kind = impl_decl, .begin = 1, .end = is,
                         .takes_block = true};
        shape.trait_begin = is + 1;
        shape.trait_end = size - 1;
        shape.trait_name = lower(p_tokens[is + 1].text);
        shape.name_end = shape.end;
        if (has_bracket(shape.begin, shape.end))
          shape.problem = generic_names(shape, shape.begin, shape.end);
        if (shape.problem.empty())
          shape.problem = names_of(shape.begin, shape.name_end, shape.name,
                                   "a book is comparable:");
        return shape;
      }
  if (block && size >= 6 && (word(0, "a") || word(0, "an"))) {
    for (std::size_t is = 2; is + 3 < size; ++is) {
      if (!word(is, "is") || !(word(is + 1, "a") || word(is + 1, "an")))
        continue;
      line_shape shape{.kind = impl_decl, .begin = 1, .end = is,
                       .takes_block = true};
      shape.trait_begin = is + 2;
      shape.trait_end = size - 1;
      shape.name_end = shape.end;
      if (has_bracket(shape.begin, shape.end))
        shape.problem = generic_names(shape, shape.begin, shape.end);
      if (shape.problem.empty())
        shape.problem = names_of(shape.begin, shape.name_end, shape.name,
                                 "a circle is a shape:");
      if (shape.problem.empty())
        shape.problem = names_of(shape.trait_begin, shape.trait_end,
                                 shape.trait_name, "a circle is a shape:");
      return shape;
    }
  }
  // `a whole number is hashable:`, `text is a hashable:` (generics.md §4):
  // a type written in words with a user trait, no article needed.
  if (block && size >= 4) {
    const std::size_t begin = word(0, "a") || word(0, "an") ? 1 : 0;
    for (std::size_t is = begin + 1; is + 2 < size; ++is) {
      if (!word(is, "is"))
        continue;
      const auto as_type = type_words(p_tokens, begin, is);
      if (!std::ranges::any_of(as_type, [&](const auto &p_match) {
            return p_match.first == is - begin;
          }))
        break;
      line_shape shape{.kind = impl_decl, .begin = begin, .end = is,
                       .takes_block = true};
      shape.trait_begin = is + 1 + (word(is + 1, "a") || word(is + 1, "an") ? 1 : 0);
      shape.trait_end = size - 1;
      shape.name_end = shape.end;
      for (auto at = begin; at < is; ++at)
        shape.name += (shape.name.empty() ? "" : "_") + lower(p_tokens[at].text);
      if (shape.trait_begin >= shape.trait_end)
        break;
      shape.problem = names_of(shape.trait_begin, shape.trait_end,
                               shape.trait_name, "a whole number is hashable:");
      return shape;
    }
  }
  // `use … "header.h" [… "library"]`: any words around the texts.
  const auto is_header = [](const token &p_token) {
    return p_token.kind == token::kind_type::text && p_token.text.ends_with(".h");
  };
  if (word(0, "use") && std::ranges::any_of(p_tokens, is_header)) {
    line_shape shape{.kind = use_c};
    std::vector<string_type> texts;
    for (const auto &item : p_tokens)
      if (item.kind == token::kind_type::text)
        texts.push_back(item.text);
    if (texts.size() > 2 || !texts[0].ends_with(".h") || block)
      shape.problem = "Write 'use \"header.h\"', or 'use \"header.h\" from "
                      "\"library\"' to name the library to link.";
    shape.name = texts[0];
    shape.library = texts.size() == 2 ? texts[1] : string_type();
    return shape;
  }
  // `use csv`: one word, a dependency's name in ovis.toml.
  if (word(0, "use") && size == 2 && p_tokens[1].kind == token::kind_type::word)
    return {.kind = use_package, .name = lower(p_tokens[1].text)};
  if (word(0, "use") && size == 3 && block && p_tokens[1].kind == token::kind_type::word)
    return {.kind = use_package,
            .name = lower(p_tokens[1].text),
            .problem = "Write 'use " + p_tokens[1].text + "' on a line of its own."};
  // `at the end, close file`, `at the end:`; `defer` is the same for readers
  // who know Zig or Go.
  if ((word(0, "at") && word(1, "the") && word(2, "end")) ||
      (word(0, "defer") && size > 1 &&
       !p_tokens[1].is_symbol("="))) {
    auto first = word(0, "defer") ? std::size_t(1) : std::size_t(3);
    if (first < size && p_tokens[first].is_symbol(","))
      ++first;
    const auto words = size - (block ? 1 : 0);
    if (block && first >= words)
      return {.kind = defer_block, .begin = first, .end = first,
              .takes_block = true};
    line_shape shape{.kind = defer_line, .begin = first, .end = size};
    if (block)
      shape.problem = "Write 'at the end:' and indent the statements under "
                      "it, or 'at the end, STATEMENT' on one line.";
    else if (first >= size)
      shape.problem = "Say what to do at the end, as in: at the end, "
                      "close file";
    return shape;
  }
  if (word(0, "give") && word(1, "back"))
    return {.kind = give_back, .begin = 2, .end = size};
  if (word(0, "do") && block && size == 2)
    return {.kind = do_block, .begin = 1, .end = 1, .takes_block = true};
  if (word(0, "leave") && (size == 1 || word(1, "with"))) {
    line_shape shape{.kind = leave_block,
                     .begin = size == 1 ? std::size_t(1) : std::size_t(2),
                     .end = size};
    if (size == 2 || block)
      shape.problem = "Write 'leave' to leave a 'do:' block, or 'leave with "
                      "VALUE' to give it a value.";
    return shape;
  }
  if ((word(0, "stop") || word(0, "skip")) && size == 1)
    return {.kind = word(0, "stop") ? stop : skip};
  // A variable's name: one word, or up to four (`fan speed`) without `of`,
  // so `price of b = 9.5` stays a field's. A leading article isn't part of
  // it. The words from `p_begin` up to `p_end`; empty if they aren't one.
  const auto name_of = [&](std::size_t p_begin, std::size_t p_end) {
    if (p_end > p_begin + 1 && is_article(p_tokens[p_begin]))
      ++p_begin;
    string_type name;
    if (p_end <= p_begin || p_end - p_begin > 4)
      return name;
    for (auto index = p_begin; index < p_end; ++index) {
      if (p_tokens[index].kind != token::kind_type::word ||
          (p_end - p_begin > 1 && p_tokens[index].is_word("of")))
        return string_type();
      name += (name.empty() ? "" : " ") + p_tokens[index].text;
    }
    return name;
  };
  const auto assigned = [&](string_type p_name, std::size_t p_value) {
    // `total = do:`: a block gives the value.
    if (block && p_value + 2 == size && word(p_value, "do"))
      return line_shape{.kind = do_block, .name = std::move(p_name),
                        .begin = size - 1, .end = size - 1,
                        .takes_block = true};
    line_shape shape{.kind = assign, .name = std::move(p_name),
                     .begin = p_value, .end = size};
    if (block)
      shape.problem = "An assignment can't open a block: remove the ':'.";
    return shape;
  };
  // `set x to 5`, `change x to 5`, `let x be 5`: `x = 5` in words. A field
  // (`set b's pages to 5`) is a statement phrase (`updates`).
  if (word(0, "set") || word(0, "change") || word(0, "let"))
    for (std::size_t at = 2; at + 1 < size && at <= 5; ++at)
      if (word(at, word(0, "let") ? "be" : "to"))
        if (auto name = name_of(1, at); !name.empty())
          return assigned(std::move(name), at + 1);
  for (std::size_t at = 1; at + 1 < size && at <= 5; ++at)
    if (p_tokens[at].is_symbol("="))
      if (auto name = name_of(0, at); !name.empty())
        return assigned(std::move(name), at + 1);
  return {.kind = phrase, .begin = 0, .end = block ? size - 1 : size,
          .takes_block = block};
}

auto readings_of(line_parser &p_parser, const line_shape &p_shape)
    -> std::vector<parse> {
  return p_shape.is_statement()
             ? p_parser.statements(p_shape.begin, p_shape.end,
                                   p_shape.takes_block)
             : p_parser.expressions(p_shape.begin, p_shape.end);
}

} // namespace ovis::phrase::internal

#include <algorithm>
#include <iterator>
#include <map>

#include <ovis/core/ast.hpp>

namespace ovis::core {

namespace {

auto fail(const sexpr &p_form, const string_type &p_message) -> error {
  return error("line " + std::to_string(p_form.location.line) + ": " +
               p_message + " In: " + p_form.to_string());
}

// Spellings that mean the same operation. The checker and code generator only
// ever see the canonical name on the right.
const std::map<string_view_type, string_view_type> &operation_aliases();

auto canonical_operation(string_view_type p_name) -> string_type {
  const auto &aliases = operation_aliases();
  if (const auto found = aliases.find(p_name); found != aliases.end())
    return string_type(found->second);
  return string_type(p_name);
}

const std::map<string_view_type, string_view_type> &operation_aliases() {
  static const std::map<string_view_type, string_view_type> aliases = {
      {"==", "="},      {"eq", "="},        {"<>", "!="},    {"/=", "!="},
      {"mod", "%"},     {"&&", "and"},      {"||", "or"},    {"!", "not"},
      {"length", "len"}, {"size", "len"},   {"count", "len"}, {"at", "get"},
      {"nth", "get"},   {"push", "append"}, {"input", "read_int"},
      {"//", "div"},    {"idiv", "div"},    {"quotient", "div"},
  };
  return aliases;
}

auto parse_base_type(string_view_type p_name) -> std::optional<base_type> {
  static const std::map<string_view_type, base_type> names = {
      {"int", base_type::integer},     {"integer", base_type::integer},
      {"i64", base_type::integer},     {"float", base_type::real},
      {"real", base_type::real},       {"double", base_type::real},
      {"number", base_type::real},     {"f64", base_type::real},
      {"bool", base_type::boolean},    {"boolean", base_type::boolean},
      {"str", base_type::string},      {"string", base_type::string},
      {"text", base_type::string},     {"void", base_type::nothing},
      {"nothing", base_type::nothing}, {"none", base_type::nothing},
      {"ptr", base_type::pointer},     {"pointer", base_type::pointer},
      {"memory", base_type::memory},
      {"?", base_type::unknown},       {"any", base_type::unknown},
  };
  if (const auto found = names.find(p_name); found != names.end())
    return found->second;
  return std::nullopt;
}

auto parse_expression(const sexpr &p_form) -> expression {
  expression result{.location = p_form.location};
  switch (p_form.kind) {
  case sexpr::kind_type::integer:
    result.kind = expression::kind_type::integer;
    result.integer = p_form.integer;
    result.is_big = p_form.is_big;
    return result;
  case sexpr::kind_type::real:
    result.kind = expression::kind_type::real;
    result.real = p_form.real;
    return result;
  case sexpr::kind_type::string:
    result.kind = expression::kind_type::string;
    result.text = p_form.text;
    return result;
  case sexpr::kind_type::symbol:
    if (p_form.text == "true" || p_form.text == "false") {
      result.kind = expression::kind_type::boolean;
      result.boolean = p_form.text == "true";
    } else {
      result.kind = expression::kind_type::variable;
      result.text = p_form.text;
    }
    return result;
  case sexpr::kind_type::list:
    break;
  }

  if (p_form.head().empty())
    throw fail(p_form, "A call must start with the name of an operation or "
                       "function.");
  result.kind = expression::kind_type::call;
  result.text = canonical_operation(p_form.head());

  auto first_argument = p_form.items.begin() + 1;
  if (result.text == "new") {
    const auto &named = p_form.items.size() < 2 ? p_form : p_form.items[1];
    // `(new (stack int) …)`: a generic record, its types given.
    if (named.is_list() && named.items.size() > 1 && named.items[0].is_symbol()) {
      auto spelled = named;
      spelled.items.insert(spelled.items.begin(),
                           sexpr{.kind = sexpr::kind_type::symbol,
                                 .text = "record",
                                 .location = named.location});
      const auto type = value_type::parse(spelled);
      if (!type)
        throw fail(named, "Expected a record's name and its types.");
      result.type_argument = *type;
    } else if (p_form.items.size() < 2 || !p_form.items[1].is_symbol()) {
      throw fail(p_form, "Expected (new NAME (FIELD VALUE)...).");
    } else {
      result.type_argument = value_type::record_of(p_form.items[1].text);
    }
    for (auto item = p_form.items.begin() + 2; item != p_form.items.end();
         ++item) {
      if (!item->is_list() || item->items.size() != 2 ||
          !item->items[0].is_symbol())
        throw fail(*item, "Expected a field as (FIELD VALUE).");
      result.fields.push_back(item->items[0].text);
      result.arguments.push_back(parse_expression(item->items[1]));
    }
    return result;
  }
  if (result.text == "field") {
    if (p_form.items.size() != 3 || !p_form.items[2].is_symbol())
      throw fail(p_form, "Expected (field RECORD NAME).");
    result.arguments.push_back(parse_expression(p_form.items[1]));
    result.fields.push_back(p_form.items[2].text);
    return result;
  }
  // Raw pointers (references.md §5): what is at the address is written
  // first, kept in `fields`: `(load i32 P)`, `(store i32 P V)`, `(size-of
  // i32)`, `(offset P N [i32])`; `(cast TYPE X)` changes the type.
  if (result.text == "load" || result.text == "store" ||
      result.text == "size-of" || result.text == "view") {
    const std::size_t count = result.text == "load"    ? 3
                              : result.text == "size-of" ? 2
                                                       : 4;
    if (p_form.items.size() != count)
      throw fail(p_form, result.text == "load"    ? "Expected (load TYPE POINTER)."
                         : result.text == "store" ? "Expected (store TYPE POINTER VALUE)."
                         : result.text == "view"  ? "Expected (view TYPE POINTER COUNT)."
                                                  : "Expected (size-of TYPE).");
    const auto pointee = parse_pointee(p_form.items[1]);
    if (!pointee)
      throw fail(p_form.items[1], "Expected what is at the address: a C "
                                  "number such as i32, or a type.");
    result.fields.push_back(*pointee);
    for (auto argument = p_form.items.begin() + 2; argument != p_form.items.end();
         ++argument)
      result.arguments.push_back(parse_expression(*argument));
    return result;
  }
  // A generic C macro's type or field argument (generics.md §9): `(offsetof
  // (type (record point)) (member y))`.
  if (result.text == "type" || result.text == "member") {
    if (p_form.items.size() != 2)
      throw fail(p_form, "Expected (type TYPE) or (member NAME).");
    if (result.text == "member") {
      if (!p_form.items[1].is_symbol())
        throw fail(p_form, "Expected (member NAME).");
      result.fields.push_back(p_form.items[1].text);
      return result;
    }
    const auto pointee = parse_pointee(p_form.items[1]);
    if (!pointee)
      throw fail(p_form.items[1], "Expected a type.");
    result.fields.push_back(*pointee);
    return result;
  }
  // Typed memory (generics.md §5): `(alloc T N)` makes room for N of T,
  // `(alloc-more P T OLD NEW)` grows it, both in the current memory.
  if (result.text == "alloc" || result.text == "alloc-more") {
    const bool is_more = result.text == "alloc-more";
    if (p_form.items.size() != (is_more ? 5 : 3))
      throw fail(p_form, is_more ? "Expected (alloc-more POINTER TYPE OLD NEW)."
                                 : "Expected (alloc TYPE COUNT).");
    const auto &type = p_form.items[is_more ? 2 : 1];
    const auto pointee = parse_pointee(type);
    if (!pointee)
      throw fail(type, "Expected what the room is for: a C number such as "
                       "i32, or a type.");
    result.fields.push_back(*pointee);
    for (std::size_t index = 1; index < p_form.items.size(); ++index)
      if (index != (is_more ? 2u : 1u))
        result.arguments.push_back(parse_expression(p_form.items[index]));
    return result;
  }
  if (result.text == "offset") {
    if (p_form.items.size() != 3 && p_form.items.size() != 4)
      throw fail(p_form, "Expected (offset POINTER COUNT [TYPE]).");
    result.arguments.push_back(parse_expression(p_form.items[1]));
    result.arguments.push_back(parse_expression(p_form.items[2]));
    if (p_form.items.size() == 4) {
      const auto pointee = parse_pointee(p_form.items[3]);
      if (!pointee)
        throw fail(p_form.items[3], "Expected what to count in: a C number "
                                    "such as i32, or a type.");
      result.fields.push_back(*pointee);
    }
    return result;
  }
  // `(map-new str int)`: an empty map from text to whole numbers.
  if (result.text == "map-new") {
    const auto key = p_form.items.size() == 3 ? value_type::parse(p_form.items[1])
                                              : std::nullopt;
    const auto value = p_form.items.size() == 3
                           ? value_type::parse(p_form.items[2])
                           : std::nullopt;
    if (!key || !value)
      throw fail(p_form, "Expected (map-new KEY-TYPE VALUE-TYPE).");
    result.type_argument = value_type::map_of(*key, *value);
    return result;
  }
  // `(as i16 X)`, `(wrap-to u8 X)`, `(clamp-to u8 X)`: a number as another
  // number type.
  // `(from-bytes i32 BYTES INDEX)`: the number whose bytes start there.
  if (result.text == "as" || result.text == "wrap-to" ||
      result.text == "clamp-to" || result.text == "from-bytes" ||
      result.text == "from-bytes-big") {
    const bool from_bytes = result.text.starts_with("from-bytes");
    if (p_form.items.size() != (from_bytes ? 4 : 3))
      throw fail(p_form, "Expected (" + result.text + " TYPE " +
                             (from_bytes ? "BYTES INDEX" : "VALUE") + ").");
    result.type_argument = value_type::parse(p_form.items[1]);
    if (!result.type_argument || !result.type_argument->is_numeric())
      throw fail(p_form.items[1], "Expected a number type such as i32 or "
                                  "float.");
    for (auto item = p_form.items.begin() + 2; item != p_form.items.end(); ++item)
      result.arguments.push_back(parse_expression(*item));
    return result;
  }
  if (result.text == "cast") {
    if (p_form.items.size() != 3)
      throw fail(p_form, "Expected (cast TYPE VALUE).");
    result.type_argument = value_type::parse(p_form.items[1]);
    if (!result.type_argument || !result.type_argument->is_known())
      throw fail(p_form.items[1], "Expected a type to change to.");
    result.arguments.push_back(parse_expression(p_form.items[2]));
    return result;
  }
  if (result.text == "array") {
    if (p_form.items.size() != 3)
      throw fail(p_form, "Expected (array TYPE COUNT).");
    result.type_argument = value_type::parse(p_form.items[1]);
    // `(array u8 256)`: 256 zeroed bytes.
    if (p_form.items[1].is_symbol())
      if (const auto number = c_number_type(p_form.items[1].text))
        result.type_argument = c_number_item(*number);
    if (!result.type_argument ||
        result.type_argument->base == base_type::nothing ||
        result.type_argument->base == base_type::pointer)
      throw fail(p_form.items[1], "Expected an element type such as int.");
    ++first_argument;
  }
  for (auto argument = first_argument; argument != p_form.items.end();
       ++argument)
    result.arguments.push_back(parse_expression(*argument));
  return result;
}

auto parse_statement(const sexpr &p_form) -> statement;
auto parse_statements(const sexpr &p_form) -> std::vector<statement>;

// `(let NAME VALUE STATEMENT...)`: small models sometimes nest what follows
// a new variable inside its let. It means the let, then the statements.
auto split_let(const sexpr &p_form) -> std::vector<sexpr> {
  const auto head = p_form.head();
  const auto &items = p_form.items;
  if ((head != "let" && head != "var" && head != "define") ||
      items.size() < 4 || (items.size() == 4 && value_type::parse(items[2])))
    return {p_form};
  auto let = p_form;
  let.items.resize(3);
  std::vector<sexpr> result = {let};
  for (auto item = items.begin() + 3; item != items.end(); ++item)
    for (auto &piece : split_let(*item))
      result.push_back(std::move(piece));
  return result;
}

auto parse_body(std::vector<sexpr>::const_iterator p_begin,
                std::vector<sexpr>::const_iterator p_end)
    -> std::vector<statement> {
  std::vector<statement> result;
  for (auto form = p_begin; form != p_end; ++form)
    for (const auto &piece : split_let(*form))
      for (auto &statement : parse_statements(piece))
        result.push_back(std::move(statement));
  return result;
}

// `(then a b)`, `(do a b)` and `(else a b)` are blocks, anything else is a
// single statement.
auto parse_block(const sexpr &p_form) -> std::vector<statement> {
  const auto head = p_form.head();
  if (head == "then" || head == "do" || head == "else" || head == "begin")
    return parse_body(p_form.items.begin() + 1, p_form.items.end());
  std::vector<statement> result;
  for (const auto &piece : split_let(p_form))
    for (auto &statement : parse_statements(piece))
      result.push_back(std::move(statement));
  return result;
}

auto expect_name(const sexpr &p_form) -> string_type {
  if (!p_form.is_symbol())
    throw fail(p_form, "Expected a name.");
  return p_form.text;
}

// `:LABEL`, the label of a block, or nothing.
auto label_of(const sexpr &p_form) -> std::optional<string_type> {
  if (!p_form.is_symbol() || p_form.text.size() < 2 ||
      !p_form.text.starts_with(':'))
    return std::nullopt;
  return p_form.text.substr(1);
}

auto is_block_form(const sexpr &p_form) -> bool {
  const auto head = p_form.head();
  return head == "block" || head == "do" || head == "begin";
}

// `(block [:LABEL] BODY...)`.
auto parse_bare_block(const sexpr &p_form) -> statement {
  statement result{.kind = statement::kind_type::block,
                   .location = p_form.location};
  auto first = p_form.items.begin() + 1;
  if (first != p_form.items.end())
    if (const auto label = label_of(*first)) {
      result.label = *label;
      ++first;
    }
  result.body = parse_body(first, p_form.items.end());
  return result;
}

// One form's statements: `(let x (block …))` is `(let x)`, its type
// inferred, then the block, which sets x; `(set x (block …))` is the block.
auto parse_statements(const sexpr &p_form) -> std::vector<statement> {
  const auto head = p_form.head();
  const auto &items = p_form.items;
  const bool is_let = head == "let" || head == "var" || head == "define";
  const bool is_set = head == "set" || head == "assign";
  if (!((is_let && (items.size() == 3 || items.size() == 4)) ||
        (is_set && items.size() == 3)) ||
      !is_block_form(items.back()))
    return {parse_statement(p_form)};
  std::vector<statement> result;
  if (is_let) {
    statement let{.kind = statement::kind_type::let,
                  .name = expect_name(items[1]),
                  .location = p_form.location};
    if (items.size() == 4) {
      let.declared_type = value_type::parse(items[2]);
      if (!let.declared_type)
        throw fail(items[2], "Unknown type.");
    }
    result.push_back(std::move(let));
  }
  auto block = parse_bare_block(items.back());
  block.name = expect_name(items[1]);
  block.location = p_form.location;
  result.push_back(std::move(block));
  return result;
}

auto parse_statement(const sexpr &p_form) -> statement {
  statement result{.location = p_form.location};
  const auto head = p_form.head();
  const auto &items = p_form.items;
  const auto size = items.size();
  using kind = statement::kind_type;

  if (head == "block")
    return parse_bare_block(p_form);
  if (head == "leave") {
    result.kind = kind::leave;
    auto first = items.begin() + 1;
    if (first != items.end())
      if (const auto label = label_of(*first)) {
        result.label = *label;
        ++first;
      }
    if (items.end() - first > 1)
      throw fail(p_form, "Expected (leave [:LABEL] [VALUE]).");
    if (first != items.end())
      result.values.push_back(parse_expression(*first));
    return result;
  }

  if (head == "let" || head == "var" || head == "define") {
    if (size < 3 || size > 4)
      throw fail(p_form, "Expected (let NAME [TYPE] VALUE).");
    result.kind = kind::let;
    result.name = expect_name(items[1]);
    if (size == 4) {
      result.declared_type = value_type::parse(items[2]);
      if (!result.declared_type)
        throw fail(items[2], "Unknown type.");
      result.values.push_back(parse_expression(items[3]));
    } else if (auto type = value_type::parse(items[2]);
               type && (items[2].is_list() ||
                        parse_base_type(items[2].text).has_value() ||
                        items[2].text.ends_with("[]"))) {
      result.declared_type = type;
    } else {
      result.values.push_back(parse_expression(items[2]));
    }
    return result;
  }

  if (head == "set" || head == "assign") {
    if (size != 3)
      throw fail(p_form, "Expected (set NAME VALUE).");
    if (items[1].head() == "get" && items[1].items.size() == 3) {
      result.kind = kind::put;
      result.name = expect_name(items[1].items[1]);
      result.values.push_back(parse_expression(items[1].items[2]));
    } else {
      result.kind = kind::set;
      result.name = expect_name(items[1]);
    }
    result.values.push_back(parse_expression(items[2]));
    return result;
  }

  if (head == "set-field" || head == "set_field") {
    if (size != 4 || !items[2].is_symbol())
      throw fail(p_form, "Expected (set-field RECORD NAME VALUE).");
    result.kind = kind::set_field;
    result.name = items[2].text;
    result.values.push_back(parse_expression(items[1]));
    result.values.push_back(parse_expression(items[3]));
    return result;
  }

  if (head == "put") {
    if (size != 4)
      throw fail(p_form, "Expected (put LIST INDEX VALUE).");
    result.kind = kind::put;
    result.name = expect_name(items[1]);
    result.values.push_back(parse_expression(items[2]));
    result.values.push_back(parse_expression(items[3]));
    return result;
  }

  if (head == "print" || head == "say" || head == "display") {
    result.kind = kind::print;
    for (std::size_t index = 1; index < size; ++index)
      result.values.push_back(parse_expression(items[index]));
    return result;
  }

  if (head == "if" || head == "when") {
    if (size < 3 || size > 4)
      throw fail(p_form, "Expected (if CONDITION (then ...) (else ...)).");
    result.kind = kind::if_;
    result.values.push_back(parse_expression(items[1]));
    result.body = parse_block(items[2]);
    if (size == 4)
      result.else_body = parse_block(items[3]);
    return result;
  }

  if (head == "while") {
    if (size < 2)
      throw fail(p_form, "Expected (while CONDITION BODY...).");
    result.kind = kind::while_;
    result.values.push_back(parse_expression(items[1]));
    result.body = parse_body(items.begin() + 2, items.end());
    return result;
  }

  if (head == "for" || head == "for-down") {
    if (size < 4)
      throw fail(p_form, "Expected (" + string_type(head) +
                             " NAME FIRST LAST BODY...).");
    result.kind = kind::for_;
    result.counts_down = head == "for-down";
    result.name = expect_name(items[1]);
    result.values.push_back(parse_expression(items[2]));
    result.values.push_back(parse_expression(items[3]));
    result.body = parse_body(items.begin() + 4, items.end());
    return result;
  }

  if (head == "each" || head == "foreach" || head == "for-each") {
    if (size < 3)
      throw fail(p_form, "Expected (each NAME LIST BODY...).");
    result.kind = kind::each;
    result.name = expect_name(items[1]);
    result.values.push_back(parse_expression(items[2]));
    result.body = parse_body(items.begin() + 3, items.end());
    return result;
  }

  if (head == "with-memory" || head == "with-new-memory") {
    const bool is_new = head == "with-new-memory";
    if (size < (is_new ? 2 : 3))
      throw fail(p_form, is_new ? "Expected (with-new-memory BODY...)."
                                : "Expected (with-memory MEMORY BODY...).");
    result.kind = kind::with_memory;
    if (!is_new)
      result.values.push_back(parse_expression(items[1]));
    result.body = parse_body(items.begin() + (is_new ? 1 : 2), items.end());
    return result;
  }

  if (head == "defer") {
    if (size < 2)
      throw fail(p_form, "Expected (defer STATEMENT...).");
    result.kind = kind::defer;
    result.body = parse_body(items.begin() + 1, items.end());
    return result;
  }

  if (head == "break" || head == "continue" || head == "pass") {
    if (size != 1)
      throw fail(p_form, "Expected (" + string_type(head) + ").");
    result.kind = head == "break"      ? kind::break_
                  : head == "continue" ? kind::continue_
                                       : kind::pass;
    return result;
  }

  if (head == "return") {
    if (size > 2)
      throw fail(p_form, "Expected (return [VALUE]).");
    result.kind = kind::return_;
    if (size == 2)
      result.values.push_back(parse_expression(items[1]));
    return result;
  }

  if (head == "fn" || head == "function" || head == "defn" ||
      head == "extern" || head == "link" || head == "use-c" ||
      head == "record" || head == "trait" || head == "impl" ||
      head == "use_c" || head == "include")
    throw fail(p_form, "`" + string_type(head) +
                           "` is only allowed at the top level.");
  if (head == "do" || head == "begin")
    return parse_bare_block(p_form);
  if (head == "then")
    throw fail(p_form, "A `then` block is only allowed directly inside an if.");

  // A bare value is only useful as the last line of a function, where the
  // elaborator turns it into the result.
  result.kind = kind::evaluate;
  result.values.push_back(parse_expression(p_form));
  return result;
}

// `(forall item (key equatable showable))`: type variables and their bounds.
auto parse_forall(const sexpr &p_form) -> std::vector<type_parameter> {
  std::vector<type_parameter> result;
  for (auto item = p_form.items.begin() + 1; item != p_form.items.end();
       ++item) {
    type_parameter parameter;
    if (item->is_symbol()) {
      parameter.name = item->text;
    } else if (item->is_list() && item->items.size() == 2 &&
               item->head() == "field" && item->items[1].is_symbol()) {
      parameter.name = item->items[1].text;
      parameter.is_field = true;
    } else if (item->is_list() && !item->items.empty() &&
               std::ranges::all_of(item->items, [](const sexpr &p_part) {
                 return p_part.is_symbol();
               })) {
      parameter.name = item->items[0].text;
      for (auto bound = item->items.begin() + 1; bound != item->items.end();
           ++bound)
        parameter.bounds.push_back(bound->text);
    } else {
      throw fail(*item, "Expected a type variable as NAME or (NAME TRAIT...).");
    }
    if (parse_base_type(parameter.name) || c_number_type(parameter.name))
      throw fail(*item, "'" + parameter.name +
                            "' is a type; a type variable needs a name of its own.");
    if (std::ranges::find(result, parameter.name, &type_parameter::name) !=
        result.end())
      throw fail(*item, "The type variable '" + parameter.name +
                            "' is written twice.");
    result.push_back(std::move(parameter));
  }
  return result;
}

auto is_forall(const sexpr &p_form) -> bool {
  return p_form.is_list() && p_form.head() == "forall";
}

auto parse_function(const sexpr &p_form) -> function {
  // `(fn NAME (forall …) (PARAMETER…) …)`: a generic function.
  if (p_form.items.size() > 2 && is_forall(p_form.items[2])) {
    auto plain = p_form;
    plain.items.erase(plain.items.begin() + 2);
    auto result = parse_function(plain);
    result.type_parameters = parse_forall(p_form.items[2]);
    return result;
  }
  const auto &items = p_form.items;
  if (items.size() < 3 || !items[2].is_list())
    throw fail(p_form, "Expected (fn NAME ((PARAMETER TYPE) ...) "
                       "RETURN-TYPE BODY...).");

  function result{.name = expect_name(items[1]), .location = p_form.location};
  // Parameter types may be left out: `(x int)`, `(x)` and `x` all work.
  for (const auto &declaration : items[2].items) {
    if (declaration.is_symbol()) {
      result.parameters.push_back({declaration.text, value_type::unknown()});
      continue;
    }
    if (!declaration.is_list() || declaration.items.empty() ||
        declaration.items.size() > 2)
      throw fail(declaration, "Expected a parameter as (NAME TYPE).");
    auto type = value_type::unknown();
    if (declaration.items.size() == 2) {
      const auto parsed = value_type::parse(declaration.items[1]);
      if (!parsed || *parsed == value_type::nothing())
        throw fail(declaration.items[1], "Unknown parameter type.");
      type = *parsed;
    }
    result.parameters.push_back({expect_name(declaration.items[0]), type});
  }

  // A missing return type is inferred by the elaborator.
  result.return_type = value_type::unknown();
  auto body_begin = items.begin() + 3;
  if (body_begin != items.end()) {
    if (const auto type = value_type::parse(*body_begin)) {
      result.return_type = *type;
      ++body_begin;
    }
  }
  result.body = parse_body(body_begin, items.end());
  return result;
}

// (extern NAME (TYPE ...) RESULT), where a parameter is TYPE or (NAME TYPE)
// and a trailing `...` makes the function variadic.
auto parse_extern(const sexpr &p_form) -> c_function {
  const auto &items = p_form.items;
  if (items.size() != 4 || !items[2].is_list())
    throw fail(p_form, "Expected (extern NAME (TYPE ...) RESULT-TYPE).");
  const auto type_of = [&](const sexpr &p_type) {
    const auto type =
        p_type.is_symbol() ? parse_c_type(p_type.text) : std::nullopt;
    if (!type)
      throw fail(p_type, "Unknown C type; use one of cint int size_t float "
                         "cfloat str ptr void.");
    return *type;
  };

  c_function result{.name = expect_name(items[1]),
                    .location = p_form.location};
  for (const auto &parameter : items[2].items) {
    if (parameter.is_symbol("...")) {
      result.is_variadic = true;
      continue;
    }
    if (result.is_variadic)
      throw fail(parameter, "`...` must come last.");
    const auto &type = parameter.is_list() && parameter.items.size() == 2
                           ? parameter.items[1]
                           : parameter;
    const auto parsed = type_of(type);
    if (parsed == c_type::void_type)
      throw fail(type, "A parameter cannot be void.");
    result.parameters.push_back(parsed);
  }
  result.result = type_of(items[3]);
  return result;
}

// `(METHOD (PARAM...) [RET])`, a method an impl must provide. The first
// parameter is the value itself and may be written without a type.
auto parse_trait_method(const sexpr &p_form, const trait &p_trait)
    -> trait_method {
  const auto &items = p_form.items;
  if (items.size() < 2 || items.size() > 3 || !items[0].is_symbol() ||
      !items[1].is_list() || items[1].items.empty())
    throw fail(p_form, "Expected a method as (NAME (it PARAMETER...) [TYPE]).");
  trait_method result{.name = items[0].text};
  for (const auto &declaration : items[1].items) {
    const auto is_first = result.parameters.empty();
    if (declaration.is_symbol() && is_first) {
      result.parameters.push_back(
          {declaration.text, value_type::record_of(p_trait.name)});
      continue;
    }
    if (!declaration.is_list() || declaration.items.size() != 2)
      throw fail(declaration, "Expected a parameter as (NAME TYPE); only the "
                              "first, the value itself, may be a bare name.");
    result.same.resize(result.parameters.size() + 1, false);
    if (declaration.items[1].is_symbol() && declaration.items[1].text == "same") {
      result.same.back() = true;
      result.parameters.push_back({expect_name(declaration.items[0]),
                                   value_type::record_of(p_trait.name)});
      continue;
    }
    const auto parsed = value_type::parse(declaration.items[1]);
    if (!parsed || *parsed == value_type::nothing())
      throw fail(declaration.items[1], "Unknown parameter type.");
    result.parameters.push_back({expect_name(declaration.items[0]), *parsed});
  }
  result.return_type = value_type::nothing();
  if (items.size() == 3) {
    const auto parsed = value_type::parse(items[2]);
    if (!parsed)
      throw fail(items[2], "Unknown return type.");
    result.return_type = *parsed;
  }
  return result;
}

// (trait NAME (METHOD (PARAM...) [RET])... (fn METHOD (PARAM...) ...)...).
// A `fn` is a default method; it becomes a function of the same name whose
// first parameter is a value of the trait.
auto parse_trait(const sexpr &p_form, std::vector<function> &p_functions)
    -> trait {
  const auto &items = p_form.items;
  if (items.size() < 2 || !items[1].is_symbol() ||
      parse_base_type(items[1].text))
    throw fail(p_form, "Expected (trait NAME (METHOD (it) [TYPE])...).");
  trait result{.name = items[1].text, .location = p_form.location};
  for (auto item = items.begin() + 2; item != items.end(); ++item) {
    if (!item->is_list() || item->items.empty())
      throw fail(*item, "Expected a method of the trait.");
    if (item->head() == "requires" && item->items.size() == 2 &&
        item->items[1].is_symbol()) {
      result.requires_traits.push_back(item->items[1].text);
      continue;
    }
    if (item->head() == "fn") {
      // `(NAME same)` parameters are the trait's record, marked.
      auto form = *item;
      std::vector<bool> same;
      if (form.items.size() > 2 && form.items[2].is_list())
        for (auto &declaration : form.items[2].items) {
          same.push_back(declaration.is_list() && declaration.items.size() == 2 &&
                         declaration.items[1].is_symbol() &&
                         declaration.items[1].text == "same");
          if (same.back()) {
            auto &type = declaration.items[1];
            const auto word = [&](string_type p_text) {
              return sexpr{.kind = sexpr::kind_type::symbol,
                           .text = std::move(p_text),
                           .location = type.location};
            };
            type = sexpr{.items = {word("record"), word(result.name)},
                         .location = type.location};
          }
        }
      auto method = parse_function(form);
      if (method.parameters.empty())
        throw fail(*item, "A method needs a first parameter, the value "
                          "itself.");
      if (method.parameters[0].type == value_type::unknown())
        method.parameters[0].type = value_type::record_of(result.name);
      trait_method declared{.name = method.name,
                            .parameters = method.parameters,
                            .return_type = method.return_type,
                            .has_default = true,
                            .same = std::move(same)};
      result.methods.push_back(std::move(declared));
      p_functions.push_back(std::move(method));
    } else {
      result.methods.push_back(parse_trait_method(*item, result));
    }
  }
  return result;
}

// (impl TRAIT TYPE (fn METHOD (PARAM...) ...)...): each `fn` becomes the
// function `METHOD__TYPE`, whose first parameter is the TYPE.
auto parse_impl(const sexpr &p_form, std::vector<function> &p_functions)
    -> impl {
  const auto &items = p_form.items;
  if (items.size() < 3 || !items[1].is_symbol() || !items[2].is_symbol())
    throw fail(p_form, "Expected (impl TRAIT TYPE (fn METHOD ...)...).");
  impl result{.trait = items[1].text,
              .type = items[2].text,
              .location = p_form.location};
  // A number's or text's canonical spelling: `integer` is `int`.
  if (const auto plain = value_type::parse(items[2]);
      plain && !plain->is_record() && plain->is_known())
    result.type = plain->to_string();
  auto first = items.begin() + 3;
  // `(impl showable stack (forall (item showable)) …)`: for every stack.
  if (first != items.end() && is_forall(*first))
    result.type_parameters = parse_forall(*first++);
  for (auto item = first; item != items.end(); ++item) {
    if (item->head() != "fn")
      throw fail(*item, "An impl holds methods written as (fn NAME ...).");
    auto method = parse_function(*item);
    if (method.parameters.empty())
      throw fail(*item, "A method needs a first parameter, the value itself.");
    if (std::ranges::find(result.methods, method.name) != result.methods.end())
      throw fail(*item, "'" + method.name + "' is written twice in this impl.");
    result.methods.push_back(method.name);
    // Every stack's: the stack of its type variables. A number or text
    // implements a trait statically (generics.md §4): `(impl hashable int
    // …)`.
    auto self = value_type::record_of(result.type);
    if (parse_base_type(result.type) || c_number_type(result.type))
      self = *value_type::parse(items[2]);
    if (!result.type_parameters.empty()) {
      std::vector<value_type> variables;
      for (const auto &parameter : result.type_parameters)
        variables.push_back(value_type::variable(parameter.name));
      self = value_type::record_of(result.type, variables);
      if (method.type_parameters.empty())
        method.type_parameters = result.type_parameters;
    }
    if (method.parameters[0].type == value_type::unknown())
      method.parameters[0].type = self;
    method.name = impl_function_name(method.name, result.type);
    p_functions.push_back(std::move(method));
  }
  return result;
}

// (record NAME (FIELD [TYPE])...), where a field may also be a bare name;
// `packed-record` for one whose fields have no gaps between them.
auto parse_record(const sexpr &p_form) -> record {
  const auto &items = p_form.items;
  if (items.size() < 2 || !items[1].is_symbol() ||
      parse_base_type(items[1].text))
    throw fail(p_form, "Expected (" + string_type(p_form.head()) +
                           " NAME (FIELD TYPE)...).");
  record result{.name = items[1].text, .location = p_form.location,
                .is_packed = p_form.head() == "packed-record"};
  auto first = items.begin() + 2;
  // `(record stack (forall item) …)`: a generic record.
  if (first != items.end() && is_forall(*first)) {
    for (auto &parameter : parse_forall(*first)) {
      if (!parameter.bounds.empty())
        throw fail(*first, "A record's type variables have no traits; say "
                           "what they need in the phrases that use them.");
      result.type_parameters.push_back(std::move(parameter.name));
    }
    ++first;
  }
  for (auto item = first; item != items.end(); ++item) {
    if (item->is_symbol()) {
      result.fields.push_back({item->text, value_type::unknown()});
      continue;
    }
    if (!item->is_list() || item->items.empty() || item->items.size() > 2)
      throw fail(*item, "Expected a field as (NAME TYPE).");
    auto type = value_type::unknown();
    if (item->items.size() == 2) {
      const auto parsed = value_type::parse(item->items[1]);
      if (!parsed || *parsed == value_type::nothing())
        throw fail(item->items[1], "Unknown field type.");
      type = *parsed;
    }
    result.fields.push_back({expect_name(item->items[0]), type});
  }
  return result;
}

} // namespace

auto trait::find(string_view_type p_method) const -> const trait_method * {
  for (const auto &method : methods)
    if (method.name == p_method)
      return &method;
  return nullptr;
}

auto impl_function_name(string_view_type p_method, string_view_type p_type)
    -> string_type {
  return string_type(p_method) + "__" + string_type(p_type);
}

auto find_trait(const program &p_program, string_view_type p_name)
    -> const trait * {
  for (const auto &unit : p_program.units)
    for (const auto &trait : unit.traits)
      if (trait.name == p_name)
        return &trait;
  return nullptr;
}

auto record::find(string_view_type p_field) const -> const record_field * {
  for (const auto &field : fields)
    if (field.name == p_field)
      return &field;
  return nullptr;
}

auto c_number_type(string_view_type p_name) -> std::optional<c_type> {
  static const std::map<string_view_type, c_type> names = {
      {"i8", c_type::int8_type},    {"int8", c_type::int8_type},
      {"u8", c_type::uint8_type},   {"uint8", c_type::uint8_type},
      {"byte", c_type::uint8_type}, {"i16", c_type::int16_type},
      {"int16", c_type::int16_type}, {"u16", c_type::uint16_type},
      {"uint16", c_type::uint16_type}, {"i32", c_type::int_type},
      {"int32", c_type::int_type},  {"u32", c_type::uint_type},
      {"uint32", c_type::uint_type}, {"u64", c_type::size_type},
      {"uint64", c_type::size_type}, {"f32", c_type::float_type},
      {"float32", c_type::float_type}};
  if (const auto found = names.find(p_name); found != names.end())
    return found->second;
  return std::nullopt;
}

auto value_type::number_type() const -> std::optional<c_type> {
  if (!is_numeric())
    return std::nullopt;
  if (pointee.empty())
    return base == base_type::integer ? c_type::long_type : c_type::double_type;
  return c_number_type(pointee);
}

auto value_type::number(c_type p_type) -> value_type {
  if (p_type == c_type::long_type)
    return integer();
  if (p_type == c_type::double_type)
    return real();
  return c_number_item(p_type);
}

auto number_bits(c_type p_type) -> unsigned {
  switch (p_type) {
  case c_type::int8_type:
  case c_type::uint8_type:
    return 8;
  case c_type::int16_type:
  case c_type::uint16_type:
    return 16;
  case c_type::int_type:
  case c_type::uint_type:
  case c_type::float_type:
    return 32;
  default:
    return 64;
  }
}

auto common_number(const value_type &p_left, const value_type &p_right)
    -> std::optional<value_type> {
  if (!p_left.is_numeric() || !p_right.is_numeric())
    return std::nullopt;
  if (p_left == p_right)
    return p_left;
  const auto left = *p_left.number_type();
  const auto right = *p_right.number_type();
  // A decimal: a `float32` stays one beside whole numbers and `float32`s.
  if (p_left.is_decimal() || p_right.is_decimal()) {
    const bool narrow = (left == c_type::float_type || p_left.is_whole()) &&
                        (right == c_type::float_type || p_right.is_whole());
    return narrow ? value_type::number(c_type::float_type) : value_type::real();
  }
  const auto signed_of = [](unsigned p_bits) {
    return p_bits == 8    ? c_type::int8_type
           : p_bits == 16 ? c_type::int16_type
           : p_bits == 32 ? c_type::int_type
                          : c_type::long_type;
  };
  const auto unsigned_of = [](unsigned p_bits) {
    return p_bits == 8    ? c_type::uint8_type
           : p_bits == 16 ? c_type::uint16_type
           : p_bits == 32 ? c_type::uint_type
                          : c_type::size_type;
  };
  const auto left_bits = number_bits(left);
  const auto right_bits = number_bits(right);
  const bool left_unsigned = is_unsigned(left);
  const bool right_unsigned = is_unsigned(right);
  if (left_unsigned == right_unsigned)
    return value_type::number(left_unsigned
                                  ? unsigned_of(std::max(left_bits, right_bits))
                                  : signed_of(std::max(left_bits, right_bits)));
  // A signed type wider than the unsigned one holds it; else one of twice
  // the unsigned one's size, which a uint64 has none of.
  const auto unsigned_bits = left_unsigned ? left_bits : right_bits;
  const auto signed_bits = left_unsigned ? right_bits : left_bits;
  if (signed_bits > unsigned_bits)
    return value_type::number(signed_of(signed_bits));
  if (unsigned_bits == 64)
    return std::nullopt;
  return value_type::number(signed_of(unsigned_bits * 2));
}

namespace {

// The smallest and largest value of a whole number type, as unsigned bits
// (the smallest as its signed value's bits).
auto number_limits(c_type p_type) -> std::pair<max_int_type, max_uint_type> {
  const auto bits = number_bits(p_type);
  if (is_unsigned(p_type))
    return {0, bits == 64 ? ~max_uint_type{0} : (max_uint_type{1} << bits) - 1};
  const auto largest = (max_uint_type{1} << (bits - 1)) - 1;
  return {-static_cast<max_int_type>(largest) - 1, largest};
}

} // namespace

auto fits_number(max_int_type p_bits, bool p_is_big, const value_type &p_type)
    -> bool {
  const auto type = p_type.number_type();
  if (!type || !p_type.is_whole())
    return p_type.is_decimal();
  const auto [smallest, largest] = number_limits(*type);
  if (p_is_big)
    return static_cast<max_uint_type>(p_bits) <= largest;
  if (p_bits < 0)
    return p_bits >= smallest;
  return static_cast<max_uint_type>(p_bits) <= largest;
}

auto number_range(const value_type &p_type) -> string_type {
  const auto [smallest, largest] = number_limits(*p_type.number_type());
  return std::to_string(smallest) + " to " + std::to_string(largest);
}

auto c_storage_name(c_type p_type) -> string_type {
  switch (p_type) {
  case c_type::string_type:
    return "str";
  case c_type::int8_type:
    return "i8";
  case c_type::uint8_type:
    return "u8";
  case c_type::int16_type:
    return "i16";
  case c_type::uint16_type:
    return "u16";
  case c_type::int_type:
    return "i32";
  case c_type::uint_type:
    return "u32";
  case c_type::size_type:
    return "u64";
  case c_type::float_type:
    return "f32";
  case c_type::long_type:
    return "int";
  case c_type::double_type:
    return "float";
  default:
    return "ptr";
  }
}

auto parse_pointee(const sexpr &p_form) -> std::optional<string_type> {
  if (p_form.is_symbol())
    if (const auto number = c_number_type(p_form.text))
      return c_storage_name(*number);
  if (p_form.is_list() && p_form.items.size() == 2 &&
      p_form.head() == "struct" && p_form.items[1].is_symbol())
    return "(struct " + p_form.items[1].text + ")";
  const auto value = value_type::parse(p_form);
  if (!value || *value == value_type::nothing() || !value->is_known())
    return std::nullopt;
  return value->to_string();
}

auto read_pointee(string_view_type p_text) -> std::optional<pointee_type> {
  std::vector<sexpr> forms;
  try {
    forms = read_sexprs(p_text);
  } catch (const error &) {
    return std::nullopt;
  }
  if (forms.size() != 1)
    return std::nullopt;
  const auto &form = forms[0];
  pointee_type result;
  if (form.is_symbol())
    if (const auto number = c_number_type(form.text)) {
      result.kind = pointee_type::kind_type::number;
      result.number = *number;
      return result;
    }
  if (form.is_list() && form.items.size() == 2 && form.head() == "struct" &&
      form.items[1].is_symbol()) {
    result.kind = pointee_type::kind_type::structure;
    result.record = form.items[1].text;
    return result;
  }
  const auto value = value_type::parse(form);
  if (!value)
    return std::nullopt;
  result.value = *value;
  return result;
}

auto c_number_item(c_type p_type) -> value_type {
  auto result = ovis_type_of(p_type);
  result.pointee = c_storage_name(p_type);
  return result;
}

auto pointee_value(const pointee_type &p_pointee) -> value_type {
  switch (p_pointee.kind) {
  case pointee_type::kind_type::number:
    return ovis_type_of(p_pointee.number);
  case pointee_type::kind_type::structure:
    return value_type::record_of(p_pointee.record);
  case pointee_type::kind_type::value:
    break;
  }
  return p_pointee.value;
}

auto c_type_named(string_view_type p_name) -> std::optional<c_type> {
  if (const auto number = c_number_type(p_name))
    return number;
  static const std::map<string_view_type, c_type> names = {
      {"int", c_type::long_type},     {"i64", c_type::long_type},
      {"float", c_type::double_type}, {"f64", c_type::double_type},
      {"str", c_type::string_type},   {"ptr", c_type::pointer_type},
      {"void", c_type::void_type}};
  if (const auto found = names.find(p_name); found != names.end())
    return found->second;
  return std::nullopt;
}

auto phrase_type(const std::vector<value_type> &p_parameters,
                 const value_type &p_result) -> value_type {
  string_type text = "(";
  for (std::size_t index = 0; index < p_parameters.size(); ++index)
    text += (index > 0 ? " " : "") + p_parameters[index].to_string();
  return value_type::pointer_to(pointer_kind::phrase,
                                text + ") " + p_result.to_string());
}

auto c_phrase_type(const std::vector<c_type> &p_parameters, c_type p_result)
    -> value_type {
  const auto name = [](c_type p_type) -> string_type {
    return p_type == c_type::void_type ? "void" : c_storage_name(p_type);
  };
  string_type text = "(";
  for (std::size_t index = 0; index < p_parameters.size(); ++index)
    text += (index > 0 ? " " : "") + name(p_parameters[index]);
  return value_type::pointer_to(pointer_kind::c_phrase,
                                text + ") " + name(p_result));
}

auto read_signature(const value_type &p_phrase)
    -> std::optional<phrase_signature> {
  if (!p_phrase.is_phrase())
    return std::nullopt;
  std::vector<sexpr> forms;
  try {
    forms = read_sexprs(p_phrase.pointee);
  } catch (const error &) {
    return std::nullopt;
  }
  if (forms.size() != 2 || !forms[0].is_list())
    return std::nullopt;
  phrase_signature result;
  const bool is_c = p_phrase.kind == pointer_kind::c_phrase;
  const auto read = [&](const sexpr &p_form, value_type &p_type,
                        c_type *p_c) -> bool {
    if (is_c) {
      if (!p_form.is_symbol())
        return false;
      const auto type = c_type_named(p_form.text);
      if (!type)
        return false;
      *p_c = *type;
      p_type = ovis_type_of(*type);
      return true;
    }
    const auto type = value_type::parse(p_form);
    if (!type)
      return false;
    p_type = *type;
    return true;
  };
  for (const auto &parameter : forms[0].items) {
    value_type type;
    c_type c = c_type::void_type;
    if (!read(parameter, type, &c))
      return std::nullopt;
    result.parameters.push_back(type);
    result.c_parameters.push_back(c);
  }
  if (!read(forms[1], result.result, &result.c_result))
    return std::nullopt;
  return result;
}

auto value_type::to_string() const -> string_type {
  if (nullable && !is_list)
    return "(maybe " + without_nothing().to_string() + ")";
  if (is_phrase())
    return string_type(kind == pointer_kind::phrase ? "(phrase " : "(c-phrase ") +
           pointee + ")";
  if (!is_list && base == base_type::pointer && kind != pointer_kind::raw)
    return string_type(kind == pointer_kind::one    ? "(ptr "
                       : kind == pointer_kind::many ? "(many "
                                                    : "(cptr ") +
           pointee + ")";
  if (is_c_array())
    return "(list " + pointee + ")";
  if (is_list && !pointee.empty() && base != base_type::map &&
      base != base_type::pointer)
    return "(list " + element().to_string() + ")";
  if (is_sized())
    return pointee;
  string_type result;
  switch (base) {
  case base_type::nothing:
    result = "void";
    break;
  case base_type::integer:
    result = "int";
    break;
  case base_type::real:
    result = "float";
    break;
  case base_type::boolean:
    result = "bool";
    break;
  case base_type::string:
    result = "str";
    break;
  case base_type::pointer:
    result = "ptr";
    break;
  case base_type::memory:
    result = "memory";
    break;
  case base_type::map:
    result = "(map " + pointee.substr(1, pointee.size() - 2) + ")";
    if (is_list)
      for (std::size_t level = 0; level <= inner_lists; ++level)
        result = "(list " + result + ")";
    return result;
  case base_type::unknown:
    result = "?";
    break;
  case base_type::variable:
    result = "(var " + record + ")";
    if (is_list)
      for (std::size_t level = 0; level <= inner_lists; ++level)
        result = "(list " + result + ")";
    return result;
  case base_type::record:
    result = "(record " + record + (arguments.empty() ? "" : " " + arguments) +
             ")";
    if (nullable)
      result = "(maybe " + result + ")";
    if (is_list)
      for (std::size_t level = 0; level <= inner_lists; ++level)
        result = "(list " + result + ")";
    return result;
  }
  if (is_list)
    for (std::size_t level = 0; level <= inner_lists; ++level)
      result += "[]";
  return result;
}

auto value_type::record_of(string_type p_name,
                           const std::vector<value_type> &p_arguments)
    -> value_type {
  auto result = record_of(std::move(p_name));
  for (const auto &argument : p_arguments)
    result.arguments += (result.arguments.empty() ? "" : " ") +
                        argument.to_string();
  return result;
}

auto value_type::record_arguments() const -> std::vector<value_type> {
  std::vector<value_type> result;
  if (arguments.empty())
    return result;
  for (const auto &form : read_sexprs(arguments))
    result.push_back(parse(form).value_or(unknown()));
  return result;
}

auto value_type::has_variables() const -> bool {
  return base == base_type::variable ||
         to_string().find("(var ") != string_type::npos;
}

auto value_type::map_of(const value_type &p_key, const value_type &p_value)
    -> value_type {
  value_type result{base_type::map};
  result.pointee = "(" + p_key.to_string() + " " + p_value.to_string() + ")";
  return result;
}

auto value_type::map_key() const -> value_type {
  const auto forms = read_sexprs(pointee);
  return forms.size() == 1 && forms[0].items.size() == 2
             ? parse(forms[0].items[0]).value_or(unknown())
             : unknown();
}

auto value_type::map_value() const -> value_type {
  const auto forms = read_sexprs(pointee);
  return forms.size() == 1 && forms[0].items.size() == 2
             ? parse(forms[0].items[1]).value_or(unknown())
             : unknown();
}

auto value_type::parse(const sexpr &p_form) -> std::optional<value_type> {
  if (p_form.is_list()) {
    // `(maybe (record node))`: a record that may be nothing.
    if (p_form.items.size() == 2 && p_form.head() == "maybe") {
      const auto inner = parse(p_form.items[1]);
      if (!inner || !inner->is_record())
        return std::nullopt;
      return inner->or_nothing();
    }
    // `(map str int)`: numbers or text as keys.
    if (p_form.items.size() == 3 && p_form.head() == "map") {
      const auto key = parse(p_form.items[1]);
      const auto value = parse(p_form.items[2]);
      if (!key || !value || *value == nothing())
        return std::nullopt;
      return map_of(*key, *value);
    }
    // `(phrase (int int) bool)`, `(c-phrase (i32) void)`.
    if (p_form.items.size() == 3 &&
        (p_form.head() == "phrase" || p_form.head() == "c-phrase") &&
        p_form.items[1].is_list()) {
      auto text = p_form.items[1].to_string() + " " + p_form.items[2].to_string();
      auto result = pointer_to(p_form.head() == "phrase" ? pointer_kind::phrase
                                                         : pointer_kind::c_phrase,
                               std::move(text));
      if (!read_signature(result))
        return std::nullopt;
      // The canonical spelling of each type.
      const auto signature = *read_signature(result);
      return p_form.head() == "phrase"
                 ? phrase_type(signature.parameters, signature.result)
                 : c_phrase_type(signature.c_parameters, signature.c_result);
    }
    // `(record stack int)`: a generic record used with these types.
    if (p_form.items.size() > 2 && p_form.head() == "record") {
      if (!p_form.items[1].is_symbol() || parse_base_type(p_form.items[1].text))
        return std::nullopt;
      std::vector<value_type> arguments;
      for (auto item = p_form.items.begin() + 2; item != p_form.items.end();
           ++item) {
        const auto argument = parse(*item);
        if (!argument || *argument == nothing())
          return std::nullopt;
        arguments.push_back(*argument);
      }
      return record_of(p_form.items[1].text, arguments);
    }
    if (p_form.items.size() != 2)
      return std::nullopt;
    if (p_form.head() == "record") {
      if (!p_form.items[1].is_symbol() || parse_base_type(p_form.items[1].text))
        return std::nullopt;
      return record_of(p_form.items[1].text);
    }
    if (p_form.head() == "var") {
      if (!p_form.items[1].is_symbol() || parse_base_type(p_form.items[1].text))
        return std::nullopt;
      return variable(p_form.items[1].text);
    }
    if (p_form.head() == "ptr" || p_form.head() == "many" ||
        p_form.head() == "cptr") {
      auto pointee = parse_pointee(p_form.items[1]);
      if (!pointee)
        return std::nullopt;
      return pointer_to(p_form.head() == "ptr"    ? pointer_kind::one
                        : p_form.head() == "many" ? pointer_kind::many
                                                  : pointer_kind::c,
                        std::move(*pointee));
    }
    if (p_form.head() != "list" && p_form.head() != "array")
      return std::nullopt;
    // `(list u8)`: an array of C numbers.
    if (p_form.items[1].is_symbol())
      if (const auto number = c_number_type(p_form.items[1].text))
        return list_of(c_number_item(*number));
    auto element = parse(p_form.items[1]);
    if (!element || element->base == base_type::nothing ||
        element->base == base_type::pointer || element->base == base_type::memory)
      return std::nullopt;
    return list_of(*element);
  }
  if (!p_form.is_symbol())
    return std::nullopt;

  string_view_type name = p_form.text;
  std::size_t levels = 0;
  while (name.ends_with("[]")) {
    name.remove_suffix(2);
    ++levels;
  }
  // A number of a size: `i32`, `byte`, `u64[]`.
  if (const auto sized = c_number_type(name)) {
    if (levels > 1)
      return std::nullopt;
    return levels == 1 ? list_of(number(*sized)) : number(*sized);
  }
  const auto base = parse_base_type(name);
  if (!base || (levels > 0 && (*base == base_type::nothing ||
                               *base == base_type::pointer ||
                               *base == base_type::memory)))
    return std::nullopt;
  return value_type{*base, levels > 0, levels > 0 ? levels - 1 : 0};
}

auto find_constant(const program &p_program, string_view_type p_name)
    -> const c_constant * {
  for (const auto &unit : p_program.units)
    for (const auto &constant : unit.constants)
      if (constant.name == p_name)
        return &constant;
  return find_c_constant(p_name);
}

auto find_record(const program &p_program, string_view_type p_name)
    -> const record * {
  for (const auto &unit : p_program.units)
    for (const auto &record : unit.records)
      if (record.name == p_name)
        return &record;
  return nullptr;
}

auto record_descriptions() -> std::map<string_type, string_type> & {
  static std::map<string_type, string_type> descriptions;
  return descriptions;
}

auto record_print_names() -> std::map<string_type, string_type> & {
  static std::map<string_type, string_type> names;
  return names;
}

// "whole numbers" for "a whole number": the kind, not one of it.
auto plural_in_english(string_type p_text) -> string_type {
  if (p_text.starts_with("a list"))
    return "lists" + p_text.substr(6);
  if (p_text.starts_with("an "))
    p_text = p_text.substr(3);
  else if (p_text.starts_with("a "))
    p_text = p_text.substr(2);
  else
    return p_text;
  // `stack of books`: the first word is the one that counts.
  const auto space = p_text.find(" of ");
  if (space != string_type::npos)
    return p_text.substr(0, space) + (p_text.substr(0, space).ends_with('s') ? "es" : "s") +
           p_text.substr(space);
  return p_text + (p_text.ends_with('s') ? "es" : "s");
}

auto describe_in_english(const value_type &p_type) -> string_type {
  if (p_type.is_map()) {
    auto key = describe_in_english(p_type.map_key());
    auto value = describe_in_english(p_type.map_value());
    // "a map of text to whole numbers": the kinds, not one of each.
    return "a map of " + plural_in_english(key) + " to " +
           plural_in_english(value);
  }
  if (p_type.is_phrase()) {
    const auto signature = read_signature(p_type);
    string_type text = p_type.kind == pointer_kind::c_phrase ? "a C phrase ("
                                                             : "a phrase (";
    if (signature) {
      for (std::size_t index = 0; index < signature->parameters.size(); ++index)
        text += (index > 0 ? ", " : "") +
                describe_in_english(signature->parameters[index]);
      text += ")";
      if (signature->result != value_type::nothing())
        text += " giving " + describe_in_english(signature->result);
      return text;
    }
    return text + p_type.pointee + ")";
  }
  if (p_type.is_c_array()) {
    static const std::map<string_view_type, string_view_type> names = {
        {"i8", "int8s"},   {"u8", "bytes"},    {"i16", "int16s"},
        {"u16", "uint16s"}, {"i32", "int32s"}, {"u32", "uint32s"},
        {"u64", "uint64s"}, {"f32", "float32s"}};
    const auto found = names.find(p_type.pointee);
    return "an array of " +
           string_type(found != names.end() ? found->second : p_type.pointee);
  }
  if (p_type.is_typed_pointer()) {
    const auto pointee = read_pointee(p_type.pointee);
    string_type what = p_type.pointee;
    bool many_of = p_type.kind == pointer_kind::many;
    if (pointee) {
      switch (pointee->kind) {
      case pointee_type::kind_type::number: {
        const auto names = std::map<c_type, string_type>{
            {c_type::int8_type, "int8"},   {c_type::uint8_type, "byte"},
            {c_type::int16_type, "int16"}, {c_type::uint16_type, "uint16"},
            {c_type::int_type, "int32"},   {c_type::uint_type, "uint32"},
            {c_type::size_type, "uint64"}, {c_type::float_type, "float32"}};
        what = names.at(pointee->number);
        what = many_of ? what + "s"
                       : (what.front() == 'i' || what.front() == 'u' ? "an " : "a ") + what;
        break;
      }
      case pointee_type::kind_type::structure: {
        auto name = pointee->record;
        std::ranges::replace(name, '_', ' ');
        what = many_of ? name + "s" : "a " + name;
        break;
      }
      case pointee_type::kind_type::value:
        what = describe_in_english(pointee->value);
        if (many_of && what.starts_with("a "))
          what = what.substr(2) + "s";
        break;
      }
    }
    return string_type(p_type.kind == pointer_kind::c ? "a C pointer to "
                       : many_of ? "a pointer to many "
                                 : "a pointer to ") +
           what;
  }
  if (p_type.is_sized()) {
    static const std::map<string_view_type, string_view_type> names = {
        {"i8", "an int8"},    {"u8", "a byte"},      {"i16", "an int16"},
        {"u16", "a uint16"},  {"i32", "an int32"},   {"u32", "a uint32"},
        {"u64", "a uint64"},  {"f32", "a float32"}};
    return string_type(names.at(p_type.pointee));
  }
  if (p_type.base == base_type::variable) {
    auto name = p_type.record;
    std::ranges::replace(name, '_', ' ');
    if (p_type.is_list)
      return "a list of " + name + (name.ends_with('s') ? "es" : "s");
    return (string_view_type("aeiou").contains(name.front()) ? "an " : "a ") +
           name;
  }
  if (p_type.base == base_type::record) {
    auto name = p_type.record;
    // A generic record's copy (`stack__int`) is said as its use.
    if (const auto found = record_descriptions().find(name);
        found != record_descriptions().end())
      name = found->second;
    else
      std::ranges::replace(name, '_', ' ');
    if (!p_type.arguments.empty()) {
      const auto arguments = p_type.record_arguments();
      for (std::size_t index = 0; index < arguments.size(); ++index)
        name += string_type(index == 0 ? " of " : " and ") +
                plural_in_english(describe_in_english(arguments[index]));
    }
    const string_type nothing = p_type.nullable ? " or nothing" : "";
    if (p_type.is_list)
      return "a list of " + plural_in_english("a " + name) + nothing;
    return (string_view_type("aeiou").contains(name.front()) ? "an " : "a ") +
           name + nothing;
  }
  const auto singular = [](base_type p_base) -> string_type {
    switch (p_base) {
    case base_type::integer:
      return "a whole number";
    case base_type::real:
      return "a decimal number";
    case base_type::boolean:
      return "true or false";
    case base_type::string:
      return "text";
    case base_type::pointer:
      return "a C pointer";
    case base_type::memory:
      return "a memory";
    case base_type::map:
      break;
    case base_type::nothing:
      return "nothing";
    case base_type::unknown:
    case base_type::record:
      break;
    }
    return "an unknown kind of value";
  };
  if (!p_type.is_list)
    return singular(p_type.base);
  if (p_type.inner_lists > 0)
    return "a list of lists" +
           string_type(p_type.inner_lists > 1 ? " of lists" : "");
  switch (p_type.base) {
  case base_type::integer:
    return "a list of whole numbers";
  case base_type::real:
    return "a list of decimal numbers";
  case base_type::boolean:
    return "a list of true-or-false values";
  case base_type::string:
    return "a list of text";
  default:
    return "a list";
  }
}

auto core_keywords() -> std::vector<string_view_type> {
  std::vector<string_view_type> result = {
      "let",   "var",     "define", "set",     "assign",   "put",
      "print", "say",     "display", "if",     "when",     "while",
      "for",   "for-down", "each",    "foreach", "for-each", "break",  "continue",
      "pass",  "defer", "with-memory", "with-new-memory",   "return",  "then",   "else",    "do",       "begin", "block", "leave",
      "fn",    "function", "defn",  "extern",  "link",
      "use-c", "use_c",   "include", "record", "packed-record", "set-field", "set_field",
      "trait", "impl"};
  for (const auto &[alias, name] : operation_aliases())
    result.push_back(alias);
  return result;
}

// Takes each `(lambda …)` out of a form, innermost first, into a `(fn …)`
// of its own: a phrase given as a value sees only its parameters, so it
// needs nothing from where it is written.
static auto hoist_lambdas(sexpr &p_form, string_view_type p_prefix,
                   std::vector<sexpr> &p_hoisted) -> void {
  if (!p_form.is_list())
    return;
  for (auto &item : p_form.items)
    hoist_lambdas(item, p_prefix, p_hoisted);
  if (p_form.head() != "lambda")
    return;
  if (p_form.items.size() < 3 || !p_form.items[1].is_list())
    throw fail(p_form, "Expected (lambda ((PARAMETER TYPE) ...) "
                       "RESULT-TYPE BODY...).");
  const auto name = string_type(p_prefix) + std::to_string(p_hoisted.size());
  auto function = p_form;
  function.items[0].text = "fn";
  function.items.insert(function.items.begin() + 1,
                        sexpr{.kind = sexpr::kind_type::symbol,
                              .text = name,
                              .location = p_form.location});
  p_hoisted.push_back(std::move(function));
  const auto location = p_form.location;
  p_form.items = {
      sexpr{.kind = sexpr::kind_type::symbol, .text = "phrase-of", .location = location},
      sexpr{.kind = sexpr::kind_type::symbol, .text = name, .location = location}};
}

auto parse_unit(const std::vector<sexpr> &p_forms,
                string_view_type p_lambda_prefix) -> unit {
  auto forms = p_forms;
  std::vector<sexpr> hoisted;
  for (auto &form : forms)
    hoist_lambdas(form, p_lambda_prefix, hoisted);
  std::ranges::move(hoisted, std::back_inserter(forms));
  unit result;
  for (const auto &form : forms) {
    const auto head = form.head();
    if (head == "fn" || head == "function" || head == "defn") {
      // No body: the model copied the declaration of a function another
      // paragraph defines (the context lists them in this form).
      if (form.items.size() <= 3 ||
          (form.items.size() == 4 && value_type::parse(form.items[3])))
        continue;
      result.functions.push_back(parse_function(form));
    } else if (head == "record" || head == "packed-record") {
      result.records.push_back(parse_record(form));
    } else if (head == "trait") {
      result.traits.push_back(parse_trait(form, result.functions));
    } else if (head == "impl") {
      result.impls.push_back(parse_impl(form, result.functions));
    } else if (head == "extern") {
      result.externs.push_back(parse_extern(form));
    } else if (head == "use-c" || head == "use_c" || head == "include") {
      if (form.items.size() < 2 || form.items.size() > 3 ||
          form.items[1].kind != sexpr::kind_type::string ||
          (form.items.size() == 3 &&
           form.items[2].kind != sexpr::kind_type::string &&
           !form.items[2].is_symbol()))
        throw fail(form, "Expected (use-c \"HEADER.h\" \"LIBRARY\").");
      result.c_imports.push_back(
          {form.items[1].text,
           form.items.size() == 3 ? form.items[2].text : string_type(),
           form.location});
    } else if (head == "link") {
      if (form.items.size() != 2 ||
          (form.items[1].kind != sexpr::kind_type::string &&
           !form.items[1].is_symbol()))
        throw fail(form, "Expected (link \"LIBRARY\").");
      result.links.push_back(form.items[1].text);
    } else {
      for (const auto &piece : split_let(form))
        for (auto &statement : parse_statements(piece))
          result.statements.push_back(std::move(statement));
    }
  }
  return result;
}

auto parse_unit(string_view_type p_text, string_view_type p_lambda_prefix)
    -> unit {
  try {
    return parse_unit(read_sexprs(p_text), p_lambda_prefix);
  } catch (const syntax_error &p_error) {
    throw error("line " + std::to_string(p_error.location.line) + ": " +
                p_error.what());
  }
}

} // namespace ovis::core

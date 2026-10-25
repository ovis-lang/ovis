#include <algorithm>
#include <cmath>
#include <map>

#include <ovis/core/ast.hpp>
#include <ovis/core/libc.hpp>

namespace ovis::core {

namespace {

auto make_table() -> std::vector<c_function> {
  using enum c_type;
  std::vector<c_function> result;
  const auto add = [&](const char_type *p_name, c_type p_result,
                       std::vector<c_type> p_parameters,
                       const char_type *p_library = "") {
    result.push_back({p_name, p_result, std::move(p_parameters), false,
                      std::nullopt, p_library});
  };
  const auto add_format = [&](const char_type *p_name,
                              std::vector<c_type> p_parameters,
                              std::size_t p_format_index) {
    result.push_back({p_name, int_type, std::move(p_parameters), true,
                      p_format_index});
  };

  // math.h
  for (const auto *const name :
       {"sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh",
        "exp", "exp2", "log", "log2", "log10", "cbrt", "ceil", "round",
        "trunc", "fabs"})
    add(name, double_type, {double_type}, "m");
  for (const auto *const name : {"atan2", "fmod", "hypot", "fmin", "fmax"})
    add(name, double_type, {double_type, double_type}, "m");

  // stdlib.h
  add("atoi", int_type, {string_type});
  add("atol", long_type, {string_type});
  add("atof", double_type, {string_type});
  add("rand", int_type, {});
  add("srand", void_type, {int_type});
  add("exit", void_type, {int_type});
  add("getenv", string_type, {string_type});
  add("system", int_type, {string_type});
  add("labs", long_type, {long_type});

  // The runtime: an error the prelude gives (lists.phr), with its line.
  add("ovis_runtime_error", void_type, {string_type});

  // string.h
  add("strlen", size_type, {string_type});
  add("strcmp", int_type, {string_type, string_type});
  add("strncmp", int_type, {string_type, string_type, size_type});
  add("strstr", string_type, {string_type, string_type});
  add("strchr", string_type, {string_type, int_type});
  add("strrchr", string_type, {string_type, int_type});
  add("strcpy", string_type, {string_type, string_type});
  add("strncpy", string_type, {string_type, string_type, size_type});
  add("strcat", string_type, {string_type, string_type});
  add("strncat", string_type, {string_type, string_type, size_type});
  add("strtok", string_type, {string_type, string_type});

  // ctype.h
  for (const auto *const name :
       {"isalpha", "isdigit", "isalnum", "isspace", "isupper", "islower",
        "ispunct", "isxdigit", "toupper", "tolower"})
    add(name, int_type, {int_type});

  // stdio.h
  add("puts", int_type, {string_type});
  add("putchar", int_type, {int_type});
  add("getchar", int_type, {});
  add_format("printf", {string_type}, 0);
  add_format("fprintf", {pointer_type, string_type}, 1);
  add_format("sprintf", {string_type, string_type}, 1);
  add_format("snprintf", {string_type, size_type, string_type}, 2);
  add("fopen", pointer_type, {string_type, string_type});
  add("fclose", int_type, {pointer_type});
  add("fgets", string_type, {string_type, int_type, pointer_type});
  add("fputs", int_type, {string_type, pointer_type});
  add("fputc", int_type, {int_type, pointer_type});
  add("fgetc", int_type, {pointer_type});
  add("feof", int_type, {pointer_type});
  // Bytes, for files laid out as a format says (a packed record's memory).
  add("fread", size_type, {pointer_type, size_type, size_type, pointer_type});
  add("fwrite", size_type, {pointer_type, size_type, size_type, pointer_type});
  add("fflush", int_type, {pointer_type});
  add("remove", int_type, {string_type});
  add("rename", int_type, {string_type, string_type});
  add("perror", void_type, {string_type});

  // time.h
  add("time", long_type, {pointer_type});
  add("clock", long_type, {});
  add("difftime", double_type, {long_type, long_type});
  return result;
}

} // namespace

auto is_unsigned(c_type p_type) -> bool {
  return p_type == c_type::uint8_type || p_type == c_type::uint16_type ||
         p_type == c_type::uint_type || p_type == c_type::size_type;
}

auto ovis_type_of(c_type p_type) -> value_type {
  switch (p_type) {
  case c_type::void_type:
    return value_type::nothing();
  case c_type::int8_type:
  case c_type::uint8_type:
  case c_type::int16_type:
  case c_type::uint16_type:
  case c_type::int_type:
  case c_type::uint_type:
  case c_type::long_type:
  case c_type::size_type:
    return value_type::integer();
  case c_type::double_type:
  case c_type::float_type:
    return value_type::real();
  case c_type::string_type:
    return value_type::string();
  case c_type::pointer_type:
    return value_type::pointer();
  }
  return value_type::nothing();
}

auto c_parameter_type(const c_function &p_function, std::size_t p_index)
    -> value_type {
  if (p_index < p_function.phrases.size() && !p_function.phrases[p_index].empty())
    return value_type::pointer_to(pointer_kind::c_phrase,
                                  p_function.phrases[p_index]);
  if (p_index < p_function.records.size() && !p_function.records[p_index].empty())
    return value_type::record_of(p_function.records[p_index]);
  return ovis_type_of(p_function.parameters[p_index]);
}

auto c_result_type(const c_function &p_function) -> value_type {
  if (!p_function.result_phrase.empty())
    return value_type::pointer_to(pointer_kind::c_phrase,
                                  p_function.result_phrase);
  if (!p_function.result_record.empty())
    return value_type::record_of(p_function.result_record);
  return ovis_type_of(p_function.result);
}

auto parse_c_type(string_view_type p_name) -> std::optional<c_type> {
  static const std::map<string_view_type, c_type> names = {
      {"void", c_type::void_type},     {"cint", c_type::int_type},
      {"int8", c_type::int8_type},     {"char", c_type::int8_type},
      {"uint8", c_type::uint8_type},   {"uchar", c_type::uint8_type},
      {"int16", c_type::int16_type},   {"short", c_type::int16_type},
      {"uint16", c_type::uint16_type}, {"ushort", c_type::uint16_type},
      {"uint", c_type::uint_type},     {"uint32", c_type::uint_type},
      {"int32", c_type::int_type},     {"int", c_type::long_type},
      {"long", c_type::long_type},     {"int64", c_type::long_type},
      {"size_t", c_type::size_type},   {"float", c_type::double_type},
      {"double", c_type::double_type}, {"cfloat", c_type::float_type},
      {"float32", c_type::float_type}, {"str", c_type::string_type},
      {"string", c_type::string_type}, {"char*", c_type::string_type},
      {"ptr", c_type::pointer_type},   {"pointer", c_type::pointer_type},
      {"void*", c_type::pointer_type}, {"FILE*", c_type::pointer_type},
  };
  if (const auto found = names.find(p_name); found != names.end())
    return found->second;
  return std::nullopt;
}

auto libc_functions() -> std::span<const c_function> {
  static const auto table = make_table();
  return table;
}

auto find_libc_function(string_view_type p_name) -> const c_function * {
  const auto table = libc_functions();
  const auto found = std::ranges::find(table, p_name, &c_function::name);
  return found == table.end() ? nullptr : &*found;
}

auto find_c_constant(string_view_type p_name) -> const c_constant * {
  using kind = c_constant::kind_type;
  static const std::vector<c_constant> constants = {
      {"null", kind::null},
      {"NULL", kind::null},
      {"EOF", kind::integer, -1},
      {"RAND_MAX", kind::runtime, 0, 0, "ovis_rand_max"},
      {"CLOCKS_PER_SEC", kind::runtime, 0, 0, "ovis_clocks_per_sec"},
      {"stdin", kind::runtime, 0, 0, "ovis_stdin"},
      {"stdout", kind::runtime, 0, 0, "ovis_stdout"},
      {"stderr", kind::runtime, 0, 0, "ovis_stderr"},
      {"pi", kind::real, 0, M_PI},
      {"M_PI", kind::real, 0, M_PI},
  };
  const auto found = std::ranges::find(constants, p_name, &c_constant::name);
  return found == constants.end() ? nullptr : &*found;
}

auto builtin_names() -> const std::set<string_view_type> & {
  static const std::set<string_view_type> names = {
      "+",         "-",         "*",        "/",         "div",
      "%",         "=",         "!=",       "<",         "<=",
      ">",         ">=",        "and",      "or",        "not",
      "int",       "float",     "sqrt",     "pow",       "floor",
      "abs",       "min",       "max",      "len",       "get",
      "list",      "array",     "append",   "sort",      "reverse",    "remove-at", "free", "keep", "copy", "new-memory", "clear-memory",
      "read_int",  "read_float", "read_line", "random",   "concat",
      "substring", "to_str",    "char_at",  "chr",       "buffer",    "set-char",
      "load",      "store",     "view",      "phrase-of", "call",
      "map-new",   "map-put",   "map-get",  "map-has",   "map-remove", "map-keys",     "offset",   "cast",      "size-of", "alloc", "alloc-more",
      "ref",       "new",       "field",
      // Numbers of a size and their bits (docs/proposals/numbers.md).
      "as", "wrap-to", "clamp-to", "to-hex", "to-binary", "from-hex",
      "bit-and", "bit-or", "bit-xor", "bit-not", "shift-left", "shift-right",
      "rotate-left", "rotate-right", "bit-test", "bit-set", "bit-clear",
      "bit-flip", "bit-field", "count-ones", "leading-zeros",
      "trailing-zeros", "byte-swap", "wrapping", "saturating", "float-bits",
      "bits-float", "to-bytes", "to-bytes-big", "from-bytes",
      "from-bytes-big"};
  return names;
}

auto is_bit_operation(string_view_type p_name) -> bool {
  static const std::set<string_view_type> names = {
      "bit-and", "bit-or", "bit-xor", "bit-not", "shift-left", "shift-right",
      "rotate-left", "rotate-right", "bit-test", "bit-set", "bit-clear",
      "bit-flip", "bit-field", "count-ones", "leading-zeros",
      "trailing-zeros", "byte-swap"};
  return names.contains(p_name);
}

auto is_builtin(string_view_type p_name) -> bool {
  return builtin_names().contains(p_name);
}

auto parse_format(string_view_type p_format) -> std::vector<format_conversion> {
  std::vector<format_conversion> result;
  for (std::size_t index = 0; index < p_format.size(); ++index) {
    if (p_format[index] != '%')
      continue;
    format_conversion conversion{.begin = index};
    ++index;
    if (index < p_format.size() && p_format[index] == '%')
      continue;
    // Flags, width, precision and length modifiers.
    while (index < p_format.size() &&
           string_view_type("-+ #0123456789.*hlLqjzt").find(p_format[index]) !=
               string_view_type::npos) {
      if (p_format[index] == '*')
        conversion.takes_star_width = true;
      ++index;
    }
    if (index >= p_format.size())
      break;
    conversion.conversion = p_format[index];
    conversion.end = index + 1;
    result.push_back(conversion);
  }
  return result;
}

} // namespace ovis::core

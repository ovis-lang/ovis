#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include <llvm/Config/llvm-config.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>

#include <ovis/core/c_import.hpp>

#ifdef OVIS_HAS_C_IMPORT
#include <clang-c/Index.h>
#endif

namespace ovis::core {

namespace {

auto read_whole_file(const std::filesystem::path &p_path) -> string_type {
  std::ifstream input(p_path, std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

auto fnv_hash(string_view_type p_text) -> string_type {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const auto character : p_text)
    hash = (hash ^ static_cast<unsigned char>(character)) * 1099511628211ULL;
  char_type text[17];
  std::snprintf(text, sizeof(text), "%016llx",
                static_cast<unsigned long long>(hash));
  return text;
}

auto split_words(string_view_type p_text) -> std::vector<string_type> {
  std::vector<string_type> result;
  std::istringstream input{string_type(p_text)};
  for (string_type word; input >> word;)
    result.push_back(word);
  return result;
}

// Runs a program and returns what it printed, or nothing if it failed.
auto run_and_capture(const string_type &p_program,
                     const std::vector<string_type> &p_arguments)
    -> std::optional<string_type> {
  const auto found = llvm::sys::findProgramByName(p_program);
  if (!found)
    return std::nullopt;
  llvm::SmallString<128> output_path;
  if (llvm::sys::fs::createTemporaryFile("ovis-capture", "txt", output_path))
    return std::nullopt;
  std::vector<llvm::StringRef> arguments = {*found};
  for (const auto &argument : p_arguments)
    arguments.push_back(argument);
  const std::optional<llvm::StringRef> redirects[] = {
      llvm::StringRef(), llvm::StringRef(output_path), llvm::StringRef()};
  const auto status =
      llvm::sys::ExecuteAndWait(*found, arguments, std::nullopt, redirects);
  auto text = read_whole_file(output_path.str().str());
  llvm::sys::fs::remove(output_path);
  if (status != 0)
    return std::nullopt;
  return text;
}

auto c_type_name(c_type p_type) -> string_view_type {
  switch (p_type) {
  case c_type::void_type:
    return "void";
  case c_type::int8_type:
    return "int8";
  case c_type::uint8_type:
    return "uint8";
  case c_type::int16_type:
    return "int16";
  case c_type::uint16_type:
    return "uint16";
  case c_type::int_type:
    return "cint";
  case c_type::uint_type:
    return "uint";
  case c_type::long_type:
    return "int";
  case c_type::size_type:
    return "size_t";
  case c_type::double_type:
    return "float";
  case c_type::float_type:
    return "cfloat";
  case c_type::string_type:
    return "str";
  case c_type::pointer_type:
    return "ptr";
  }
  return "?";
}

// Lowercase words in a name: `sqlite3_open_v2` → sqlite3 open v2,
// `zlibVersion` → zlib version.
auto name_words(string_view_type p_name) -> std::vector<string_type> {
  std::vector<string_type> result;
  string_type word;
  const auto flush = [&]() {
    if (!word.empty())
      result.push_back(word);
    word.clear();
  };
  for (std::size_t index = 0; index < p_name.size(); ++index) {
    const auto character = static_cast<unsigned char>(p_name[index]);
    if (!std::isalnum(character)) {
      flush();
      continue;
    }
    if (std::isupper(character) && index > 0 &&
        std::islower(static_cast<unsigned char>(p_name[index - 1])))
      flush();
    word.push_back(static_cast<char_type>(std::tolower(character)));
  }
  flush();
  return result;
}

auto text_words(string_view_type p_text) -> std::set<string_type> {
  std::set<string_type> result;
  for (const auto &word : name_words(p_text))
    if (word.size() >= 3)
      result.insert(word);
  return result;
}

#ifdef OVIS_HAS_C_IMPORT

auto to_string(CXString p_text) -> string_type {
  const auto *const characters = clang_getCString(p_text);
  string_type result = characters != nullptr ? characters : "";
  clang_disposeString(p_text);
  return result;
}

auto scalar_type(CXType p_type) -> std::optional<c_type> {
  switch (clang_getCanonicalType(p_type).kind) {
  case CXType_Void:
    return c_type::void_type;
  case CXType_Bool:
  case CXType_UChar:
  case CXType_Char_U:
    return c_type::uint8_type;
  case CXType_SChar:
  case CXType_Char_S:
    return c_type::int8_type;
  case CXType_Short:
    return c_type::int16_type;
  case CXType_UShort:
    return c_type::uint16_type;
  case CXType_Int:
    return c_type::int_type;
  case CXType_UInt:
    return c_type::uint_type;
  case CXType_Long:
  case CXType_LongLong:
  case CXType_ULong:
  case CXType_ULongLong:
    return clang_Type_getSizeOf(p_type) == 8 ? c_type::long_type
                                              : c_type::int_type;
  case CXType_Float:
    return c_type::float_type;
  case CXType_Double:
    return c_type::double_type;
  case CXType_Enum:
    return clang_Type_getSizeOf(p_type) == 8 ? c_type::long_type
                                              : c_type::int_type;
  default:
    return std::nullopt;
  }
}

struct mapped_type {
  c_type type;
  std::optional<c_type> pointee;
};

auto is_character(CXTypeKind p_kind) -> bool {
  return p_kind == CXType_Char_S || p_kind == CXType_Char_U ||
         p_kind == CXType_SChar || p_kind == CXType_UChar;
}

auto map_type(CXType p_type) -> std::optional<mapped_type> {
  const auto canonical = clang_getCanonicalType(p_type);
  if (canonical.kind == CXType_Pointer ||
      canonical.kind == CXType_ConstantArray ||
      canonical.kind == CXType_IncompleteArray) {
    const auto pointee = clang_getCanonicalType(
        canonical.kind == CXType_Pointer ? clang_getPointeeType(canonical)
                                         : clang_getArrayElementType(canonical));
    if (is_character(pointee.kind))
      return mapped_type{c_type::string_type, std::nullopt};
    if (pointee.kind == CXType_Pointer) {
      const auto inner =
          clang_getCanonicalType(clang_getPointeeType(pointee)).kind;
      return mapped_type{c_type::pointer_type,
                         is_character(inner) ? c_type::string_type
                                             : c_type::pointer_type};
    }
    const auto scalar = scalar_type(pointee);
    if (scalar && *scalar != c_type::void_type)
      return mapped_type{c_type::pointer_type, scalar};
    // void *, struct handles, callbacks: only a pointer or null can be passed.
    return mapped_type{c_type::pointer_type, std::nullopt};
  }
  if (const auto scalar = scalar_type(canonical))
    return mapped_type{*scalar, std::nullopt};
  return std::nullopt;
}

auto describe_c_type(CXType p_type) -> string_type {
  return to_string(clang_getTypeSpelling(p_type));
}

auto first_sentence(string_type p_comment) -> string_type {
  // Strip comment markers and collapse whitespace.
  string_type text;
  for (const auto character : p_comment)
    text.push_back(character == '*' || character == '/' ? ' ' : character);
  auto words = split_words(text);
  string_type result;
  for (const auto &word : words) {
    if (result.size() + word.size() > 120)
      break;
    if (!result.empty())
      result.push_back(' ');
    result += word;
    if (word.ends_with('.'))
      break;
  }
  return result;
}

auto parse_number(const std::vector<string_type> &p_tokens)
    -> std::optional<std::pair<bool, double>> {
  // Accepts N, -N, (N) and (-N); returns (is_real, value).
  std::vector<string_type> tokens = p_tokens;
  if (tokens.size() >= 2 && tokens.front() == "(" && tokens.back() == ")")
    tokens = {tokens.begin() + 1, tokens.end() - 1};
  bool negative = false;
  if (tokens.size() == 2 && tokens[0] == "-") {
    negative = true;
    tokens.erase(tokens.begin());
  }
  if (tokens.size() != 1)
    return std::nullopt;
  auto literal = tokens[0];
  if (literal.empty() ||
      !(std::isdigit(static_cast<unsigned char>(literal[0])) ||
        literal[0] == '.'))
    return std::nullopt;
  const bool is_hex = literal.starts_with("0x") || literal.starts_with("0X");
  const bool is_real =
      !is_hex && literal.find_first_of(".eE") != string_type::npos;
  while (!literal.empty() && string_view_type(is_real ? "fFlL" : "uUlL")
                                     .contains(literal.back()))
    literal.pop_back();
  char_type *end = nullptr;
  double value = is_real
                     ? std::strtod(literal.c_str(), &end)
                     : static_cast<double>(std::strtoull(literal.c_str(), &end, 0));
  if (end == nullptr || *end != '\0')
    return std::nullopt;
  return std::pair{is_real, negative ? -value : value};
}

struct import_state {
  CXTranslationUnit unit;
  CXFile header;
  c_library *library;
  std::set<string_type> seen;
  // The complete structs and unions the header defines, by their
  // declaration's USR: the name Ovis calls each (its tag, or the typedef's
  // for an unnamed one).
  std::map<string_type, string_type> record_names;
  // Structs declared elsewhere that a function or field reaches (`struct
  // tm` for `localtime`), to import after the header's own.
  std::vector<CXCursor> reached;
  // The directory of the library's own headers, if it isn't the system's.
  std::optional<std::filesystem::path> beside;
  // A system header's directory: its private parts there (`_stdlib.h`,
  // `_time.h`; macOS's SDK keeps declarations in them) count as it.
  std::optional<std::filesystem::path> system;
  // Function-like macros, read once the functions they call are known.
  std::vector<std::vector<string_type>> macros;
};

// The C record a type is: a struct the header defines, or one it reaches
// from another header (named, and imported later).
auto record_name(import_state &p_state, CXType p_type)
    -> std::optional<string_type> {
  const auto canonical = clang_getCanonicalType(p_type);
  if (canonical.kind != CXType_Record)
    return std::nullopt;
  const auto declaration = clang_getTypeDeclaration(canonical);
  const auto usr = to_string(clang_getCursorUSR(declaration));
  const auto found = p_state.record_names.find(usr);
  if (found != p_state.record_names.end())
    return found->second;
  const auto definition = clang_getCursorDefinition(declaration);
  if (clang_Cursor_isNull(definition))
    return std::nullopt; // incomplete: a handle
  auto name = to_string(clang_getCursorSpelling(definition));
  if (clang_Cursor_isAnonymous(definition) || name.empty()) {
    // `typedef struct { … } div_t;`: the typedef that names it.
    if (clang_getTypeDeclaration(p_type).kind == CXCursor_TypedefDecl)
      name = to_string(clang_getCursorSpelling(clang_getTypeDeclaration(p_type)));
    else
      return std::nullopt;
  }
  if (name.empty() || name.find(' ') != string_type::npos)
    return std::nullopt;
  p_state.record_names.emplace(usr, name);
  p_state.reached.push_back(definition);
  return name;
}

// A pointer to a function: its C phrase signature (`(i32 ptr) i32`), when
// every type in it is one Ovis passes.
auto c_phrase_of(CXType p_type) -> std::optional<string_type> {
  const auto canonical = clang_getCanonicalType(p_type);
  if (canonical.kind != CXType_Pointer)
    return std::nullopt;
  const auto function = clang_getCanonicalType(clang_getPointeeType(canonical));
  if (function.kind != CXType_FunctionProto &&
      function.kind != CXType_FunctionNoProto)
    return std::nullopt;
  const auto result = map_type(clang_getResultType(function));
  if (!result || clang_isFunctionTypeVariadic(function))
    return std::nullopt;
  std::vector<c_type> parameters;
  const auto count = clang_getNumArgTypes(function);
  for (int index = 0; index < count; ++index) {
    const auto mapped =
        map_type(clang_getArgType(function, static_cast<unsigned>(index)));
    if (!mapped)
      return std::nullopt;
    parameters.push_back(mapped->type);
  }
  return c_phrase_type(parameters, result->type).pointee;
}

// A pointer to a struct the header defines.
auto pointed_record(import_state &p_state, CXType p_type)
    -> std::optional<string_type> {
  const auto canonical = clang_getCanonicalType(p_type);
  if (canonical.kind != CXType_Pointer)
    return std::nullopt;
  return record_name(p_state, clang_getPointeeType(canonical));
}

// The header itself, or one of the library's own headers beside it (not
// the system's: those would bring all of libc).
auto is_in_header(const import_state &p_state, CXCursor p_cursor) -> bool {
  CXFile file = nullptr;
  clang_getSpellingLocation(clang_getCursorLocation(p_cursor), &file, nullptr,
                            nullptr, nullptr);
  if (file == nullptr)
    return false;
  if (clang_File_isEqual(file, p_state.header))
    return true;
  if (!p_state.beside && !p_state.system)
    return false;
  std::error_code ignored;
  const auto path = std::filesystem::weakly_canonical(
      to_string(clang_getFileName(file)), ignored);
  if (p_state.system)
    return path.parent_path() == *p_state.system &&
           path.filename().string().starts_with("_");
  return path.parent_path() == *p_state.beside;
}

auto import_function(import_state &p_state, CXCursor p_cursor) -> void {
  auto name = to_string(clang_getCursorSpelling(p_cursor));
  if (name.starts_with("_") || !p_state.seen.insert(name).second)
    return;
  c_function function{.name = name};
  auto result = map_type(clang_getCursorResultType(p_cursor));
  // A struct given back by value: copied into a new record by a wrapper.
  if (!result)
    if (const auto by_value =
            record_name(p_state, clang_getCursorResultType(p_cursor))) {
      result = mapped_type{c_type::pointer_type, std::nullopt};
      function.result_by_value = true;
      function.result_record = *by_value;
    }
  if (!result) {
    p_state.library->skipped.push_back(
        name + ": returns " +
        describe_c_type(clang_getCursorResultType(p_cursor)) +
        ", which Ovis can't use yet");
    return;
  }
  function.result = result->type;
  if (!function.result_by_value)
    function.result_record =
        pointed_record(p_state, clang_getCursorResultType(p_cursor)).value_or("");
  function.result_phrase =
      c_phrase_of(clang_getCursorResultType(p_cursor)).value_or("");
  function.result_spelling =
      describe_c_type(clang_getCursorResultType(p_cursor));
  const auto count = clang_Cursor_getNumArguments(p_cursor);
  for (int index = 0; index < count; ++index) {
    const auto type = clang_getCursorType(
        clang_Cursor_getArgument(p_cursor, static_cast<unsigned>(index)));
    auto mapped = map_type(type);
    const auto by_value = mapped ? std::nullopt : record_name(p_state, type);
    if (by_value)
      mapped = mapped_type{c_type::pointer_type, std::nullopt};
    if (!mapped) {
      p_state.library->skipped.push_back(
          name + ": takes " + describe_c_type(type) +
          ", which Ovis can't pass yet");
      return;
    }
    function.parameter_names.push_back(to_string(clang_getCursorSpelling(
        clang_Cursor_getArgument(p_cursor, static_cast<unsigned>(index)))));
    function.parameters.push_back(mapped->type);
    function.pointees.push_back(mapped->pointee);
    function.records.push_back(
        by_value ? *by_value : pointed_record(p_state, type).value_or(""));
    function.by_value.push_back(by_value.has_value());
    function.phrases.push_back(c_phrase_of(type).value_or(""));
    function.spellings.push_back(describe_c_type(type));
  }
  function.is_variadic =
      clang_isFunctionTypeVariadic(clang_getCursorType(p_cursor)) != 0;
  if (function.is_variadic && name.find("printf") != string_type::npos &&
      !function.parameters.empty() &&
      function.parameters.back() == c_type::string_type)
    function.format_index = function.parameters.size() - 1;
  function.comment =
      first_sentence(to_string(clang_Cursor_getRawCommentText(p_cursor)));
  p_state.library->functions.push_back(std::move(function));
}

// `#define deflateInit(strm, level) deflateInit_((strm), (level), …)`: a
// macro that is one call of a function the header has, each of its
// parameters given as one of the call's arguments, becomes a function of
// those arguments' types, called through a wrapper that expands it.
auto import_function_macro(import_state &p_state,
                           const std::vector<string_type> &p_tokens) -> void {
  // NAME ( P , P ) CALLEE ( ARGS )
  if (p_tokens.size() < 6 || p_tokens[1] != "(")
    return;
  const auto &name = p_tokens[0];
  std::vector<string_type> parameters;
  std::size_t at = 2;
  for (; at < p_tokens.size() && p_tokens[at] != ")"; ++at)
    if (p_tokens[at] != ",")
      parameters.push_back(p_tokens[at]);
  ++at;
  if (at + 2 >= p_tokens.size() || p_tokens[at + 1] != "(" ||
      p_tokens.back() != ")" || name.starts_with("_") ||
      p_state.seen.contains(name))
    return;
  const auto callee = std::ranges::find(p_state.library->functions,
                                        p_tokens[at], &c_function::name);
  if (callee == p_state.library->functions.end() || callee->is_variadic)
    return;
  // The call's arguments, split at commas outside brackets.
  std::vector<std::vector<string_type>> arguments(1);
  int depth = 0;
  for (auto index = at + 2; index + 1 < p_tokens.size(); ++index) {
    const auto &item = p_tokens[index];
    depth += item == "(" ? 1 : item == ")" ? -1 : 0;
    if (item == "," && depth == 0)
      arguments.emplace_back();
    else
      arguments.back().push_back(item);
  }
  if (depth != 0 || arguments.size() != callee->parameters.size())
    return;
  // Each parameter is an argument by itself (in brackets, maybe cast).
  const auto argument_of = [&](const string_type &p_parameter)
      -> std::optional<std::size_t> {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      auto words = arguments[index];
      std::erase(words, "(");
      std::erase(words, ")");
      if (!words.empty() && words.back() == p_parameter &&
          std::ranges::count(arguments[index], p_parameter) == 1)
        return index;
    }
    return std::nullopt;
  };
  c_function function{.name = name,
                      .result = callee->result,
                      .comment = callee->comment,
                      .result_record = callee->result_record,
                      .result_phrase = callee->result_phrase,
                      .result_spelling = callee->result_spelling,
                      .is_macro = true};
  for (const auto &parameter : parameters) {
    const auto index = argument_of(parameter);
    if (!index || callee->by_value[*index])
      return;
    function.parameter_names.push_back(parameter);
    function.parameters.push_back(callee->parameters[*index]);
    function.pointees.push_back(callee->pointees[*index]);
    function.records.push_back(callee->records[*index]);
    function.phrases.push_back(callee->phrases[*index]);
    function.by_value.push_back(false);
    function.spellings.push_back(callee->spellings[*index]);
  }
  p_state.seen.insert(name);
  p_state.library->functions.push_back(std::move(function));
}

// A function-like macro of one C expression (`MAX(a, b) ((a) > (b) ? (a)
// : (b))`) or of statements (`do { … } while (0)`, or several): a generic C
// phrase (generics.md §9), copied for each call's arguments' C types.
// Macros that build tokens (`#`, `##`), declare things or leave a brace
// open are left out, as Zig's translate-c leaves them.
auto import_generic_macro(import_state &p_state,
                          const std::vector<string_type> &p_tokens) -> void {
  if (p_tokens.size() < 3 || p_tokens[1] != "(")
    return;
  const auto &name = p_tokens[0];
  if (name.starts_with("_") || p_state.seen.contains(name))
    return;
  c_function function{.name = name, .comment = "A C macro.",
                      .is_c_generic = true};
  std::vector<string_type> parameters;
  std::size_t at = 2;
  for (; at < p_tokens.size() && p_tokens[at] != ")"; ++at) {
    if (p_tokens[at] == ",")
      continue;
    if (p_tokens[at] == "...")
      function.macro_variadic = true;
    else
      parameters.push_back(p_tokens[at]);
  }
  ++at;
  const std::vector<string_type> body(p_tokens.begin() + static_cast<std::ptrdiff_t>(
                                          std::min(at, p_tokens.size())),
                                      p_tokens.end());
  const auto skip = [&](const string_type &p_why) {
    p_state.library->skipped.push_back(name + ": a C macro that " + p_why);
  };
  if (body.empty())
    return;
  int depth = 0;
  bool top_semicolon = false;
  for (const auto &item : body) {
    if (item == "#" || item == "##")
      return skip("makes names or text from tokens");
    depth += item == "(" || item == "{" || item == "[" ? 1
             : item == ")" || item == "}" || item == "]" ? -1
                                                          : 0;
    if (depth < 0)
      return skip("closes a bracket it doesn't open");
    if (item == ";" && depth == 0)
      top_semicolon = true;
  }
  if (depth != 0)
    return skip("leaves a bracket open");
  static const std::set<string_view_type> declarations = {
      "typedef", "extern", "static", "inline", "enum", "union", "_Static_assert"};
  if (declarations.contains(body.front()))
    return skip("declares something");
  static const std::set<string_view_type> statements = {"do", "if", "for",
                                                        "while", "switch", "{"};
  function.macro_statements = statements.contains(body.front()) || top_semicolon;
  // What each parameter is, by where its body uses it.
  for (const auto &parameter : parameters) {
    auto role = c_function::macro_role::value;
    for (std::size_t index = 0; index < body.size(); ++index) {
      if (body[index] != parameter)
        continue;
      const auto before = [&](std::size_t p_back) -> string_view_type {
        return index >= p_back ? string_view_type(body[index - p_back]) : "";
      };
      // The token after it, past closing brackets: `(a) = …`.
      auto next = index + 1;
      while (next < body.size() && body[next] == ")")
        ++next;
      const string_view_type after = next < body.size() ? string_view_type(body[next]) : "";
      // Which argument of a call it is: `offsetof(t, m)`'s second.
      std::size_t argument = 0;
      string_view_type callee;
      for (std::size_t back = index, level = 0; back-- > 0;) {
        if (body[back] == ")")
          ++level;
        else if (body[back] == "(") {
          if (level == 0) {
            callee = back > 0 ? string_view_type(body[back - 1]) : "";
            break;
          }
          --level;
        } else if (body[back] == "," && level == 0)
          ++argument;
      }
      const auto is_offsetof = callee == "offsetof" || callee == "__builtin_offsetof";
      const auto is_va_arg = callee == "va_arg" || callee == "__builtin_va_arg";
      if (before(1) == "." || before(1) == "->" || (is_offsetof && argument == 1)) {
        role = c_function::macro_role::field;
        break;
      }
      const bool cast = before(1) == "(" && index + 1 < body.size() &&
                        (body[index + 1] == "*" ||
                         (body[index + 1] == ")" && index + 2 < body.size() &&
                          (std::isalnum(static_cast<unsigned char>(body[index + 2][0])) ||
                           body[index + 2] == "(" || body[index + 2][0] == '_')));
      if ((is_offsetof && argument == 0) || (is_va_arg && argument == 1) ||
          callee == "sizeof" || callee == "__typeof__" || callee == "typeof" ||
          callee == "_Alignof" || cast) {
        role = c_function::macro_role::type;
        break;
      }
      static const std::set<string_view_type> assignments = {
          "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
          "++", "--"};
      if (assignments.contains(after) || before(1) == "++" || before(1) == "--" ||
          (before(1) == "&" && (index < 2 || !std::isalnum(static_cast<unsigned char>(
                                                    before(2).empty() ? ' ' : before(2)[0])))))
        role = c_function::macro_role::assigned;
    }
    function.macro_roles.push_back(role);
  }
  function.parameter_names = parameters;
  function.parameters.assign(parameters.size(), c_type::long_type);
  function.pointees.assign(parameters.size(), std::nullopt);
  function.records.assign(parameters.size(), "");
  function.phrases.assign(parameters.size(), "");
  function.by_value.assign(parameters.size(), false);
  function.spellings.assign(parameters.size(), "");
  function.is_variadic = function.macro_variadic;
  function.macro_name = name;
  p_state.seen.insert(name);
  p_state.library->functions.push_back(std::move(function));
}

// A parameter passed on to another generic macro as one of its types or
// fields is one too (`container_of(p, T, m)` gives T and m to `offsetof`).
auto pass_roles(import_state &p_state,
                const std::map<string_type, std::vector<string_type>> &p_bodies)
    -> void {
  auto &functions = p_state.library->functions;
  for (bool changed = true; changed;) {
    changed = false;
    for (auto &function : functions) {
      if (!function.is_c_generic)
        continue;
      const auto found = p_bodies.find(function.name);
      if (found == p_bodies.end())
        continue;
      const auto &tokens = found->second;
      // Its parameters, as written.
      std::vector<string_type> parameters;
      std::size_t at = 2;
      for (; at < tokens.size() && tokens[at] != ")"; ++at)
        if (tokens[at] != "," && tokens[at] != "...")
          parameters.push_back(tokens[at]);
      for (auto index = at + 1; index + 1 < tokens.size(); ++index) {
        const auto inner = std::ranges::find(functions, tokens[index], &c_function::name);
        if (inner == functions.end() || !inner->is_c_generic ||
            tokens[index + 1] != "(")
          continue;
        std::size_t argument = 0;
        int level = 0;
        for (auto item = index + 2; item < tokens.size(); ++item) {
          if (tokens[item] == "(")
            ++level;
          else if (tokens[item] == ")" && level-- == 0)
            break;
          else if (tokens[item] == "," && level == 0)
            ++argument;
          else if (level == 0 && argument < inner->macro_roles.size() &&
                   inner->macro_roles[argument] != c_function::macro_role::value)
            if (const auto own = std::ranges::find(parameters, tokens[item]);
                own != parameters.end()) {
              auto &role = function.macro_roles[static_cast<std::size_t>(
                  own - parameters.begin())];
              if (role != inner->macro_roles[argument]) {
                role = inner->macro_roles[argument];
                changed = true;
              }
            }
        }
      }
    }
  }
}

auto import_macro(import_state &p_state, CXCursor p_cursor) -> void {
  if (clang_Cursor_isMacroFunctionLike(p_cursor)) {
    CXToken *tokens = nullptr;
    unsigned count = 0;
    clang_tokenize(p_state.unit, clang_getCursorExtent(p_cursor), &tokens,
                   &count);
    std::vector<string_type> spellings;
    for (unsigned index = 0; index < count; ++index)
      spellings.push_back(
          to_string(clang_getTokenSpelling(p_state.unit, tokens[index])));
    clang_disposeTokens(p_state.unit, tokens, count);
    p_state.macros.push_back(std::move(spellings));
    return;
  }
  CXToken *tokens = nullptr;
  unsigned count = 0;
  clang_tokenize(p_state.unit, clang_getCursorExtent(p_cursor), &tokens,
                 &count);
  std::vector<string_type> spellings;
  for (unsigned index = 0; index < count; ++index)
    spellings.push_back(
        to_string(clang_getTokenSpelling(p_state.unit, tokens[index])));
  clang_disposeTokens(p_state.unit, tokens, count);
  if (spellings.size() < 2 || spellings[0].starts_with("_") ||
      !p_state.seen.insert(spellings[0]).second)
    return;
  const auto number =
      parse_number({spellings.begin() + 1, spellings.end()});
  if (!number)
    return;
  c_constant constant{.name = spellings[0]};
  if (number->first) {
    constant.kind = c_constant::kind_type::real;
    constant.real = number->second;
  } else {
    constant.integer = static_cast<max_int_type>(number->second);
  }
  p_state.library->constants.push_back(std::move(constant));
}

// A struct or union's fields as a C record's. Fields Ovis can't read
// (bitfields, arrays other than of characters) are left out and listed.
auto import_record(import_state &p_state, CXCursor p_cursor,
                   const string_type &p_name) -> void {
  if (!p_state.seen.insert("struct " + p_name).second)
    return;
  const auto type = clang_getCursorType(p_cursor);
  record result{.name = p_name,
                .is_c = true,
                .size = static_cast<std::uint64_t>(clang_Type_getSizeOf(type)),
                .c_spelling = describe_c_type(type)};
  struct fields_state {
    import_state *state;
    record *result;
  } walk{&p_state, &result};
  clang_visitChildren(
      p_cursor,
      [](CXCursor p_field, CXCursor, CXClientData p_data) -> CXChildVisitResult {
        auto &walk = *static_cast<fields_state *>(p_data);
        if (clang_getCursorKind(p_field) != CXCursor_FieldDecl)
          return CXChildVisit_Continue;
        const auto name = to_string(clang_getCursorSpelling(p_field));
        const auto field_type = clang_getCursorType(p_field);
        const auto skip = [&](const string_type &p_why) {
          walk.state->library->skipped.push_back(walk.result->name + "." + name +
                                                 ": " + p_why);
          return CXChildVisit_Continue;
        };
        if (name.empty())
          return skip("an unnamed member");
        if (clang_Cursor_isBitField(p_field))
          return skip("a bitfield, which Ovis can't read yet");
        record_field field{
            .name = name,
            .offset = static_cast<std::uint64_t>(
                clang_Cursor_getOffsetOfField(p_field) / 8)};
        const auto canonical = clang_getCanonicalType(field_type);
        if (canonical.kind == CXType_ConstantArray &&
            is_character(clang_getCanonicalType(
                             clang_getArrayElementType(canonical)).kind)) {
          field.type = value_type::string();
          field.text_length =
              static_cast<std::uint64_t>(clang_getNumElements(canonical));
        } else if (const auto inner = record_name(*walk.state, field_type)) {
          field.type = value_type::record_of(*inner);
          field.inline_record = true;
        } else if (const auto pointed = pointed_record(*walk.state, field_type)) {
          field.type = value_type::record_of(*pointed);
          field.storage = c_type::pointer_type;
        } else if (const auto mapped = map_type(field_type);
                   mapped && canonical.kind != CXType_ConstantArray &&
                   canonical.kind != CXType_IncompleteArray) {
          field.type = ovis_type_of(mapped->type);
          field.storage = mapped->type;
        } else {
          return skip("a " + describe_c_type(field_type) +
                      ", which Ovis can't read yet");
        }
        walk.result->fields.push_back(std::move(field));
        return CXChildVisit_Continue;
      },
      &walk);
  p_state.library->records.push_back(std::move(result));
}

// First, the names of the structs the header defines, so a field or a
// function may name one declared further down.
auto name_records(CXCursor p_cursor, CXCursor, CXClientData p_data)
    -> CXChildVisitResult {
  auto &state = *static_cast<import_state *>(p_data);
  if (!is_in_header(state, p_cursor))
    return CXChildVisit_Continue;
  const auto kind = clang_getCursorKind(p_cursor);
  if ((kind == CXCursor_StructDecl || kind == CXCursor_UnionDecl) &&
      clang_isCursorDefinition(p_cursor) && !clang_Cursor_isAnonymous(p_cursor)) {
    const auto name = to_string(clang_getCursorSpelling(p_cursor));
    if (!name.empty() && !name.starts_with("_"))
      state.record_names.try_emplace(to_string(clang_getCursorUSR(p_cursor)),
                                     name);
  }
  if (kind == CXCursor_TypedefDecl) {
    // `typedef struct { … } Vector2;`: the typedef names it.
    const auto underlying = clang_getCanonicalType(
        clang_getTypedefDeclUnderlyingType(p_cursor));
    const auto declaration = clang_getTypeDeclaration(underlying);
    // `typedef struct z_stream_s { … } z_stream;`: the name C code uses.
    const auto definition = clang_getCursorDefinition(declaration);
    if (underlying.kind == CXType_Record && !clang_Cursor_isNull(definition) &&
        is_in_header(state, definition))
      state.record_names.insert_or_assign(
          to_string(clang_getCursorUSR(definition)),
          to_string(clang_getCursorSpelling(p_cursor)));
  }
  return CXChildVisit_Continue;
}

auto visit(CXCursor p_cursor, CXCursor, CXClientData p_data)
    -> CXChildVisitResult {
  auto &state = *static_cast<import_state *>(p_data);
  if (!is_in_header(state, p_cursor))
    return CXChildVisit_Continue;
  switch (clang_getCursorKind(p_cursor)) {
  case CXCursor_FunctionDecl:
    import_function(state, p_cursor);
    break;
  case CXCursor_MacroDefinition:
    import_macro(state, p_cursor);
    break;
  case CXCursor_EnumDecl:
    return CXChildVisit_Recurse;
  case CXCursor_StructDecl:
  case CXCursor_UnionDecl:
  case CXCursor_TypedefDecl: {
    const auto declaration =
        clang_getCursorKind(p_cursor) == CXCursor_TypedefDecl
            ? clang_getTypeDeclaration(clang_getCanonicalType(
                  clang_getTypedefDeclUnderlyingType(p_cursor)))
            : p_cursor;
    if (!clang_isCursorDefinition(declaration))
      break;
    const auto found = state.record_names.find(
        to_string(clang_getCursorUSR(declaration)));
    if (found != state.record_names.end())
      import_record(state, declaration, found->second);
    break;
  }
  case CXCursor_EnumConstantDecl: {
    auto name = to_string(clang_getCursorSpelling(p_cursor));
    if (state.seen.insert(name).second)
      state.library->constants.push_back(
          {.name = name,
           .integer = clang_getEnumConstantDeclValue(p_cursor)});
    break;
  }
  default:
    break;
  }
  return CXChildVisit_Continue;
}

// The compiler's own headers a release package carries beside ovis
// (lib/clang/N, docs/deployment.md), if this ovis came from one.
auto bundled_resource_directory() -> std::optional<string_type> {
  static int anchor;
  const std::filesystem::path executable =
      llvm::sys::fs::getMainExecutable(nullptr, &anchor);
  if (executable.empty())
    return std::nullopt;
  const auto directory = executable.parent_path().parent_path() / "lib" /
                         "clang" / std::to_string(LLVM_VERSION_MAJOR);
  if (std::error_code ignored;
      std::filesystem::exists(directory / "include" / "stddef.h", ignored))
    return directory.string();
  return std::nullopt;
}

// What libclang needs to find the compiler's own headers (`stddef.h`) and
// the system's. A release package's own come first. Homebrew's libclang on
// macOS knows neither: it gets the resource directory of the clang beside
// it (or the one on the PATH) and the SDK (`xcrun --show-sdk-path`).
// Worked out once.
auto platform_flags() -> const std::vector<string_type> & {
  static const std::vector<string_type> flags = [] {
    std::vector<string_type> result;
    if (const auto bundled = bundled_resource_directory())
      result.push_back("-resource-dir=" + *bundled);
#ifdef __APPLE__
#ifdef OVIS_CLANG_RESOURCE_DIR
    if (std::error_code ignored;
        result.empty() &&
        std::filesystem::exists(OVIS_CLANG_RESOURCE_DIR "/include", ignored))
      result.emplace_back("-resource-dir=" OVIS_CLANG_RESOURCE_DIR);
#endif
    const auto trimmed = [](string_type p_text) {
      while (!p_text.empty() && (p_text.back() == '\n' || p_text.back() == ' '))
        p_text.pop_back();
      return p_text;
    };
    if (result.empty())
      if (const auto directory = run_and_capture("clang", {"-print-resource-dir"}))
        if (const auto path = trimmed(*directory); !path.empty())
          result.push_back("-resource-dir=" + path);
    if (const auto sdk = run_and_capture("xcrun", {"--show-sdk-path"}))
      if (const auto path = trimmed(*sdk); !path.empty()) {
        result.emplace_back("-isysroot");
        result.push_back(path);
      }
#endif
    return result;
  }();
  return flags;
}

auto parse_header(c_library &p_library,
                  const std::vector<string_type> &p_include_flags) -> void {
  const auto header = p_library.header_path.string();
  const auto source = "#include \"" + header + "\"\n";
  CXUnsavedFile unsaved{"ovis_import.c", source.c_str(),
                        static_cast<unsigned long>(source.size())};
  std::vector<const char *> arguments = {"-x", "c", "-std=gnu11",
                                         "-fparse-all-comments"};
  for (const auto &flag : p_include_flags)
    arguments.push_back(flag.c_str());
  for (const auto &flag : platform_flags())
    arguments.push_back(flag.c_str());

  const auto index = clang_createIndex(0, 0);
  CXTranslationUnit unit = nullptr;
  const auto code = clang_parseTranslationUnit2(
      index, "ovis_import.c", arguments.data(),
      static_cast<int>(arguments.size()), &unsaved, 1,
      CXTranslationUnit_DetailedPreprocessingRecord |
          CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_Incomplete,
      &unit);
  if (code != CXError_Success || unit == nullptr) {
    clang_disposeIndex(index);
    throw error("Cannot read the C header " + header + ".");
  }
  for (unsigned diagnostic = 0; diagnostic < clang_getNumDiagnostics(unit);
       ++diagnostic) {
    const auto item = clang_getDiagnostic(unit, diagnostic);
    const auto severity = clang_getDiagnosticSeverity(item);
    const auto text = to_string(clang_formatDiagnostic(
        item, clang_defaultDiagnosticDisplayOptions()));
    clang_disposeDiagnostic(item);
    if (severity >= CXDiagnostic_Error) {
      clang_disposeTranslationUnit(unit);
      clang_disposeIndex(index);
      throw error("The C header " + header + " has errors: " + text);
    }
  }

  // The files read, for a build's dependency file; the system's are left out.
  struct reading {
    c_library *library;
    std::vector<std::filesystem::path> system;
  } inclusions{&p_library, system_include_directories()};
  clang_getInclusions(
      unit,
      [](CXFile p_file, CXSourceLocation *, unsigned, CXClientData p_data) {
        auto &state = *static_cast<reading *>(p_data);
        std::error_code ignored;
        const std::filesystem::path path = std::filesystem::weakly_canonical(
            to_string(clang_getFileName(p_file)), ignored);
        if (path.empty() || path.filename() == "ovis_import.c")
          return;
        for (const auto &directory : state.system) {
          const auto [end, unused] = std::mismatch(
              directory.begin(), directory.end(), path.begin(), path.end());
          if (end == directory.end())
            return;
        }
        state.library->dependencies.push_back(path);
      },
      &inclusions);

  import_state state{unit, clang_getFile(unit, header.c_str()), &p_library,
                     {}, {}, {}, {}, {}};
  const auto directory = p_library.header_path.parent_path();
  const auto system = system_include_directories();
  if (std::ranges::find(system, directory) == system.end())
    state.beside = directory;
  else
    state.system = std::filesystem::weakly_canonical(directory);
  clang_visitChildren(clang_getTranslationUnitCursor(unit), name_records, &state);
  clang_visitChildren(clang_getTranslationUnitCursor(unit), visit, &state);
  for (const auto &macro : state.macros)
    import_function_macro(state, macro);
  std::map<string_type, std::vector<string_type>> bodies;
  for (const auto &macro : state.macros) {
    import_generic_macro(state, macro);
    if (!macro.empty())
      bodies.emplace(macro.front(), macro);
  }
  pass_roles(state, bodies);
  // The structs the functions reach, and those their fields reach.
  for (std::size_t index = 0; index < state.reached.size(); ++index) {
    const auto cursor = state.reached[index];
    import_record(state, cursor,
                  state.record_names.at(to_string(clang_getCursorUSR(cursor))));
  }
  clang_disposeTranslationUnit(unit);
  clang_disposeIndex(index);
}

#endif

auto find_header(string_view_type p_header,
                 const std::vector<std::filesystem::path> &p_directories)
    -> std::optional<std::filesystem::path> {
  const std::filesystem::path header{string_type(p_header)};
  std::error_code code;
  if (header.is_absolute())
    return std::filesystem::exists(header, code)
               ? std::optional(std::filesystem::canonical(header))
               : std::nullopt;
  for (const auto &directory : p_directories)
    if (std::filesystem::exists(directory / header, code))
      return std::filesystem::canonical(directory / header);
  return std::nullopt;
}

} // namespace

auto system_include_directories() -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> result;
  for (const auto *const directory :
       {"/usr/local/include", "/usr/include/x86_64-linux-gnu",
        "/usr/include/aarch64-linux-gnu", "/usr/include",
        "/opt/homebrew/include"})
    result.emplace_back(directory);
#ifdef __APPLE__
  // Headers live in the SDK, not /usr/include.
  if (const auto sdk = run_and_capture("xcrun", {"--show-sdk-path"})) {
    auto path = *sdk;
    while (!path.empty() && (path.back() == '\n' || path.back() == ' '))
      path.pop_back();
    if (!path.empty())
      result.emplace_back(std::filesystem::path(path) / "usr" / "include");
  }
#endif
  return result;
}

auto has_c_import_support() -> bool {
#ifdef OVIS_HAS_C_IMPORT
  return true;
#else
  return false;
#endif
}

namespace {

// Libraries read so far, by header, library and the flags they were read with.
auto import_cache() -> std::map<string_type, c_library> & {
  static std::map<string_type, c_library> cache;
  return cache;
}

} // namespace

auto imported_dependencies() -> std::vector<std::filesystem::path> {
  std::set<std::filesystem::path> unique;
  for (const auto &[key, library] : import_cache())
    unique.insert(library.dependencies.begin(), library.dependencies.end());
  return {unique.begin(), unique.end()};
}

auto import_c_library(string_view_type p_header, string_view_type p_library,
                      const c_import_options &p_options) -> const c_library & {
  auto &cache = import_cache();
  string_type key = string_type(p_header) + "|" + string_type(p_library);
  for (const auto &directory : p_options.include_directories)
    key += "|I" + directory.string();
  for (const auto &directory : p_options.library_directories)
    key += "|L" + directory.string();
  for (const auto &flag : p_options.compiler_flags)
    key += "|F" + flag;
  key += p_options.link_by_caller ? "|caller" : "|self";
  if (const auto found = cache.find(key); found != cache.end())
    return found->second;

#ifndef OVIS_HAS_C_IMPORT
  throw error("This build of Ovis can't read C headers (it was built without "
              "libclang); declare the functions with (extern ...) instead.");
#else
  c_library library;

  // pkg-config knows include and link flags for most libraries. Try the
  // library name, then the header's name (zlib.h → zlib).
  std::vector<string_type> packages;
  if (!p_library.empty())
    packages.emplace_back(p_library);
  const auto stem =
      std::filesystem::path(string_type(p_header)).stem().string();
  if (stem != p_library)
    packages.push_back(stem);
  std::vector<string_type> include_flags;
  std::optional<string_type> library_flags;
  // Under CMake the target's flags come with the call and its library is
  // linked by CMake: pkg-config has nothing to add.
  for (const auto &package : p_options.link_by_caller ? std::vector<string_type>()
                                                      : packages) {
    library_flags = run_and_capture("pkg-config", {"--libs", package});
    if (!library_flags)
      continue;
    for (auto &flag :
         split_words(run_and_capture("pkg-config", {"--cflags", package})
                         .value_or("")))
      include_flags.push_back(std::move(flag));
    break;
  }

  std::vector<std::filesystem::path> directories =
      p_options.include_directories;
  for (const auto &flag : include_flags)
    if (flag.starts_with("-I"))
      directories.emplace_back(flag.substr(2));
  for (auto &directory : system_include_directories())
    directories.push_back(std::move(directory));
  const auto header_path = find_header(p_header, directories);
  if (!header_path)
    throw error("Cannot find the C header " + string_type(p_header) +
                ". Install the library's development files, or pass -I with "
                "the directory that holds it.");
  library.header_path = *header_path;
  library.header_hash = fnv_hash(read_whole_file(*header_path));

  for (const auto &directory : p_options.include_directories)
    include_flags.push_back("-I" + directory.string());
  for (const auto &flag : p_options.compiler_flags)
    include_flags.push_back(flag);
  parse_header(library, include_flags);
  // What a wrapper for a struct passed by value is compiled with.
  for (auto &function : library.functions) {
    function.header_path = library.header_path.string();
    function.compile_flags = include_flags;
    function.header_hash = library.header_hash;
  }

  if (p_options.link_by_caller)
    return cache.emplace(key, std::move(library)).first->second;
  for (const auto &directory : p_options.library_directories)
    library.link_flags.push_back("-L" + directory.string());
  if (library_flags) {
    for (auto &flag : split_words(*library_flags))
      library.link_flags.push_back(std::move(flag));
  } else if (!p_library.empty()) {
    library.link_flags.push_back("-l" + string_type(p_library));
  }
  return cache.emplace(key, std::move(library)).first->second;
#endif
}

auto c_macro_answers() -> std::map<string_type, string_type> & {
  static std::map<string_type, string_type> answers;
  return answers;
}

auto c_macro_answers_used() -> std::set<string_type> & {
  static std::set<string_type> used;
  return used;
}

auto c_macro_probes() -> std::size_t & {
  static std::size_t probes = 0;
  return probes;
}

namespace {

// A C type as a part of a copy's name: `u32`, `f64`, `s`, `item_p`.
auto macro_code(const macro_argument &p_argument) -> string_type {
  static const std::map<string_view_type, string_view_type> names = {
      {"int8_t", "i8"},    {"uint8_t", "u8"},   {"int16_t", "i16"},
      {"uint16_t", "u16"}, {"int32_t", "i32"},  {"uint32_t", "u32"},
      {"long long", "i64"}, {"uint64_t", "u64"}, {"float", "f32"},
      {"double", "f64"},   {"char *", "s"},     {"void *", "p"},
      {"int", "i32"}};
  string_type code;
  if (const auto found = names.find(p_argument.spelling); found != names.end())
    code = found->second;
  else
    for (const auto character : p_argument.spelling)
      if (std::isalnum(static_cast<unsigned char>(character)) != 0)
        code.push_back(character);
      else if (character == '*')
        code += "_p";
      else if (character == ' ' && !code.empty() && code.back() != '_')
        code.push_back('_');
  switch (p_argument.role) {
  case c_function::macro_role::value:
    return code;
  case c_function::macro_role::type:
    return "t" + code;
  case c_function::macro_role::field:
    return "m" + p_argument.spelling;
  case c_function::macro_role::assigned:
    return "r" + code;
  }
  return code;
}

auto macro_key(const macro_request &p_request) -> string_type {
  string_type key = p_request.macro->header_hash + " " + p_request.macro->name + "(";
  for (std::size_t index = 0; index < p_request.arguments.size(); ++index) {
    const auto &argument = p_request.arguments[index];
    static constexpr char roles[] = {'v', 't', 'f', 'r'};
    key += string_type(index > 0 ? ", " : "") +
           roles[static_cast<std::size_t>(argument.role)] + ":" + argument.spelling;
  }
  return key + ")";
}

// The copy a call calls, from clang's answer: "TYPE SPELLING" (the
// result's c_type as a number, then as C spells it) or "!WHY".
auto macro_copy(const macro_request &p_request, const string_type &p_answer)
    -> macro_answer {
  if (p_answer.starts_with("!"))
    return {std::nullopt, p_answer.substr(1)};
  const auto &macro = *p_request.macro;
  c_function copy{.name = macro.name,
                  .comment = "A C macro.",
                  .is_macro = true};
  const auto space = p_answer.find(' ');
  copy.result = static_cast<c_type>(std::stoi(p_answer.substr(0, space)));
  copy.result_spelling = space == string_type::npos ? "void" : p_answer.substr(space + 1);
  copy.header_path = macro.header_path;
  copy.compile_flags = p_request.flags.empty() ? macro.compile_flags : p_request.flags;
  copy.macro_headers = p_request.headers;
  copy.header_hash = macro.header_hash;
  copy.macro_name = macro.name;
  copy.library = macro.library;
  std::vector<string_type> codes;
  for (const auto &argument : p_request.arguments) {
    codes.push_back(macro_code(argument));
    const auto parameter = "p" + std::to_string(copy.parameters.size());
    switch (argument.role) {
    case c_function::macro_role::value:
      copy.parameters.push_back(argument.type);
      copy.spellings.push_back(argument.spelling);
      copy.pointees.emplace_back();
      copy.macro_arguments.push_back(parameter);
      copy.records.push_back(argument.record);
      copy.phrases.emplace_back();
      copy.by_value.push_back(false);
      continue;
    case c_function::macro_role::assigned:
      copy.parameters.push_back(c_type::pointer_type);
      copy.spellings.push_back(argument.spelling + " *");
      copy.pointees.emplace_back(argument.type);
      copy.macro_arguments.push_back("(*" + parameter + ")");
      break;
    case c_function::macro_role::type:
    case c_function::macro_role::field:
      copy.macro_arguments.push_back(argument.spelling);
      continue;
    }
    copy.records.emplace_back();
    copy.phrases.emplace_back();
    copy.by_value.push_back(false);
  }
  for (std::size_t index = 0; index < codes.size(); ++index)
    copy.name += (index == 0 ? "__" : "_") + codes[index];
  return {std::move(copy), {}};
}

} // namespace

namespace {

// How C spells a value of this Ovis type, and how Ovis passes it.
auto c_spelling_of(const program &p_program, const value_type &p_type)
    -> std::optional<std::pair<string_type, c_type>> {
  const auto record_spelling = [&](const string_type &p_name) -> string_type {
    const auto *const found = find_record(p_program, p_name);
    if (found == nullptr || !found->is_c)
      return "";
    return found->c_spelling.empty() ? "struct " + p_name : found->c_spelling;
  };
  if (p_type.is_list || !p_type.is_known())
    return std::nullopt;
  if (p_type.is_numeric()) {
    const auto number = *p_type.number_type();
    static const std::map<c_type, string_view_type> names = {
        {c_type::int8_type, "int8_t"},   {c_type::uint8_type, "uint8_t"},
        {c_type::int16_type, "int16_t"}, {c_type::uint16_type, "uint16_t"},
        {c_type::int_type, "int32_t"},   {c_type::uint_type, "uint32_t"},
        {c_type::long_type, "long long"}, {c_type::size_type, "uint64_t"},
        {c_type::float_type, "float"},   {c_type::double_type, "double"}};
    return std::pair{string_type(names.at(number)), number};
  }
  if (p_type == value_type::boolean())
    return std::pair{string_type("int"), c_type::int_type};
  if (p_type == value_type::string())
    return std::pair{string_type("char *"), c_type::string_type};
  if (p_type.is_record()) {
    const auto spelling = record_spelling(p_type.record);
    if (spelling.empty())
      return std::nullopt;
    return std::pair{spelling + " *", c_type::pointer_type};
  }
  if (p_type.is_pointer() && !p_type.is_phrase()) {
    if (p_type.kind == pointer_kind::raw)
      return std::pair{string_type("void *"), c_type::pointer_type};
    const auto pointee = read_pointee(p_type.pointee);
    if (!pointee)
      return std::nullopt;
    if (pointee->kind == pointee_type::kind_type::number)
      return std::pair{c_spelling_of(p_program, value_type::number(pointee->number))
                               ->first + " *",
                       c_type::pointer_type};
    if (pointee->kind == pointee_type::kind_type::structure) {
      const auto spelling = record_spelling(pointee->record);
      if (spelling.empty())
        return std::nullopt;
      return std::pair{spelling + " *", c_type::pointer_type};
    }
    if (const auto inner = c_spelling_of(p_program, pointee->value))
      return std::pair{inner->first + " *", c_type::pointer_type};
  }
  return std::nullopt;
}

} // namespace

auto macro_arguments_of(const program &p_program, const c_function &p_macro,
                        const std::vector<expression> &p_arguments,
                        const std::function<value_type(std::size_t)> &p_type_of,
                        bool p_guess, string_type &p_problem)
    -> std::optional<std::vector<macro_argument>> {
  using role_type = c_function::macro_role;
  std::vector<macro_argument> result;
  if (p_arguments.size() < p_macro.macro_roles.size() ||
      (p_arguments.size() > p_macro.macro_roles.size() && !p_macro.macro_variadic)) {
    p_problem = "The C macro '" + p_macro.name + "' takes " +
                std::to_string(p_macro.macro_roles.size()) +
                (p_macro.macro_variadic ? " or more" : "") + " argument(s), not " +
                std::to_string(p_arguments.size()) + ".";
    return std::nullopt;
  }
  bool waiting = false;
  for (std::size_t index = 0; index < p_arguments.size(); ++index) {
    const auto role =
        index < p_macro.macro_roles.size() ? p_macro.macro_roles[index] : role_type::value;
    const auto &argument = p_arguments[index];
    const bool is_type = argument.kind == expression::kind_type::call &&
                         argument.text == "type";
    const bool is_member = argument.kind == expression::kind_type::call &&
                           argument.text == "member";
    const auto place = "Argument " + std::to_string(index + 1) + " of the C macro '" +
                       p_macro.name + "'";
    if (role == role_type::type) {
      if (!is_type) {
        p_problem = place + " is a type: write one, as in 'a whole number' or "
                            "'an int32'.";
        return std::nullopt;
      }
      const auto pointee = read_pointee(argument.fields.empty() ? "" : argument.fields[0]);
      std::optional<std::pair<string_type, c_type>> spelled;
      if (pointee && pointee->kind == pointee_type::kind_type::number)
        spelled = c_spelling_of(p_program, value_type::number(pointee->number));
      else if (pointee && pointee->kind == pointee_type::kind_type::structure)
        spelled = c_spelling_of(p_program, value_type::record_of(pointee->record));
      else if (pointee)
        spelled = c_spelling_of(p_program, pointee->value);
      if (!spelled) {
        p_problem = place + " is a type C has: a number, text, a pointer or a C "
                            "record.";
        return std::nullopt;
      }
      // A C record as a type is its struct, not a pointer to it.
      auto spelling = spelled->first;
      if (pointee && (pointee->kind == pointee_type::kind_type::structure ||
                      (pointee->kind == pointee_type::kind_type::value &&
                       pointee->value.is_record())))
        spelling = spelling.substr(0, spelling.size() - 2);
      result.push_back({role, spelling, spelled->second});
      continue;
    }
    if (role == role_type::field) {
      if (!is_member || argument.fields.empty()) {
        p_problem = place + " is a field: write its name.";
        return std::nullopt;
      }
      result.push_back({role, argument.fields[0], c_type::long_type});
      continue;
    }
    if (is_type || is_member) {
      p_problem = place + " is a value, not a " + (is_type ? "type." : "field.");
      return std::nullopt;
    }
    auto type = p_type_of(index);
    if (role == role_type::assigned) {
      // `ref x`: the macro sets x, through its address.
      if (argument.kind != expression::kind_type::call || argument.text != "ref" ||
          argument.arguments.size() != 1) {
        p_problem = place + " is set by the macro: give its address, as in 'ref x'.";
        return std::nullopt;
      }
      const auto pointee = type.is_typed_pointer() ? read_pointee(type.pointee)
                                                   : std::nullopt;
      type = !pointee ? value_type::unknown()
             : pointee->kind == pointee_type::kind_type::number
                 ? value_type::number(pointee->number)
                 : pointee_value(*pointee);
    }
    if (!type.is_known() || type.has_variables()) {
      if (!p_guess || type.has_variables()) {
        waiting = true;
        continue;
      }
      type = value_type::integer();
    }
    const auto spelled = c_spelling_of(p_program, type);
    if (!spelled) {
      p_problem = place + " can't be " + describe_in_english(type) +
                  ": C takes numbers, text, pointers and C records.";
      return std::nullopt;
    }
    result.push_back({role, spelled->first, spelled->second,
                      type.is_record() ? type.record : string_type()});
  }
  if (waiting)
    return std::nullopt;
  return result;
}

auto headers_of(const program &p_program)
    -> std::pair<std::vector<string_type>, std::vector<string_type>> {
  std::pair<std::vector<string_type>, std::vector<string_type>> result;
  for (const auto &unit : p_program.units)
    for (const auto &function : unit.externs) {
      if (!function.header_path.empty() &&
          std::ranges::find(result.first, function.header_path) == result.first.end())
        result.first.push_back(function.header_path);
      for (const auto &flag : function.compile_flags)
        if (std::ranges::find(result.second, flag) == result.second.end())
          result.second.push_back(flag);
    }
  return result;
}

auto macro_result_record(const program &p_program, const c_function &p_copy)
    -> string_type {
  if (p_copy.result != c_type::pointer_type)
    return "";
  for (const auto &unit : p_program.units)
    for (const auto &record : unit.records)
      if (record.is_c &&
          ((record.c_spelling.empty() ? "struct " + record.name : record.c_spelling) +
           " *") == p_copy.result_spelling)
        return record.name;
  return "";
}

auto copy_c_macros(const std::vector<macro_request> &p_requests)
    -> std::vector<macro_answer> {
  auto &answers = c_macro_answers();
  std::vector<macro_answer> result(p_requests.size());
  // Those clang hasn't answered, by the header (and flags) they need.
  std::map<string_type, std::vector<std::size_t>> asking;
  for (std::size_t index = 0; index < p_requests.size(); ++index) {
    const auto key = macro_key(p_requests[index]);
    if (answers.contains(key))
      continue;
    string_type group = p_requests[index].macro->header_path;
    for (const auto &flag : p_requests[index].macro->compile_flags)
      group += "\n" + flag;
    for (const auto &header : p_requests[index].headers)
      group += "\nh" + header;
    for (const auto &flag : p_requests[index].flags)
      group += "\n" + flag;
    asking[group].push_back(index);
  }
#ifdef OVIS_HAS_C_IMPORT
  for (const auto &[group, indices] : asking) {
    const auto &first = *p_requests[indices.front()].macro;
    // One file for every call: `typedef __typeof__(MAX(a0, a1)) r0;`, or a
    // function calling a macro of statements; each call's lines noted.
    const auto &request_of_group = p_requests[indices.front()];
    string_type source = "#include <stdint.h>\n#include <stddef.h>\n";
    std::size_t line = 3;
    std::vector<string_type> included = {first.header_path};
    for (const auto &header : request_of_group.headers)
      if (std::ranges::find(included, header) == included.end())
        included.push_back(header);
    for (const auto &header : included) {
      source += "#include \"" + header + "\"\n";
      ++line;
    }
    std::vector<std::pair<std::size_t, std::size_t>> lines;
    // Each call in a function of its own, its values local variables (a
    // compound literal of them is fine there).
    for (const auto index : indices) {
      const auto &request = p_requests[index];
      const auto begin = line;
      source += "void ovis_f" + std::to_string(index) + "(void) {\n";
      ++line;
      string_type arguments;
      for (std::size_t at = 0; at < request.arguments.size(); ++at) {
        const auto &argument = request.arguments[at];
        const auto variable = "ovis_a" + std::to_string(at);
        if (argument.role == c_function::macro_role::value ||
            argument.role == c_function::macro_role::assigned) {
          source += "  " + argument.spelling + " " + variable + ";\n";
          ++line;
          arguments += (at > 0 ? ", " : "") + variable;
        } else {
          arguments += (at > 0 ? ", " : "") + argument.spelling;
        }
      }
      if (request.macro->macro_statements)
        source += "  " + request.macro->name + "(" + arguments + ");\n}\n";
      else
        source += "  typedef __typeof__(" + request.macro->name + "(" + arguments +
                  ")) ovis_r" + std::to_string(index) + ";\n}\n";
      line += 2;
      lines.emplace_back(begin, line);
    }
    ++c_macro_probes();
    std::vector<const char *> flags = {"-x", "c", "-std=gnu11", "-ferror-limit=0"};
    for (const auto &flag : first.compile_flags)
      flags.push_back(flag.c_str());
    for (const auto &flag : request_of_group.flags)
      flags.push_back(flag.c_str());
    for (const auto &flag : platform_flags())
      flags.push_back(flag.c_str());
    CXUnsavedFile unsaved{"ovis_macros.c", source.c_str(),
                          static_cast<unsigned long>(source.size())};
    const auto index = clang_createIndex(0, 0);
    CXTranslationUnit unit = nullptr;
    if (clang_parseTranslationUnit2(index, "ovis_macros.c", flags.data(),
                                    static_cast<int>(flags.size()), &unsaved, 1,
                                    CXTranslationUnit_None, &unit) != CXError_Success ||
        unit == nullptr) {
      clang_disposeIndex(index);
      for (const auto each : indices)
        answers[macro_key(p_requests[each])] = "!clang could not read the header";
      continue;
    }
    std::map<std::size_t, string_type> problems;
    for (unsigned diagnostic = 0; diagnostic < clang_getNumDiagnostics(unit);
         ++diagnostic) {
      const auto item = clang_getDiagnostic(unit, diagnostic);
      if (clang_getDiagnosticSeverity(item) >= CXDiagnostic_Error) {
        CXFile file = nullptr;
        unsigned at = 0;
        clang_getExpansionLocation(clang_getDiagnosticLocation(item), &file, &at,
                                   nullptr, nullptr);
        if (file != nullptr &&
            to_string(clang_getFileName(file)).ends_with("ovis_macros.c"))
          for (std::size_t each = 0; each < lines.size(); ++each)
            if (at >= lines[each].first && at < lines[each].second)
              problems.try_emplace(each, to_string(clang_getDiagnosticSpelling(item)));
      }
      clang_disposeDiagnostic(item);
    }
    struct found_types {
      std::map<std::size_t, CXType> types;
    } found;
    clang_visitChildren(
        clang_getTranslationUnitCursor(unit),
        [](CXCursor p_cursor, CXCursor, CXClientData p_data) {
          // The typedefs are in the probes' functions.
          if (clang_getCursorKind(p_cursor) == CXCursor_FunctionDecl ||
              clang_getCursorKind(p_cursor) == CXCursor_CompoundStmt ||
              clang_getCursorKind(p_cursor) == CXCursor_DeclStmt)
            return CXChildVisit_Recurse;
          if (clang_getCursorKind(p_cursor) != CXCursor_TypedefDecl)
            return CXChildVisit_Continue;
          const auto name = to_string(clang_getCursorSpelling(p_cursor));
          if (!name.starts_with("ovis_r") ||
              name.find_first_not_of("0123456789", 6) != string_type::npos)
            return CXChildVisit_Continue;
          static_cast<found_types *>(p_data)->types[std::stoul(name.substr(6))] =
              clang_getTypedefDeclUnderlyingType(p_cursor);
          return CXChildVisit_Continue;
        },
        &found);
    for (std::size_t each = 0; each < indices.size(); ++each) {
      const auto index_of = indices[each];
      const auto key = macro_key(p_requests[index_of]);
      if (const auto problem = problems.find(each); problem != problems.end()) {
        answers[key] = "!" + problem->second;
        continue;
      }
      if (p_requests[index_of].macro->macro_statements) {
        answers[key] = std::to_string(static_cast<int>(c_type::void_type)) + " void";
        continue;
      }
      const auto type = found.types.find(index_of);
      const auto canonical = type == found.types.end()
                                 ? CXType{}
                                 : clang_getCanonicalType(type->second);
      if (type == found.types.end()) {
        answers[key] = "!clang gave it no type";
      } else if (canonical.kind == CXType_Void) {
        answers[key] = std::to_string(static_cast<int>(c_type::void_type)) + " void";
      } else if (const auto mapped = map_type(type->second)) {
        answers[key] = std::to_string(static_cast<int>(mapped->type)) + " " +
                       describe_c_type(canonical);
      } else {
        answers[key] = "!it gives back " + describe_c_type(canonical) +
                       ", which Ovis can't take yet";
      }
    }
    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(index);
  }
#else
  for (const auto &[group, indices] : asking)
    for (const auto each : indices)
      answers[macro_key(p_requests[each])] =
          "!this build of Ovis can't read C headers";
#endif
  for (std::size_t index = 0; index < p_requests.size(); ++index) {
    const auto key = macro_key(p_requests[index]);
    c_macro_answers_used().insert(key);
    result[index] = macro_copy(p_requests[index], answers.at(key));
  }
  return result;
}

auto resolve_c_imports(program &p_program, const c_import_options &p_options)
    -> std::vector<diagnostic> {
  std::vector<diagnostic> result;
  for (std::size_t index = 0; index < p_program.units.size(); ++index) {
    auto &unit = p_program.units[index];
    std::vector<c_function> imported;
    for (const auto &request : unit.c_imports) {
      try {
        const auto &library =
            import_c_library(request.header, request.library, p_options);
        for (auto function : library.functions) {
          // Ovis's own `free`, `abs`… do what C's would (`free` frees
          // either kind of memory).
          if (is_builtin(function.name))
            continue;
          function.library = request.header;
          imported.push_back(std::move(function));
        }
        unit.constants.insert(unit.constants.end(), library.constants.begin(),
                              library.constants.end());
        // A struct is one type in the whole program, however many files
        // use its header.
        for (const auto &record : library.records)
          if (!find_record(p_program, record.name)) {
            auto added = record;
            added.location = request.location;
            unit.records.push_back(std::move(added));
          }
        unit.link_flags.insert(unit.link_flags.end(),
                               library.link_flags.begin(),
                               library.link_flags.end());
      } catch (const error &p_error) {
        result.push_back({index, request.location, p_error.what()});
      }
    }
    // The paragraph's own (extern ...) declarations win over imported ones.
    imported.insert(imported.end(), unit.externs.begin(), unit.externs.end());
    unit.externs = std::move(imported);
  }
  return result;
}

} // namespace ovis::core

#ifndef OVIS_CORE_AST_HPP
#define OVIS_CORE_AST_HPP

#include <map>
#include <optional>
#include <vector>

#include <ovis/core/libc.hpp>
#include <ovis/core/sexpr.hpp>
#include <ovis/def.hpp>

// The abstract syntax tree of Ovis Core, the strict language the English
// source is translated into.
namespace ovis::core {

// `unknown` (written `?`) only exists between parsing and elaboration: the
// translator may leave types out and the elaborator infers them.
enum class base_type {
  nothing,
  integer,
  real,
  boolean,
  string,
  pointer,
  // A place values are made in (`new-memory`); see docs/proposals/memory.md.
  memory,
  unknown,
  record,
  // A map from keys (whole numbers or text) to values: `(map str int)`.
  // Its key and value types are in `value_type::pointee`, `(KEY VALUE)`.
  map,
  // A generic's type variable (`(var item)`, docs/proposals/generics.md):
  // its name is in `value_type::record`. It is rigid: only itself.
  variable
};

// What a typed pointer may do (docs/proposals/references.md §3, Zig's
// kinds): `raw` is C's `void *` (`ptr`); `one` points at one value (`(ptr
// T)`), `many` at several with no length (`(many T)`), `c` is what a C
// header gives (`(cptr T)`: one or many, maybe null).
// `phrase` and `c_phrase` are phrases as values (references.md §7b): a
// pointer to a function, its signature in `pointee` as `(ARG...) RESULT`,
// Ovis's types for a `phrase`, C's (`i32`, `f64`…) for a `c_phrase`.
enum class pointer_kind : unsigned char { raw, one, many, c, phrase, c_phrase };

struct value_type {
  base_type base = base_type::nothing;
  bool is_list = false;
  // Lists inside the list: a list of lists of int is `is_list` with 1 here.
  std::size_t inner_lists = 0;
  // The record's name, when `base` is `record`.
  string_type record;
  // A typed pointer (`base` is `pointer`): its kind, and what is at the
  // address, as written in Ovis Core: a C number (`i32`, `u8`, `f32`…), an
  // Ovis value's slot (`int` is 64 bits, `str`, `(record book)`, another
  // pointer), or a struct's own memory (`(struct tm)`).
  pointer_kind kind = pointer_kind::raw;
  string_type pointee;
  // A record that may be nothing (`(maybe (record node))`, `a node or
  // nothing`; references.md §3). A record is never nothing otherwise.
  bool nullable = false;
  // A generic record's types (`(record stack int)`): their Core, one after
  // another (`int (record book)`). Empty for other records.
  string_type arguments;

  static constexpr auto nothing() -> value_type { return {}; }
  static constexpr auto integer() -> value_type { return {base_type::integer}; }
  static constexpr auto real() -> value_type { return {base_type::real}; }
  static constexpr auto boolean() -> value_type { return {base_type::boolean}; }
  static constexpr auto string() -> value_type { return {base_type::string}; }
  static constexpr auto pointer() -> value_type { return {base_type::pointer}; }
  static constexpr auto memory() -> value_type { return {base_type::memory}; }
  static constexpr auto unknown() -> value_type { return {base_type::unknown}; }
  static constexpr auto list_of(base_type p_base) -> value_type {
    return {p_base, true};
  }
  static auto list_of(const value_type &p_element) -> value_type {
    value_type result{p_element.base, true,
                      p_element.is_list ? p_element.inner_lists + 1 : 0,
                      p_element.record};
    result.nullable = p_element.nullable;
    result.arguments = p_element.arguments;
    // A C array (`(list u8)`): its items' C type, kept through lists of
    // them (`(list (list u8))`).
    if (p_element.base != base_type::pointer)
      result.pointee = p_element.pointee;
    return result;
  }
  // A list whose items are C numbers (`(list u8)`, an array of bytes); a
  // list of them isn't one, though it keeps their type.
  auto is_c_array() const -> bool {
    return is_list && inner_lists == 0 && !pointee.empty() &&
           base != base_type::map;
  }
  static auto record_of(string_type p_name) -> value_type {
    return {base_type::record, false, 0, std::move(p_name)};
  }
  // A generic record used with these types, `(record stack int)`.
  static auto record_of(string_type p_name,
                        const std::vector<value_type> &p_arguments)
      -> value_type;
  // A generic record's types, as `arguments` holds them.
  auto record_arguments() const -> std::vector<value_type>;
  static auto variable(string_type p_name) -> value_type {
    return {base_type::variable, false, 0, std::move(p_name)};
  }
  auto is_variable() const -> bool {
    return !is_list && base == base_type::variable;
  }
  // Whether a type variable is anywhere in it (`(list (var item))`).
  auto has_variables() const -> bool;
  // The same type, which may be nothing, or which isn't.
  auto or_nothing() const -> value_type {
    auto result = *this;
    result.nullable = true;
    return result;
  }
  auto without_nothing() const -> value_type {
    auto result = *this;
    result.nullable = false;
    return result;
  }
  static auto pointer_to(pointer_kind p_kind, string_type p_pointee)
      -> value_type {
    return {base_type::pointer, false, 0, {}, p_kind, std::move(p_pointee)};
  }
  // Any pointer: C's `void *` or a typed one.
  auto is_pointer() const -> bool {
    return !is_list && base == base_type::pointer;
  }
  auto is_typed_pointer() const -> bool {
    return !is_list && base == base_type::pointer && kind != pointer_kind::raw;
  }
  auto is_phrase() const -> bool {
    return is_pointer() &&
           (kind == pointer_kind::phrase || kind == pointer_kind::c_phrase);
  }

  auto is_numeric() const -> bool {
    return !is_list && (base == base_type::integer || base == base_type::real);
  }
  // Numbers of a size (docs/proposals/numbers.md): a whole number or a
  // decimal whose `pointee` names its C number (`i8`…`u64`, `f32`), as a C
  // array's items do. `int` and `float` (64 bits) have none.
  auto is_whole() const -> bool { return !is_list && base == base_type::integer; }
  auto is_decimal() const -> bool { return !is_list && base == base_type::real; }
  auto is_sized() const -> bool { return is_numeric() && !pointee.empty(); }
  // The C number it is kept as: `long_type` for `int`, `double_type` for
  // `float`.
  auto number_type() const -> std::optional<c_type>;
  // `int` for `long_type`, `float` for `double_type`, else the sized one.
  static auto number(c_type p_type) -> value_type;
  // What a list holds: another list, or the base type. A map keeps its
  // key and value types; a list of numbers of a size (a C array) its
  // items' size.
  auto element() const -> value_type {
    auto result = inner_lists > 0 ? value_type{base, true, inner_lists - 1, record}
                                  : value_type{base, false, 0, record};
    result.nullable = nullable;
    result.arguments = arguments;
    if (base != base_type::pointer)
      result.pointee = pointee;
    return result;
  }
  static auto map_of(const value_type &p_key, const value_type &p_value)
      -> value_type;
  auto is_map() const -> bool { return !is_list && base == base_type::map; }
  // A map's key and value types.
  auto map_key() const -> value_type;
  auto map_value() const -> value_type;
  auto is_record() const -> bool {
    return !is_list && base == base_type::record;
  }
  auto is_known() const -> bool { return base != base_type::unknown; }
  auto to_string() const -> string_type;

  // Accepts the type spellings an LLM is likely to produce: `int`, `float`,
  // `int[]`, `(list int)` and so on. A record type is `(record NAME)`: a bare
  // name would read the same as a variable in `(let x y)`.
  static auto parse(const sexpr &p_form) -> std::optional<value_type>;

  friend auto operator==(const value_type &, const value_type &)
      -> bool = default;
};

struct expression {
  enum class kind_type { integer, real, boolean, string, variable, call };

  kind_type kind = kind_type::integer;
  max_int_type integer = 0;
  max_float_type real = 0;
  // A whole number written above the largest signed 64-bit one: its bits,
  // which only a uint64 holds.
  bool is_big = false;
  bool boolean = false;
  // String contents, variable name or the called operation.
  string_type text;
  std::vector<expression> arguments;
  // A call of a generic function: the types its type variables stand for
  // here, in the order of its `forall` (worked out by the elaborator).
  std::vector<value_type> type_arguments;
  // The element type of `(array TYPE COUNT)`, and of an empty `(list)` once
  // the elaborator has worked it out. The record of `(new NAME ...)`.
  std::optional<value_type> type_argument;
  // The fields named by `(new NAME (FIELD VALUE)...)`, one per argument, and
  // the one of `(field X NAME)`.
  std::vector<string_type> fields;
  source_location location;

  // Filled in by the checker.
  value_type type;
};

struct statement {
  enum class kind_type {
    let,
    set,
    put,
    print,
    if_,
    while_,
    for_,
    each,
    break_,
    continue_,
    return_,
    evaluate,
    pass,
    // `(defer STATEMENT...)`: `body` runs whenever control leaves the block
    // this is in, last deferred first.
    defer,
    // `(set-field X NAME VALUE)`: `values` are X and VALUE, `name` the field.
    set_field,
    // `(with-memory M BODY...)`: `body` is made in the memory M, which is the
    // current one until the block is left in any way. `(with-new-memory
    // BODY...)` has no `values`: the memory is made for the block and freed
    // when it is left.
    with_memory,
    // `(block [:LABEL] BODY...)` (also `(do …)`, `(begin …)`): a block of
    // its own, as Zig's `{ … }`: names declared in it end with it, and its
    // `defer`s run when it is left. `(set NAME (block …))` gives it a value,
    // Zig's `blk: { … break :blk v; }`: `name` is the variable its `leave`
    // sets (`(let NAME (block …))` reads as `(let NAME)` and the `set`).
    block,
    // `(leave [:LABEL] [VALUE])`: leaves the innermost block (or the one
    // labelled), giving it VALUE.
    leave
  };

  kind_type kind = kind_type::pass;
  string_type name;
  std::optional<value_type> declared_type;
  // `(for-down NAME FIRST LAST …)`: a `for_` that counts from FIRST down to
  // LAST.
  bool counts_down = false;
  // A `block`'s or `leave`'s label, without its `:`; empty for the
  // innermost block.
  string_type label;
  std::vector<expression> values;
  std::vector<statement> body;
  std::vector<statement> else_body;
  source_location location;
};

struct parameter {
  string_type name;
  value_type type;
  // Left out and worked out by the elaborator: one type for every call
  // (generics.md §3), which `(x: any thing)` would make many.
  bool is_inferred = false;
};

// A generic's type variable and the traits it must have (its bounds):
// `(forall (item comparable))` (docs/proposals/generics.md §3).
struct type_parameter {
  string_type name;
  std::vector<string_type> bounds;
  // `(forall r (field f))`: a field of a record type variable, given by a
  // call written `NAME:RECORD:FIELD` (a template's field slot); each record
  // and field gets a copy before elaboration (`copy_field_generics`).
  bool is_field = false;
};

struct function {
  string_type name;
  // A generic function's type variables (`(fn NAME (forall …) …)`); it is
  // copied for each use with concrete types (`push__int`) by the checker.
  std::vector<type_parameter> type_parameters;
  // A copy's generic and the types it was copied for, for messages.
  string_type copy_of;
  std::vector<value_type> copied_for;
  std::vector<parameter> parameters;
  value_type return_type;
  std::vector<statement> body;
  source_location location;

  auto is_generic() const -> bool { return !type_parameters.empty(); }
};

// `(record NAME (FIELD TYPE)...)`. A field's type may be left out and is
// inferred from the values given to it.
struct record_field {
  string_type name;
  value_type type;
  // A C record's field (references.md §1): where it is, and how C keeps it.
  // A number of `storage`'s size; `char[N]` text held in the struct
  // (`text_length` N); a struct held in it (`inline_record`: reading it
  // gives the record at that place).
  std::uint64_t offset = 0;
  std::optional<c_type> storage;
  std::uint64_t text_length = 0;
  bool inline_record = false;
};

struct record {
  string_type name;
  // A generic record's type variables (`(record stack (forall item) …)`):
  // each use with concrete types is a record of its own (`stack__int`).
  std::vector<string_type> type_parameters;
  // A copy's generic, for messages.
  string_type copy_of;
  std::vector<record_field> fields;
  source_location location;
  // A C struct imported from a header: C's layout, no hidden tag, so no
  // trait dispatch; made zeroed, fields left out.
  bool is_c = false;
  std::uint64_t size = 0;
  // A C struct's type as C spells it (`struct ovt_box`, `z_stream`).
  string_type c_spelling;
  // `(packed-record NAME ...)` (numbers.md B6): its fields one after
  // another with no gaps, and no hidden tag, so its memory is its fields
  // (a file format's header) and it can't implement a trait.
  bool is_packed = false;

  // Whether it starts with the hidden number that says which record it
  // is, for trait dispatch.
  auto has_tag() const -> bool { return !is_c && !is_packed; }

  auto find(string_view_type p_field) const -> const record_field *;
};

// `(trait NAME (METHOD (PARAM...) [RET])... (fn METHOD ...)...)`: phrases a
// type can provide. A method written as `(METHOD ...)` must be provided by
// every impl; a `(fn ...)` is a default, used where an impl leaves it out.
// A method's first parameter is the value itself (`it`).
struct trait_method {
  string_type name;
  std::vector<parameter> parameters;
  value_type return_type;
  // Whether the trait gives it a body (a function of the same name).
  bool has_default = false;
  // Parameters written `(NAME same)`: the same record as the first, whose
  // impl is called. An impl takes them as its record; a dispatched call
  // checks it when the program runs.
  std::vector<bool> same;

  auto is_same(std::size_t p_index) const -> bool {
    return p_index < same.size() && same[p_index];
  }
};

struct trait {
  string_type name;
  std::vector<trait_method> methods;
  // `(requires OTHER)`: every record of this trait is also OTHER, so a
  // value of it may be used as one.
  std::vector<string_type> requires_traits;
  source_location location;

  auto find(string_view_type p_method) const -> const trait_method *;
};

// `(impl TRAIT TYPE (fn METHOD ...)...)`: a record's methods for a trait.
// The functions are in `unit::functions` as `METHOD__TYPE`; this lists what
// was written.
struct impl {
  string_type trait;
  string_type type;
  // An impl for every use of a generic record: its type variables, with
  // what each must be (`(forall (item showable))`).
  std::vector<type_parameter> type_parameters;
  std::vector<string_type> methods;
  source_location location;
};

// The function that holds `p_method` of `p_type`'s impl.
auto impl_function_name(string_view_type p_method, string_view_type p_type)
    -> string_type;

// `(use-c "header.h" "library")`: a C library whose header is imported.
struct c_import {
  string_type header;
  // The library to link (as in -lNAME) or a pkg-config package; may be empty
  // for header-only use or when the header's name is the package's.
  string_type library;
  source_location location;
};

// Everything one English paragraph translated into.
struct unit {
  // The module (source file) the paragraph is in, e.g. `main` or
  // `pricing.tax`. Empty for single-file programs.
  string_type module;
  std::vector<function> functions;
  std::vector<record> records;
  std::vector<trait> traits;
  std::vector<impl> impls;
  // C functions declared with `(extern ...)`, and those imported from the
  // headers of `c_imports`.
  std::vector<c_function> externs;
  // Libraries requested with `(link "name")`.
  std::vector<string_type> links;
  std::vector<c_import> c_imports;
  // Filled in when `c_imports` are resolved: the headers' constants and the
  // flags needed to link the libraries.
  std::vector<c_constant> constants;
  std::vector<string_type> link_flags;
  // For each line of this unit's Ovis Core (counting from 1), the line of
  // the source file as written that it comes from; empty for units with no
  // source file (the prelude). Checked builds report runtime errors with it.
  std::vector<std::size_t> source_lines;
  // Top-level statements, run in order as part of the program's entry point.
  // Top-level `let`s become globals visible to every function.
  std::vector<statement> statements;
};

struct program {
  std::vector<unit> units;
};

// A constant from an imported C header, or a built-in C constant (`null`,
// `EOF`, ...).
auto find_constant(const program &p_program, string_view_type p_name)
    -> const c_constant *;

auto find_record(const program &p_program, string_view_type p_name)
    -> const record *;

auto find_trait(const program &p_program, string_view_type p_name)
    -> const trait *;

// The type as an author would say it: "a whole number", "a list of text".
auto describe_in_english(const value_type &p_type) -> string_type;
// "whole numbers" for "a whole number", "stacks of text" for "a stack of
// text".
auto plural_in_english(string_type p_text) -> string_type;
// How the copies of generic records are said (`stack__int` is "stack of
// whole numbers"), filled in as they are made.
auto record_descriptions() -> std::map<string_type, string_type> &;
// How a record is printed (`an entry with …`) where messages say it
// otherwise: a package's record, when two versions of its package are in
// the program, is "a log 1.1.0 entry" in messages (packages.md §4).
auto record_print_names() -> std::map<string_type, string_type> &;

// What is at a typed pointer's address (`value_type::pointee`).
struct pointee_type {
  enum class kind_type { number, value, structure };
  kind_type kind = kind_type::value;
  // A C number of its own size (`i32`, `u8`, `f32`).
  c_type number = c_type::int_type;
  // An Ovis value's slot: `int` (64 bits), `float`, `str`, a record, a
  // list, another pointer.
  value_type value;
  // `(struct NAME)`: the record's own memory, for many of them in a row.
  string_type record;
};

// The canonical text of a pointee written in Core (`i32`, `byte`, `int`,
// `(record book)`, `(struct tm)`, `(ptr u8)`), or nothing if it isn't one.
auto parse_pointee(const sexpr &p_form) -> std::optional<string_type>;
auto read_pointee(string_view_type p_text) -> std::optional<pointee_type>;
// What reading the pointee gives: a C number as a whole number or a
// decimal, a struct as its record (a pointer into the memory).
auto pointee_value(const pointee_type &p_pointee) -> value_type;
// How a pointee names what C keeps: `i32`, `u8`, `str`, `ptr`; 64-bit
// numbers are Ovis's `int` and `float`.
auto c_storage_name(c_type p_type) -> string_type;
// An item of a C array of that type: the value it reads as, with its C
// type in `pointee`.
auto c_number_item(c_type p_type) -> value_type;
// A phrase type's signature: `(int str) bool` for a phrase, `(i32 ptr) i32`
// for a C phrase, whose types are C's (`int`/`float` are 64-bit, `void`).
struct phrase_signature {
  std::vector<value_type> parameters;
  value_type result;
  // For a C phrase: each parameter's and the result's C type.
  std::vector<c_type> c_parameters;
  c_type c_result = c_type::void_type;
};
auto read_signature(const value_type &p_phrase)
    -> std::optional<phrase_signature>;
auto phrase_type(const std::vector<value_type> &p_parameters,
                 const value_type &p_result) -> value_type;
auto c_phrase_type(const std::vector<c_type> &p_parameters, c_type p_result)
    -> value_type;
// C's types by the names a C phrase's signature uses.
auto c_type_named(string_view_type p_name) -> std::optional<c_type>;

// Numbers (docs/proposals/numbers.md §3).
// Its size in bits.
auto number_bits(c_type p_type) -> unsigned;
// The type two numbers are worked out in: the same type; mixed sizes the
// smaller type that holds both (`int8` and `int32` an `int32`, `uint32` and
// `int32` an `int` of 64 bits); a decimal if either is (a `float32` with
// whole numbers or `float32`s). Nothing for a `uint64` with a signed whole
// number: no type holds both.
auto common_number(const value_type &p_left, const value_type &p_right)
    -> std::optional<value_type>;
// Whether the whole number with these bits (`p_is_big`: read as unsigned)
// fits in the whole number type.
auto fits_number(max_int_type p_bits, bool p_is_big, const value_type &p_type)
    -> bool;
// The smallest and largest values of a whole number type, as text.
auto number_range(const value_type &p_type) -> string_type;

// The C number type for `i8`…`u64`, `f32`, or nothing for other names.
auto c_number_type(string_view_type p_name) -> std::optional<c_type>;

// Every word the parser gives a meaning as the head of a form (statements,
// blocks, `fn`, `extern`, and the accepted spellings of built-ins).
auto core_keywords() -> std::vector<string_view_type>;

// Turns the S-expressions of one paragraph into a unit. Throws `error`.
// A `(lambda (PARAMETER...) [RESULT] BODY...)` in it becomes a function of
// the unit named `p_lambda_prefix` and a number, given as `(phrase-of
// NAME)`: units that have one need prefixes no other unit has.
auto parse_unit(const std::vector<sexpr> &p_forms,
                string_view_type p_lambda_prefix = "lambda_") -> unit;
auto parse_unit(string_view_type p_text,
                string_view_type p_lambda_prefix = "lambda_") -> unit;

} // namespace ovis::core

#endif // OVIS_CORE_AST_HPP

#ifndef OVIS_PHRASE_TEMPLATES_HPP
#define OVIS_PHRASE_TEMPLATES_HPP

#include <filesystem>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include <ovis/def.hpp>

// Phrase templates: the phrases the front end knows, each with typed slots
// and the Ovis Core it stands for. Built-in ones are in `phrases/*.phr`.
namespace ovis::phrase {

// A slot's or a result's type, as written in a template.
struct type_name {
  enum class kind_type {
    // int, float, num, str, bool, any, or type (the slot takes a type,
    // written in words: "whole number"), size (it takes a type of number:
    // "whole number", "decimal", "int32", "16-bit whole number"; numbers.md),
    // or a trait (`showable`,
    // `comparable`, `equatable`): any type that implements it.
    builtin,
    // An uppercase name such as T: the same type wherever it appears. With
    // a `bound` (`T:comparable`) only types that implement that trait.
    variable,
    // The name of a slot of type `type`: the type the author wrote there.
    // As a result type, also a field slot's name: that field's type.
    type_slot,
    // `field of T`, `num field of T`: the name of a field of the record T
    // (a type variable or a slot of type `type`); `name` is the type the
    // field must have.
    field
  };

  kind_type kind = kind_type::builtin;
  string_type name;
  // For a field: the record's type variable or slot.
  string_type record;
  // For a variable: the trait its type must implement, or empty.
  string_type bound;
  // How many `[]`: 1 is a list of `name`.
  std::size_t list_depth = 0;
  // A generic phrase's parameter of a generic record (`stack of item`):
  // `any` of that record (in `record`) whose types are these. Not written
  // in `.phr` files; the front end sets it.
  std::vector<type_name> arguments;

  auto to_string() const -> string_type;
  friend auto operator==(const type_name &, const type_name &) -> bool = default;
};

struct slot {
  string_type name;
  type_name type;
  // `{items: any, ...}`: one or more values, separated by commas.
  bool is_variadic = false;
};

// One part of a pattern. Words are stored in lower case.
struct pattern_element {
  enum class kind_type {
    word,
    slot,
    // `[the]`: the words in `branches[0]` may be left out.
    optional,
    // `(a|b c)`, or `decimal[s]` (decimal | decimals): one of `branches`.
    choice
  };

  kind_type kind = kind_type::word;
  string_type word;
  // Index into `phrase_template::slots`.
  std::size_t slot = 0;
  std::vector<std::vector<pattern_element>> branches;

  friend auto operator==(const pattern_element &, const pattern_element &)
      -> bool = default;
};

using pattern = std::vector<pattern_element>;

struct phrase_template {
  enum class kind_type { expression, statement };

  kind_type kind = kind_type::expression;
  // The first is the canonical pattern; the others are its `alias:` lines.
  std::vector<pattern> patterns;
  std::vector<slot> slots;
  // What an expression gives back.
  std::optional<type_name> result;
  // A statement ending in `: BLOCK` takes the lines indented under it.
  bool takes_block = false;
  // One Ovis Core form, with `$name` for slots, `$name...` for a variadic
  // slot spliced in, and `BLOCK` for the block.
  string_type core;
  // The Ovis Core used instead when the trait-typed slots (`comparable`,
  // `T:equatable`, ...) hold a record, written on a `for records:` line.
  // In it `$slot` after a `_` or `:` is the record's name, so
  // `comes_before__$a` calls the function the record's author wrote.
  string_type record_core;
  // The manual's words (manual-and-editor.md §2.1): one or two sentences
  // from the `about:` line, and the `example:` lines, each a line that
  // reads to this template.
  string_type about;
  std::vector<string_type> examples;
  // How the manual names it when its pattern is only the words it is found
  // by: a C function's signature (`deflateInit(strm, level) gives …`).
  string_type title;
  // `=> records`: the phrase is for records only, so it has no Ovis Core of
  // its own and `core` is empty.
  bool records_only = false;
  // Where it was written, for errors.
  string_type file;
  std::size_t line = 0;
  // The package whose file defines it (proposals/packages.md §5): seen
  // only where that package is (vocabulary::sees). Empty: Ovis's own, or a
  // phrases/ folder's, seen everywhere.
  string_type package;
};

// Reads templates in the `.phr` format:
//
//   # comment
//   expr  average of {xs: num[]}        : float => (/ (float (sum $xs)) ...)
//         alias: mean of {xs}
//         about: The average of the numbers.
//         example: show mean of [1, 2, 3]
//   stmt  add {x: T} to {xs: T[]}               => (append $xs $x)
//   stmt  keep doing {n: int} times: BLOCK      => (for _ 1 $n BLOCK)
//
// Throws `error` naming `p_file` and the line.
auto read_templates(string_view_type p_text, string_view_type p_file)
    -> std::vector<phrase_template>;

// A pattern as a person reads it, slots as the kind of value that goes
// there and choices as their first way: "add VALUE to LIST".
auto spoken_pattern(const phrase_template &p_template, const pattern &p_pattern)
    -> string_type;

// A pattern written back in the `.phr` format, slots with their types:
// "add {x: T} to {xs: T[]}".
auto to_string(const phrase_template &p_template, const pattern &p_pattern)
    -> string_type;

// The `phrases/*.phr` templates and `phrases/*.core` prelude compiled into
// Ovis: (file name, contents).
auto builtin_phrase_files()
    -> const std::vector<std::pair<string_view_type, string_view_type>> &;

// `type` and `size`: a slot that takes a type written in words.
auto is_type_slot_name(string_view_type p_name) -> bool;

// Whether a type name is a trait slot type: `num` and `any` are built in
// to the front end, the others are what a record can implement.
auto is_trait_name(string_view_type p_name) -> bool;
// Traits a record can implement: `showable`, `comparable`, `equatable`.
auto is_record_trait_name(string_view_type p_name) -> bool;
// A slot typed by one of those, or by a variable bound to one (`T:comparable`).
auto is_trait_slot(const type_name &p_type) -> bool;

// Every template of the `.phr` files of `builtin_phrase_files()`, read once.
auto builtin_templates() -> const std::vector<phrase_template> &;

// The Ovis Core functions of the `.core` files, which templates may call.
auto builtin_prelude() -> string_type;

// A phrase library read when a program is built, from a folder of `.phr`,
// `.core` and `.generic` files like `phrases/` in the source tree: a team's
// own phrases without rebuilding `ovis`. Its templates come after the
// built-in ones (so the built-in wording ranks first among equals).
struct phrase_library {
  std::vector<phrase_template> templates;
  // The Ovis Core of the `.core` files, which the templates may call.
  string_type prelude;
  // The `.generic` functions, by name, as `builtin_generics()`.
  std::map<string_type, string_type> generics;
  // File name → a hash of its contents, for the lockfile.
  std::map<string_type, string_type> hashes;
};

// Reads every phrase file of `p_directory`, in name order. Throws `error`
// with "FILE:LINE: what is wrong" for a template that doesn't read, a
// generic of the wrong shape, or Ovis Core that doesn't parse.
auto load_phrase_library(const std::filesystem::path &p_directory)
    -> phrase_library;

// The generic functions of the `.generic` files, by name: Ovis Core with
// `$T` for a record and `$F` for one of its fields, named `NAME:$T:$F` (or
// `NAME:$T`, when no field matters). A
// template calls one as `(NAME:$f …)`, where `$f` is a `field of T` slot.
auto builtin_generics() -> const std::map<string_type, string_type> &;

} // namespace ovis::phrase

#endif // OVIS_PHRASE_TEMPLATES_HPP

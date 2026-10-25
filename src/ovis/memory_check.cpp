#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <utility>

#include <ovis/core/memory_check.hpp>

// The strict memory rules (docs/proposals/memory.md §2 and §4) as a forward
// pass over each function and the top-level statements. It follows where each
// variable's value may have been made: in the program's memory, or in a
// `with` block's new memory. Values reached through calls are not followed.
namespace ovis::core {

namespace {

auto is_heap(const value_type &p_type) -> bool {
  return p_type.is_list || p_type.base == base_type::string ||
         p_type.is_record() || p_type.is_map();
}

auto noun(const value_type &p_type) -> string_type {
  return p_type.is_list     ? "list"
         : p_type.is_record() ? "record"
         : p_type.is_map()    ? "map"
                              : "text";
}

enum class result_kind { unknown, none, existing, fresh, mixed };

// What an expression gives: nothing to free, a value that exists already
// (read from a variable, a field, an item), or a value made right here.
struct origin {
  enum class kind_type { none, existing, fresh };
  kind_type kind = kind_type::none;
  // `fresh`: the block it is made in; 0 is the program's memory.
  std::size_t block = 0;
  // Read from a variable: its name and what it may hold.
  string_type variable;
  std::set<std::size_t> arena;
  std::map<std::size_t, std::size_t> stale;
  // A list written out, or read from a variable: where its items were made.
  std::set<std::size_t> item_arena;
  std::map<std::size_t, std::size_t> item_stale;
};

struct variable_state {
  bool assigned = false;
  // May hold a value made in the program's memory that nothing has freed or
  // taken.
  bool new_unfreed = false;
  bool holds_fresh = false;
  bool holds_existing = false;
  // The block the variable got its first value in.
  std::size_t birth = 0;
  // Live blocks that made its value, and ended ones with their `with` line.
  std::set<std::size_t> arena;
  std::map<std::size_t, std::size_t> stale;
  // A list's (or record's) items: the blocks they were made in, as far as
  // the program says (items read from it are followed, so a list of lists
  // made in a block is stale with it).
  std::set<std::size_t> item_arena;
  std::map<std::size_t, std::size_t> item_stale;

  friend auto operator==(const variable_state &, const variable_state &)
      -> bool = default;
};

using state = std::map<string_type, variable_state>;

auto join_states(state &p_into, const state &p_other) -> void {
  for (const auto &[name, other] : p_other) {
    auto [found, added] = p_into.try_emplace(name, other);
    if (added)
      continue;
    auto &mine = found->second;
    mine.assigned = mine.assigned || other.assigned;
    mine.new_unfreed = mine.new_unfreed || other.new_unfreed;
    mine.holds_fresh = mine.holds_fresh || other.holds_fresh;
    mine.holds_existing = mine.holds_existing || other.holds_existing;
    mine.arena.insert(other.arena.begin(), other.arena.end());
    mine.stale.insert(other.stale.begin(), other.stale.end());
    mine.item_arena.insert(other.item_arena.begin(), other.item_arena.end());
    mine.item_stale.insert(other.item_stale.begin(), other.item_stale.end());
    if (!mine.assigned)
      mine.birth = other.birth;
  }
}

class analyzer {
public:
  analyzer(const program &p_program, std::size_t p_first_unit)
      : source(p_program), first_unit(p_first_unit) {
    for (const auto &unit : p_program.units)
      for (const auto &function : unit.functions)
        defined.insert(function.name);
  }

  auto run() -> std::vector<diagnostic> {
    // The call graph settles which phrases give back new values: a few
    // passes with nothing reported, then the one that reports.
    for (int round = 0; round < 3; ++round)
      pass(false);
    pass(true);
    return std::move(diagnostics);
  }

private:
  const program &source;
  std::size_t first_unit;
  std::vector<diagnostic> diagnostics;
  std::set<std::pair<std::size_t, string_type>> reported;
  std::map<string_type, result_kind> results;
  std::set<string_type> defined;
  // One block per `with` statement, however often a loop reads it.
  std::map<const statement *, std::size_t> block_ids;
  bool reporting = false;
  bool final_pass = false;

  // The function being analysed.
  state current;
  std::size_t unit_index = 0;
  std::vector<std::size_t> blocks = {0};
  std::map<std::size_t, std::size_t> block_parent;
  std::map<std::size_t, std::size_t> block_line;
  std::set<std::size_t> untracked;
  // `with scratch:` blocks: the memory variable each one makes values in.
  // Its values outlive the block, until `clear scratch` or `free scratch`.
  std::map<std::size_t, string_type> named_blocks;
  // Blocks a value can be stale from (made in a memory that ended), with
  // why, for the message.
  std::map<std::size_t, string_type> ended_named;
  std::size_t next_block = 1;
  bool returns_fresh = false;
  bool returns_existing = false;
  // The bare blocks around: each one's label and the variable it gives a
  // value to (empty if none), innermost last.
  std::vector<std::pair<string_type, string_type>> bare_blocks;
  bool in_loop = false;
  // The blocks being walked, innermost last: each one's `defer`s so far.
  std::vector<std::vector<const statement *>> deferred;

  auto pass(bool p_report) -> void {
    final_pass = p_report;
    state globals;
    for (unit_index = 0; unit_index < source.units.size(); ++unit_index) {
      const auto &unit = source.units[unit_index];
      reporting = p_report && unit_index >= first_unit;
      for (const auto &function : unit.functions)
        analyze_function(function);
      current = std::move(globals);
      blocks = {0};
      for (const auto &statement : unit.statements)
        walk(statement);
      globals = std::move(current);
    }
  }

  auto analyze_function(const function &p_function) -> void {
    current.clear();
    blocks = {0};
    returns_fresh = returns_existing = false;
    for (const auto &parameter : p_function.parameters) {
      auto &variable = current[parameter.name];
      variable.assigned = true;
      variable.holds_existing = is_heap(parameter.type);
    }
    for (const auto &statement : p_function.body)
      walk(statement);
    result_kind kind = result_kind::none;
    if (returns_fresh && returns_existing)
      kind = result_kind::mixed;
    else if (returns_fresh)
      kind = result_kind::fresh;
    else if (returns_existing)
      kind = result_kind::existing;
    results[p_function.name] = kind;
    if (kind == result_kind::mixed)
      report(p_function.location,
             "`" + p_function.name + "` gives back new " +
                 noun(p_function.return_type) +
                 " in one place and existing " + noun(p_function.return_type) +
                 " in another, so a caller couldn't know whether to free the "
                 "result. Give back `keep` of the existing value there, or "
                 "give back no new value.");
  }

  // A Core line as the line of the source file it comes from.
  auto shown_line(std::size_t p_line) const -> std::size_t {
    const auto &lines = source.units[unit_index].source_lines;
    return p_line < lines.size() ? lines[p_line] : p_line;
  }

  auto report(const source_location &p_location, string_type p_message) -> void {
    if (!reporting)
      return;
    if (!reported.insert({unit_index * 1000000 + p_location.line, p_message})
             .second)
      return;
    diagnostics.push_back({unit_index, p_location, std::move(p_message)});
  }

  // Blocks.

  auto current_block() const -> std::size_t { return blocks.back(); }

  // Whether `p_inner` is `p_outer` or made inside it.
  auto is_inside(std::size_t p_inner, std::size_t p_outer) const -> bool {
    if (p_outer == 0)
      return true;
    for (auto block = p_inner; block != 0;) {
      if (block == p_outer)
        return true;
      const auto found = block_parent.find(block);
      block = found == block_parent.end() ? 0 : found->second;
    }
    return false;
  }

  // Expressions.

  enum class place { stored, inside };

  auto read_variable(const expression &p_expression) -> origin {
    origin result;
    result.kind = is_heap(p_expression.type) ? origin::kind_type::existing
                                             : origin::kind_type::none;
    result.variable = p_expression.text;
    const auto found = current.find(p_expression.text);
    if (found == current.end())
      return result;
    const auto &variable = found->second;
    result.arena = variable.arena;
    result.stale = variable.stale;
    result.item_arena = variable.item_arena;
    result.item_stale = variable.item_stale;
    for (const auto &[block, line] : variable.stale)
      report_stale(p_expression, "`" + p_expression.text + "`", block, line);
    return result;
  }

  auto report_stale(const expression &p_expression, const string_type &p_what,
                    std::size_t p_block, std::size_t p_line) -> void {
    if (const auto named = named_blocks.find(p_block); named != named_blocks.end())
      report(p_expression.location,
             p_what + " was made in `" + named->second + "`, which was " +
                 ended_named[p_block] + " on line " + std::to_string(p_line) +
                 ". Keep a copy (`keep`) outside it if it is needed after.");
    else
      report(p_expression.location,
             p_what + " was made in the new memory of line " +
                 std::to_string(p_line) +
                 ", which was freed when that block ended. Keep a copy "
                 "(`keep`) if it is needed after it.");
  }

  // A block whose values may still be read after it ends: one of a named
  // memory.
  auto lasts(std::size_t p_block) const -> bool {
    return named_blocks.contains(p_block);
  }

  // A value made here where nothing can free it.
  auto misplaced(const expression &p_expression, const char *p_where) -> void {
    report(p_expression.location,
           "This makes new " + noun(p_expression.type) + " " + p_where +
               ", where nothing can free it. Store it in a variable first, "
               "then use that.");
  }

  auto fresh_origin(std::size_t p_block) -> origin {
    origin result;
    result.kind = origin::kind_type::fresh;
    result.block = p_block;
    return result;
  }

  // The parts of a join, nested joins flattened.
  static auto is_join(const expression &p_expression) -> bool {
    return p_expression.kind == expression::kind_type::call &&
           ((p_expression.text == "+" &&
             p_expression.type == value_type::string()) ||
            p_expression.text == "concat");
  }

  auto eval_join_parts(const expression &p_expression) -> void {
    for (const auto &argument : p_expression.arguments) {
      if (is_join(argument)) {
        eval_join_parts(argument);
        continue;
      }
      // A number or truth value as text is written into the join.
      if (argument.kind == expression::kind_type::call &&
          argument.text == "to_str" && argument.arguments.size() == 1 &&
          !is_heap(argument.arguments[0].type)) {
        eval(argument.arguments[0], place::inside, "as a part of a join");
        continue;
      }
      const auto part = eval(argument, place::inside, "as a part of a join");
      (void)part;
    }
  }

  auto call_result(const expression &p_call) -> origin {
    const auto &name = p_call.text;
    const auto count = p_call.arguments.size();
    const auto heap = is_heap(p_call.type);
    // Every argument is a place nothing new may be made in.
    const auto arguments = [&](const char *p_where) {
      for (const auto &argument : p_call.arguments)
        eval(argument, place::inside, p_where);
    };
    if (is_join(p_call)) {
      eval_join_parts(p_call);
      return fresh_origin(current_block());
    }
    if (name == "to_str") {
      if (count == 1 && p_call.arguments[0].type == value_type::string())
        return eval(p_call.arguments[0], place::inside, "as an argument");
      arguments("as an argument");
      return fresh_origin(current_block());
    }
    if (name == "list" || name == "new") {
      // Items (fields) are existing values: the list holds what they were
      // made in.
      auto result = fresh_origin(current_block());
      for (const auto &argument : p_call.arguments) {
        const auto item = eval(argument, place::inside,
                               "in the making of another value");
        result.item_arena.insert(item.arena.begin(), item.arena.end());
      }
      return result;
    }
    if (name == "substring" || name == "chr" || name == "buffer" ||
        name == "read_line" || name == "array" || name == "map-new" ||
        name == "map-keys") {
      arguments("in the making of another value");
      return fresh_origin(current_block());
    }
    if (name == "copy") {
      if (count == 1)
        eval(p_call.arguments[0], place::inside, "as what `a copy of` copies");
      return fresh_origin(current_block());
    }
    if (name == "keep") {
      if (count == 1)
        eval(p_call.arguments[0], place::inside, "as what `keep` copies");
      const auto found = block_parent.find(current_block());
      return fresh_origin(found == block_parent.end() ? 0 : found->second);
    }
    if (name == "get" || name == "field" || name == "map-get") {
      origin result;
      result.kind = heap ? origin::kind_type::existing : origin::kind_type::none;
      for (std::size_t index = 0; index < count; ++index) {
        const auto container =
            eval(p_call.arguments[index], place::inside, "as an argument");
        // An item of a list (a field of a record) whose items were made in
        // a memory: it lives there.
        if (index == 0 && heap) {
          result.arena = container.item_arena;
          result.stale = container.item_stale;
          for (const auto &[block, line] : container.item_stale)
            report_stale(p_call, "This item", block, line);
        }
      }
      return result;
    }
    // A phrase the program or the prelude defines: it knows what it gives back.
    const auto found = results.find(name);
    if (defined.contains(name)) {
      for (const auto &argument : p_call.arguments) {
        eval(argument, place::inside, "as an argument");
        // A phrase may keep what it is given: the caller is no longer the
        // one to free it.
        if (argument.kind == expression::kind_type::variable)
          if (auto variable = current.find(argument.text); variable != current.end())
            variable->second.new_unfreed = false;
      }
      origin result;
      if (!heap)
        return result;
      if (found != results.end() &&
          (found->second == result_kind::fresh || found->second == result_kind::mixed))
        return fresh_origin(current_block());
      result.kind = origin::kind_type::existing;
      return result;
    }
    arguments("as an argument");
    origin result;
    result.kind = heap ? origin::kind_type::existing : origin::kind_type::none;
    return result;
  }

  auto eval(const expression &p_expression, place p_place, const char *p_where)
      -> origin {
    switch (p_expression.kind) {
    case expression::kind_type::integer:
    case expression::kind_type::real:
    case expression::kind_type::boolean:
      return {};
    case expression::kind_type::string: {
      origin result;
      result.kind = origin::kind_type::existing;
      return result;
    }
    case expression::kind_type::variable:
      return read_variable(p_expression);
    case expression::kind_type::call:
      break;
    }
    auto result = call_result(p_expression);
    if (result.kind == origin::kind_type::fresh && p_place == place::inside)
      misplaced(p_expression, p_where);
    return result;
  }

  // Statements.

  // A value is put where it outlives the blocks it was made in? Reports it.
  auto check_outlives(const origin &p_value, const string_type &p_target,
                      const source_location &p_location) -> void {
    std::set<std::size_t> made_in;
    for (const auto block : p_value.arena)
      if (!lasts(block))
        made_in.insert(block);
    if (p_value.kind == origin::kind_type::fresh && p_value.block != 0 &&
        !untracked.contains(p_value.block))
      made_in.insert(p_value.block);
    if (made_in.empty())
      return;
    const auto found = current.find(p_target);
    const auto birth = found == current.end() ? 0 : found->second.birth;
    for (const auto block : made_in)
      if (!is_inside(birth, block)) {
        report(p_location,
               "This value is made in the new memory of line " +
                   std::to_string(block_line[block]) +
                   ", which is freed when that block ends; it can't be stored "
                   "in `" + p_target + "`, which is from outside it. Use `keep` "
                   "to store a copy.");
        return;
      }
  }

  auto assign(const string_type &p_name, const origin &p_value,
              const value_type &p_type, const source_location &p_location)
      -> void {
    auto &variable = current[p_name];
    const bool first = !variable.assigned;
    if (first)
      variable.birth = current_block();
    const bool heap = is_heap(p_type);
    if (p_value.kind == origin::kind_type::fresh) {
      if (current_block() == 0 && variable.new_unfreed)
        report(p_location,
               "`" + p_name + "` gets new " + noun(p_type) + " here" +
                   (in_loop ? ", on every pass of the loop," : "") +
                   " and its old " + noun(p_type) +
                   " is never freed. Free it first (`free " + p_name +
                   "`), or make it in a new memory (`with new memory:`).");
      check_outlives(p_value, p_name, p_location);
      variable.new_unfreed = p_value.block == 0;
      variable.arena.clear();
      variable.stale.clear();
      if (p_value.block != 0 &&
          (!untracked.contains(p_value.block) || lasts(p_value.block)))
        variable.arena.insert(p_value.block);
      variable.item_arena = p_value.item_arena;
      variable.item_stale = p_value.item_stale;
      variable.holds_fresh = true;
    } else {
      check_outlives(p_value, p_name, p_location);
      // Taking another variable's value takes over freeing it.
      bool moved = false;
      if (!p_value.variable.empty() && p_value.variable != p_name)
        if (auto source_variable = current.find(p_value.variable);
            source_variable != current.end() && source_variable->second.new_unfreed) {
          source_variable->second.new_unfreed = false;
          moved = true;
        }
      variable.new_unfreed = moved;
      variable.arena = p_value.arena;
      variable.stale = p_value.stale;
      variable.item_arena = p_value.item_arena;
      variable.item_stale = p_value.item_stale;
      if (heap && p_value.kind == origin::kind_type::existing) {
        variable.holds_existing = true;
        // A variable read: what it held counts as the history too.
        if (const auto source_variable = current.find(p_value.variable);
            source_variable != current.end() && source_variable->second.holds_fresh &&
            !source_variable->second.holds_existing)
          variable.holds_fresh = true;
      }
    }
    variable.assigned = true;
  }

  // Putting a value in a list, or a field of a record: the value now lives
  // there, and it may not outlive its block.
  auto store_into(const string_type &p_target, const origin &p_value,
                  const source_location &p_location) -> void {
    check_outlives(p_value, p_target, p_location);
    if (!p_value.variable.empty())
      if (auto variable = current.find(p_value.variable); variable != current.end())
        variable->second.new_unfreed = false;
    if (auto target = current.find(p_target); target != current.end()) {
      auto &items = target->second.item_arena;
      items.insert(p_value.arena.begin(), p_value.arena.end());
      if (p_value.kind == origin::kind_type::fresh && p_value.block != 0 &&
          (!untracked.contains(p_value.block) || lasts(p_value.block)))
        items.insert(p_value.block);
    }
  }

  // Values made in `p_block` are gone: reading them is reported.
  auto end_block(std::size_t p_block, std::size_t p_line) -> void {
    for (auto &[name, variable] : current) {
      if (variable.arena.erase(p_block) > 0)
        variable.stale[p_block] = p_line;
      if (variable.item_arena.erase(p_block) > 0)
        variable.item_stale[p_block] = p_line;
    }
  }

  // `clear scratch`, `free scratch`: what its `with` blocks made is gone.
  auto end_named(const string_type &p_memory, const string_type &p_how,
                 std::size_t p_line) -> void {
    for (const auto &[block, name] : named_blocks)
      if (name == p_memory) {
        ended_named[block] = p_how;
        end_block(block, p_line);
      }
  }

  // A block's `at the end` lines run when it is left: in a loop's body,
  // at the end of every pass, so `at the end, free xs` frees the list
  // before the next pass gives `xs` a new one. They are walked there.
  auto walk_block(const std::vector<statement> &p_body) -> void {
    deferred.emplace_back();
    for (const auto &statement : p_body)
      walk(statement);
    auto ending = std::move(deferred.back());
    deferred.pop_back();
    for (auto at = ending.rbegin(); at != ending.rend(); ++at)
      walk_block((*at)->body);
  }

  auto loop(const std::function<void()> &p_body) -> void {
    const auto outer = std::exchange(in_loop, true);
    // A second pass sees what the first left behind; each report is made once.
    for (int round = 0; round < 3; ++round) {
      const auto before = current;
      p_body();
      join_states(current, before);
      if (current == before)
        break;
    }
    in_loop = outer;
  }

  auto walk(const statement &p_statement) -> void {
    using kind = statement::kind_type;
    const auto &values = p_statement.values;
    switch (p_statement.kind) {
    case kind::let:
      if (values.empty()) {
        current[p_statement.name];
        break;
      }
      [[fallthrough]];
    case kind::set: {
      const auto value = eval(values[0], place::stored, "");
      assign(p_statement.name, value, values[0].type, p_statement.location);
      break;
    }
    case kind::put: {
      eval(values[0], place::inside, "as an index");
      const auto value = eval(values[1], place::stored, "");
      store_into(p_statement.name, value, p_statement.location);
      break;
    }
    case kind::print:
      for (const auto &value : values)
        eval(value, place::inside, "inside `show`");
      break;
    case kind::if_: {
      eval(values[0], place::inside, "in a condition");
      const auto before = current;
      walk_block(p_statement.body);
      auto after_then = std::move(current);
      current = before;
      walk_block(p_statement.else_body);
      join_states(current, after_then);
      break;
    }
    case kind::while_:
      loop([&] {
        eval(values[0], place::inside, "in a condition");
        walk_block(p_statement.body);
      });
      break;
    case kind::for_:
      eval(values[0], place::inside, "as a bound");
      eval(values[1], place::inside, "as a bound");
      current[p_statement.name].assigned = true;
      loop([&] { walk_block(p_statement.body); });
      break;
    case kind::each: {
      eval(values[0], place::inside, "as what to go through");
      auto &variable = current[p_statement.name];
      variable.assigned = true;
      variable.birth = current_block();
      variable.holds_existing = true;
      loop([&] { walk_block(p_statement.body); });
      break;
    }
    case kind::return_: {
      if (values.empty())
        break;
      const auto value = eval(values[0], place::stored, "");
      if (!is_heap(values[0].type))
        break;
      // What leaves the function lives on: not in a block's memory.
      bool in_block = value.kind == origin::kind_type::fresh && value.block != 0 &&
                      !untracked.contains(value.block);
      if (std::ranges::any_of(value.arena,
                              [&](std::size_t p_block) { return !lasts(p_block); }) ||
          in_block)
        report(p_statement.location,
               "This value is made in a new memory that is freed before the "
               "function gives it back. Give back `keep` of it instead.");
      if (value.kind == origin::kind_type::fresh) {
        returns_fresh = true;
      } else if (const auto found = current.find(value.variable);
                 found != current.end() && found->second.holds_fresh &&
                 !found->second.holds_existing) {
        returns_fresh = true;
        found->second.new_unfreed = false;
      } else if (found != current.end() && found->second.holds_fresh &&
                 found->second.holds_existing) {
        returns_fresh = returns_existing = true;
      } else {
        returns_existing = true;
      }
      break;
    }
    case kind::evaluate: {
      const auto &call = values[0];
      if (call.kind == expression::kind_type::call && call.text == "append" &&
          call.arguments.size() == 2 &&
          call.arguments[0].kind == expression::kind_type::variable) {
        read_variable(call.arguments[0]);
        const auto value = eval(call.arguments[1], place::stored, "");
        store_into(call.arguments[0].text, value, p_statement.location);
        break;
      }
      // A map keeps its key and value.
      if (call.kind == expression::kind_type::call && call.text == "map-put" &&
          call.arguments.size() == 3 &&
          call.arguments[0].kind == expression::kind_type::variable) {
        read_variable(call.arguments[0]);
        const auto key = eval(call.arguments[1], place::stored, "");
        store_into(call.arguments[0].text, key, p_statement.location);
        const auto value = eval(call.arguments[2], place::stored, "");
        store_into(call.arguments[0].text, value, p_statement.location);
        break;
      }
      if (call.kind == expression::kind_type::call &&
          (call.text == "free" || call.text == "clear-memory") &&
          call.arguments.size() == 1 &&
          call.arguments[0].kind == expression::kind_type::variable &&
          call.arguments[0].type == value_type::memory()) {
        end_named(call.arguments[0].text,
                  call.text == "free" ? "freed" : "cleared",
                  shown_line(p_statement.location.line));
        break;
      }
      if (call.kind == expression::kind_type::call && call.text == "free" &&
          call.arguments.size() == 1 &&
          call.arguments[0].kind == expression::kind_type::variable) {
        read_variable(call.arguments[0]);
        if (auto variable = current.find(call.arguments[0].text);
            variable != current.end())
          variable->second.new_unfreed = false;
        break;
      }
      const auto value = eval(call, place::stored, "");
      if (value.kind == origin::kind_type::fresh)
        report(p_statement.location,
               "This makes new " + noun(call.type) +
                   " and drops it, so nothing can free it. Store it in a "
                   "variable first.");
      break;
    }
    case kind::set_field: {
      const auto object = eval(values[0], place::inside, "as a record");
      const auto value = eval(values[1], place::stored, "");
      store_into(object.variable.empty() ? string_type() : object.variable, value,
                 p_statement.location);
      break;
    }
    case kind::defer: {
      if (!deferred.empty()) {
        deferred.back().push_back(&p_statement);
        break;
      }
      // In a function's body or at the top level it runs when the function
      // or program ends, after lines not walked yet: it may not have run.
      const auto before = current;
      walk_block(p_statement.body);
      join_states(current, before);
      break;
    }
    case kind::with_memory: {
      if (!values.empty())
        eval(values[0], place::inside, "as a memory");
      const auto [id, added] = block_ids.try_emplace(&p_statement, next_block);
      if (added)
        ++next_block;
      const auto block = id->second;
      block_parent[block] = current_block();
      block_line[block] = shown_line(p_statement.location.line);
      // A memory the program names lives on after the block: its values
      // are stale once it is cleared or freed (`end_named`). One given some
      // other way (a field, a call) isn't followed.
      if (!values.empty()) {
        untracked.insert(block);
        if (values[0].kind == expression::kind_type::variable)
          named_blocks[block] = values[0].text;
      }
      blocks.push_back(block);
      walk_block(p_statement.body);
      blocks.pop_back();
      // What the block made is gone.
      if (!lasts(block))
        end_block(block, shown_line(p_statement.location.line));
      break;
    }
    case kind::block:
      bare_blocks.emplace_back(p_statement.label, p_statement.name);
      walk_block(p_statement.body);
      bare_blocks.pop_back();
      break;
    case kind::leave: {
      if (values.empty())
        break;
      const auto value = eval(values[0], place::stored, "");
      const auto target = std::find_if(
          bare_blocks.rbegin(), bare_blocks.rend(), [&](const auto &p_block) {
            return p_statement.label.empty() || p_block.first == p_statement.label;
          });
      if (target != bare_blocks.rend() && !target->second.empty())
        assign(target->second, value, values[0].type, p_statement.location);
      break;
    }
    case kind::break_:
    case kind::continue_:
    case kind::pass:
      break;
    }
  }
};

} // namespace

auto check_memory(const program &p_program, std::size_t p_first_unit)
    -> std::vector<diagnostic> {
  return analyzer(p_program, p_first_unit).run();
}

} // namespace ovis::core

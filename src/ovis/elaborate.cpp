#include <algorithm>
#include <cctype>
#include <deque>
#include <functional>
#include <map>
#include <set>

#include <ovis/core/c_import.hpp>
#include <ovis/core/elaborate.hpp>

namespace ovis::core {

namespace {

auto loose(string_view_type p_name) -> string_type {
  string_type result;
  for (const auto character : p_name)
    if (character != '_' && character != '-')
      result.push_back(
          static_cast<char_type>(std::tolower(static_cast<unsigned char>(character))));
  return result;
}

auto edit_distance(string_view_type p_left, string_view_type p_right)
    -> std::size_t {
  std::vector<std::size_t> row(p_right.size() + 1);
  for (std::size_t column = 0; column < row.size(); ++column)
    row[column] = column;
  for (std::size_t line = 1; line <= p_left.size(); ++line) {
    auto diagonal = row[0];
    row[0] = line;
    for (std::size_t column = 1; column <= p_right.size(); ++column) {
      const auto above = row[column];
      row[column] = std::min(
          {row[column] + 1, row[column - 1] + 1,
           diagonal + (p_left[line - 1] == p_right[column - 1] ? 0 : 1)});
      diagonal = above;
    }
  }
  return row.back();
}

// Type inference.
//
// Every variable, parameter, return value and expression gets a slot. Slots
// that must hold the same type are merged (union-find); values flowing into a
// slot widen it (int -> float). Pinned slots have a stated type and never
// change; the checker reports anything that doesn't fit them.
class inference {
public:
  enum class kind {
    none,
    integer,
    real,
    boolean,
    string,
    pointer,
    memory,
    nothing,
    list,
    record,
    map,
    // A generic's type variable, by name (`record`): rigid, it is only
    // itself (docs/proposals/generics.md §6).
    variable
  };

  using slot_id = std::size_t;

private:
  struct slot {
    slot_id parent;
    kind value = kind::none;
    slot_id element = 0; // for lists, and a map's values
    slot_id key = 0;     // for a map's keys
    string_type record;  // for records, and a type variable's name
    // A generic record's types (`(record stack int)`), in its order.
    std::vector<slot_id> arguments;
    bool nullable = false; // a record that may be nothing
    bool pinned = false;
    // For a typed pointer: its kind and what it points at. For a number of
    // a size, its C number (`i32`, `f32`; numbers.md).
    pointer_kind pointer = pointer_kind::raw;
    string_type pointee;
    // Numbers written in the program, and what is worked out from only
    // them: they take the size of what they meet (`b + 1` is a byte). A
    // firm slot (a variable, a field, a list's items) that gets one holds
    // a whole number or a decimal of 64 bits, so `count = 0` doesn't
    // become a byte later.
    bool soft = false;
    bool firm = true;
  };
  // `(ref X)`: the pointer's slot, and X's, whose type it points at.
  std::vector<std::pair<slot_id, slot_id>> references;
  // `(phrase-of NAME)`: the value's slot, and the phrase's parameters' and
  // result's, whose types make its type.
  struct phrase_value {
    slot_id slot;
    std::vector<slot_id> parameters;
    slot_id result;
  };
  std::vector<phrase_value> phrase_values;
  // `(call F ARG...)`: the result follows F's type once it is known.
  struct phrase_call {
    slot_id result;
    slot_id phrase;
    std::vector<slot_id> arguments;
    bool is_done = false;
  };
  std::vector<phrase_call> phrase_calls;
  // `(map-put m k v)`, `(map-get m k)`, `(map-keys m)`…: the key, value and
  // result join the map's types once it is known to be a map.
  struct map_use {
    slot_id map;
    std::optional<slot_id> key;
    std::optional<slot_id> value;
    std::optional<slot_id> result;
    bool is_keys = false;
    bool is_done = false;
  };
  std::vector<map_use> map_uses;
  // `for each` over a value not known yet: the collection and the item.
  std::vector<std::pair<slot_id, slot_id>> items;
  // Waits on types not known yet (a C macro's arguments): asked again while
  // solving, each says whether it changed anything.
  std::vector<std::function<bool()>> waits;
  // `(field X NAME)` and `(set-field X NAME ...)`: the field's slot is only
  // known once X's record is.
  struct field_access {
    slot_id object;
    string_type field;
    slot_id result;
    bool is_done = false;
  };

  std::vector<slot> slots;
  std::vector<std::pair<slot_id, slot_id>> flows;
  // Applied only if the first slot ends up unconstrained.
  std::vector<std::pair<slot_id, slot_id>> fallbacks;
  std::vector<field_access> accesses;
  // record → field → slot.
  std::map<string_type, std::map<string_type, slot_id>> record_fields;

  static auto kind_of(base_type p_base) -> kind {
    switch (p_base) {
    case base_type::nothing:
      return kind::nothing;
    case base_type::integer:
      return kind::integer;
    case base_type::real:
      return kind::real;
    case base_type::boolean:
      return kind::boolean;
    case base_type::string:
      return kind::string;
    case base_type::pointer:
      return kind::pointer;
    case base_type::memory:
      return kind::memory;
    case base_type::unknown:
      return kind::none;
    case base_type::record:
      return kind::record;
    case base_type::map:
      return kind::map;
    case base_type::variable:
      return kind::variable;
    }
    return kind::none;
  }

  // Returns whether anything changed.
  auto widen(slot_id p_target, const slot &p_source) -> bool {
    auto &target = slots[find(p_target)];
    if (p_source.value == kind::none)
      return false;
    if (p_source.value == kind::map) {
      if (target.value == kind::none && !target.pinned) {
        target.value = kind::map;
        target.key = p_source.key;
        target.element = p_source.element;
        return true;
      }
      if (target.value == kind::map) {
        const auto key = target.key;
        const auto element = target.element;
        const bool changed = unify(key, p_source.key);
        return unify(element, p_source.element) || changed;
      }
      return false;
    }
    if (p_source.value == kind::list) {
      if (target.value == kind::none && !target.pinned) {
        target.value = kind::list;
        target.element = p_source.element;
        target.pointee = p_source.pointee;
        return true;
      }
      if (target.value == kind::list) {
        // A C array's item type, found after the list was (`len` first).
        bool changed = false;
        if (target.pointee.empty() && !p_source.pointee.empty() &&
            !target.pinned) {
          target.pointee = p_source.pointee;
          changed = true;
        }
        return unify(target.element, p_source.element) || changed;
      }
      return false;
    }
    // A generic record's uses join their types.
    if (p_source.value == kind::record && target.value == kind::record &&
        target.record == p_source.record &&
        target.arguments != p_source.arguments)
      return join_arguments(p_target, p_source.arguments);
    // Two records that are one trait (`[c1, a square …]`): the trait.
    if (!target.pinned && p_source.value == kind::record &&
        target.value == kind::record && target.record != p_source.record) {
      if (implements(p_source.record, target.record))
        return false;
      for (const auto &[trait, record] : trait_impls)
        if (record == target.record && implements(p_source.record, trait)) {
          target.record = trait;
          return true;
        }
    }
    if (!target.pinned && p_source.value == kind::pointer &&
        target.value == kind::pointer && target.pointee.empty() &&
        !p_source.pointee.empty()) {
      target.pointer = p_source.pointer;
      target.pointee = p_source.pointee;
      return true;
    }
    // A record that may be nothing makes the slot one that may be.
    if (!target.pinned && p_source.value == kind::record &&
        target.value == kind::record && p_source.nullable && !target.nullable &&
        (target.record == p_source.record)) {
      target.nullable = true;
      return true;
    }
    if (is_number(target.value) && is_number(p_source.value))
      return widen_number(target, p_source);
    if (target.value == kind::none && is_number(p_source.value) &&
        !target.pinned) {
      target.value = p_source.value;
      target.soft = p_source.soft && !target.firm;
      target.pointee = target.soft && target.firm ? "" : p_source.pointee;
      if (p_source.soft && target.firm)
        target.pointee.clear();
      return true;
    }
    if (target.pinned || target.value == p_source.value)
      return false;
    if (p_source.value == kind::pointer) {
      if (target.value != kind::none)
        return false;
      target.value = kind::pointer;
      target.pointer = p_source.pointer;
      target.pointee = p_source.pointee;
      return true;
    }
    if (p_source.value == kind::record) {
      if (target.value != kind::none)
        return false; // A different type: the checker explains.
      target.value = kind::record;
      target.record = p_source.record;
      target.nullable = p_source.nullable;
      target.arguments = p_source.arguments;
      return true;
    }
    if (p_source.value == kind::variable) {
      // A number meeting a type variable's values takes its type, as a
      // number written in the program takes the size of what it meets
      // (`sum = 0`, then `sum + value of n`); the checker asks that the
      // variable is a number.
      if (is_number(target.value) && !target.pinned) {
        target.value = kind::variable;
        target.record = p_source.record;
        target.pointee.clear();
        target.soft = false;
        return true;
      }
      if (target.value != kind::none)
        return false; // A different type: the checker explains.
      target.value = kind::variable;
      target.record = p_source.record;
      return true;
    }
    if (target.value == kind::none ||
        (target.value == kind::integer && p_source.value == kind::real) ||
        // `+` with text joins text.
        (p_source.value == kind::string &&
         (target.value == kind::integer || target.value == kind::real))) {
      target.value = p_source.value;
      return true;
    }
    return false;
  }

  // A generic record's types meeting another use's: each pair is one type;
  // a use not saying them (`(new stack …)`) takes the other's.
  auto join_arguments(slot_id p_target, const std::vector<slot_id> &p_arguments)
      -> bool {
    auto &target = slots[find(p_target)];
    if (p_arguments.empty())
      return false;
    if (target.arguments.empty()) {
      target.arguments = p_arguments;
      return true;
    }
    const auto own = target.arguments;
    bool changed = false;
    for (std::size_t index = 0; index < std::min(own.size(), p_arguments.size());
         ++index)
      changed = unify(own[index], p_arguments[index]) || changed;
    return changed;
  }

  static auto is_number(kind p_kind) -> bool {
    return p_kind == kind::integer || p_kind == kind::real;
  }

  static auto number_of(const slot &p_slot) -> value_type {
    auto result = p_slot.value == kind::integer ? value_type::integer()
                                                : value_type::real();
    result.pointee = p_slot.pointee;
    return result;
  }

  // Numbers meeting (numbers.md §3): mixed sizes widen to their common
  // type; a number written in the program takes the other's size.
  auto widen_number(slot &p_target, const slot &p_source) -> bool {
    if (p_target.pinned)
      return false;
    const auto take = [&](kind p_kind, string_type p_pointee) {
      if (p_target.value == p_kind && p_target.pointee == p_pointee &&
          !p_target.soft)
        return false;
      p_target.value = p_kind;
      p_target.pointee = std::move(p_pointee);
      p_target.soft = false;
      return true;
    };
    if (p_source.soft) {
      // `b + 1.5`: a decimal.
      if (p_source.value == kind::real && p_target.value == kind::integer)
        return take(kind::real, "");
      return false;
    }
    if (p_target.soft)
      return p_target.value == kind::real && p_source.value == kind::integer
                 ? take(kind::real, "")
                 : take(p_source.value, p_source.pointee);
    const auto common =
        common_number(number_of(p_target), number_of(p_source));
    if (!common)
      return false; // a uint64 and a signed number: the checker explains.
    return take(common->is_whole() ? kind::integer : kind::real,
                common->pointee);
  }

  // (trait, record) for every impl, so records sharing a trait can meet.
  std::set<std::pair<string_type, string_type>> trait_impls;

  auto implements(const string_type &p_record, const string_type &p_trait) const
      -> bool {
    return p_record == p_trait || trait_impls.contains({p_trait, p_record});
  }

public:
  auto set_trait_impls(std::set<std::pair<string_type, string_type>> p_impls)
      -> void {
    trait_impls = std::move(p_impls);
  }

  struct trait_hint {
    slot_id slot;
    string_type trait;
    source_location location;
    std::size_t unit_index;
  };
  // Slots used as the value of a trait's phrase: if nothing else says what
  // they are, they hold that trait.
  std::vector<trait_hint> hints;

  auto find(slot_id p_slot) -> slot_id {
    while (slots[p_slot].parent != p_slot) {
      slots[p_slot].parent = slots[slots[p_slot].parent].parent;
      p_slot = slots[p_slot].parent;
    }
    return p_slot;
  }

  auto fresh() -> slot_id {
    slots.push_back({slots.size()});
    return slots.size() - 1;
  }

  // What arithmetic works out: a number written in the program stays one
  // through it (`2 * 3`).
  auto fresh_number() -> slot_id {
    const auto result = fresh();
    slots[result].firm = false;
    return result;
  }

  auto literal(const value_type &p_type) -> slot_id {
    const auto result = fresh_number();
    slots[result].value = p_type.is_whole() ? kind::integer : kind::real;
    slots[result].pointee = p_type.pointee;
    slots[result].soft = !p_type.is_sized();
    return result;
  }

  auto of(const value_type &p_type, bool p_pinned) -> slot_id {
    const auto result = fresh();
    if (p_type.is_map()) {
      // A key of a size stays it: keys are compared at their size.
      const auto key =
          of(p_type.map_key(), p_pinned || p_type.map_key().is_sized());
      const auto element = of(p_type.map_value(), p_pinned);
      slots[result].value = kind::map;
      slots[result].key = key;
      slots[result].element = element;
      slots[result].pinned = p_pinned;
      return result;
    }
    if (p_type.is_list) {
      const auto element = of(p_type.element(), p_pinned);
      slots[result].value = kind::list;
      slots[result].element = element;
      slots[result].pointee = p_type.pointee;
      slots[result].pinned = p_pinned;
      return result;
    }
    slots[result].value = kind_of(p_type.base);
    slots[result].record = p_type.record;
    if (p_type.is_record())
      for (const auto &argument : p_type.record_arguments()) {
        const auto slot = of(argument, p_pinned);
        slots[result].arguments.push_back(slot);
      }
    slots[result].nullable = p_type.nullable;
    slots[result].pointer = p_type.kind;
    slots[result].pointee = p_type.pointee;
    slots[result].pinned = p_pinned && p_type.is_known();
    return result;
  }

  auto reference(slot_id p_pointer, slot_id p_target) -> void {
    references.emplace_back(p_pointer, p_target);
  }
  auto use_map(slot_id p_map, std::optional<slot_id> p_key,
               std::optional<slot_id> p_value, std::optional<slot_id> p_result,
               bool p_is_keys) -> void {
    map_uses.push_back({p_map, p_key, p_value, p_result, p_is_keys});
  }
  auto phrase_of(slot_id p_slot, std::vector<slot_id> p_parameters,
                 slot_id p_result) -> void {
    phrase_values.push_back({p_slot, std::move(p_parameters), p_result});
  }
  auto wait(std::function<bool()> p_wait) -> void {
    waits.push_back(std::move(p_wait));
  }
  auto call_phrase(slot_id p_result, slot_id p_phrase,
                   std::vector<slot_id> p_arguments) -> void {
    phrase_calls.push_back({p_result, p_phrase, std::move(p_arguments)});
  }

  auto add_record_field(const string_type &p_record, const string_type &p_field,
                        slot_id p_slot) -> void {
    record_fields[p_record][p_field] = p_slot;
  }

  // The slot of `p_object`'s field `p_field`, settled while solving.
  auto access(slot_id p_object, const string_type &p_field) -> slot_id {
    const auto result = fresh();
    accesses.push_back({p_object, p_field, result});
    return result;
  }

  auto value(slot_id p_slot) -> kind { return slots[find(p_slot)].value; }

  auto hint(slot_id p_slot, string_type p_trait, source_location p_location,
            std::size_t p_unit_index) -> void {
    hints.push_back({p_slot, std::move(p_trait), p_location, p_unit_index});
  }

  // Gives each still-open slot the trait whose phrases were used on it.
  // Slots used with phrases of several traits are returned (with where) so
  // the author can be asked for the type. Returns whether any slot changed.
  auto settle_hints(std::vector<std::pair<trait_hint, string_type>> &p_unclear)
      -> bool {
    std::map<slot_id, std::set<string_type>> by_slot;
    std::map<slot_id, const trait_hint *> first;
    for (const auto &each : hints) {
      const auto root = find(each.slot);
      if (slots[root].value != kind::none || slots[root].pinned)
        continue;
      by_slot[root].insert(each.trait);
      first.try_emplace(root, &each);
    }
    bool changed = false;
    for (const auto &[root, names] : by_slot) {
      if (names.size() == 1) {
        slots[root].value = kind::record;
        slots[root].record = *names.begin();
        changed = true;
        continue;
      }
      string_type joined;
      for (const auto &name : names)
        joined += (joined.empty() ? "" : "' and '") + name;
      p_unclear.emplace_back(*first.at(root), joined);
    }
    return changed;
  }

  // The element slot of a list, making the slot a list if it is still open.
  auto element_of(slot_id p_slot) -> slot_id {
    const auto root = find(p_slot);
    if (slots[root].value == kind::none && !slots[root].pinned) {
      const auto element = fresh();
      slots[find(p_slot)].value = kind::list;
      slots[find(p_slot)].element = element;
    }
    const auto &result = slots[find(p_slot)];
    // A map is gone through by its keys.
    if (result.value == kind::map)
      return result.key;
    return result.value == kind::list ? result.element : fresh();
  }

  // What `for each` gives: a list's items or a map's keys, once the slot is
  // known to be one; a list if nothing says.
  auto item_of(slot_id p_collection) -> slot_id {
    const auto &found = slots[find(p_collection)];
    if (found.value == kind::list || found.value == kind::map)
      return element_of(p_collection);
    const auto item = fresh();
    items.emplace_back(p_collection, item);
    return item;
  }

  // A map's value slot, if the slot is a map.
  auto map_value_of(slot_id p_slot) -> std::optional<slot_id> {
    const auto &found = slots[find(p_slot)];
    if (found.value != kind::map)
      return std::nullopt;
    return found.element;
  }

  auto unify(slot_id p_left, slot_id p_right) -> bool {
    const auto left = find(p_left);
    const auto right = find(p_right);
    if (left == right)
      return false;
    const auto left_state = slots[left];
    const auto right_state = slots[right];
    slots[right].parent = left;
    if (right_state.pinned && !left_state.pinned) {
      slots[left].soft = false;
      slots[left].value = right_state.value;
      slots[left].element = right_state.element;
      slots[left].record = right_state.record;
      slots[left].pointer = right_state.pointer;
      slots[left].pointee = right_state.pointee;
      slots[left].key = right_state.key;
      slots[left].nullable = right_state.nullable;
      slots[left].arguments = right_state.arguments;
      if (left_state.value == kind::record && right_state.value == kind::record &&
          left_state.record == right_state.record &&
          left_state.arguments.size() == right_state.arguments.size())
        for (std::size_t index = 0; index < left_state.arguments.size(); ++index)
          unify(left_state.arguments[index], right_state.arguments[index]);
      if (left_state.value == kind::map && right_state.value == kind::map) {
        unify(left_state.key, right_state.key);
        unify(left_state.element, right_state.element);
      }
      slots[left].pinned = true;
      if (left_state.value == kind::list && right_state.value == kind::list)
        unify(left_state.element, right_state.element);
    } else if (!left_state.pinned) {
      widen(left, right_state);
    } else if (left_state.value == kind::list &&
               right_state.value == kind::list) {
      unify(left_state.element, right_state.element);
    } else if (left_state.value == kind::map &&
               right_state.value == kind::map) {
      unify(left_state.key, right_state.key);
      unify(left_state.element, right_state.element);
    } else if (left_state.value == kind::record &&
               right_state.value == kind::record &&
               left_state.record == right_state.record) {
      join_arguments(left, right_state.arguments);
    }
    return true;
  }

  auto flow(slot_id p_from, slot_id p_to) -> void {
    flows.emplace_back(p_from, p_to);
  }
  auto fallback(slot_id p_slot, slot_id p_default) -> void {
    fallbacks.emplace_back(p_slot, p_default);
  }

  auto solve() -> void {
    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto &[from, to] : flows)
        changed = widen(to, slots[find(from)]) || changed;
      changed = settle_accesses() || changed;
      // A phrase's value once its types are known; a call through one.
      for (const auto &value : phrase_values) {
        auto &slot = slots[find(value.slot)];
        if (!slot.pointee.empty())
          continue;
        std::vector<value_type> parameters;
        for (const auto parameter : value.parameters)
          parameters.push_back(resolve(parameter));
        const auto result = resolve(value.result);
        if (!result.is_known() ||
            std::ranges::any_of(parameters, [](const value_type &p_type) {
              return !p_type.is_known();
            }))
          continue;
        const auto type = phrase_type(parameters, result);
        auto &target = slots[find(value.slot)];
        target.value = kind::pointer;
        target.pointer = type.kind;
        target.pointee = type.pointee;
        changed = true;
      }
      for (auto &call : phrase_calls) {
        if (call.is_done)
          continue;
        const auto signature = read_signature(resolve(call.phrase));
        if (!signature)
          continue;
        call.is_done = true;
        for (std::size_t index = 0;
             index < std::min(call.arguments.size(), signature->parameters.size());
             ++index)
          fallback(call.arguments[index], of(signature->parameters[index], false));
        unify(call.result, of(signature->result, false));
        changed = true;
      }
      for (auto &[collection, item] : items) {
        const auto value = slots[find(collection)].value;
        if (value != kind::list && value != kind::map)
          continue;
        changed = unify(item, element_of(collection)) || changed;
      }
      for (auto &use : map_uses) {
        if (use.is_done || slots[find(use.map)].value != kind::map)
          continue;
        use.is_done = true;
        const auto key = slots[find(use.map)].key;
        const auto element = slots[find(use.map)].element;
        if (use.key)
          flow(*use.key, key);
        if (use.value)
          flow(*use.value, element);
        if (use.result && use.is_keys) {
          slots[find(*use.result)].value = kind::list;
          slots[find(*use.result)].element = key;
        } else if (use.result) {
          unify(*use.result, element);
        }
        changed = true;
      }
      for (auto &waiting : waits)
        changed = waiting() || changed;
      // `(ref x)` points at x's type once it is known.
      for (const auto &[pointer, target] : references) {
        auto &result = slots[find(pointer)];
        if (!result.pointee.empty() || result.pinned)
          continue;
        const auto type = resolve(target);
        if (!type.is_known())
          continue;
        result.pointer = pointer_kind::one;
        result.pointee = type.to_string();
        changed = true;
      }
      if (changed)
        continue;
      for (const auto &[target, preferred] : fallbacks) {
        if (value(target) == kind::none && value(preferred) != kind::none) {
          unify(target, preferred);
          changed = true;
        }
      }
      if (!changed)
        changed = guess_records();
      // Gone through, and nothing else says what it is: a list.
      if (!changed)
        for (const auto &[collection, item] : items)
          if (slots[find(collection)].value == kind::none &&
              !slots[find(collection)].pinned) {
            unify(item, element_of(collection));
            changed = true;
          }
    }
  }

  // Joins each access whose record is known to that record's field.
  auto settle_accesses() -> bool {
    bool changed = false;
    for (auto &access : accesses) {
      if (access.is_done)
        continue;
      const auto &object = slots[find(access.object)];
      if (object.value != kind::record)
        continue;
      access.is_done = true;
      // A generic record's field has the types of this use.
      if (const auto generic = generic_records.find(object.record);
          generic != generic_records.end()) {
        const auto record = generic->first;
        const auto &parameters = generic->second.parameters;
        ensure_arguments(access.object, parameters.size());
        const auto field = generic->second.fields.find(access.field);
        if (field != generic->second.fields.end() &&
            field->second.has_variables()) {
          std::map<string_type, slot_id> bound;
          const auto arguments = slots[find(access.object)].arguments;
          for (std::size_t index = 0;
               index < std::min(parameters.size(), arguments.size()); ++index)
            bound[parameters[index]] = arguments[index];
          unify(instantiate(field->second, bound), access.result);
          changed = true;
          continue;
        }
      }
      const auto record = record_fields.find(object.record);
      if (record == record_fields.end())
        continue;
      const auto field = record->second.find(access.field);
      if (field == record->second.end())
        continue; // No such field: the checker explains.
      unify(field->second, access.result);
      changed = true;
    }
    return changed;
  }

public:
  // A generic record's type variables and its fields' stated types.
  struct generic_record {
    std::vector<string_type> parameters;
    std::map<string_type, value_type> fields;
  };
  std::map<string_type, generic_record> generic_records;

  // A generic record's use with types not said yet: open ones.
  auto ensure_arguments(slot_id p_slot, std::size_t p_count) -> void {
    if (!slots[find(p_slot)].arguments.empty())
      return;
    std::vector<slot_id> arguments;
    for (std::size_t index = 0; index < p_count; ++index)
      arguments.push_back(fresh());
    slots[find(p_slot)].arguments = std::move(arguments);
  }

  // A slot for the type, its type variables those of `p_bound` (others
  // stay themselves): a generic's parameter at one call, a generic
  // record's field in one use.
  auto instantiate(const value_type &p_type,
                   const std::map<string_type, slot_id> &p_bound) -> slot_id {
    if (!p_type.has_variables())
      return of(p_type, true);
    if (p_type.is_variable()) {
      const auto found = p_bound.find(p_type.record);
      return found != p_bound.end() ? found->second : of(p_type, true);
    }
    const auto result = fresh();
    if (p_type.is_map()) {
      const auto key = instantiate(p_type.map_key(), p_bound);
      const auto element = instantiate(p_type.map_value(), p_bound);
      slots[result].value = kind::map;
      slots[result].key = key;
      slots[result].element = element;
      return result;
    }
    if (p_type.is_list) {
      const auto element = instantiate(p_type.element(), p_bound);
      slots[result].value = kind::list;
      slots[result].element = element;
      return result;
    }
    if (p_type.is_record()) {
      std::vector<slot_id> arguments;
      for (const auto &argument : p_type.record_arguments())
        arguments.push_back(instantiate(argument, p_bound));
      slots[result].value = kind::record;
      slots[result].record = p_type.record;
      slots[result].nullable = p_type.nullable;
      slots[result].arguments = std::move(arguments);
      return result;
    }
    // A pointer to a type variable's values (`(many (var item))`): its
    // kind now, and what it points at once the variables are known; or,
    // given a pointer, the variables are what it points at.
    slots[result].value = kind_of(p_type.base);
    slots[result].pointer = p_type.kind;
    const auto generic = read_sexprs(p_type.pointee);
    const auto pointee = generic.size() == 1 ? value_type::parse(generic.front())
                                             : std::nullopt;
    if (!pointee)
      return result;
    auto bound = p_bound;
    waits.push_back([this, result, pointee = *pointee, bound, done = false]() mutable {
      if (done)
        return false;
      const auto &slot = slots[find(result)];
      if (!slot.pointee.empty()) {
        done = true;
        const auto given = read_pointee(slot.pointee);
        if (!given)
          return false;
        const auto value = given->kind == pointee_type::kind_type::number
                               ? value_type::number(given->number)
                               : pointee_value(*given);
        return bind(pointee, value, bound);
      }
      // What it points at, once each variable in it is known.
      const auto type = resolve_with(pointee, bound);
      if (!type)
        return false;
      done = true;
      slots[find(result)].pointee =
          parse_pointee(read_sexprs(type->to_string()).front()).value_or("");
      return true;
    });
    return result;
  }

  // The type with each bound variable as its slot resolves to, if every
  // one is known all through.
  auto resolve_with(const value_type &p_type,
                    const std::map<string_type, slot_id> &p_bound)
      -> std::optional<value_type> {
    std::map<string_type, value_type> known_types;
    for (const auto &[name, slot] : p_bound) {
      const auto type = resolve(slot);
      if (is_complete(type))
        known_types.emplace(name, type);
    }
    std::optional<value_type> result = p_type;
    const std::function<bool(const value_type &)> all_known =
        [&](const value_type &p_part) {
          if (p_part.is_variable())
            return known_types.contains(p_part.record);
          if (p_part.is_list)
            return all_known(p_part.element());
          if (p_part.is_record())
            return std::ranges::all_of(p_part.record_arguments(), all_known);
          return true;
        };
    if (!all_known(p_type))
      return std::nullopt;
    // Written out by hand: monomorphise.hpp's `substitute` is the checker's.
    const std::function<value_type(const value_type &)> put =
        [&](const value_type &p_part) -> value_type {
      if (p_part.is_variable())
        return known_types.at(p_part.record);
      if (p_part.is_list)
        return value_type::list_of(put(p_part.element()));
      if (p_part.is_record() && !p_part.arguments.empty()) {
        std::vector<value_type> arguments;
        for (const auto &argument : p_part.record_arguments())
          arguments.push_back(put(argument));
        auto record = value_type::record_of(p_part.record, arguments);
        record.nullable = p_part.nullable;
        return record;
      }
      return p_part;
    };
    return put(*result);
  }

  // A given type where a generic's type with variables goes: each variable
  // is the matching part.
  auto bind(const value_type &p_generic, const value_type &p_given,
            const std::map<string_type, slot_id> &p_bound) -> bool {
    if (p_generic.is_variable()) {
      const auto found = p_bound.find(p_generic.record);
      return found != p_bound.end() && unify(found->second, of(p_given, false));
    }
    if (p_generic.is_list && p_given.is_list)
      return bind(p_generic.element(), p_given.element(), p_bound);
    if (p_generic.is_record() && p_given.is_record()) {
      const auto generic = p_generic.record_arguments();
      const auto given = p_given.record_arguments();
      bool changed = false;
      for (std::size_t index = 0; index < std::min(generic.size(), given.size());
           ++index)
        changed = bind(generic[index], given[index], p_bound) || changed;
      return changed;
    }
    if (p_generic.is_phrase() && p_given.is_phrase()) {
      const auto generic = read_signature(p_generic);
      const auto given = read_signature(p_given);
      if (!generic || !given ||
          generic->parameters.size() != given->parameters.size())
        return false;
      bool changed = false;
      for (std::size_t index = 0; index < generic->parameters.size(); ++index)
        changed = bind(generic->parameters[index], given->parameters[index],
                       p_bound) || changed;
      return bind(generic->result, given->result, p_bound) || changed;
    }
    return false;
  }

  // A phrase given where a generic takes one of its type variables' values
  // (`(phrase ((var item)) (var result))`, the library's `where` and
  // `applied to each`): the variables go into the phrase's parameters and
  // come out of its result. A phrase named there (`double _`) is joined by
  // its slots, so its untyped parameters learn from the list; one held in
  // a variable, by its type once that is known.
  auto bind_phrase(slot_id p_given, const value_type &p_generic,
                   const std::map<string_type, slot_id> &p_bound) -> void {
    const auto signature = read_signature(p_generic);
    if (!signature)
      return;
    waits.push_back([this, p_given, signature = *signature, bound = p_bound,
                     done = false]() mutable {
      if (done)
        return false;
      for (const auto &value : phrase_values) {
        if (find(value.slot) != find(p_given) ||
            value.parameters.size() != signature.parameters.size())
          continue;
        done = true;
        for (std::size_t index = 0; index < value.parameters.size(); ++index)
          flow(instantiate(signature.parameters[index], bound),
               value.parameters[index]);
        flow(value.result, instantiate(signature.result, bound));
        return true;
      }
      const auto given = resolve(p_given);
      if (!given.is_phrase())
        return false;
      done = true;
      return bind(phrase_type(signature.parameters, signature.result), given,
                  bound) || true;
    });
  }

  // Whether a type is known all through (no `?` in a list or record).
  static auto is_complete(const value_type &p_type) -> bool {
    return p_type.is_known() &&
           p_type.to_string().find('?') == string_type::npos;
  }

  // `(field b title)` where nothing else says what `b` is: the record, if
  // only one has a field of that name.
  auto guess_records() -> bool {
    for (auto &access : accesses) {
      if (access.is_done || value(access.object) != kind::none ||
          slots[find(access.object)].pinned)
        continue;
      std::vector<string_type> owners;
      for (const auto &[record, fields] : record_fields)
        if (fields.contains(access.field))
          owners.push_back(record);
      if (owners.size() != 1)
        continue;
      auto &object = slots[find(access.object)];
      object.value = kind::record;
      object.record = owners.front();
      return true;
    }
    return false;
  }

  // `unknown` where nothing decided the type.
  auto resolve(slot_id p_slot) -> value_type {
    const auto &found = slots[find(p_slot)];
    switch (found.value) {
    case kind::none:
      return value_type::unknown();
    case kind::integer:
    case kind::real:
      return found.soft ? (found.value == kind::integer ? value_type::integer()
                                                        : value_type::real())
                        : number_of(found);
    case kind::boolean:
      return value_type::boolean();
    case kind::string:
      return value_type::string();
    case kind::pointer:
      if (found.pointer != pointer_kind::raw && !found.pointee.empty())
        return value_type::pointer_to(found.pointer, found.pointee);
      return value_type::pointer();
    case kind::memory:
      return value_type::memory();
    case kind::nothing:
      return value_type::nothing();
    case kind::list: {
      auto result = value_type::list_of(resolve(found.element));
      if (result.inner_lists == 0 && !found.pointee.empty())
        result.pointee = found.pointee; // a C array's item type
      return result;
    }
    case kind::record: {
      auto result = value_type::record_of(found.record);
      if (!found.arguments.empty()) {
        std::vector<value_type> arguments;
        // A copy: resolving may follow the slots back here (a node of
        // nodes), and `found` is not kept while it does.
        const auto own = found.arguments;
        const auto nullable = found.nullable;
        for (const auto argument : own)
          arguments.push_back(resolve(argument));
        result = value_type::record_of(result.record, arguments);
        result.nullable = nullable;
        return result;
      }
      result.nullable = found.nullable;
      return result;
    }
    case kind::map:
      return value_type::map_of(resolve(found.key), resolve(found.element));
    case kind::variable:
      return value_type::variable(found.record);
    }
    return value_type::unknown();
  }
};

using slot_id = inference::slot_id;

class elaborator {
  struct function_info {
    function *declaration;
    std::size_t unit_index;
    std::vector<slot_id> parameters;
    slot_id result;
    bool returns_value = false;
  };
  struct variable_target {
    statement *declaration;
    slot_id variable;
    slot_id initial_value;
    bool has_initial_value;
    std::size_t unit_index;
  };
  struct list_target {
    expression *list;
    slot_id element;
    std::vector<slot_id> items;
    std::size_t unit_index;
  };
  struct condition_target {
    expression *condition;
    slot_id slot;
    std::size_t unit_index;
  };
  struct field_target {
    record_field *declaration;
    string_type record;
    slot_id slot;
    source_location location;
    std::size_t unit_index;
  };

  program &target;
  inference types;
  std::vector<diagnostic> notes;
  std::map<string_type, function_info> functions;
  std::map<string_type, const c_function *> c_functions;
  std::map<string_type, slot_id> globals;
  std::vector<std::map<string_type, slot_id>> scopes;
  // Trait methods by name, and which records implement which traits.
  std::map<string_type, trait *> method_owner;
  std::set<std::pair<string_type, string_type>> implemented;
  std::vector<diagnostic> problems;
  function_info *current_function = nullptr;
  std::size_t unit_index = 0;

  std::vector<variable_target> variables;
  std::vector<list_target> lists;
  std::vector<condition_target> conditions;
  std::vector<field_target> fields;
  // Calls of a generic C macro (generics.md §9), until their arguments'
  // types are known and clang has said what the copy for them gives back.
  struct macro_call {
    expression *call;
    const c_function *macro;
    std::vector<slot_id> arguments;
    slot_id result;
    std::size_t unit_index;
    bool is_done = false;
  };
  std::deque<macro_call> macro_calls;
  // The copies made for the calls, added to their macro's unit at the end.
  std::vector<c_function> macro_copies;
  // The program's headers and their flags, which a macro's probe reads.
  std::pair<std::vector<string_type>, std::vector<string_type>> program_headers;
  // Calls of a generic function: a slot for each of its type variables
  // here, and its result once the function's is known.
  struct generic_call {
    expression *call;
    function_info *callee;
    std::map<string_type, slot_id> bound;
    slot_id result;
    bool is_done = false;
  };
  std::deque<generic_call> generic_calls;
  // `(new stack …)` of a generic record, whose types are written back.
  std::vector<std::pair<expression *, slot_id>> generic_news;

  // A generic call's result: the function's, with this call's types, once
  // it is known (with `p_guess`, as far as it is).
  auto settle_result(generic_call &p_call, bool p_guess) -> bool {
    if (p_call.is_done)
      return false;
    const auto &declaration = *p_call.callee->declaration;
    if (!declaration.return_type.is_known() && !p_call.callee->returns_value) {
      p_call.is_done = true;
      types.unify(p_call.result, known(value_type::nothing()));
      return true;
    }
    const auto type = declaration.return_type.is_known()
                          ? declaration.return_type
                          : types.resolve(p_call.callee->result);
    if (!p_guess && !inference::is_complete(type))
      return false;
    p_call.is_done = true;
    types.unify(p_call.result, types.instantiate(type, p_call.bound));
    return true;
  }
  // The blocks being walked, innermost last: each one's label and, for one
  // giving a value, its variable's slot, which its `leave`s flow into.
  struct block_target {
    string_type label;
    std::optional<slot_id> value;
  };
  std::vector<block_target> block_targets;

  // Asks clang, in one file, about each macro call whose arguments' types
  // are known (with `p_guess`, one not known yet is a whole number), and
  // calls the copy made for them. Returns whether any was answered.
  auto ask_macros(bool p_guess) -> bool {
    std::vector<macro_call *> ready;
    std::vector<macro_request> requests;
    for (auto &call : macro_calls) {
      if (call.is_done)
        continue;
      string_type problem;
      const auto arguments = macro_arguments_of(
          target, *call.macro, call.call->arguments,
          [&](std::size_t p_index) {
            return p_index < call.arguments.size() ? types.resolve(call.arguments[p_index])
                                                   : value_type::unknown();
          },
          p_guess, problem);
      if (!problem.empty()) {
        call.is_done = true;
        problems.push_back({call.unit_index, call.call->location, problem});
        continue;
      }
      if (!arguments)
        continue;
      ready.push_back(&call);
      requests.push_back({call.macro, *arguments, program_headers.first,
                          program_headers.second});
    }
    if (requests.empty())
      return false;
    const auto answers = copy_c_macros(requests);
    for (std::size_t index = 0; index < ready.size(); ++index) {
      auto &call = *ready[index];
      call.is_done = true;
      if (!answers[index].copy) {
        problems.push_back(
            {call.unit_index, call.call->location,
             "The C macro '" + call.macro->name + "' can't be given " + [&] {
               string_type given;
               for (const auto &argument : requests[index].arguments)
                 given += (given.empty() ? "" : ", ") + argument.spelling;
               return given.empty() ? string_type("nothing") : given;
             }() + ": clang says \"" + answers[index].problem + "\"."});
        continue;
      }
      auto copy = *answers[index].copy;
      copy.result_record = macro_result_record(target, copy);
      // The call calls the copy, with the values it is given at run time:
      // the types and fields are in the copy.
      call.call->text = copy.name;
      std::vector<expression> values;
      std::vector<slot_id> slots;
      for (std::size_t at = 0; at < call.call->arguments.size(); ++at) {
        const auto role = at < call.macro->macro_roles.size()
                              ? call.macro->macro_roles[at]
                              : c_function::macro_role::value;
        if (role == c_function::macro_role::type ||
            role == c_function::macro_role::field)
          continue;
        values.push_back(std::move(call.call->arguments[at]));
        slots.push_back(call.arguments[at]);
      }
      call.call->arguments = std::move(values);
      call.arguments = std::move(slots);
      // Values not known yet (guessed) are what the copy takes.
      for (std::size_t at = 0; at < call.arguments.size(); ++at)
        if (!types.resolve(call.arguments[at]).is_known())
          types.unify(call.arguments[at], known(c_parameter_type(copy, at)));
      types.unify(call.result, types.of(c_result_type(copy), true));
      if (std::ranges::find(macro_copies, copy.name, &c_function::name) ==
          macro_copies.end())
        macro_copies.push_back(std::move(copy));
    }
    return true;
  }

  // The copies made for macro calls, in the unit their macro is in.
  auto add_macro_copies() -> void {
    for (auto &copy : macro_copies)
      for (auto &unit : target.units)
        if (std::ranges::find(unit.externs, copy.macro_name, &c_function::name) !=
            unit.externs.end()) {
          if (std::ranges::find(unit.externs, copy.name, &c_function::name) ==
              unit.externs.end())
            unit.externs.push_back(std::move(copy));
          break;
        }
  }

  auto note(source_location p_location, string_type p_message) -> void {
    for (const auto &existing : notes)
      if (existing.unit_index == unit_index && existing.message == p_message)
        return;
    notes.push_back({unit_index, p_location, std::move(p_message)});
  }

  auto at_top_level() const -> bool {
    return current_function == nullptr && scopes.size() == 1;
  }

  auto lookup(const string_type &p_name) -> std::optional<slot_id> {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
      if (const auto found = scope->find(p_name); found != scope->end())
        return found->second;
    if (const auto found = globals.find(p_name); found != globals.end())
      return found->second;
    return std::nullopt;
  }

  auto declare(const string_type &p_name, slot_id p_slot) -> void {
    if (at_top_level())
      globals[p_name] = p_slot;
    else
      scopes.back()[p_name] = p_slot;
  }

  auto visible_variables() const -> std::vector<string_type> {
    std::set<string_type> names;
    for (const auto &scope : scopes)
      for (const auto &[name, slot] : scope)
        names.insert(name);
    for (const auto &[name, slot] : globals)
      names.insert(name);
    return {names.begin(), names.end()};
  }

  auto callable_names() const -> std::vector<string_type> {
    std::vector<string_type> names;
    for (const auto &[name, info] : functions)
      names.push_back(name);
    for (const auto &[name, function] : c_functions)
      names.push_back(name);
    for (const auto &[name, owner] : method_owner)
      names.push_back(name);
    for (const auto &name : builtin_names())
      if (std::isalpha(static_cast<unsigned char>(name.front())))
        names.emplace_back(name);
    return names;
  }

  auto is_callable(const string_type &p_name) const -> bool {
    return functions.contains(p_name) || c_functions.contains(p_name) ||
           method_owner.contains(p_name) || is_builtin(p_name);
  }

  // Expressions.

  auto walk_arguments(expression &p_call) -> std::vector<slot_id> {
    std::vector<slot_id> result;
    for (auto &argument : p_call.arguments)
      result.push_back(walk(argument));
    return result;
  }

  auto known(const value_type &p_type) -> slot_id {
    return types.of(p_type, false);
  }

  auto walk_call(expression &p_call) -> slot_id {
    auto &name = p_call.text;
    // A C macro's type or field argument: no value.
    if (name == "type" || name == "member")
      return types.fresh();
    if (!is_callable(name) && !lookup(name)) {
      if (const auto guess = closest_name(name, callable_names())) {
        note(p_call.location, "Read `" + name + "` as `" + *guess + "`.");
        name = *guess;
      }
    }
    // A phrase's name, not a value to walk.
    if (name == "phrase-of") {
      const auto result = types.fresh();
      types.unify(result, known(value_type::pointer()));
      if (p_call.arguments.size() == 1)
        if (const auto found = functions.find(p_call.arguments[0].text);
            found != functions.end())
          // A phrase that gives nothing back is `void` once it is read
          // whole; its type needs that now.
          types.phrase_of(result, found->second.parameters,
                          has_value_return(found->second.declaration->body)
                              ? found->second.result
                              : known(value_type::nothing()));
      return result;
    }
    const auto arguments = walk_arguments(p_call);
    if (name == "call") {
      const auto result = types.fresh();
      if (!arguments.empty())
        types.call_phrase(result, arguments[0],
                          {arguments.begin() + 1, arguments.end()});
      return result;
    }

    if (name == "+" || name == "-" || name == "*" || name == "%" ||
        name == "min" || name == "max" || name == "abs" || name == "div" ||
        name == "bit-and" || name == "bit-or" || name == "bit-xor") {
      const auto result = types.fresh_number();
      for (const auto argument : arguments)
        types.flow(argument, result);
      return result;
    }
    // `/` of float32s is one; of whole numbers, a decimal.
    if (name == "/") {
      const auto result = types.fresh_number();
      types.flow(types.literal(value_type::real()), result);
      for (const auto argument : arguments)
        types.flow(argument, result);
      return result;
    }
    // The number's own type, whatever the place or count.
    if (core::is_bit_operation(name) && name != "bit-test" &&
        name != "count-ones" && name != "leading-zeros" &&
        name != "trailing-zeros") {
      const auto result = types.fresh_number();
      if (!arguments.empty())
        types.flow(arguments[0], result);
      for (std::size_t index = 1; index < arguments.size(); ++index)
        types.fallback(arguments[index], known(value_type::integer()));
      return result;
    }
    if (name == "bit-test")
      return known(value_type::boolean());
    if (name == "wrapping" || name == "saturating") {
      const auto result = types.fresh_number();
      if (!arguments.empty())
        types.flow(arguments[0], result);
      return result;
    }
    // A decimal's bits are a whole number of its size, and back.
    if ((name == "float-bits" || name == "bits-float") && !arguments.empty()) {
      const auto result = types.fresh();
      const auto argument = arguments[0];
      const bool to_whole = name == "float-bits";
      types.wait([this, result, argument, to_whole] {
        if (types.resolve(result).is_known())
          return false;
        const auto type = types.resolve(argument);
        if (!type.is_numeric())
          return false;
        const bool narrow = type.is_sized() && core::number_bits(*type.number_type()) == 32;
        types.unify(result, known(to_whole ? value_type::number(narrow ? c_type::uint_type
                                                                       : c_type::size_type)
                                           : narrow ? value_type::number(c_type::float_type)
                                                    : value_type::real()));
        return true;
      });
      return result;
    }
    if (name == "to-bytes" || name == "to-bytes-big")
      return types.of(value_type::list_of(value_type::number(c_type::uint8_type)),
                      true);
    if ((name == "from-bytes" || name == "from-bytes-big") && p_call.type_argument)
      return known(*p_call.type_argument);
    if (name == "as" || name == "wrap-to" || name == "clamp-to")
      return p_call.type_argument ? known(*p_call.type_argument)
                                  : types.fresh();
    if (name == "to-hex" || name == "to-binary")
      return known(value_type::string());
    if (name == "float" || name == "sqrt" || name == "pow" ||
        name == "floor" || name == "read_float")
      return known(value_type::real());
    if (name == "int" || name == "read_int" || name == "random" ||
        name == "char_at" || name == "count-ones" || name == "leading-zeros" ||
        name == "trailing-zeros" || name == "from-hex")
      return known(value_type::integer());
    if (name == "=" || name == "!=" || name == "<" || name == "<=" ||
        name == ">" || name == ">=") {
      if (arguments.size() == 2) {
        types.fallback(arguments[0], arguments[1]);
        types.fallback(arguments[1], arguments[0]);
      }
      return known(value_type::boolean());
    }
    if (name == "and" || name == "or" || name == "not") {
      for (const auto argument : arguments)
        types.fallback(argument, known(value_type::boolean()));
      return known(value_type::boolean());
    }
    if (name == "ref") {
      const auto result = types.fresh();
      types.unify(result, known(value_type::pointer()));
      if (arguments.size() == 1)
        types.reference(result, arguments[0]);
      return result;
    }
    if (name == "load" && !p_call.fields.empty()) {
      if (const auto pointee = read_pointee(p_call.fields[0]))
        return known(pointee_value(*pointee));
      return types.fresh();
    }
    if (name == "store") {
      if (arguments.size() == 2 && !p_call.fields.empty())
        if (const auto pointee = read_pointee(p_call.fields[0]))
          types.fallback(arguments[1], known(pointee_value(*pointee)));
      return known(value_type::nothing());
    }
    if (name == "size-of")
      return known(value_type::integer());
    // Room for N of T: a pointer to many of them (generics.md §5).
    if ((name == "alloc" || name == "alloc-more") && !p_call.fields.empty()) {
      for (std::size_t index = name == "alloc" ? 0 : 1; index < arguments.size();
           ++index)
        types.fallback(arguments[index], known(value_type::integer()));
      return types.of(value_type::pointer_to(pointer_kind::many, p_call.fields[0]),
                      true);
    }
    if (name == "map-new" && p_call.type_argument)
      return types.of(*p_call.type_argument, false);
    if (name.starts_with("map-") && !arguments.empty()) {
      // Joined to the map's key and value types once it is known.
      const auto result = name == "map-has"   ? known(value_type::boolean())
                          : name == "map-get" || name == "map-keys"
                              ? types.fresh()
                              : known(value_type::nothing());
      types.use_map(arguments[0],
                    arguments.size() > 1 ? std::optional(arguments[1]) : std::nullopt,
                    name == "map-put" && arguments.size() == 3
                        ? std::optional(arguments[2])
                        : std::nullopt,
                    name == "map-get" || name == "map-keys" ? std::optional(result)
                                                            : std::nullopt,
                    name == "map-keys");
      return result;
    }
    if (name == "view" && !p_call.fields.empty())
      if (const auto pointee = read_pointee(p_call.fields[0])) {
        if (arguments.size() == 2)
          types.fallback(arguments[1], known(value_type::integer()));
        return types.of(pointee->kind == pointee_type::kind_type::number
                            ? value_type::list_of(c_number_item(pointee->number))
                            : value_type::list_of(pointee->value),
                        true);
      }
    if (name == "offset") {
      if (arguments.size() == 2)
        types.fallback(arguments[1], known(value_type::integer()));
      return arguments.empty() ? types.fresh() : arguments[0];
    }
    if (name == "cast" && p_call.type_argument)
      return known(*p_call.type_argument);
    if (name == "new" && p_call.type_argument) {
      const auto result = known(*p_call.type_argument);
      // A generic record whose types the fields given decide.
      if (const auto generic = types.generic_records.find(p_call.type_argument->record);
          generic != types.generic_records.end())
      {
        types.ensure_arguments(result, generic->second.parameters.size());
        generic_news.emplace_back(&p_call, result);
      }
      for (std::size_t index = 0;
           index < std::min(arguments.size(), p_call.fields.size()); ++index)
        types.flow(arguments[index], types.access(result, p_call.fields[index]));
      return result;
    }
    if (name == "field") {
      if (arguments.empty() || p_call.fields.empty())
        return types.fresh();
      return types.access(arguments[0], p_call.fields[0]);
    }
    if (name == "set-char") {
      if (arguments.size() == 3) {
        types.unify(arguments[0], known(value_type::string()));
        types.unify(arguments[1], known(value_type::integer()));
        types.unify(arguments[2], known(value_type::integer()));
      }
      return known(value_type::nothing());
    }
    if (name == "concat" || name == "substring" || name == "to_str" ||
        name == "chr" || name == "buffer" || name == "read_line")
      return known(value_type::string());
    if (name == "len" || name == "sort" || name == "reverse") {
      // `len` also counts a map's entries.
      if (!arguments.empty())
        name == "len" ? types.item_of(arguments[0])
                      : types.element_of(arguments[0]);
      return known(name == "len" ? value_type::integer()
                                 : value_type::nothing());
    }
    if (name == "get") {
      if (arguments.empty())
        return types.fresh();
      return types.element_of(arguments[0]);
    }
    if (name == "new-memory")
      return known(value_type::memory());
    if (name == "clear-memory" || name == "free")
      return known(value_type::nothing());
    // `keep` gives back what it is given.
    if (name == "keep" || name == "copy")
      return arguments.empty() ? types.fresh() : arguments[0];
    if (name == "remove-at") {
      if (arguments.size() == 2) {
        types.element_of(arguments[0]);
        types.flow(arguments[1], types.of(value_type::integer(), true));
      }
      return known(value_type::nothing());
    }
    if (name == "append") {
      if (arguments.size() == 2)
        types.flow(arguments[1], types.element_of(arguments[0]));
      return known(value_type::nothing());
    }
    // `(array u8 256)`: an array of bytes, as it says.
    if (name == "array" && p_call.type_argument &&
        !p_call.type_argument->pointee.empty())
      return types.of(value_type::list_of(*p_call.type_argument), true);
    if (name == "list" || name == "array") {
      const auto element =
          p_call.type_argument ? types.of(*p_call.type_argument, true)
                               : types.fresh();
      if (name == "list")
        for (const auto argument : arguments)
          types.flow(argument, element);
      if (name == "list" ||
          (p_call.type_argument && !p_call.type_argument->is_known()))
        lists.push_back({&p_call, element,
                         name == "list" ? arguments
                                        : std::vector<slot_id>(),
                         unit_index});
      auto result = types.fresh();
      types.unify(result, types.of(value_type::list_of(base_type::unknown),
                                   false));
      types.unify(types.element_of(result), element);
      return result;
    }

    if (const auto method = method_owner.find(name);
        method != method_owner.end() && !arguments.empty()) {
      auto &owner = *method->second;
      const auto &signature = *owner.find(name);
      types.hint(arguments[0], owner.name, p_call.location, unit_index);
      // A default method is also a function, with the types it works out.
      if (!signature.has_default) {
        for (std::size_t index = 1;
             index < std::min(arguments.size(), signature.parameters.size());
             ++index)
          types.flow(arguments[index], known(signature.parameters[index].type));
        return known(signature.return_type);
      }
    }
    if (const auto found = functions.find(name);
        found != functions.end() && found->second.declaration->is_generic()) {
      // Its type variables are new at each call (instantiation).
      auto &callee = found->second;
      auto &waiting = generic_calls.emplace_back(
          generic_call{&p_call, &callee, {}, types.fresh()});
      for (const auto &parameter : callee.declaration->type_parameters)
        waiting.bound[parameter.name] = types.fresh();
      for (std::size_t index = 0;
           index < std::min(arguments.size(), callee.parameters.size());
           ++index) {
        const auto &type = callee.declaration->parameters[index].type;
        types.flow(arguments[index], type.has_variables()
                                         ? types.instantiate(type, waiting.bound)
                                         : callee.parameters[index]);
        if (type.is_phrase() && type.has_variables())
          types.bind_phrase(arguments[index], type, waiting.bound);
      }
      types.wait([this, &waiting] { return settle_result(waiting, false); });
      return waiting.result;
    }
    if (const auto found = functions.find(name); found != functions.end()) {
      auto &callee = found->second;
      for (std::size_t index = 0;
           index < std::min(arguments.size(), callee.parameters.size());
           ++index)
        types.flow(arguments[index], callee.parameters[index]);
      return callee.result;
    }
    if (const auto found = c_functions.find(name); found != c_functions.end()) {
      const auto &callee = *found->second;
      if (callee.is_generic_macro()) {
        // In a generic phrase, a type variable's values: each copy of the
        // phrase asks about its own (the checker); here the result is
        // taken to be the first argument's type.
        if (current_function != nullptr &&
            current_function->declaration->is_generic()) {
          const auto result = types.fresh();
          if (!arguments.empty())
            types.fallback(result, arguments[0]);
          return result;
        }
        auto &waiting = macro_calls.emplace_back(
            macro_call{&p_call, &callee, arguments, types.fresh(), unit_index});
        return waiting.result;
      }
      if (!callee.is_variadic &&
          arguments.size() + 1 == callee.parameters.size() &&
          callee.parameters.back() == c_type::pointer_type) {
        note(p_call.location, "`" + name +
                                  "` gets null for its last argument, which "
                                  "was left out.");
        p_call.arguments.push_back({.kind = expression::kind_type::variable,
                                    .text = "null",
                                    .location = p_call.location});
      }
      for (std::size_t index = 0;
           index < std::min(arguments.size(), callee.parameters.size());
           ++index) {
        types.fallback(arguments[index],
                       known(c_parameter_type(callee, index)));
        // (ref x) into `int *` or `double *`: x holds that kind of value.
        const auto &argument = p_call.arguments[index];
        if (argument.kind == expression::kind_type::call &&
            argument.text == "ref" && argument.arguments.size() == 1 &&
            index < callee.pointees.size() && callee.pointees[index])
          if (const auto variable = lookup(argument.arguments[0].text))
            types.fallback(*variable,
                           known(ovis_type_of(*callee.pointees[index])));
      }
      return types.of(c_result_type(callee), true);
    }
    return types.fresh();
  }

  auto walk(expression &p_expression) -> slot_id {
    using kind = expression::kind_type;
    switch (p_expression.kind) {
    case kind::integer:
      // Above the largest signed one: only a uint64 holds it.
      return types.literal(p_expression.is_big
                               ? value_type::number(c_type::size_type)
                               : value_type::integer());
    case kind::real:
      return types.literal(value_type::real());
    case kind::boolean:
      return known(value_type::boolean());
    case kind::string:
      return known(value_type::string());
    case kind::variable: {
      if (const auto found = lookup(p_expression.text))
        return *found;
      if (const auto *const constant = find_constant(target, p_expression.text)) {
        switch (constant->kind) {
        case c_constant::kind_type::integer:
          return known(value_type::integer());
        case c_constant::kind_type::real:
          return known(value_type::real());
        case c_constant::kind_type::null:
          return known(value_type::pointer());
        case c_constant::kind_type::runtime:
          return known(p_expression.text.starts_with("std")
                           ? value_type::pointer()
                           : value_type::integer());
        }
      }
      if (const auto guess =
              closest_name(p_expression.text, visible_variables())) {
        note(p_expression.location,
             "Read `" + p_expression.text + "` as `" + *guess + "`.");
        p_expression.text = *guess;
        return *lookup(*guess);
      }
      return types.fresh();
    }
    case kind::call:
      return walk_call(p_expression);
    }
    return types.fresh();
  }

  // Statements.

  auto walk_block(std::vector<statement> &p_body) -> void {
    scopes.emplace_back();
    for (auto &statement : p_body)
      walk(statement);
    scopes.pop_back();
  }

  auto walk_condition(expression &p_condition) -> void {
    const auto slot = walk(p_condition);
    types.fallback(slot, known(value_type::boolean()));
    conditions.push_back({&p_condition, slot, unit_index});
  }

  auto existing_in_scope(const string_type &p_name) -> std::optional<slot_id> {
    if (at_top_level()) {
      if (const auto found = globals.find(p_name); found != globals.end())
        return found->second;
      return std::nullopt;
    }
    if (const auto found = scopes.back().find(p_name);
        found != scopes.back().end())
      return found->second;
    return std::nullopt;
  }

  auto walk_let(statement &p_statement) -> void {
    const bool has_value = !p_statement.values.empty();
    const auto value =
        has_value ? walk(p_statement.values[0]) : types.fresh();

    if (const auto existing = existing_in_scope(p_statement.name);
        existing && has_value) {
      types.flow(value, *existing);
      return;
    }

    const bool is_stated =
        p_statement.declared_type && p_statement.declared_type->is_known() &&
        !(p_statement.declared_type->is_list &&
          p_statement.declared_type->base == base_type::unknown);
    const auto variable =
        p_statement.declared_type ? types.of(*p_statement.declared_type, true)
                                  : types.fresh();
    types.flow(value, variable);
    if (!is_stated)
      variables.push_back({&p_statement, variable, value, has_value,
                           unit_index});
    declare(p_statement.name, variable);
  }

  auto walk(statement &p_statement) -> void {
    using kind = statement::kind_type;
    auto &values = p_statement.values;
    switch (p_statement.kind) {
    case kind::let:
      walk_let(p_statement);
      break;
    case kind::set:
      if (const auto found = lookup(p_statement.name)) {
        types.flow(walk(values[0]), *found);
      } else {
        // Assigning to a new name declares it.
        p_statement.kind = kind::let;
        walk_let(p_statement);
      }
      break;
    case kind::put: {
      const auto list = lookup(p_statement.name);
      const auto index = walk(values[0]);
      const auto value = walk(values[1]);
      types.fallback(index, known(value_type::integer()));
      if (list)
        types.flow(value, types.element_of(*list));
      break;
    }
    case kind::print:
    case kind::evaluate:
      for (auto &value : values)
        walk(value);
      break;
    case kind::if_:
      walk_condition(values[0]);
      walk_block(p_statement.body);
      walk_block(p_statement.else_body);
      break;
    case kind::while_:
      walk_condition(values[0]);
      walk_block(p_statement.body);
      break;
    case kind::for_: {
      types.fallback(walk(values[0]), known(value_type::integer()));
      types.fallback(walk(values[1]), known(value_type::integer()));
      scopes.push_back(
          {{p_statement.name, types.of(value_type::integer(), true)}});
      walk_block(p_statement.body);
      scopes.pop_back();
      break;
    }
    case kind::each: {
      const auto list = walk(values[0]);
      scopes.push_back({{p_statement.name, types.item_of(list)}});
      walk_block(p_statement.body);
      scopes.pop_back();
      break;
    }
    case kind::return_:
      if (current_function != nullptr && !values.empty()) {
        current_function->returns_value = true;
        types.flow(walk(values[0]), current_function->result);
      } else {
        for (auto &value : values)
          walk(value);
      }
      break;
    case kind::set_field: {
      const auto object = walk(values[0]);
      types.flow(walk(values[1]), types.access(object, p_statement.name));
      break;
    }
    case kind::with_memory:
      if (!values.empty())
        types.fallback(walk(values[0]), known(value_type::memory()));
      walk_block(p_statement.body);
      break;
    case kind::defer:
      walk_block(p_statement.body);
      break;
    case kind::block: {
      std::optional<slot_id> target;
      if (!p_statement.name.empty())
        target = lookup(p_statement.name); // the checker reports a missing one
      block_targets.push_back({p_statement.label, target});
      walk_block(p_statement.body);
      block_targets.pop_back();
      break;
    }
    case kind::leave: {
      const auto target = std::find_if(
          block_targets.rbegin(), block_targets.rend(), [&](const auto &p_block) {
            return p_statement.label.empty() || p_block.label == p_statement.label;
          });
      for (auto &value : values) {
        const auto slot = walk(value);
        if (target != block_targets.rend() && target->value)
          types.flow(slot, *target->value);
      }
      break;
    }
    case kind::break_:
    case kind::continue_:
    case kind::pass:
      break;
    }
  }

  // Implicit return.

  static auto has_value_return(const std::vector<statement> &p_body) -> bool {
    for (const auto &statement : p_body) {
      if (statement.kind == statement::kind_type::return_ &&
          !statement.values.empty())
        return true;
      if (has_value_return(statement.body) ||
          has_value_return(statement.else_body))
        return true;
    }
    return false;
  }

  auto produces_value(const function &p_function,
                      const expression &p_expression) const -> bool {
    if (p_expression.kind != expression::kind_type::call)
      return true;
    const auto &name = p_expression.text;
    if (name == "append" || name == "sort" || name == "reverse" ||
        name == "remove-at" || name == "free" || name == "clear-memory" ||
        name == "store" || name == "set-char")
      return false;
    if (is_builtin(name))
      return true;
    // Calls made for their effect (`printf`, a procedure) shouldn't turn
    // into the result unless the function says it returns something.
    if (!p_function.return_type.is_known())
      return false;
    return c_functions.contains(name) || functions.contains(name) ||
           method_owner.contains(name);
  }

  // `(fn square (x) (* x x))` returns `(* x x)`.
  auto add_implicit_return(function &p_function) -> void {
    if (p_function.return_type == value_type::nothing() ||
        p_function.body.empty() || has_value_return(p_function.body))
      return;
    // A trailing `defer` is cleanup, not the function's value.
    auto last_index = p_function.body.size();
    while (last_index > 0 &&
           p_function.body[last_index - 1].kind == statement::kind_type::defer)
      --last_index;
    if (last_index == 0)
      return;
    auto &last = p_function.body[last_index - 1];
    if (last.kind == statement::kind_type::evaluate &&
        produces_value(p_function, last.values[0])) {
      last.kind = statement::kind_type::return_;
      // The return goes after the defers, or it would leave before they are
      // registered and they would never run.
      std::rotate(p_function.body.begin() + (last_index - 1),
                  p_function.body.begin() + last_index, p_function.body.end());
      note(last.location, "`" + p_function.name +
                              "` gives back the value it works out last.");
      return;
    }
    if ((last.kind == statement::kind_type::let ||
         last.kind == statement::kind_type::set) &&
        p_function.return_type.is_known()) {
      statement result{.kind = statement::kind_type::return_,
                       .location = last.location};
      result.values.push_back({.kind = expression::kind_type::variable,
                               .text = last.name,
                               .location = last.location});
      note(last.location, "`" + p_function.name + "` gives back `" + last.name +
                              "`, the last value it works out.");
      p_function.body.push_back(std::move(result));
    }
  }

  // Writing the results back.

  // The inferred type, or the default when nothing decided it.
  auto settle(slot_id p_slot, bool &p_was_defaulted) -> value_type {
    const auto type = types.resolve(p_slot);
    p_was_defaulted = !type.is_known();
    if (!p_was_defaulted)
      return type;
    return type.is_list ? value_type::list_of(base_type::integer)
                        : value_type::integer();
  }

  auto write_back() -> void {
    // First: a condition wrapped below moves the call it holds.
    for (auto &call : generic_calls) {
      call.call->type_arguments.clear();
      for (const auto &parameter : call.callee->declaration->type_parameters)
        call.call->type_arguments.push_back(
            types.resolve(call.bound.at(parameter.name)));
    }
    for (const auto &[call, slot] : generic_news)
      call->type_argument = types.resolve(slot).without_nothing();
    for (auto &field : fields) {
      if (field.declaration->type.is_known() &&
          !(field.declaration->type.is_list &&
            field.declaration->type.base == base_type::unknown))
        continue;
      unit_index = field.unit_index;
      bool was_defaulted = false;
      field.declaration->type = settle(field.slot, was_defaulted);
      auto owner = describe_in_english(value_type::record_of(field.record));
      owner.front() = 'A';
      note(field.location,
           owner + "'s `" + field.declaration->name + "` is " +
               describe_in_english(field.declaration->type) +
               (was_defaulted ? ", since nothing says otherwise."
                              : ", from the values it is given."));
    }

    for (auto &[name, info] : functions) {
      unit_index = info.unit_index;
      auto &declaration = *info.declaration;
      for (std::size_t index = 0; index < info.parameters.size(); ++index) {
        auto &parameter = declaration.parameters[index];
        if (parameter.type.is_known() &&
            !(parameter.type.is_list &&
              parameter.type.base == base_type::unknown))
          continue;
        bool was_defaulted = false;
        parameter.type = settle(info.parameters[index], was_defaulted);
        parameter.is_inferred = true;
        note(declaration.location,
             "`" + name + "` takes `" + parameter.name + "` as " +
                 describe_in_english(parameter.type) +
                 (was_defaulted ? ", since nothing says otherwise."
                                : ", from how it is used."));
      }
      if (declaration.return_type.is_known())
        continue;
      if (!info.returns_value) {
        declaration.return_type = value_type::nothing();
        continue;
      }
      bool was_defaulted = false;
      declaration.return_type = settle(info.result, was_defaulted);
      note(declaration.location,
           "`" + name + "` gives back " +
               describe_in_english(declaration.return_type) +
               (was_defaulted ? ", since nothing says otherwise."
                              : ", from what it returns."));
    }

    for (auto &variable : variables) {
      unit_index = variable.unit_index;
      auto &declaration = *variable.declaration;
      const auto natural = types.resolve(variable.initial_value);
      bool was_defaulted = false;
      const auto type = settle(variable.variable, was_defaulted);
      if (was_defaulted)
        note(declaration.location,
             "Nothing says what `" + declaration.name + "` holds; it is " +
                 describe_in_english(type) + ".");
      else if (variable.has_initial_value && natural.is_known() &&
               natural != type)
        note(declaration.location,
             "`" + declaration.name + "` holds " + describe_in_english(type) +
                 ", because it is given one later on.");
      declaration.declared_type = type;
    }

    for (auto &list : lists) {
      unit_index = list.unit_index;
      auto element = types.resolve(list.element);
      if (element == value_type::nothing() || element.is_pointer())
        continue; // Not a valid list; the checker explains why.
      if (element.is_record()) {
        std::vector<value_type> kinds;
        for (const auto item : list.items)
          kinds.push_back(types.resolve(item));
        if (std::ranges::any_of(kinds, [&](const auto &p_kind) {
              return p_kind != element;
            })) {
          const auto shared = common_traits(kinds);
          // With several, the first: its value can still be asked for
          // the others' phrases (the checker allows it).
          if (!shared.empty()) {
            element = value_type::record_of(shared.front());
            note(list.list->location,
                 "This list holds " + describe_in_english(element) +
                     "s: its items are all of them.");
          }
        }
      }
      if (!element.is_known()) {
        note(list.list->location,
             "Nothing says what this list holds; it holds whole numbers.");
        element = value_type::integer();
      }
      list.list->type_argument = element;
    }

    // A default method's types were worked out like any function's.
    for (auto &unit : target.units)
      for (auto &trait : unit.traits)
        for (auto &method : trait.methods)
          if (method.has_default)
            if (const auto found = functions.find(method.name);
                found != functions.end()) {
              for (std::size_t index = 0;
                   index < std::min(method.parameters.size(),
                                    found->second.declaration->parameters.size());
                   ++index)
                method.parameters[index].type =
                    found->second.declaration->parameters[index].type;
              method.return_type = found->second.declaration->return_type;
            }

    for (auto &condition : conditions) {
      if (types.resolve(condition.slot) != value_type::integer())
        continue;
      unit_index = condition.unit_index;
      note(condition.condition->location,
           "A number used as a yes-or-no condition means \"is not zero\".");
      expression wrapped{.kind = expression::kind_type::call,
                         .text = "!=",
                         .location = condition.condition->location};
      wrapped.arguments.push_back(std::move(*condition.condition));
      wrapped.arguments.push_back({.kind = expression::kind_type::integer,
                                   .location = wrapped.location});
      *condition.condition = std::move(wrapped);
    }
  }

  // The types a trait fixes are given to the functions that provide its
  // methods before anything is inferred: an impl's method takes the
  // record as its value and the trait's other types.
  auto pin_trait_types() -> void {
    std::map<string_type, function *> by_name;
    for (auto &unit : target.units) {
      for (auto &trait : unit.traits)
        for (const auto &method : trait.methods)
          method_owner.emplace(method.name, &trait);
      for (auto &function : unit.functions)
        by_name.emplace(function.name, &function);
      for (const auto &impl : unit.impls)
        implemented.emplace(impl.trait, impl.type);
    }
    for (const auto &unit : target.units) {
      for (const auto &impl : unit.impls) {
        const auto *const owner = find_trait(target, impl.trait);
        if (owner == nullptr)
          continue;
        for (const auto &name : impl.methods) {
          const auto *const method = owner->find(name);
          const auto found = by_name.find(impl_function_name(name, impl.type));
          if (method == nullptr || method->has_default ||
              found == by_name.end())
            continue;
          auto &function = *found->second;
          for (std::size_t index = 0;
               index < std::min(function.parameters.size(),
                                method->parameters.size());
               ++index)
            if (!function.parameters[index].type.is_known())
              // The first is its record, with an impl for every stack's
              // type variables (parse_impl wrote it).
              function.parameters[index].type =
                  index == 0 || method->is_same(index)
                      ? (function.parameters[0].type.is_known()
                             ? function.parameters[0].type
                             : value_type::record_of(impl.type))
                      : method->parameters[index].type;
          if (!function.return_type.is_known())
            function.return_type = method->return_type;
        }
      }
    }
  }

  // An impl's version of a default method has the default's types.
  auto join_overrides() -> void {
    for (const auto &unit : target.units) {
      for (const auto &impl : unit.impls) {
        const auto *const owner = find_trait(target, impl.trait);
        if (owner == nullptr)
          continue;
        for (const auto &name : impl.methods) {
          const auto *const method = owner->find(name);
          const auto written = functions.find(impl_function_name(name, impl.type));
          const auto base = functions.find(name);
          if (method == nullptr || !method->has_default ||
              written == functions.end() || base == functions.end())
            continue;
          const auto count = std::min(written->second.parameters.size(),
                                      base->second.parameters.size());
          for (std::size_t index = 1; index < count; ++index)
            types.unify(written->second.parameters[index],
                        base->second.parameters[index]);
          types.unify(written->second.result, base->second.result);
        }
      }
    }
  }

  // Records that implement one trait in common: what a list of different
  // records can be a list of.
  auto common_traits(const std::vector<value_type> &p_kinds) const
      -> std::vector<string_type> {
    std::vector<string_type> result;
    for (const auto &[name, owner] : method_owner) {
      (void)name;
      if (std::ranges::find(result, owner->name) != result.end())
        continue;
      const bool all = std::ranges::all_of(p_kinds, [&](const auto &p_kind) {
        return p_kind.is_record() &&
               (p_kind.record == owner->name ||
                implemented.contains({owner->name, p_kind.record}));
      });
      if (all)
        result.push_back(owner->name);
    }
    std::ranges::sort(result);
    return result;
  }

public:
  explicit elaborator(program &p_program) : target(p_program) {}

  auto run() -> std::vector<diagnostic> {
    for (const auto &function : libc_functions())
      c_functions[function.name] = &function;
    for (unit_index = 0; unit_index < target.units.size(); ++unit_index)
      for (const auto &function : target.units[unit_index].externs)
        c_functions[function.name] = &function;
    program_headers = headers_of(target);

    for (unit_index = 0; unit_index < target.units.size(); ++unit_index) {
      for (auto &record : target.units[unit_index].records) {
        if (!record.type_parameters.empty()) {
          auto &generic = types.generic_records[record.name];
          generic.parameters = record.type_parameters;
          for (const auto &field : record.fields)
            generic.fields[field.name] = field.type;
        }
        for (auto &field : record.fields) {
          const auto slot = types.of(field.type, true);
          types.add_record_field(record.name, field.name, slot);
          fields.push_back(
              {&field, record.name, slot, record.location, unit_index});
        }
      }
    }
    pin_trait_types();
    types.set_trait_impls(implemented);
    for (unit_index = 0; unit_index < target.units.size(); ++unit_index) {
      for (auto &function : target.units[unit_index].functions) {
        function_info info{&function, unit_index};
        for (const auto &parameter : function.parameters)
          info.parameters.push_back(types.of(parameter.type, true));
        info.result = types.of(function.return_type, true);
        functions.emplace(function.name, std::move(info));
      }
    }
    join_overrides();
    for (unit_index = 0; unit_index < target.units.size(); ++unit_index)
      for (auto &function : target.units[unit_index].functions)
        add_implicit_return(function);

    scopes.emplace_back();
    for (unit_index = 0; unit_index < target.units.size(); ++unit_index)
      for (auto &statement : target.units[unit_index].statements)
        walk(statement);
    scopes.pop_back();

    for (auto &[name, info] : functions) {
      unit_index = info.unit_index;
      current_function = &info;
      scopes.emplace_back();
      for (std::size_t index = 0; index < info.parameters.size(); ++index)
        scopes.back()[info.declaration->parameters[index].name] =
            info.parameters[index];
      for (auto &statement : info.declaration->body)
        walk(statement);
      scopes.pop_back();
      current_function = nullptr;
    }

    types.solve();
    std::vector<std::pair<inference::trait_hint, string_type>> unclear;
    while (true) {
      unclear.clear();
      if (!types.settle_hints(unclear))
        break;
      types.solve();
    }
    // Macros, as their arguments' types are known: an answer may say more.
    while (ask_macros(false))
      types.solve();
    // A macro whose arguments nothing typed: whole numbers.
    bool guessed = false;
    while (ask_macros(true)) {
      guessed = true;
      types.solve();
    }
    // A generic call whose function's result is still open: as far as it
    // is known.
    for (auto &call : generic_calls)
      guessed = settle_result(call, true) || guessed;
    if (guessed)
      types.solve();
    for (const auto &[hint, names] : unclear)
      problems.push_back(
          {hint.unit_index, hint.location,
           "The phrases used on this value belong to the traits '" + names +
               "', so Ovis can't tell which one it is: write its type."});
    write_back();
    add_macro_copies();
    std::ranges::stable_sort(notes, [](const auto &p_left, const auto &p_right) {
      return std::pair(p_left.unit_index, p_left.location.line) <
             std::pair(p_right.unit_index, p_right.location.line);
    });
    return std::move(notes);
  }

  auto take_problems() -> std::vector<diagnostic> {
    return std::move(problems);
  }
};

} // namespace

auto closest_name(string_view_type p_name,
                  const std::vector<string_type> &p_candidates)
    -> std::optional<string_type> {
  const auto key = loose(p_name);
  if (key.empty())
    return std::nullopt;
  const auto limit = key.size() <= 3 ? 0 : key.size() <= 6 ? 1 : 2;

  std::optional<string_type> best;
  std::size_t best_distance = SIZE_MAX;
  bool is_unique = false;
  for (const auto &candidate : p_candidates) {
    if (candidate == p_name)
      continue;
    const auto distance = edit_distance(key, loose(candidate));
    if (distance > static_cast<std::size_t>(limit) &&
        !(distance == 0 && limit == 0))
      continue;
    if (distance < best_distance) {
      best = candidate;
      best_distance = distance;
      is_unique = true;
    } else if (distance == best_distance) {
      is_unique = false;
    }
  }
  return is_unique ? best : std::nullopt;
}

auto elaborate(program &p_program, std::vector<diagnostic> *p_problems)
    -> std::vector<diagnostic> {
  elaborator work(p_program);
  auto notes = work.run();
  if (p_problems != nullptr)
    *p_problems = work.take_problems();
  return notes;
}

} // namespace ovis::core

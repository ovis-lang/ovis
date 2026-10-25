#include <algorithm>
#include <deque>
#include <functional>
#include <set>

#include <ovis/core/monomorphise.hpp>

namespace ovis::core {

namespace {

// Core text with each `(var NAME)` replaced: a pointer's pointee, a
// phrase's signature, a generic record's types.
auto substitute_text(const string_type &p_text, const type_bindings &p_bindings)
    -> string_type {
  if (p_text.find("(var ") == string_type::npos)
    return p_text;
  const std::function<sexpr(const sexpr &)> replace = [&](const sexpr &p_form) {
    if (p_form.is_list() && p_form.items.size() == 2 && p_form.head() == "var" &&
        p_form.items[1].is_symbol())
      if (const auto found = p_bindings.find(p_form.items[1].text);
          found != p_bindings.end())
        return read_sexprs(found->second.to_string()).front();
    auto result = p_form;
    for (auto &item : result.items)
      item = replace(item);
    return result;
  };
  string_type result;
  for (const auto &form : read_sexprs(p_text))
    result += (result.empty() ? "" : " ") + replace(form).to_string();
  return result;
}

auto substitute(expression &p_expression, const type_bindings &p_bindings)
    -> void {
  p_expression.type = substitute(p_expression.type, p_bindings);
  if (p_expression.type_argument)
    p_expression.type_argument = substitute(*p_expression.type_argument, p_bindings);
  for (auto &type : p_expression.type_arguments)
    type = substitute(type, p_bindings);
  // `(load (var item) p)`: what is at a pointer, written as Core text.
  for (auto &field : p_expression.fields)
    field = substitute_text(field, p_bindings);
  for (auto &argument : p_expression.arguments)
    substitute(argument, p_bindings);
}

auto substitute(std::vector<statement> &p_body, const type_bindings &p_bindings)
    -> void {
  for (auto &statement : p_body) {
    if (statement.declared_type)
      statement.declared_type = substitute(*statement.declared_type, p_bindings);
    for (auto &value : statement.values)
      substitute(value, p_bindings);
    substitute(statement.body, p_bindings);
    substitute(statement.else_body, p_bindings);
  }
}

} // namespace

auto substitute(const value_type &p_type, const type_bindings &p_bindings)
    -> value_type {
  if (!p_type.has_variables())
    return p_type;
  if (p_type.is_variable()) {
    const auto found = p_bindings.find(p_type.record);
    return found != p_bindings.end() ? found->second : p_type;
  }
  if (p_type.is_list)
    return value_type::list_of(substitute(p_type.element(), p_bindings));
  if (p_type.is_map())
    return value_type::map_of(substitute(p_type.map_key(), p_bindings),
                              substitute(p_type.map_value(), p_bindings));
  if (p_type.is_record()) {
    std::vector<value_type> arguments;
    for (const auto &argument : p_type.record_arguments())
      arguments.push_back(substitute(argument, p_bindings));
    auto result = value_type::record_of(p_type.record, arguments);
    result.nullable = p_type.nullable;
    return result;
  }
  auto result = p_type;
  result.pointee = substitute_text(p_type.pointee, p_bindings);
  // `(many (var item))` for whole numbers is `(many int)`: a C number's
  // own spelling where there is one.
  if (result.is_typed_pointer() && !result.is_phrase())
    if (const auto forms = read_sexprs(result.pointee); forms.size() == 1)
      if (const auto canonical = parse_pointee(forms.front()))
        result.pointee = *canonical;
  return result;
}

auto substitute(function &p_function, const type_bindings &p_bindings) -> void {
  for (auto &parameter : p_function.parameters)
    parameter.type = substitute(parameter.type, p_bindings);
  p_function.return_type = substitute(p_function.return_type, p_bindings);
  substitute(p_function.body, p_bindings);
}

auto match_type(const value_type &p_generic, const value_type &p_given,
                type_bindings &p_bindings) -> bool {
  if (!p_generic.has_variables())
    return true;
  if (p_generic.is_variable()) {
    if (!p_given.is_known())
      return true;
    const auto [found, added] = p_bindings.emplace(p_generic.record, p_given);
    return added || found->second == p_given;
  }
  if (p_generic.is_list) {
    if (!p_given.is_list)
      return false;
    return match_type(p_generic.element(), p_given.element(), p_bindings);
  }
  if (p_generic.is_map()) {
    if (!p_given.is_map())
      return false;
    return match_type(p_generic.map_key(), p_given.map_key(), p_bindings) &&
           match_type(p_generic.map_value(), p_given.map_value(), p_bindings);
  }
  if (p_generic.is_record()) {
    if (!p_given.is_record() || p_given.record != p_generic.record)
      return false;
    const auto generic = p_generic.record_arguments();
    const auto given = p_given.record_arguments();
    if (generic.size() != given.size())
      return false;
    for (std::size_t index = 0; index < generic.size(); ++index)
      if (!match_type(generic[index], given[index], p_bindings))
        return false;
    return true;
  }
  // A phrase of type variables' values: the given one's parameters and
  // result.
  if (p_generic.is_phrase()) {
    if (!p_given.is_phrase())
      return false;
    const auto generic = read_signature(p_generic);
    const auto given = read_signature(p_given);
    if (!generic || !given ||
        generic->parameters.size() != given->parameters.size())
      return false;
    for (std::size_t index = 0; index < generic->parameters.size(); ++index)
      if (!match_type(generic->parameters[index], given->parameters[index],
                      p_bindings))
        return false;
    return match_type(generic->result, given->result, p_bindings);
  }
  // A pointer to a type variable's values: what the given one points at.
  if (p_generic.is_typed_pointer() && p_given.is_typed_pointer()) {
    const auto generic = read_sexprs(p_generic.pointee);
    const auto given = read_sexprs(p_given.pointee);
    if (generic.size() != 1 || given.size() != 1)
      return false;
    const auto inner = value_type::parse(generic.front());
    auto other = value_type::parse(given.front());
    if (!other)
      if (const auto pointee = read_pointee(p_given.pointee))
        other = pointee->kind == pointee_type::kind_type::number
                    ? value_type::number(pointee->number)
                    : pointee_value(*pointee);
    return inner && other && match_type(*inner, *other, p_bindings);
  }
  return true;
}

auto mangle(const value_type &p_type) -> string_type {
  if (p_type.is_list)
    return "list_" + mangle(p_type.element());
  if (p_type.is_map())
    return "map_" + mangle(p_type.map_key()) + "_" + mangle(p_type.map_value());
  if (p_type.is_sized())
    return p_type.pointee;
  switch (p_type.base) {
  case base_type::nothing:
    return "void";
  case base_type::integer:
    return "int";
  case base_type::real:
    return "float";
  case base_type::boolean:
    return "bool";
  case base_type::string:
    return "str";
  case base_type::memory:
    return "memory";
  case base_type::unknown:
    return "unknown";
  case base_type::variable:
    return "var_" + p_type.record;
  case base_type::map:
    break;
  case base_type::record: {
    string_type result = (p_type.nullable ? "maybe_record_" : "record_") +
                         p_type.record;
    for (const auto &argument : p_type.record_arguments())
      result += "__" + mangle(argument);
    return result;
  }
  case base_type::pointer: {
    if (p_type.kind == pointer_kind::raw)
      return "ptr";
    string_type result = p_type.kind == pointer_kind::many ? "many_"
                         : p_type.kind == pointer_kind::c  ? "cptr_"
                                                           : "ptr_";
    for (const auto character : p_type.pointee)
      if (std::isalnum(static_cast<unsigned char>(character)) != 0)
        result.push_back(character);
      else if (character == ' ')
        result.push_back('_');
    return result;
  }
  }
  return "unknown";
}

auto copy_name(string_view_type p_name, const std::vector<value_type> &p_types)
    -> string_type {
  string_type result(p_name);
  for (const auto &type : p_types) {
    auto part = mangle(type);
    // A record of the program is its name: `push__book`, `stack__int`.
    if (type.is_record() && !type.nullable && part.starts_with("record_"))
      part = part.substr(7);
    result += "__" + part;
  }
  return result;
}

namespace {

auto for_each_type(expression &p_expression,
                   const std::function<void(value_type &)> &p_visit) -> void {
  p_visit(p_expression.type);
  if (p_expression.type_argument)
    p_visit(*p_expression.type_argument);
  for (auto &type : p_expression.type_arguments)
    p_visit(type);
  for (auto &argument : p_expression.arguments)
    for_each_type(argument, p_visit);
}

// A type's generic records, wherever they are in it: a list's items, a
// map's keys and values, what a pointer points at.
auto rename_records(value_type &p_type,
                    const std::function<string_type(const value_type &)> &p_name)
    -> void {
  if (p_type.base == base_type::record && !p_type.arguments.empty() &&
      !p_type.has_variables()) {
    // Only the record's own part: `(list (record stack int))` keeps its list.
    auto own = p_type;
    own.is_list = false;
    own.inner_lists = 0;
    own.nullable = false;
    p_type.record = p_name(own);
    p_type.arguments.clear();
    return;
  }
  const auto rename_text = [&](string_type &p_text) {
    if (p_text.find("(record ") == string_type::npos)
      return;
    const std::function<sexpr(const sexpr &)> walk = [&](const sexpr &p_form) {
      if (p_form.is_list() && p_form.head() == "record" && p_form.items.size() > 2)
        if (auto type = value_type::parse(p_form)) {
          rename_records(*type, p_name);
          return read_sexprs(type->to_string()).front();
        }
      auto result = p_form;
      for (auto &item : result.items)
        item = walk(item);
      return result;
    };
    string_type result;
    for (const auto &form : read_sexprs(p_text))
      result += (result.empty() ? "" : " ") + walk(form).to_string();
    p_text = result;
  };
  if (p_type.is_map() || p_type.is_pointer())
    rename_text(p_type.pointee);
}

} // namespace

auto for_each_type(std::vector<statement> &p_body,
                   const std::function<void(value_type &)> &p_visit) -> void {
  for (auto &statement : p_body) {
    if (statement.declared_type)
      p_visit(*statement.declared_type);
    for (auto &value : statement.values)
      for_each_type(value, p_visit);
    for_each_type(statement.body, p_visit);
    for_each_type(statement.else_body, p_visit);
  }
}

auto for_each_type(function &p_function,
                   const std::function<void(value_type &)> &p_visit) -> void {
  for (auto &parameter : p_function.parameters)
    p_visit(parameter.type);
  p_visit(p_function.return_type);
  for_each_type(p_function.body, p_visit);
}

namespace {

auto has_field_parameter(const function &p_function) -> bool {
  return std::ranges::any_of(p_function.type_parameters, &type_parameter::is_field);
}

// A call's name with each part (split at `:` and `__`) that names a type
// variable or field replaced: `prelude_sort_by:r:f`, `comes_before__T`.
auto rename_parts(const string_type &p_name,
                  const std::map<string_type, string_type> &p_parts)
    -> string_type {
  string_type result;
  string_type part;
  const auto flush = [&] {
    const auto found = p_parts.find(part);
    result += found != p_parts.end() ? found->second : part;
    part.clear();
  };
  for (std::size_t at = 0; at < p_name.size(); ++at) {
    if (p_name[at] == ':') {
      flush();
      result += ':';
    } else if (p_name.compare(at, 2, "__") == 0) {
      flush();
      result += "__";
      ++at;
    } else {
      part.push_back(p_name[at]);
    }
  }
  flush();
  return result;
}

auto rename_fields(std::vector<statement> &p_body,
                   const std::map<string_type, string_type> &p_parts) -> void;

auto rename_fields(expression &p_expression,
                   const std::map<string_type, string_type> &p_parts) -> void {
  if (p_expression.kind == expression::kind_type::call) {
    p_expression.text = rename_parts(p_expression.text, p_parts);
    if (p_expression.text == "field" || p_expression.text == "new")
      for (auto &field : p_expression.fields)
        if (const auto found = p_parts.find(field); found != p_parts.end())
          field = found->second;
  }
  for (auto &argument : p_expression.arguments)
    rename_fields(argument, p_parts);
}

auto rename_fields(std::vector<statement> &p_body,
                   const std::map<string_type, string_type> &p_parts) -> void {
  for (auto &statement : p_body) {
    if (statement.kind == statement::kind_type::set_field)
      if (const auto found = p_parts.find(statement.name); found != p_parts.end())
        statement.name = found->second;
    for (auto &value : statement.values)
      rename_fields(value, p_parts);
    rename_fields(statement.body, p_parts);
    rename_fields(statement.else_body, p_parts);
  }
}

} // namespace

auto copy_field_generics(program &p_program) -> std::vector<diagnostic> {
  std::vector<diagnostic> problems;
  // Every generic may be called so; one with a field may only be.
  std::map<string_type, std::pair<function, std::size_t>> generics;
  for (std::size_t index = 0; index < p_program.units.size(); ++index)
    for (const auto &each : p_program.units[index].functions)
      if (each.is_generic())
        generics.try_emplace(each.name, each, index);
  if (std::ranges::none_of(generics, [](const auto &p_entry) {
        return has_field_parameter(p_entry.second.first);
      }))
    return problems;
  std::set<string_type> made;
  // Copies to look through, as (unit, function name).
  std::deque<std::pair<std::size_t, string_type>> waiting;
  const std::function<void(expression &, std::size_t)> visit =
      [&](expression &p_expression, std::size_t p_unit) {
        for (auto &argument : p_expression.arguments)
          visit(argument, p_unit);
        if (p_expression.kind != expression::kind_type::call)
          return;
        const auto colon = p_expression.text.find(':');
        if (colon == string_type::npos)
          return;
        const auto found = generics.find(p_expression.text.substr(0, colon));
        if (found == generics.end())
          return;
        const auto &[generic, unit] = found->second;
        std::vector<string_type> given;
        for (auto at = colon; at != string_type::npos;) {
          const auto next = p_expression.text.find(':', at + 1);
          given.push_back(p_expression.text.substr(at + 1, next == string_type::npos
                                                               ? string_type::npos
                                                               : next - at - 1));
          at = next;
        }
        if (given.size() != generic.type_parameters.size()) {
          problems.push_back({p_unit, p_expression.location,
                              "'" + generic.name + "' takes " +
                                  std::to_string(generic.type_parameters.size()) +
                                  " records and fields after its name."});
          return;
        }
        string_type name = generic.name;
        for (const auto &part : given)
          name += "__" + part;
        p_expression.text = name;
        if (!made.insert(name).second)
          return;
        auto copy = generic;
        type_bindings bindings;
        std::map<string_type, string_type> parts;
        for (std::size_t index = 0; index < given.size(); ++index) {
          const auto &parameter = generic.type_parameters[index];
          parts[parameter.name] = given[index];
          if (!parameter.is_field)
            bindings[parameter.name] = value_type::record_of(given[index]);
        }
        substitute(copy, bindings);
        rename_fields(copy.body, parts);
        copy.name = name;
        copy.type_parameters.clear();
        p_program.units[unit].functions.push_back(std::move(copy));
        waiting.emplace_back(unit, name);
      };
  const std::function<void(std::vector<statement> &, std::size_t)> visit_body =
      [&](std::vector<statement> &p_body, std::size_t p_unit) {
        for (auto &statement : p_body) {
          for (auto &value : statement.values)
            visit(value, p_unit);
          visit_body(statement.body, p_unit);
          visit_body(statement.else_body, p_unit);
        }
      };
  // Copies are added to the units as they are made, so a body is looked
  // through out of its function and put back by name.
  const auto visit_function = [&](std::size_t p_unit, const string_type &p_name) {
    auto &functions = p_program.units[p_unit].functions;
    const auto at = std::ranges::find(functions, p_name, &function::name);
    if (at == functions.end())
      return;
    auto body = std::move(at->body);
    visit_body(body, p_unit);
    std::ranges::find(p_program.units[p_unit].functions, p_name, &function::name)
        ->body = std::move(body);
  };
  for (std::size_t index = 0; index < p_program.units.size(); ++index) {
    std::vector<string_type> names;
    for (const auto &each : p_program.units[index].functions)
      names.push_back(each.name);
    // A generic with a field is looked through in its copies.
    for (const auto &name : names)
      if (!made.contains(name) &&
          !(generics.contains(name) && has_field_parameter(generics.at(name).first)))
        visit_function(index, name);
    auto statements = std::move(p_program.units[index].statements);
    visit_body(statements, index);
    p_program.units[index].statements = std::move(statements);
  }
  while (!waiting.empty()) {
    const auto [unit, name] = waiting.front();
    waiting.pop_front();
    visit_function(unit, name);
  }
  for (auto &unit : p_program.units)
    std::erase_if(unit.functions, has_field_parameter);
  return problems;
}

auto make_record_copies(program &p_program) -> std::vector<diagnostic> {
  std::vector<diagnostic> problems;
  // The generic records, and the unit each is in.
  std::map<string_type, std::pair<const record *, std::size_t>> generics;
  for (std::size_t index = 0; index < p_program.units.size(); ++index)
    for (const auto &record : p_program.units[index].records)
      if (!record.type_parameters.empty())
        generics.emplace(record.name, std::pair{&record, index});
  // The copies, made as they are met; their fields may meet more.
  std::map<string_type, std::pair<record, std::size_t>> made;
  std::deque<string_type> waiting;
  const std::function<string_type(const value_type &)> name_of =
      [&](const value_type &p_use) -> string_type {
    const auto found = generics.find(p_use.record);
    const auto arguments = p_use.record_arguments();
    const auto name = copy_name(p_use.record, arguments);
    if (found == generics.end() || made.contains(name))
      return name;
    const auto &[generic, unit] = found->second;
    type_bindings bindings;
    for (std::size_t index = 0;
         index < std::min(arguments.size(), generic->type_parameters.size());
         ++index)
      bindings[generic->type_parameters[index]] = arguments[index];
    auto copy = *generic;
    copy.name = name;
    copy.copy_of = generic->name;
    copy.type_parameters.clear();
    for (auto &field : copy.fields)
      field.type = substitute(field.type, bindings);
    auto said = describe_in_english(p_use);
    record_descriptions()[name] = said.substr(said.find(' ') + 1);
    made.emplace(name, std::pair{std::move(copy), unit});
    waiting.push_back(name);
    return name;
  };
  const auto visit = [&](value_type &p_type) { rename_records(p_type, name_of); };

  for (auto &unit : p_program.units) {
    for (auto &function : unit.functions)
      if (!function.is_generic())
        for_each_type(function, visit);
    for_each_type(unit.statements, visit);
    for (auto &record : unit.records)
      if (record.type_parameters.empty())
        for (auto &field : record.fields)
          visit(field.type);
    for (auto &trait : unit.traits)
      for (auto &method : trait.methods) {
        for (auto &parameter : method.parameters)
          visit(parameter.type);
        visit(method.return_type);
      }
  }
  while (!waiting.empty()) {
    auto &copy = made.at(waiting.front()).first;
    waiting.pop_front();
    for (auto &field : copy.fields)
      visit(field.type);
  }

  // An impl for every stack is an impl of each copy whose phrases were made
  // (the checker made those whose bounds its types meet).
  std::set<string_type> functions;
  for (const auto &unit : p_program.units)
    for (const auto &function : unit.functions)
      functions.insert(function.name);
  for (auto &unit : p_program.units) {
    std::vector<impl> added;
    for (const auto &each : unit.impls) {
      if (each.type_parameters.empty())
        continue;
      for (const auto &[name, copy] : made) {
        if (copy.first.copy_of != each.type)
          continue;
        const bool complete = std::ranges::all_of(each.methods, [&](const auto &p_method) {
          return functions.contains(impl_function_name(p_method, name));
        });
        if (!complete)
          continue;
        auto instance = each;
        instance.type = name;
        instance.type_parameters.clear();
        added.push_back(std::move(instance));
      }
    }
    std::erase_if(unit.impls, [](const impl &p_impl) {
      return !p_impl.type_parameters.empty();
    });
    unit.impls.insert(unit.impls.end(), added.begin(), added.end());
  }
  for (auto &[name, copy] : made)
    p_program.units[copy.second].records.push_back(std::move(copy.first));
  for (auto &unit : p_program.units) {
    std::erase_if(unit.records, [](const record &p_record) {
      return !p_record.type_parameters.empty();
    });
    std::erase_if(unit.functions,
                  [](const function &p_function) { return p_function.is_generic(); });
  }
  return problems;
}

} // namespace ovis::core

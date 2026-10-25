#include <algorithm>
#include <cctype>
#include <numeric>
#include <set>

#include "questions.hpp"

namespace ovis::phrase::internal {

namespace {

// How alike two words are, for ranking which template an unknown phrase
// might mean: whole words, then a shared start, then shared trigrams.
auto word_similarity(string_view_type p_left, string_view_type p_right)
    -> double {
  if (same_word(p_left, p_right))
    return 3;
  const auto left = lower(p_left);
  const auto right = lower(p_right);
  std::size_t prefix = 0;
  while (prefix < left.size() && prefix < right.size() &&
         left[prefix] == right[prefix])
    ++prefix;
  if (prefix >= 4)
    return 2;
  std::set<string_type> left_trigrams;
  std::set<string_type> right_trigrams;
  for (std::size_t index = 0; index + 3 <= left.size(); ++index)
    left_trigrams.insert(left.substr(index, 3));
  for (std::size_t index = 0; index + 3 <= right.size(); ++index)
    right_trigrams.insert(right.substr(index, 3));
  std::size_t shared = 0;
  for (const auto &trigram : left_trigrams)
    shared += right_trigrams.contains(trigram);
  const auto all = left_trigrams.size() + right_trigrams.size() - shared;
  return all > 0 && double(shared) / double(all) >= 0.4 ? 1 : 0;
}

// A type of number misspelled (`int23`, `unit8`): the names of number
// types within two edits of the word (numbers.md §6).
auto number_type_names_like(string_view_type p_word) -> std::vector<string_type> {
  static const std::vector<string_type> names = {
      "int8",   "uint8",  "int16",   "uint16",  "int32", "uint32",
      "int64",  "uint64", "float32", "float64", "byte",  "short"};
  const auto word = lower(p_word);
  std::vector<string_type> result;
  if (word.size() < 4)
    return result;
  for (const auto &name : names) {
    std::vector<std::size_t> row(name.size() + 1);
    for (std::size_t column = 0; column < row.size(); ++column)
      row[column] = column;
    for (std::size_t line = 1; line <= word.size(); ++line) {
      auto diagonal = row[0];
      row[0] = line;
      for (std::size_t column = 1; column <= name.size(); ++column) {
        const auto above = row[column];
        row[column] = std::min({row[column] + 1, row[column - 1] + 1,
                                diagonal + (word[line - 1] == name[column - 1] ? 0 : 1)});
        diagonal = above;
      }
    }
    if (row.back() > 0 && row.back() <= 2)
      result.push_back(name);
  }
  return result;
}

// The words of a reading, outside its texts.
auto reading_words(const string_type &p_reading) -> std::vector<string_type> {
  std::vector<string_type> result;
  string_type word;
  bool in_text = false;
  for (const auto character : p_reading + " ") {
    if (character == '"')
      in_text = !in_text;
    if (!in_text && (std::isalnum(static_cast<unsigned char>(character)) != 0 ||
                     character == '_')) {
      word.push_back(character);
    } else if (!word.empty()) {
      result.push_back(std::move(word));
      word.clear();
    }
  }
  return result;
}

// Where the words of `p_name` (`fan speed`) are in `p_tokens`, or npos.
auto find_words(const std::vector<token> &p_tokens, const string_type &p_name)
    -> std::size_t {
  std::vector<string_type> words;
  for (std::size_t begin = 0; begin <= p_name.size();) {
    auto end = p_name.find(' ', begin);
    if (end == string_type::npos)
      end = p_name.size();
    words.push_back(p_name.substr(begin, end - begin));
    begin = end + 1;
  }
  for (std::size_t at = 0; at + words.size() <= p_tokens.size(); ++at) {
    bool matches = true;
    for (std::size_t index = 0; index < words.size() && matches; ++index)
      matches = p_tokens[at + index].kind == token::kind_type::word &&
                p_tokens[at + index].text == words[index];
    if (matches)
      return at;
  }
  return string_type::npos;
}

auto is_function_word(string_view_type p_word) -> bool {
  static const std::set<string_view_type> words = {
      "of", "to", "in", "from", "by", "for", "with", "and", "a",
      "an", "the", "is", "at", "on", "as", "into", "it"};
  return words.contains(lower(p_word));
}

// A phrase's own words, in every wording and branch.
auto phrase_words(const phrase_template &p_phrase) -> std::vector<string_type> {
  std::vector<string_type> result;
  for (const auto &pattern : p_phrase.patterns)
    for (const auto &element : pattern) {
      if (element.kind == pattern_element::kind_type::word)
        result.push_back(element.word);
      for (const auto &branch : element.branches)
        for (const auto &part : branch)
          result.push_back(part.word);
    }
  return result;
}

// The words of meaning a reading adds to the span: its words less one of
// each argument's, function words and words like one of the span's.
auto added_words(const string_type &p_reading,
                 const std::vector<parse> &p_arguments,
                 const std::vector<string_type> &p_verb) -> std::size_t {
  auto words = reading_words(p_reading);
  for (const auto &item : p_arguments)
    for (const auto &word : reading_words(item.reading))
      if (const auto at = std::ranges::find(words, word); at != words.end())
        words.erase(at);
  return static_cast<std::size_t>(
      std::ranges::count_if(words, [&](const string_type &p_word) {
        return !is_function_word(p_word) &&
               std::ranges::none_of(p_verb, [&](const string_type &p_other) {
                 return word_similarity(p_word, p_other) >= 2;
               });
      }));
}

} // namespace

// How much lower a reading ranks for using the arguments in another order
// than written.
constexpr double order_penalty = 0.1;

auto unknown_candidates(const std::vector<token> &p_tokens, std::size_t p_begin,
                        std::size_t p_end, bool p_statement, bool p_block,
                        const std::map<string_type, value_kind> &p_scope,
                        const std::vector<string_type> &p_recent,
                        const vocabulary &p_vocabulary,
                        const std::function<bool(const string_type &)> &p_fits)
    -> std::vector<candidate> {
  line_parser parser(p_tokens, p_scope, p_vocabulary, false);
  std::vector<parse> arguments;
  std::vector<string_type> verb;
  // Names that are also plain words (`how many items in items`): each may
  // be part of the verb instead.
  std::vector<std::pair<std::size_t, string_type>> name_words;
  for (auto index = p_begin; index < p_end; ++index) {
    const auto &item = p_tokens[index];
    if (is_article(item) || item.is_symbol(","))
      continue;
    if (item.is_symbol("(") || item.is_symbol("[")) {
      auto close = index;
      for (int level = 0; close < p_end; ++close) {
        level += p_tokens[close].is_symbol("(") || p_tokens[close].is_symbol("[");
        level -= p_tokens[close].is_symbol(")") || p_tokens[close].is_symbol("]");
        if (level == 0)
          break;
      }
      const auto &inside = parser.expressions(index, close + 1);
      if (!inside.empty())
        arguments.push_back(inside.front());
      index = close;
      continue;
    }
    // A minus before a value is part of it (`how far -5 is from zero`),
    // unless it follows a value (`3 - 5`).
    if (item.is_symbol("-") && index + 1 < p_end &&
        (index == p_begin || parser.expressions(index - 1, index).empty())) {
      const auto &negative = parser.expressions(index, index + 2);
      if (!negative.empty()) {
        arguments.push_back(negative.front());
        ++index;
        continue;
      }
    }
    // A name of several words is one argument (`split some line by ","`
    // is `some line split by ","`), the longest one here, or its words
    // would be read apart (`line` as `ask for a line`).
    std::size_t name_length = 0;
    for (const auto &[name, type] : p_scope) {
      const auto length = static_cast<std::size_t>(std::ranges::count(name, ' ') + 1);
      if (length > 1 && length > name_length && index + length <= p_end &&
          find_words({p_tokens.begin() + static_cast<std::ptrdiff_t>(index),
                      p_tokens.begin() + static_cast<std::ptrdiff_t>(index + length)},
                     name) == 0)
        name_length = length;
    }
    if (name_length > 0) {
      const auto &whole = parser.expressions(index, index + name_length);
      if (!whole.empty()) {
        arguments.push_back(whole.front());
        index += name_length - 1;
        continue;
      }
    }
    const auto &single = parser.expressions(index, index + 1);
    if (!single.empty()) {
      if (item.kind == token::kind_type::word && p_scope.contains(item.text))
        name_words.emplace_back(arguments.size(), item.text);
      arguments.push_back(single.front());
    } else if (item.kind == token::kind_type::word) {
      verb.push_back(item.text);
    }
  }

  // The arguments as written, and with one more: a recent name the line may
  // have left out (`show rounded to 2 decimals`), offered only when it ranks
  // above every reading of the arguments as written. Each set has the verb words it
  // leaves over and how much lower it ranks.
  struct argument_set {
    std::vector<parse> arguments;
    std::vector<string_type> verb;
    double penalty = 0;
    // A name read as one of the phrase's words, which must then be one.
    string_type as_word;
  };
  std::vector<argument_set> argument_sets = {{arguments, verb, 0, {}}};
  if (name_words.size() > 1)
    for (const auto &[position, word] : name_words) {
      auto fewer = arguments;
      fewer.erase(fewer.begin() + static_cast<std::ptrdiff_t>(position));
      auto more_verb = verb;
      more_verb.push_back(word);
      argument_sets.push_back({std::move(fewer), std::move(more_verb), 0.5, word});
    }
  for (std::size_t index = 0; index < p_recent.size() && index < 3; ++index) {
    const auto found = p_scope.find(p_recent[index]);
    if (found == p_scope.end())
      continue;
    auto more = arguments;
    more.push_back({.core = core_name(found->first),
                    .reading = found->first,
                    .type = found->second});
    argument_sets.push_back({std::move(more), verb, 1, {}});
  }

  // A reading that leaves out what the line is about is never offered, so
  // a question can't settle on one that type-checks but means something
  // else: a field the line names (`set winnie's age to 13` isn't `show
  // winnie, 13`), or, for a phrase taking any number of values (`show`),
  // every word besides them (`remove x from xs` isn't `show x, xs`).
  const auto drops_meaning = [&](const string_type &p_reading,
                                 const std::vector<string_type> &p_verb,
                                 double p_score, bool p_takes_any) {
    if (p_takes_any && p_score <= 0 &&
        std::ranges::any_of(p_verb, [](const string_type &p_word) {
          return !is_function_word(p_word);
        }))
      return true;
    const auto words = reading_words(p_reading);
    return std::ranges::any_of(p_verb, [&](const string_type &p_word) {
      return p_vocabulary.is_field_word(p_word) &&
             std::ranges::none_of(words, [&](const string_type &p_other) {
               return same_word(p_word, p_other);
             });
    });
  };

  std::vector<candidate> result;
  std::set<string_type> seen;
  std::set<string_type> left_out;
  // Readings with the arguments in another order than written: lower only
  // among equals, never cut for it (`3 goes into n` is `n is divisible
  // by 3`).
  std::set<string_type> reordered;
  for (const auto &[arguments, verb, penalty, as_word] : argument_sets)
  for (const auto &item : p_vocabulary.templates) {
    if (!p_vocabulary.sees(item.package))
      continue;
    if ((item.kind == phrase_template::kind_type::statement) != p_statement ||
        (p_statement && item.takes_block != p_block))
      continue;
    std::vector<std::size_t> slots;
    for (const auto &element : item.patterns[0])
      if (element.kind == pattern_element::kind_type::slot)
        slots.push_back(element.slot);
    // Types and fields are words, not arguments.
    if (std::ranges::any_of(slots, [&](std::size_t p_slot) {
          return is_type_slot_name(item.slots[p_slot].type.name) ||
                 item.slots[p_slot].type.kind == type_name::kind_type::field;
        }))
      continue;
    const auto variadic = !slots.empty() && item.slots[slots.back()].is_variadic;
    // A left-out argument fills a fixed slot, not one more of a list.
    if ((penalty > 0 && variadic) || arguments.size() < slots.size() ||
        (!variadic && arguments.size() != slots.size()) || arguments.size() > 4)
      continue;

    double score = 0;
    // The part of the score words of meaning gave (not `a`, `of`, `the`).
    double content_score = 0;
    bool complete = true;
    for (const auto &word : verb) {
      double best = 0;
      for (const auto &alternative : item.patterns)
        for (const auto &element : alternative) {
          std::vector<string_type> words;
          if (element.kind == pattern_element::kind_type::word)
            words.push_back(element.word);
          for (const auto &branch : element.branches)
            for (const auto &part : branch)
              words.push_back(part.word);
          for (const auto &candidate_word : words)
            best = std::max(best, word_similarity(word, candidate_word) *
                                      (is_function_word(word) ? 0.2 : 1.0));
        }
      score += best;
      if (!is_function_word(word))
        content_score += best;
      complete = complete && (best > 0 || is_function_word(word));
      // `reverse people` for `remove winnie from people` leaves winnie out.
      if (word == as_word && best <= 0)
        complete = false, score = -1e9;
    }
    if (score < -1e8)
      continue;
    // A dependency's phrase is offered only if it shares a word of meaning
    // with the span (phrase-scope.md S3): synonyms are the library's, which
    // is measured; unrelated package phrases would fill the 9 shown.
    if (p_vocabulary.is_dependency(item.package)) {
      const auto words = phrase_words(item);
      if (std::ranges::none_of(verb, [&](const string_type &p_word) {
            return !is_function_word(p_word) &&
                   std::ranges::any_of(words, [&](const string_type &p_other) {
                     return word_similarity(p_word, p_other) >= 2;
                   });
          }))
        continue;
    }
    // The wording the line is closest to is the one offered: `number times
    // 2`, not `number multiplied by 2`, when the line says `times`.
    std::size_t chosen = 0;
    {
      double chosen_score = -1;
      for (std::size_t index = 0; index < item.patterns.size(); ++index) {
        double total = 0;
        for (const auto &word : verb) {
          if (is_function_word(word))
            continue;
          double best = 0;
          for (const auto &element : item.patterns[index]) {
            std::vector<string_type> words;
            if (element.kind == pattern_element::kind_type::word)
              words.push_back(element.word);
            for (const auto &branch : element.branches)
              for (const auto &part : branch)
                words.push_back(part.word);
            for (const auto &candidate_word : words)
              best = std::max(best, word_similarity(word, candidate_word));
          }
          total += best;
        }
        if (total > chosen_score) {
          chosen_score = total;
          chosen = index;
        }
      }
    }
    const auto &pattern = item.patterns[chosen];
    if (chosen != 0) {
      slots.clear();
      for (const auto &element : pattern)
        if (element.kind == pattern_element::kind_type::slot)
          slots.push_back(element.slot);
    }
    // A phrase that takes anything (`free x`, `keep x`) fits every line:
    // offered only when a word of the line is like one of its own.
    if (content_score <= 0 && !slots.empty() &&
        std::ranges::all_of(slots, [&](std::size_t p_slot) {
          const auto &type = item.slots[p_slot].type;
          return type.list_depth == 0 &&
                 ((type.kind == type_name::kind_type::builtin &&
                   type.name == "any") ||
                  (type.kind == type_name::kind_type::variable &&
                   type.bound.empty()));
        }))
      continue;

    std::vector<std::size_t> order(arguments.size());
    std::iota(order.begin(), order.end(), 0);
    do {
      slot_fills fills;
      fills.values.resize(item.slots.size());
      fills.types.resize(item.slots.size());
      for (std::size_t index = 0; index < order.size(); ++index) {
        const auto slot = slots[std::min(index, slots.size() - 1)];
        fills.values[slot].push_back(arguments[order[index]]);
      }
      if (const auto built = build_phrase(item, pattern, fills, p_vocabulary))
        if (!drops_meaning(built->reading, verb, score, variadic && arguments.size() > 1) &&
            seen.insert(built->reading).second && p_fits(built->reading)) {
          if (penalty >= 1)
            left_out.insert(built->reading);
          const bool as_written = std::ranges::is_sorted(order);
          if (!as_written)
            reordered.insert(built->reading);
          // The order as written comes first among equals.
          result.push_back({built->reading,
                            score - penalty - (as_written ? 0 : order_penalty),
                            complete && penalty == 0,
                            added_words(built->reading, arguments, verb)});
        }
    } while (std::ranges::next_permutation(order).found);
  }
  // A line that starts with a record's name likely makes one, whatever words
  // it puts between the values (`a person namely "Winnie", aged 12`): offer
  // `a person with name "Winnie", age 12`, each value given a field its type
  // fits, in the order written.
  auto first = p_begin;
  while (first < p_end && is_article(p_tokens[first]))
    ++first;
  const auto [record, length] = p_vocabulary.record_at(p_tokens, first, p_end);
  if (!p_statement && record != nullptr && !record->is_trait &&
      !arguments.empty() && arguments.size() <= std::min<std::size_t>(
                                                   record->fields.size(), 4)) {
    // The verb words besides the record's own, each scored against the
    // chosen fields' names.
    const auto rest = std::vector<string_type>(
        verb.begin() + static_cast<std::ptrdiff_t>(
                           std::min(verb.size(), record->words.size())),
        verb.end());
    // With its article only if the span has it: `a` before the span stays.
    const auto article =
        first > p_begin
            ? core::describe_in_english(core::value_type::record_of(record->name))
            : string_type(record->words.front()) +
                  [&] {
                    string_type rest;
                    for (std::size_t index = 1; index < record->words.size(); ++index)
                      rest += " " + record->words[index];
                    return rest;
                  }();
    std::vector<const record_field *> chosen;
    std::set<const record_field *> taken;
    const std::function<void()> choose_fields = [&] {
      if (chosen.size() < arguments.size()) {
        for (const auto &field : record->fields)
          if (!taken.contains(&field) &&
              assignable(field.type, arguments[chosen.size()].type)) {
            chosen.push_back(&field);
            taken.insert(&field);
            choose_fields();
            taken.erase(&field);
            chosen.pop_back();
          }
        return;
      }
      auto reading = article + " with";
      for (std::size_t index = 0; index < chosen.size(); ++index) {
        const auto &value = arguments[index].reading;
        reading += (index > 0 ? ", " : " ") + chosen[index]->spoken() + " " +
                   (value.find(", ") != string_type::npos ? "(" + value + ")"
                                                          : value);
      }
      if (drops_meaning(reading, verb, 1, false) ||
          !seen.insert(reading).second || !p_fits(reading))
        return;
      // The record's words are like the phrase's own.
      double score = 3.0 * double(record->words.size());
      bool complete = true;
      for (const auto &word : rest) {
        double best = is_function_word(word) ? 0.2 : 0.0;
        for (const auto *const field : chosen)
          for (const auto &field_word : field->words)
            best = std::max(best, word_similarity(word, field_word) *
                                      (is_function_word(word) ? 0.2 : 1.0));
        score += best;
        complete = complete && best > 0;
      }
      // Fields in the order they were declared come first among equals.
      if (!std::ranges::is_sorted(chosen))
        score -= order_penalty;
      result.push_back({reading, score, complete});
    };
    choose_fields();
  }

  double best_as_written = -1e9;
  for (const auto &item : result)
    if (!left_out.contains(item.reading))
      best_as_written = std::max(best_as_written, item.score);
  std::erase_if(result, [&](const candidate &p_item) {
    return left_out.contains(p_item.reading) && p_item.score <= best_as_written;
  });
  // The 8 best fit in one question (phrase-grammar §4). Readings as good
  // as the 8th stay, for a run-off: the ranking can't tell them apart, and
  // cutting them by the library's order lost right readings.
  const auto unordered = [&](const candidate &p_item) {
    return p_item.score + (reordered.contains(p_item.reading) ? order_penalty : 0);
  };
  if (result.size() > per_question) {
    std::vector<double> scores;
    for (const auto &item : result)
      scores.push_back(unordered(item));
    std::ranges::sort(scores, std::ranges::greater{});
    const auto last = scores[per_question - 1];
    std::erase_if(result,
                  [&](const candidate &p_item) { return unordered(p_item) < last; });
  }
  // The cap, like the cut above, doesn't count the order: a reordered reading
  // is lower only among the readings kept. Three questions' worth: as the
  // library grows, ties fill two and a half questions and the reordered
  // readings that tie at the end were the first lost.
  const auto cap = 3 * per_question;
  if (result.size() > cap) {
    std::ranges::stable_sort(result, std::ranges::greater{}, unordered);
    result.resize(cap);
  }
  std::ranges::stable_sort(result, std::ranges::greater{}, &candidate::score);
  return result;
}

auto closest_templates(const std::vector<string_type> &p_words,
                       const std::vector<phrase_template> &p_templates,
                       std::size_t p_limit) -> std::vector<string_type> {
  struct match {
    double score;
    std::size_t length;
    string_type spoken;
  };
  std::vector<match> matches;
  for (const auto &item : p_templates) {
    std::optional<match> best;
    for (const auto &pattern : item.patterns) {
      std::vector<string_type> words;
      for (const auto &element : pattern) {
        if (element.kind == pattern_element::kind_type::word)
          words.push_back(element.word);
        for (const auto &branch : element.branches)
          for (const auto &part : branch)
            words.push_back(part.word);
      }
      double score = 0;
      for (const auto &word : p_words) {
        if (is_function_word(word))
          continue;
        double closest = 0;
        for (const auto &other : words)
          if (const auto alike = word_similarity(word, other); alike >= 2)
            closest = std::max(closest, alike);
        score += closest;
      }
      if (score > 0 && (!best || score > best->score))
        best = match{score, pattern.size(), spoken_pattern(item, pattern)};
    }
    if (best)
      matches.push_back(*best);
  }
  // Best first; among equals, the shorter phrase.
  std::ranges::stable_sort(matches, [](const match &p_left, const match &p_right) {
    return p_left.score != p_right.score ? p_left.score > p_right.score
                                         : p_left.length < p_right.length;
  });
  std::vector<string_type> result;
  for (const auto &item : matches)
    if (std::ranges::find(result, item.spoken) == result.end() &&
        result.size() < p_limit)
      result.push_back(item.spoken);
  return result;
}

auto manual_matches(const std::vector<string_type> &p_words,
                    const std::vector<phrase_template> &p_templates,
                    std::size_t p_limit) -> std::vector<std::size_t> {
  struct match {
    double score;
    std::size_t length;
    std::size_t index;
  };
  std::vector<match> matches;
  for (std::size_t index = 0; index < p_templates.size(); ++index) {
    const auto &item = p_templates[index];
    std::vector<string_type> pattern_words;
    for (const auto &pattern : item.patterns)
      for (const auto &element : pattern) {
        if (element.kind == pattern_element::kind_type::word)
          pattern_words.push_back(element.word);
        for (const auto &branch : element.branches)
          for (const auto &part : branch)
            pattern_words.push_back(part.word);
      }
    std::vector<string_type> about_words;
    string_type current;
    for (const auto letter : item.about + ' ') {
      if (std::isalpha(static_cast<unsigned char>(letter)))
        current += static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
      else {
        if (current.size() >= 3 && !is_function_word(current))
          about_words.push_back(current);
        current.clear();
      }
    }
    double score = 0;
    for (const auto &word : p_words) {
      if (is_function_word(word))
        continue;
      double closest = 0;
      for (const auto &other : pattern_words)
        if (const auto alike = word_similarity(word, other); alike >= 2)
          closest = std::max(closest, alike);
      for (const auto &other : about_words)
        if (const auto alike = word_similarity(word, other); alike >= 2)
          closest = std::max(closest, alike / 2);
      score += closest;
    }
    if (score > 0)
      matches.push_back({score, item.patterns.front().size(), index});
  }
  // Best first; among equals, the shorter phrase, then the library's order.
  std::ranges::stable_sort(matches, [](const match &p_left, const match &p_right) {
    return p_left.score != p_right.score ? p_left.score > p_right.score
                                         : p_left.length < p_right.length;
  });
  std::vector<std::size_t> result;
  for (const auto &item : matches)
    if (result.size() < p_limit)
      result.push_back(item.index);
  return result;
}

auto edit_candidates(const std::vector<token> &p_tokens, std::size_t p_begin,
                     std::size_t p_end, bool p_whole, bool p_statement,
                     bool p_block,
                     const std::map<string_type, value_kind> &p_scope,
                     const vocabulary &p_vocabulary,
                     const std::function<bool(const string_type &)> &p_fits)
    -> std::vector<candidate> {
  const std::vector<token> span(p_tokens.begin() + static_cast<std::ptrdiff_t>(p_begin),
                                p_tokens.begin() + static_cast<std::ptrdiff_t>(p_end));
  // What the line is about stays: names (of one word or several), values,
  // fields, records.
  std::set<std::size_t> in_names;
  for (const auto &[name, type] : p_scope)
    if (name.find(' ') != string_type::npos)
      if (const auto at = find_words(span, name); at != string_type::npos)
        for (std::size_t index = 0; index < static_cast<std::size_t>(
                                            std::ranges::count(name, ' ') + 1);
             ++index)
          in_names.insert(at + index);
  const auto is_kept = [&](std::size_t p_at) {
    const auto &item = span[p_at];
    return item.kind != token::kind_type::word || p_scope.contains(item.text) ||
           in_names.contains(p_at) ||
           p_vocabulary.is_field_word(item.text) ||
           p_vocabulary.record_at(span, p_at, p_at + 1).first != nullptr;
  };
  // Words an edit may put in: the built-in forms' and the glue between
  // values, and the words of phrases that share a word with the span.
  std::set<string_type> inserted = {
      "of", "is", "are", "to", "by", "from", "in", "with", "and", "not", "the",
      "set", "change", "increase", "decrease", "multiply", "divide", "add",
      "subtract", "'s", ","};
  for (const auto &item : p_vocabulary.templates) {
    if (!p_vocabulary.sees(item.package))
      continue;
    const auto words = phrase_words(item);
    const auto shares = std::ranges::any_of(words, [&](const string_type &p_word) {
      return !is_function_word(p_word) &&
             std::ranges::any_of(span, [&](const token &p_token) {
               return p_token.kind == token::kind_type::word &&
                      word_similarity(p_token.text, p_word) >= 2;
             });
    });
    if (shares)
      inserted.insert(words.begin(), words.end());
  }

  struct edited {
    std::vector<token> tokens;
    double cost;
  };
  std::vector<edited> edits;
  const auto as_token = [](const string_type &p_word) {
    return token{.kind = p_word == "'s" || p_word == "," ? token::kind_type::symbol
                                                          : token::kind_type::word,
                 .text = p_word};
  };
  for (std::size_t at = 0; at <= span.size(); ++at) {
    // A word left out.
    for (const auto &word : inserted) {
      auto tokens = span;
      tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(at), as_token(word));
      edits.push_back({std::move(tokens), is_function_word(word) ? 0.5 : 1.0});
    }
    if (at == span.size() || is_kept(at))
      continue;
    // Extra words that only join, up to three together (`which are`,
    // `does`): one with meaning is never dropped, or `winnie's age in
    // months` would read as `winnie's age`.
    static const std::set<string_type> fillers = {
        "does", "do", "did", "please", "which", "who", "that", "are", "then",
        "just", "also"};
    const auto is_filler = [&](const token &p_token) {
      return p_token.kind == token::kind_type::word &&
             (is_function_word(p_token.text) ||
              fillers.contains(lower(p_token.text)));
    };
    double dropped = 0;
    for (auto last = at; last < span.size() && last < at + 3 && !is_kept(last) &&
                         is_filler(span[last]);
         ++last) {
      dropped += 0.4;
      auto fewer = span;
      fewer.erase(fewer.begin() + static_cast<std::ptrdiff_t>(at),
                  fewer.begin() + static_cast<std::ptrdiff_t>(last + 1));
      const auto only_values = std::ranges::all_of(fewer, [&](const token &p_token) {
        return p_token.kind != token::kind_type::word ||
               p_scope.contains(p_token.text) || is_function_word(p_token.text);
      });
      // Only values left: offered last (`sort out xs` is `sort xs`, but
      // `the biggest of numbers` isn't `numbers`).
      if (!fewer.empty() && !(p_whole && only_values))
        edits.push_back({std::move(fewer), dropped + (only_values ? 10 : 0)});
    }
    // Another word for one of the phrase's, spelt alike (`decrement` for
    // `decrease`), or a joining word: one far from it (`median` for
    // `average`) is a meaning the model chooses, not an edit.
    for (const auto &word : inserted) {
      if (word == "'s" || word == "," || same_word(word, span[at].text) ||
          (!is_function_word(span[at].text) &&
           word_similarity(span[at].text, word) < 1))
        continue;
      auto tokens = span;
      tokens[at] = as_token(word);
      edits.push_back({std::move(tokens),
                       1.2 - 0.1 * word_similarity(span[at].text, word)});
    }
    for (const auto &name : number_type_names_like(span[at].text)) {
      auto tokens = span;
      tokens[at] = as_token(name);
      edits.push_back({std::move(tokens), 1.0});
    }
  }

  // Two edits, only for a statement: a verb of the built-in updates put
  // first, and the words between the target and the value (one or two,
  // none the line is about) made `to` or `by`: `w's age is now 13` is
  // `set w's age to 13`. More would multiply the edits to parse.
  if (p_statement && p_whole) {
    // `set` first: the words between say "becomes" far more often than an
    // amount (`change` reads the same as `set`, so it is left out).
    static const std::vector<string_type> verbs = {
        "set", "increase", "decrease", "multiply", "divide"};
    for (const auto &verb : verbs)
      for (std::size_t at = 1; at < span.size(); ++at)
        for (std::size_t length = 1; length <= 2 && at + length < span.size();
             ++length) {
          bool joins = true;
          for (auto index = at; index < at + length; ++index)
            joins = joins && span[index].kind == token::kind_type::word &&
                    !is_kept(index);
          if (!joins)
            continue;
          for (const auto *const glue : {"to", "by"}) {
            if ((string_view_type(glue) == "to") != (verb == "set"))
              continue;
            std::vector<token> tokens = {as_token(verb)};
            tokens.insert(tokens.end(), span.begin(),
                          span.begin() + static_cast<std::ptrdiff_t>(at));
            tokens.push_back(as_token(glue));
            tokens.insert(tokens.end(),
                          span.begin() + static_cast<std::ptrdiff_t>(at + length),
                          span.end());
            edits.push_back(
                {std::move(tokens), (verb == "set" ? 2.0 : 2.5) + 0.2 * length});
          }
        }
  }

  std::map<string_type, double> found;
  for (const auto &[tokens, cost] : edits) {
    line_parser parser(tokens, p_scope, p_vocabulary, false);
    auto readings = p_statement ? parser.statements(0, tokens.size(), p_block)
                                : parser.expressions(0, tokens.size());
    for (const auto &reading : readings) {
      if (reading.holes > 0)
        continue;
      const auto known = found.find(reading.reading);
      if (known != found.end()) {
        known->second = std::min(known->second, cost);
        continue;
      }
      if (p_fits(reading.reading))
        found[reading.reading] = cost;
    }
  }
  std::vector<candidate> result;
  for (const auto &[reading, cost] : found)
    result.push_back({reading, -cost, false});
  std::ranges::stable_sort(result, std::ranges::greater{}, &candidate::score);
  if (result.size() > per_question)
    result.resize(per_question);
  return result;
}

auto name_candidates(const std::vector<token> &p_tokens, std::size_t p_at,
                     const line_shape &p_shape,
                     const std::map<string_type, value_kind> &p_scope,
                     const std::vector<string_type> &p_recent,
                     const vocabulary &p_vocabulary) -> std::vector<candidate> {
  std::vector<candidate> result;
  for (const auto &name : p_recent) {
    if (!p_scope.contains(name))
      continue;
    auto tokens = p_tokens;
    tokens[p_at].text = name;
    line_parser parser(tokens, p_scope, p_vocabulary, false);
    if (!readings_of(parser, p_shape).empty())
      result.push_back({name});
  }
  return result;
}

auto word_candidates(const std::vector<token> &p_tokens, std::size_t p_at,
                     const line_shape &p_shape,
                     const std::map<string_type, value_kind> &p_scope,
                     const std::vector<string_type> &p_words,
                     const vocabulary &p_vocabulary) -> std::vector<candidate> {
  std::vector<candidate> result;
  for (const auto &word : p_words) {
    // A field of several words (`number of wheels`) is several tokens.
    auto tokens = p_tokens;
    std::vector<token> spliced;
    try {
      spliced = tokenize(word, p_tokens[p_at].line);
    } catch (const lex_error &) {
      continue;
    }
    tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(p_at));
    tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(p_at),
                  spliced.begin(), spliced.end());
    line_parser parser(tokens, p_scope, p_vocabulary, false);
    if (!readings_of(parser, p_shape).empty())
      result.push_back({word});
  }
  return result;
}

auto is_pronoun(string_view_type p_word) -> bool {
  const auto word = lower(p_word);
  return word == "it" || word == "them";
}

auto ranking_shortcut(const std::vector<candidate> &p_candidates)
    -> std::optional<std::size_t> {
  if (p_candidates.empty())
    return std::nullopt;
  const auto best = static_cast<std::size_t>(
      std::ranges::max_element(p_candidates, {}, &candidate::score) -
      p_candidates.begin());
  double second = 0;
  for (std::size_t index = 0; index < p_candidates.size(); ++index)
    if (index != best)
      second = std::max(second, p_candidates[index].score);
  if (p_candidates[best].covers_span &&
      p_candidates[best].added_words <= shortcut_added_words &&
      p_candidates[best].score >= shortcut_score &&
      p_candidates[best].score - second >= shortcut_margin)
    return best;
  return std::nullopt;
}

namespace {

constexpr string_view_type question_instructions =
    "You help a compiler read a program written in plain English. Some words "
    "in a line are not in the compiler's vocabulary. You are shown the line "
    "and numbered phrases the compiler knows. Answer with the number of the "
    "phrase that means the same as the unknown words, as an English speaker "
    "would understand them. If none means the same, answer with the number "
    "of \"none of these\".";

} // namespace

auto question_asker::available() -> bool {
  if (!unavailable.empty())
    return false;
  if (model == nullptr)
    unavailable = "there is no model (--offline)";
  // The first question also loads the model, which says nothing about
  // how long questions take, so it doesn't count.
  else if (timing.model_ms - timing.first_question_ms >
           options.model_budget_seconds * 1000)
    unavailable = "the model used up its time (--model-budget)";
  return unavailable.empty();
}

auto question_asker::count_time(double p_milliseconds) -> void {
  if (timing.asked == 0)
    timing.first_question_ms += p_milliseconds;
  timing.model_ms += p_milliseconds;
}

auto question_asker::order_ties(const string_type &p_span,
                                std::vector<candidate> &p_options) -> void {
  // Runs of equal score, as [begin, end) of the sorted readings.
  std::vector<std::pair<std::size_t, std::size_t>> ties;
  for (std::size_t begin = 0; begin < p_options.size();) {
    auto end = begin + 1;
    while (end < p_options.size() &&
           p_options[end].score == p_options[begin].score)
      ++end;
    if (end - begin > 1)
      ties.emplace_back(begin, end);
    begin = end;
  }
  if (ties.empty() || !available())
    return;

  std::vector<string_type> texts = {p_span};
  for (const auto &[begin, end] : ties)
    for (auto index = begin; index < end; ++index)
      texts.push_back(p_options[index].reading);
  const auto start = clock_type::now();
  std::vector<std::vector<float>> vectors;
  try {
    vectors = model->embed(texts);
  } catch (const error &p_error) {
    unavailable = p_error.what();
    return;
  }
  count_time(milliseconds_since(start));
  if (vectors.size() != texts.size())
    return;

  std::size_t next = 1;
  for (const auto &[begin, end] : ties) {
    std::vector<std::pair<double, candidate>> scored;
    for (auto index = begin; index < end; ++index, ++next) {
      double similarity = 0;
      for (std::size_t at = 0; at < vectors[0].size(); ++at)
        similarity += double(vectors[0][at]) * vectors[next][at];
      scored.emplace_back(similarity, p_options[index]);
    }
    std::ranges::stable_sort(scored, std::ranges::greater{},
                             &std::pair<double, candidate>::first);
    for (auto index = begin; index < end; ++index)
      p_options[index] = scored[index - begin].second;
    // How far the first is ahead, for judging when to trust it.
    if (begin == 0 && options.log)
      options.log("embedding: \"" + p_span + "\" is " +
                  scored[0].second.reading + ", similarity " +
                  std::to_string(scored[0].first) + ", margin " +
                  std::to_string(scored[0].first - scored[1].first));
  }
}

auto question_asker::ask(const string_type &p_line, const string_type &p_known,
         const string_type &p_span, const std::vector<candidate> &p_options)
    -> model_answer {
  if (!available())
    return {};

  // The readings in `p_order` (indices into `p_options`), "none" last.
  const auto question = [&](const std::vector<std::size_t> &p_order) {
    auto text = "Line: " + p_line + "\n";
    if (!p_known.empty())
      text += "Known: " + p_known + "\n";
    text += "Which phrase means the same as \"" + p_span + "\"?\n";
    for (std::size_t index = 0; index < p_order.size(); ++index)
      text += string_type(1, english::choice_digit(index + 1)) + ". " +
              p_options[p_order[index]].reading + "\n";
    text += string_type(1, english::choice_digit(p_options.size() + 1)) +
            ". none of these\nAnswer:";
    return std::vector<english::chat_message>{
        {"system", string_type(question_instructions)}, {"user", text}};
  };
  std::vector<std::size_t> order(p_options.size());
  std::iota(order.begin(), order.end(), 0);

  const auto start = clock_type::now();
  model_answer answer;
  try {
    answer.probabilities = model->choose(question(order), p_options.size() + 1);
    if (options.ask_both_orders && p_options.size() > 1) {
      std::ranges::reverse(order);
      const auto reversed =
          model->choose(question(order), p_options.size() + 1);
      for (std::size_t index = 0; index < order.size(); ++index)
        answer.probabilities[order[index]] =
            (answer.probabilities[order[index]] + reversed[index]) / 2;
      answer.probabilities.back() =
          (answer.probabilities.back() + reversed.back()) / 2;
    }
  } catch (const error &p_error) {
    unavailable = p_error.what();
    return {};
  }
  const auto elapsed = milliseconds_since(start);
  count_time(elapsed);
  ++timing.asked;

  std::vector<std::size_t> ranked(answer.probabilities.size());
  std::iota(ranked.begin(), ranked.end(), 0);
  std::ranges::stable_sort(ranked, [&](std::size_t p_left, std::size_t p_right) {
    return answer.probabilities[p_left] > answer.probabilities[p_right];
  });
  const auto best = ranked[0];
  const auto margin =
      answer.probabilities[best] - answer.probabilities[ranked[1]];
  if (best < p_options.size() && answer.probabilities[best] >= 0.6 &&
      margin >= 0.3)
    answer.choice = best;
  if (options.log) {
    string_type summary = "asked about \"" + p_span + "\":";
    for (std::size_t index = 0; index < ranked.size() && index < 3; ++index) {
      const auto option = ranked[index];
      summary += " " +
                 (option < p_options.size() ? p_options[option].reading
                                            : string_type("none")) +
                 " " +
                 std::to_string(int(answer.probabilities[option] * 100 + 0.5)) +
                 "%";
    }
    options.log(summary + " (" + std::to_string(int(elapsed)) + " ms)");
  }
  return answer;
}

auto question_asker::choose(const string_type &p_line, const string_type &p_known,
            const string_type &p_span, std::vector<candidate> p_options)
    -> std::optional<string_type> {
  not_rejected.clear();
  for (const auto &item : p_options)
    not_rejected.push_back(item.reading);
  while (p_options.size() > per_question) {
    std::vector<candidate> winners;
    for (std::size_t begin = 0; begin < p_options.size(); begin += per_question) {
      std::vector<candidate> group(
          p_options.begin() + static_cast<std::ptrdiff_t>(begin),
          p_options.begin() +
              static_cast<std::ptrdiff_t>(std::min(begin + per_question, p_options.size())));
      // A group of one goes on to the final unasked.
      if (group.size() == 1) {
        winners.push_back(group.front());
        continue;
      }
      const auto answer = ask(p_line, p_known, p_span, group);
      if (!unavailable.empty())
        return std::nullopt;
      // A group's favourite goes on even when it isn't sure yet.
      const auto best = static_cast<std::size_t>(
          std::ranges::max_element(answer.probabilities) -
          answer.probabilities.begin());
      if (best < group.size())
        winners.push_back(group[best]);
    }
    p_options = std::move(winners);
  }
  if (p_options.empty()) {
    // Every group found its readings no better than none.
    not_rejected.clear();
    return std::nullopt;
  }
  const auto answer = ask(p_line, p_known, p_span, p_options);
  if (answer.choice)
    return p_options[*answer.choice].reading;
  // Run-offs' losers were turned down for a better reading.
  if (!answer.probabilities.empty()) {
    std::vector<std::size_t> kept;
    for (std::size_t index = 0; index < p_options.size(); ++index)
      if (answer.probabilities[index] > answer.probabilities.back())
        kept.push_back(index);
    std::ranges::stable_sort(kept, [&](std::size_t p_left, std::size_t p_right) {
      return answer.probabilities[p_left] > answer.probabilities[p_right];
    });
    not_rejected.clear();
    for (const auto index : kept)
      not_rejected.push_back(p_options[index].reading);
  }
  return std::nullopt;
}

} // namespace ovis::phrase::internal

namespace ovis::phrase::internal {

namespace {

constexpr string_view_type rewrite_instructions =
    "You help a compiler read a program written in plain English. The "
    "compiler reads only some wordings. You are shown a line and a part of "
    "it the compiler can't read, and you write that part again in wording "
    "the compiler reads, meaning the same.\n"
    "Rules:\n"
    "- Use only the forms and phrases below, the program's names, records "
    "and fields, and the numbers and texts written in the part.\n"
    "- Keep every name, field, number and text of the part.\n"
    "- Answer with the rewritten part only, on one line.\n"
    "\n"
    "Forms:\n"
    "- a RECORD with FIELD VALUE, FIELD VALUE: a new record\n"
    "- NAME's FIELD, or FIELD of NAME: a field\n"
    "- set NAME to VALUE, set NAME's FIELD to VALUE\n"
    "- increase X by N, decrease X by N, add N to X, subtract N from X, "
    "multiply X by N, divide X by N\n"
    "- [VALUE, VALUE]: a list\n"
    "- VALUE + VALUE, and - * / = != < > <= >= and or not\n"
    "\n"
    "Examples, for a program with a record `a box has: width (whole "
    "number), label (text)` and a box called b:\n"
    "Part: \"a box that is 3 wide with \"x\" on it\"\n"
    "Answer: a box with width 3, label \"x\"\n"
    "Part: \"make b 2 wider\"\n"
    "Answer: increase b's width by 2\n"
    "\n"
    "Phrases (capitals are values):\n";

// Words of the built-in forms (`updates`, `new_records`, operators), which
// are no template's.
constexpr string_view_type form_words[] = {
    "a", "an", "the", "with", "named", "called", "and", "or", "not", "of",
    "set", "change", "to", "let", "be", "increase", "decrease", "multiply",
    "divide", "by", "add", "subtract", "from", "true", "false", "nothing"};

auto is_builtin(const phrase_template &p_template) -> bool {
  return p_template.file.ends_with(".phr");
}

// A GBNF string literal of `p_text`.
auto gbnf_literal(string_view_type p_text) -> string_type {
  string_type result = "\"";
  for (const auto character : p_text) {
    if (character == '"' || character == '\\')
      result.push_back('\\');
    result.push_back(character);
  }
  return result + "\"";
}

auto is_value_token(const token &p_token) -> bool {
  return p_token.kind == token::kind_type::text ||
         p_token.kind == token::kind_type::number;
}

} // namespace

auto rewrite_request_of(const string_type &p_line, const string_type &p_span,
                        const std::vector<token> &p_span_tokens,
                        const std::map<string_type, value_kind> &p_scope,
                        const std::vector<string_type> &p_recent,
                        const vocabulary &p_vocabulary) -> rewrite_request {
  rewrite_request result;
  result.system = string_type(rewrite_instructions);
  std::set<string_type> words(std::begin(form_words), std::end(form_words));
  std::set<string_type> listed;
  string_type own;
  for (const auto &item : p_vocabulary.templates) {
    if (!p_vocabulary.sees(item.package))
      continue;
    for (const auto &pattern : item.patterns)
      for (const auto &element : pattern) {
        if (element.kind == pattern_element::kind_type::word)
          words.insert(element.word);
        for (const auto &branch : element.branches)
          for (const auto &part : branch)
            words.insert(part.word);
      }
    const auto hint = spoken_pattern(item, item.patterns.front());
    if (!listed.insert(hint).second)
      continue;
    if (is_builtin(item))
      result.system += "- " + hint + "\n";
    else
      own += "- " + hint + "\n";
  }

  auto &user = result.user;
  if (!p_vocabulary.records.empty()) {
    user += "Records:\n";
    for (const auto &[name, record] : p_vocabulary.records) {
      if (record.is_trait)
        continue;
      string_type written;
      for (const auto &word : record.words) {
        written += (written.empty() ? "" : " ") + word;
        words.insert(word);
      }
      string_type fields;
      for (const auto &field : record.fields) {
        fields += (fields.empty() ? "" : ", ") + field.spoken() + " " +
                  field.type.english();
        for (const auto &word : field.words)
          words.insert(word);
      }
      user += "- " + core::describe_in_english(core::value_type::record_of(record.name)) +
              " has: " + fields + "\n";
    }
  }
  if (!own.empty())
    user += "Phrases of this program:\n" + own;
  string_type names;
  for (std::size_t index = 0; index < p_recent.size() && index < 30; ++index) {
    const auto found = p_scope.find(p_recent[index]);
    if (found == p_scope.end())
      continue;
    names += (names.empty() ? "" : " ") + found->first + " is " +
             found->second.english() + ".";
    words.insert(found->first);
  }
  if (!names.empty())
    user += "Names: " + names + "\n";
  user += "Line: " + p_line + "\nPart: \"" + p_span + "\"\nAnswer:";

  // Only those words, the part's own texts, any number, and operators: the
  // model can't copy an example or invent a name.
  string_type word_rule;
  for (const auto &word : words)
    if (!word.empty())
      word_rule += (word_rule.empty() ? "" : " | ") + gbnf_literal(word);
  std::set<string_type> texts;
  for (const auto &item : p_span_tokens)
    if (item.kind == token::kind_type::text)
      texts.insert(quote(item.text));
  string_type atom = "word \"'s\"? | number";
  string_type text_rule;
  for (const auto &text : texts)
    text_rule += (text_rule.empty() ? "" : " | ") + gbnf_literal(text);
  if (!text_rule.empty())
    atom += " | text";
  auto &grammar = result.grammar;
  grammar = "root ::= part (sep part){0,40}\n"
            "sep ::= \" \" | \", \"\n"
            "part ::= (\"(\" | \"[\")* (" + atom + ") (\")\" | \"]\")* | "
            "operator\n"
            "operator ::= \"+\" | \"-\" | \"*\" | \"/\" | \"=\" | \"!=\" | "
            "\"<\" | \">\" | \"<=\" | \">=\"\n"
            "number ::= \"-\"? [0-9]+ (\".\" [0-9]+)?\n"
            "word ::= " + word_rule + "\n";
  if (!text_rule.empty())
    grammar += "text ::= " + text_rule + "\n";
  return result;
}

auto has_anchor(const std::vector<token> &p_span_tokens,
                const std::map<string_type, value_kind> &p_scope,
                const vocabulary &p_vocabulary) -> bool {
  for (std::size_t index = 0; index < p_span_tokens.size(); ++index) {
    const auto &item = p_span_tokens[index];
    if (item.kind == token::kind_type::text ||
        item.kind == token::kind_type::number ||
        (item.kind == token::kind_type::word &&
         (p_scope.contains(item.text) || p_vocabulary.is_field_word(item.text))) ||
        p_vocabulary.record_at(p_span_tokens, index, index + 1).first != nullptr)
      return true;
  }
  return std::ranges::any_of(p_scope, [&](const auto &p_name) {
    return p_name.first.find(' ') != string_type::npos &&
           find_words(p_span_tokens, p_name.first) != string_type::npos;
  });
}

auto rewrite_loss(const std::vector<token> &p_span_tokens,
                  const std::vector<token> &p_rewrite_tokens,
                  const std::map<string_type, value_kind> &p_scope,
                  const vocabulary &p_vocabulary) -> string_type {
  const auto kept = [&](const token &p_token) {
    return std::ranges::any_of(p_rewrite_tokens, [&](const token &p_other) {
      if (is_value_token(p_token))
        return p_other.kind == p_token.kind && p_other.text == p_token.text;
      return p_other.kind == token::kind_type::word &&
             same_word(p_other.text, p_token.text);
    });
  };
  for (std::size_t index = 0; index < p_span_tokens.size(); ++index) {
    const auto &item = p_span_tokens[index];
    string_type what;
    if (item.kind == token::kind_type::text)
      what = "the text " + quote(item.text);
    else if (item.kind == token::kind_type::number)
      what = "the number " + item.text;
    else if (item.kind == token::kind_type::word && p_scope.contains(item.text))
      what = "the name " + item.text;
    else if (item.kind == token::kind_type::word &&
             p_vocabulary.is_field_word(item.text))
      what = "the field " + item.text;
    else if (const auto *const record =
                 p_vocabulary.record_at(p_span_tokens, index, index + 1).first;
             record != nullptr &&
             // `the car` may be the variable holding it: `the_car`.
             std::ranges::none_of(p_rewrite_tokens, [&](const token &p_other) {
               const auto found = p_scope.find(p_other.text);
               return p_other.kind == token::kind_type::word &&
                      found != p_scope.end() &&
                      found->second == record->type();
             }))
      what = "the record " + item.text;
    if (!what.empty() && !kept(item))
      return "it leaves out " + what;
  }
  // A name of several words is kept whole.
  for (const auto &[name, type] : p_scope)
    if (name.find(' ') != string_type::npos &&
        find_words(p_span_tokens, name) != string_type::npos &&
        find_words(p_rewrite_tokens, name) == string_type::npos)
      return "it leaves out the name " + name;
  return {};
}

auto question_asker::rewrite(
    const rewrite_request &p_request,
    const std::vector<std::pair<string_type, string_type>> &p_tried)
    -> std::optional<string_type> {
  if (!available())
    return std::nullopt;
  std::vector<english::chat_message> messages = {
      {"system", p_request.system}, {"user", p_request.user}};
  for (const auto &[reply, reason] : p_tried) {
    messages.push_back({"assistant", reply});
    messages.push_back(
        {"user", "The compiler can't use that: " + reason +
                     ". Write the part again another way.\nAnswer:"});
  }
  const auto start = clock_type::now();
  string_type reply;
  try {
    reply = model->chat(messages, p_request.grammar);
  } catch (const error &p_error) {
    unavailable = p_error.what();
    return std::nullopt;
  }
  const auto elapsed = milliseconds_since(start);
  count_time(elapsed);
  ++timing.asked;
  while (!reply.empty() && std::isspace(static_cast<unsigned char>(reply.back())))
    reply.pop_back();
  reply.erase(0, reply.find_first_not_of(" \t\n"));
  if (options.log)
    options.log("rewrote " +
                p_request.user.substr(p_request.user.rfind("Part: ") + 6,
                                      p_request.user.rfind("\nAnswer:") -
                                          p_request.user.rfind("Part: ") - 6) +
                " as \"" + reply + "\" (" + std::to_string(int(elapsed)) +
                " ms)");
  if (reply.empty())
    return std::nullopt;
  return reply;
}

} // namespace ovis::phrase::internal

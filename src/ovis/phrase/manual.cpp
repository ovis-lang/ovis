#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>

#include <ovis/core/modules.hpp>
#include <ovis/phrase/front_end.hpp>

#include "questions.hpp"

namespace ovis::phrase {

namespace {

// What an entry is embedded as: every way of saying it, then its sentence.
// The examples are left out: their names would pull a query toward them.
auto entry_text(const phrase_template &p_item) -> string_type {
  string_type text;
  for (const auto &pattern : p_item.patterns)
    text += spoken_pattern(p_item, pattern) + ". ";
  return text + p_item.about;
}

// A key for the cache, not a safeguard: FNV-1a over the model and the text.
auto cache_key(const string_type &p_model, const string_type &p_text)
    -> std::uint64_t {
  std::uint64_t key = 14695981039346656037ULL;
  for (const auto letter : p_model + '\n' + p_text)
    key = (key ^ static_cast<unsigned char>(letter)) * 1099511628211ULL;
  return key;
}

constexpr char cache_magic[] = "OVMAN1\n";

using vectors_by_key = std::map<std::uint64_t, std::vector<float>>;

auto read_cache(const std::filesystem::path &p_path) -> vectors_by_key {
  vectors_by_key result;
  std::ifstream file(p_path, std::ios::binary);
  char magic[sizeof cache_magic - 1] = {};
  std::uint32_t size = 0;
  std::uint64_t count = 0;
  if (!file.read(magic, sizeof magic) ||
      std::memcmp(magic, cache_magic, sizeof magic) != 0 ||
      !file.read(reinterpret_cast<char *>(&size), sizeof size) ||
      !file.read(reinterpret_cast<char *>(&count), sizeof count) || size == 0)
    return {};
  for (std::uint64_t index = 0; index < count; ++index) {
    std::uint64_t key = 0;
    std::vector<float> vector(size);
    if (!file.read(reinterpret_cast<char *>(&key), sizeof key) ||
        !file.read(reinterpret_cast<char *>(vector.data()),
                   static_cast<std::streamsize>(size * sizeof(float))))
      return {};
    result.emplace(key, std::move(vector));
  }
  return result;
}

// A cache that can't be written is only slower next time.
auto write_cache(const std::filesystem::path &p_path,
                 const vectors_by_key &p_vectors) -> void {
  if (p_vectors.empty())
    return;
  std::error_code ignored;
  std::filesystem::create_directories(p_path.parent_path(), ignored);
  const auto temporary = std::filesystem::path(p_path).concat(".tmp");
  {
    std::ofstream file(temporary, std::ios::binary);
    const auto size = static_cast<std::uint32_t>(p_vectors.begin()->second.size());
    const auto count = static_cast<std::uint64_t>(p_vectors.size());
    file.write(cache_magic, sizeof cache_magic - 1);
    file.write(reinterpret_cast<const char *>(&size), sizeof size);
    file.write(reinterpret_cast<const char *>(&count), sizeof count);
    for (const auto &[key, vector] : p_vectors) {
      file.write(reinterpret_cast<const char *>(&key), sizeof key);
      file.write(reinterpret_cast<const char *>(vector.data()),
                 static_cast<std::streamsize>(size * sizeof(float)));
    }
    if (!file)
      return;
  }
  std::filesystem::rename(temporary, p_path, ignored);
}

// How alike each entry is to the query, by the dot product of unit vectors;
// empty when the model can't embed or fails (the search is then lexical).
// Only entries the cache lacks are embedded.
auto embedding_scores(const string_type &p_query,
                      const std::vector<string_type> &p_texts,
                      const manual_embeddings &p_options) -> std::vector<double> {
  try {
    const auto model = p_options.model->fingerprint() + p_options.model->name();
    auto cache = p_options.cache.empty() ? vectors_by_key{} : read_cache(p_options.cache);
    std::vector<std::uint64_t> keys;
    std::vector<string_type> missing;
    for (const auto &text : p_texts) {
      keys.push_back(cache_key(model, text));
      if (!cache.contains(keys.back()) &&
          std::ranges::find(missing, text) == missing.end())
        missing.push_back(text);
    }
    if (!missing.empty()) {
      // The built-in model reads a bounded batch (64 texts, 4096 tokens).
      constexpr std::size_t batch = 32;
      for (std::size_t begin = 0; begin < missing.size(); begin += batch) {
        const auto end = std::min(missing.size(), begin + batch);
        const auto made = p_options.model->embed(
            {missing.begin() + static_cast<std::ptrdiff_t>(begin),
             missing.begin() + static_cast<std::ptrdiff_t>(end)});
        if (made.size() != end - begin)
          return {};
        for (std::size_t index = begin; index < end; ++index)
          cache[cache_key(model, missing[index])] = made[index - begin];
      }
      if (!p_options.cache.empty())
        write_cache(p_options.cache, cache);
    }
    const auto asked = p_options.model->embed({p_query});
    if (asked.size() != 1)
      return {};
    std::vector<double> scores;
    for (const auto key : keys) {
      const auto &vector = cache.at(key);
      if (vector.size() != asked[0].size())
        return {};
      double alike = 0;
      for (std::size_t at = 0; at < vector.size(); ++at)
        alike += double(vector[at]) * asked[0][at];
      scores.push_back(alike);
    }
    return scores;
  } catch (const error &) {
    return {};
  }
}

// Reciprocal-rank fusion's constant: small, so the first few of either list
// count much more than the rest.
constexpr double fusion_constant = 10;
// A word the author wrote is surer evidence than a similarity of 3B's vectors,
// so the lexical place counts this many times over.
constexpr double lexical_weight = 3;

} // namespace

auto manual_search(const std::vector<string_type> &p_words,
                   const std::vector<phrase_template> &p_more,
                   std::size_t p_limit, const manual_embeddings &p_embeddings)
    -> std::vector<manual_entry> {
  // The built-in wording first, so it ranks first among equals.
  auto templates = builtin_templates();
  templates.insert(templates.end(), p_more.begin(), p_more.end());
  std::vector<std::size_t> chosen;
  if (p_words.empty()) {
    for (std::size_t index = 0; index < templates.size(); ++index)
      chosen.push_back(index);
  } else {
    const auto lexical =
        internal::manual_matches(p_words, templates, templates.size());
    chosen = lexical;
    std::vector<double> alike;
    if (p_embeddings.model != nullptr) {
      std::vector<string_type> texts;
      for (const auto &item : templates)
        texts.push_back(entry_text(item));
      string_type query;
      for (const auto &word : p_words)
        query += (query.empty() ? "" : " ") + word;
      alike = embedding_scores(query, texts, p_embeddings);
    }
    if (!alike.empty()) {
      // Each list gives an entry 1 / (constant + its place); an entry only one
      // list has gets that list's share. Ties keep the lexical order.
      std::vector<std::size_t> by_meaning(templates.size());
      std::iota(by_meaning.begin(), by_meaning.end(), 0);
      std::ranges::stable_sort(by_meaning, std::ranges::greater{},
                               [&](std::size_t p_index) { return alike[p_index]; });
      std::vector<double> score(templates.size(), 0);
      for (std::size_t place = 0; place < by_meaning.size(); ++place)
        score[by_meaning[place]] += 1 / (fusion_constant + double(place));
      for (std::size_t place = 0; place < lexical.size(); ++place)
        score[lexical[place]] += lexical_weight / (fusion_constant + double(place));
      std::vector<std::size_t> order(templates.size());
      std::iota(order.begin(), order.end(), 0);
      // The lexical order first, so it decides among equals.
      std::vector<std::size_t> rank(templates.size(), templates.size());
      for (std::size_t place = 0; place < lexical.size(); ++place)
        rank[lexical[place]] = place;
      std::ranges::stable_sort(order, [&](std::size_t p_left, std::size_t p_right) {
        return score[p_left] != score[p_right] ? score[p_left] > score[p_right]
                                               : rank[p_left] < rank[p_right];
      });
      chosen = std::move(order);
    }
    if (p_limit != 0 && chosen.size() > p_limit)
      chosen.resize(p_limit);
  }
  std::vector<manual_entry> result;
  for (const auto index : chosen)
    result.push_back(manual_entry_of(templates[index]));
  return result;
}

auto manual_entry_of(const phrase_template &p_item) -> manual_entry {
  manual_entry entry{.topic = std::filesystem::path(p_item.file).stem().string(),
                     .package = p_item.package,
                     .pattern = p_item.title.empty()
                                    ? spoken_pattern(p_item, p_item.patterns.front())
                                    : p_item.title,
                     .about = p_item.about,
                     .examples = p_item.examples};
  for (std::size_t other = 1; other < p_item.patterns.size(); ++other)
    entry.aliases.push_back(spoken_pattern(p_item, p_item.patterns[other]));
  return entry;
}


namespace {

// A header's pieces: runs of words between slots (`(total)`).
struct header_shape {
  std::vector<std::vector<string_type>> runs;
  // The `.phr` pattern: "apply the discount to {total}".
  string_type pattern;
  // `$total $amount`: the Core the reader asks every slot to appear in.
  string_type core;
};

auto is_word_letter(char p_letter) -> bool {
  return std::isalnum(static_cast<unsigned char>(p_letter)) || p_letter == '_' ||
         p_letter == '\'' || p_letter == '-';
}

// A line's words, lower case, outside quoted texts.
auto words_of(const string_type &p_text) -> std::vector<string_type> {
  std::vector<string_type> words;
  string_type current;
  bool quoted = false;
  const auto finish = [&] {
    if (!current.empty())
      words.push_back(current);
    current.clear();
  };
  for (const auto letter : p_text + ' ') {
    if (letter == '"') {
      quoted = !quoted;
      finish();
    } else if (!quoted && is_word_letter(letter))
      current += static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    else
      finish();
  }
  return words;
}

auto trimmed(const string_type &p_text) -> string_type {
  const auto first = p_text.find_first_not_of(" \t");
  if (first == string_type::npos)
    return {};
  return p_text.substr(first, p_text.find_last_not_of(" \t") - first + 1);
}

// `to apply the discount to (total):` → its shape; none for a header with
// anything this scan doesn't understand (the compiler still reads it).
auto header_of(const string_type &p_line) -> std::optional<header_shape> {
  if (!p_line.starts_with("to ") || !p_line.ends_with(":"))
    return std::nullopt;
  auto text = p_line.substr(3, p_line.size() - 4);
  // `as NAME` and `gives TYPE` are not part of what is said.
  string_type outside;
  int depth = 0;
  for (const auto letter : text) {
    depth += letter == '(' ? 1 : letter == ')' ? -1 : 0;
    outside += depth == 0 && letter != ')' ? letter : ' ';
  }
  std::size_t cut = text.size();
  for (const char *stop : {" gives ", " as "})
    cut = std::min(cut, outside.find(stop));
  text.resize(cut);
  header_shape shape;
  std::vector<string_type> run;
  string_type word;
  bool plain = true;
  const auto add = [&](const string_type &p_piece) {
    shape.pattern += (shape.pattern.empty() ? "" : " ") + p_piece;
  };
  const auto finish_word = [&] {
    if (word.empty())
      return;
    plain = plain && std::ranges::all_of(word, is_word_letter);
    run.push_back(word);
    add(word);
    word.clear();
  };
  for (std::size_t at = 0; at < text.size(); ++at) {
    if (text[at] == '(') {
      finish_word();
      const auto close = text.find(')', at);
      if (close == string_type::npos)
        return std::nullopt;
      auto name = trimmed(text.substr(at + 1, close - at - 1));
      name = trimmed(name.substr(0, name.find(':')));
      std::ranges::replace(name, ' ', '_');
      if (name.empty() || !std::ranges::all_of(name, is_word_letter))
        return std::nullopt;
      if (!run.empty())
        shape.runs.push_back(std::exchange(run, {}));
      add("{" + name + ": any}");
      shape.core += " $" + name;
      at = close;
    } else if (text[at] == ' ' || text[at] == '\t')
      finish_word();
    else
      word += static_cast<char>(std::tolower(static_cast<unsigned char>(text[at])));
  }
  finish_word();
  if (!run.empty())
    shape.runs.push_back(std::move(run));
  if (!plain || shape.runs.empty())
    return std::nullopt;
  return shape;
}

// Whether a line's words have each run of the header, in order, with
// something between (the slots' values).
auto reads_to(const header_shape &p_shape, const std::vector<string_type> &p_words)
    -> bool {
  auto from = p_words.begin();
  for (const auto &run : p_shape.runs) {
    const auto at = std::search(from, p_words.end(), run.begin(), run.end());
    if (at == p_words.end())
      return false;
    from = at + static_cast<std::ptrdiff_t>(run.size());
  }
  return true;
}

// `example: LINE` after a comment's `#`: the line.
auto example_of(const string_type &p_comment) -> std::optional<string_type> {
  constexpr std::string_view key = "example:";
  if (!p_comment.starts_with(key))
    return std::nullopt;
  auto text = trimmed(p_comment.substr(key.size()));
  if (text.empty())
    return std::nullopt;
  return text;
}

// `a book has:` / `an author has:` (a record) and `a shape can:` (a trait):
// its name, and whether it is a trait.
struct declaration {
  string_type name;
  bool trait = false;
};

auto declaration_of(const string_type &p_line) -> std::optional<declaration> {
  declaration result;
  string_type text;
  if (p_line.ends_with(" has:"))
    text = p_line.substr(0, p_line.size() - 5);
  else if (p_line.ends_with(" can:")) {
    text = p_line.substr(0, p_line.size() - 5);
    result.trait = true;
  } else
    return std::nullopt;
  for (const char *article : {"a ", "an "})
    if (text.starts_with(article)) {
      result.name = trimmed(text.substr(std::string_view(article).size()));
      break;
    }
  const auto words = words_of(result.name);
  if (words.empty() || result.name.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ _-") !=
                           string_type::npos)
    return std::nullopt;
  return result;
}

// `a circle is a shape:` → circle, shape.
auto implementation_of(const string_type &p_line)
    -> std::optional<std::pair<string_type, string_type>> {
  if (!p_line.ends_with(":") || !(p_line.starts_with("a ") || p_line.starts_with("an ")))
    return std::nullopt;
  const auto at = p_line.find(" is a");
  if (at == string_type::npos)
    return std::nullopt;
  auto record = trimmed(p_line.substr(p_line.starts_with("an ") ? 3 : 2, at - (p_line.starts_with("an ") ? 3 : 2)));
  auto rest = p_line.substr(at + 5, p_line.size() - at - 6);
  if (rest.starts_with("n "))
    rest = rest.substr(2);
  else if (rest.starts_with(" "))
    rest = rest.substr(1);
  else
    return std::nullopt;
  rest = trimmed(rest);
  if (record.empty() || rest.empty())
    return std::nullopt;
  return std::make_pair(record, rest);
}

// `price_total = 5`, at the top of a package's file: its name and value.
auto value_of(const string_type &p_line) -> std::optional<std::pair<string_type, string_type>> {
  const auto equals = p_line.find(" = ");
  if (equals == string_type::npos)
    return std::nullopt;
  const auto name = trimmed(p_line.substr(0, equals));
  if (name.empty() || !std::ranges::all_of(name, [](char p_letter) {
        return std::isalnum(static_cast<unsigned char>(p_letter)) || p_letter == '_';
      }))
    return std::nullopt;
  return std::make_pair(name, trimmed(p_line.substr(equals + 3)));
}

auto indent_of(const string_type &p_line) -> std::size_t {
  return p_line.find_first_not_of(" \t");
}

// `a, b and c` for a list of pieces.
auto joined(const std::vector<string_type> &p_pieces, const char *p_separator = ", ")
    -> string_type {
  string_type result;
  for (const auto &piece : p_pieces)
    result += (result.empty() ? "" : p_separator) + piece;
  return result;
}

} // namespace

auto program_phrase_templates(const std::vector<phrase_file> &p_files,
                              const std::filesystem::path &p_from)
    -> std::vector<phrase_template> {
  struct written {
    phrase_template item;
    header_shape shape;
  };
  struct use_line {
    std::size_t file, line;
    string_type text;
  };
  // Records, traits and a package's top-level values (L7 rest): the same
  // scan, since their headers are declared before any line is read.
  struct declared {
    phrase_template item;
    header_shape shape;
    string_type name;
    bool trait = false;
    std::vector<string_type> needs, gives;
    std::size_t file = 0, line = 0;
  };
  std::vector<written> found;
  std::vector<declared> things;
  std::vector<std::pair<string_type, string_type>> implementations;
  std::vector<use_line> uses;
  for (std::size_t index = 0; index < p_files.size(); ++index) {
    const auto &file = p_files[index];
    const auto shown =
        file.name.empty() ? file.path.filename().string() : file.name;
    std::istringstream input(file.text);
    std::vector<string_type> comments;
    std::set<string_type> values;
    // The record or trait whose indented lines are being read, and the
    // indent of its own lines (what is deeper is a default's body).
    std::optional<std::size_t> open;
    std::size_t open_indent = 0;
    string_type line;
    for (std::size_t number = 1; std::getline(input, line); ++number) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      const auto text = trimmed(line);
      const bool top = !line.empty() && line.front() != ' ' && line.front() != '\t';
      if (top && text.front() != '#')
        open.reset();
      if (text.empty()) {
        comments.clear();
      } else if (top && text.front() == '#') {
        comments.push_back(trimmed(text.substr(1)));
      } else if (text.front() == '#' || text.starts_with("@[")) {
        continue;
      } else if (open && !top) {
        const auto indent = indent_of(line);
        if (open_indent == 0)
          open_indent = indent;
        if (indent == open_indent) {
          auto &thing = things[*open];
          // A trait's `describe (it) twice:` has a default; a field has none.
          (text.ends_with(":") ? thing.gives : thing.needs)
              .push_back(text.ends_with(":") ? text.substr(0, text.size() - 1) : text);
        }
      } else if (top && declaration_of(text)) {
        const auto what = *declaration_of(text);
        const auto above = std::exchange(comments, {});
        declared thing{.name = what.name, .trait = what.trait, .file = index, .line = number};
        auto words = words_of(what.name);
        thing.shape.runs = {words};
        thing.shape.pattern = joined(words, " ");
        try {
          auto read = read_templates("stmt  " + thing.shape.pattern + " => (nop)\n", shown);
          if (read.size() != 1)
            continue;
          thing.item = std::move(read.front());
        } catch (const error &) {
          continue;
        }
        for (const auto &comment : above) {
          if (const auto example = example_of(comment))
            thing.item.examples.push_back(*example);
          else
            thing.item.about += (thing.item.about.empty() ? "" : " ") + comment;
        }
        thing.item.title = string_type(what.trait ? "a " : "a ") + what.name +
                           (what.trait ? " (trait)" : " (record)");
        thing.item.file = shown;
        thing.item.line = number;
        thing.item.package = file.package;
        open = things.size();
        open_indent = 0;
        things.push_back(std::move(thing));
      } else if (top && implementation_of(text)) {
        comments.clear();
        implementations.push_back(*implementation_of(text));
        open.reset();
      } else if (top && !file.package.empty() && value_of(text) &&
                 values.insert(value_of(text)->first).second) {
        // A package's constant: its name's words find it, its value is in
        // the title. (A program's own names are variables, `--for`.)
        auto name = value_of(text)->first;
        const auto above = std::exchange(comments, {});
        declared thing{.name = name, .file = index, .line = number};
        std::ranges::replace(name, '_', ' ');
        const auto words = words_of(name);
        if (words.empty())
          continue;
        thing.shape.runs = {words};
        thing.shape.pattern = joined(words, " ");
        try {
          auto read = read_templates("stmt  " + thing.shape.pattern + " => (nop)\n", shown);
          if (read.size() != 1)
            continue;
          thing.item = std::move(read.front());
        } catch (const error &) {
          continue;
        }
        for (const auto &comment : above)
          if (const auto example = example_of(comment))
            thing.item.examples.push_back(*example);
          else
            thing.item.about += (thing.item.about.empty() ? "" : " ") + comment;
        thing.item.title = text.size() > 60 ? text.substr(0, 57) + "..." : text;
        thing.item.file = shown;
        thing.item.line = number;
        thing.item.package = file.package;
        things.push_back(std::move(thing));
        uses.push_back({index, number, text});
      } else if (top && text.starts_with("to ")) {
        const auto shape = header_of(text);
        const auto above = std::exchange(comments, {});
        if (!shape)
          continue;
        // Read as a template whose Core is of no use (it only names the slots); a slot takes `any`, the
        // header says no more.
        try {
          auto read = read_templates(
              "stmt  " + shape->pattern + " => (nop" + shape->core + ")\n", shown);
          if (read.size() != 1)
            continue;
          auto &item = read.front();
          for (const auto &comment : above) {
            // `# example: LINE` is an example the author wrote (L9), not
            // part of the sentence.
            if (const auto example = example_of(comment))
              item.examples.push_back(*example);
            else
              item.about += (item.about.empty() ? "" : " ") + comment;
          }
          item.file = shown;
          item.line = number;
          item.package = file.package;
          found.push_back({std::move(item), *shape});
        } catch (const error &) {
        }
      } else {
        comments.clear();
        uses.push_back({index, number, text});
      }
    }
  }
  const phrase_file *from = nullptr;
  for (const auto &file : p_files)
    if (!p_from.empty() && file.path.lexically_normal() == p_from.lexically_normal())
      from = &file;
  std::vector<phrase_template> result;
  for (auto &entry : found) {
    auto &item = entry.item;
    // The author's own examples come first; the uses fill up to two.
    const auto written_examples = item.examples.size();
    for (const auto &use : uses) {
      if (item.examples.size() >= std::max<std::size_t>(2, written_examples))
        break;
      if (!reads_to(entry.shape, words_of(use.text)))
        continue;
      const auto &file = p_files[use.file];
      item.examples.push_back(
          use.text + "   (" +
          (file.name.empty() ? file.path.filename().string() : file.name) +
          ", line " + std::to_string(use.line) + ")");
    }
    // A package's phrase reads only where that package is used.
    if (from != nullptr && !item.package.empty() && from->package.empty() &&
        !from->sees.empty() &&
        std::ranges::find(from->sees, item.package) == from->sees.end())
      item.about += (item.about.empty() ? "" : " ") + string_type("Not used by this file: ") +
                    "write `use " + item.package.substr(0, item.package.find('@')) +
                    "` to read it.";
    result.push_back(std::move(item));
  }
  for (auto &thing : things) {
    auto &item = thing.item;
    const auto sentence = [&](const string_type &p_text) {
      item.about += (item.about.empty() ? "" : " ") + p_text;
    };
    if (thing.trait) {
      if (!thing.needs.empty())
        sentence("Needs " + joined(thing.needs, "; ") + ".");
      if (!thing.gives.empty())
        sentence("Gives " + joined(thing.gives, "; ") + " by default.");
    } else if (!thing.needs.empty())
      sentence("Has " + joined(thing.needs) + ".");
    std::vector<string_type> who;
    for (const auto &[record, trait] : implementations)
      if (thing.trait && trait == thing.name)
        who.push_back(record);
      else if (!thing.trait && record == thing.name)
        who.push_back(trait);
    if (!who.empty())
      sentence(string_type(thing.trait ? "Implemented by " : "Is a ") + joined(who) + ".");
    const auto written_examples = item.examples.size();
    for (const auto &use : uses) {
      if (item.examples.size() >= std::max<std::size_t>(2, written_examples))
        break;
      if (use.file == thing.file && use.line == thing.line)
        continue;
      if (!reads_to(thing.shape, words_of(use.text)))
        continue;
      const auto &file = p_files[use.file];
      item.examples.push_back(
          use.text + "   (" +
          (file.name.empty() ? file.path.filename().string() : file.name) +
          ", line " + std::to_string(use.line) + ")");
    }
    result.push_back(std::move(item));
  }
  // The package's version, once (L7): the id has only the major.
  for (auto &item : result) {
    for (const auto &file : p_files)
      if (!item.package.empty() && file.package == item.package &&
          !file.package_version.empty()) {
        item.about += (item.about.empty() ? "" : " ") + string_type("From ") +
                      item.package.substr(0, item.package.find('@')) + " " +
                      file.package_version + ".";
        break;
      }
  }
  return result;
}


namespace {

// `deflateInit` → deflate, init; `ovt_point_sum` → ovt, point, sum: the
// words an author could say it by.
auto name_words(const string_type &p_name) -> std::vector<string_type> {
  std::vector<string_type> words;
  string_type current;
  const auto finish = [&] {
    if (!current.empty())
      words.push_back(current);
    current.clear();
  };
  for (std::size_t at = 0; at < p_name.size(); ++at) {
    const auto letter = static_cast<unsigned char>(p_name[at]);
    if (!std::isalnum(letter)) {
      finish();
      continue;
    }
    // A capital after a small letter or digit starts a word; the last
    // capital of a run starts one before a small letter (`CXCursor` → cx,
    // cursor).
    const bool upper = std::isupper(letter);
    const bool after_lower = at > 0 && !std::isupper(static_cast<unsigned char>(p_name[at - 1])) &&
                             std::isalnum(static_cast<unsigned char>(p_name[at - 1]));
    const bool before_lower = at + 1 < p_name.size() && std::islower(static_cast<unsigned char>(p_name[at + 1]));
    const bool after_upper = at > 0 && std::isupper(static_cast<unsigned char>(p_name[at - 1]));
    if (upper && (after_lower || (after_upper && before_lower)))
      finish();
    current += static_cast<char>(std::tolower(letter));
  }
  finish();
  return words;
}

auto word_pattern(const std::vector<string_type> &p_words) -> pattern {
  pattern result;
  for (const auto &word : p_words)
    result.push_back({.kind = pattern_element::kind_type::word, .word = word});
  return result;
}

} // namespace

auto c_header_templates(const std::vector<phrase_file> &p_files,
                        const std::filesystem::path &p_from,
                        const core::c_import_options &p_imports)
    -> std::vector<phrase_template> {
  if (!core::has_c_import_support())
    return {};
  const phrase_file *from = nullptr;
  for (const auto &file : p_files)
    if (!p_from.empty() && file.path.lexically_normal() == p_from.lexically_normal())
      from = &file;
  // The header lines of the file, and the lines of the program to find uses in.
  struct header {
    string_type name, library;
  };
  std::vector<header> headers;
  std::vector<std::pair<string_type, string_type>> lines;
  for (const auto &file : p_files) {
    const auto shown = file.name.empty() ? file.path.filename().string() : file.name;
    std::istringstream input(file.text);
    string_type line;
    for (std::size_t number = 1; std::getline(input, line); ++number) {
      const auto text = trimmed(line);
      if (text.empty() || text.front() == '#' || text.starts_with("@["))
        continue;
      const bool top = line.front() != ' ' && line.front() != '\t';
      if (top && text.starts_with("use ") && text.find(".h\"") != string_type::npos) {
        if (from != nullptr && &file != from)
          continue;
        std::vector<string_type> texts;
        for (std::size_t at = 0; (at = text.find('"', at)) != string_type::npos;) {
          const auto close = text.find('"', at + 1);
          if (close == string_type::npos)
            break;
          texts.push_back(text.substr(at + 1, close - at - 1));
          at = close + 1;
        }
        if (!texts.empty() && texts[0].ends_with(".h") &&
            std::ranges::none_of(headers, [&](const header &p_seen) {
              return p_seen.name == texts[0];
            }))
          headers.push_back({texts[0], texts.size() > 1 ? texts[1] : string_type()});
      } else
        lines.emplace_back(text + "   (" + shown + ", line " + std::to_string(number) + ")",
                           text);
    }
  }
  const auto uses_of = [&](const string_type &p_name) {
    std::vector<string_type> found;
    string_type wanted;
    for (const auto letter : p_name)
      wanted += static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    for (const auto &[shown, text] : lines) {
      if (found.size() >= 2)
        break;
      const auto words = words_of(text);
      if (std::ranges::find(words, wanted) != words.end())
        found.push_back(shown);
    }
    return found;
  };
  std::vector<phrase_template> result;
  std::vector<phrase_template> used;
  for (const auto &entry : headers) {
    const core::c_library *library = nullptr;
    try {
      library = &core::import_c_library(entry.name, entry.library, p_imports);
    } catch (const error &) {
      continue;
    }
    const auto stem = std::filesystem::path(entry.name).filename().string();
    const auto add = [&](const string_type &p_name,
                         const std::vector<string_type> &p_more_words, string_type p_title,
                         string_type p_about) {
      auto words = name_words(p_name);
      words.insert(words.end(), p_more_words.begin(), p_more_words.end());
      phrase_template item;
      item.patterns.push_back(word_pattern(words));
      item.title = std::move(p_title);
      item.about = std::move(p_about);
      item.examples = uses_of(p_name);
      item.file = stem;
      (item.examples.empty() ? result : used).push_back(std::move(item));
    };
    for (const auto &function : library->functions) {
      string_type title = function.name + "(";
      std::vector<string_type> parameter_words;
      for (std::size_t index = 0; index < function.parameters.size(); ++index) {
        const auto named = index < function.parameter_names.size() &&
                           !function.parameter_names[index].empty();
        if (named) {
          title += (index ? ", " : "") + function.parameter_names[index];
          for (auto &word : name_words(function.parameter_names[index]))
            parameter_words.push_back(std::move(word));
        } else
          title += string_type(index ? ", " : "") + "_";
        if (!function.is_c_generic)
          title += ": " + core::describe_in_english(core::c_parameter_type(function, index));
      }
      if (function.is_variadic)
        title += function.parameters.empty() ? "..." : ", ...";
      title += ")";
      if (function.is_c_generic)
        title += "   a C macro, for any types";
      else {
        const auto result_type = core::c_result_type(function);
        if (result_type != core::value_type::nothing())
          title += " gives " + core::describe_in_english(result_type);
        if (function.is_macro)
          title += "   a C macro";
      }
      auto about = function.comment;
      if (function.is_c_generic && about == "A C macro.")
        about.clear();
      add(function.name, parameter_words, std::move(title),
          (about.empty() ? "" : about + " ") + "From " + entry.name + ".");
    }
    for (const auto &constant : library->constants) {
      string_type value;
      switch (constant.kind) {
      case core::c_constant::kind_type::integer:
        value = std::to_string(constant.integer);
        break;
      case core::c_constant::kind_type::real:
        value = std::to_string(constant.real);
        break;
      case core::c_constant::kind_type::null:
        value = "null";
        break;
      case core::c_constant::kind_type::runtime:
        break;
      }
      add(constant.name, {}, constant.name + (value.empty() ? "" : " = " + value),
          "A C constant from " + entry.name + ".");
    }
    for (const auto &record : library->records) {
      string_type title = record.name + " has ";
      std::vector<string_type> field_words;
      for (std::size_t index = 0; index < record.fields.size(); ++index) {
        title += string_type(index ? ", " : "") + record.fields[index].name + " (" +
                 core::describe_in_english(record.fields[index].type) + ")";
        for (auto &word : name_words(record.fields[index].name))
          field_words.push_back(std::move(word));
      }
      add(record.name, field_words, std::move(title),
          "A C " + string_type(record.c_spelling.starts_with("union") ? "union" : "struct") +
              " from " + entry.name + ".");
    }
  }
  used.insert(used.end(), std::make_move_iterator(result.begin()),
              std::make_move_iterator(result.end()));
  return used;
}

auto check_package_examples(const std::vector<phrase_file> &p_files, std::size_t p_first,
                            const front_end_options &p_options) -> std::vector<string_type> {
  std::vector<string_type> problems;
  for (auto index = p_first; index < p_files.size(); ++index) {
    const auto &file = p_files[index];
    struct example {
      std::size_t line;
      string_type text;
      std::optional<header_shape> shape;
    };
    std::vector<example> found;
    std::vector<std::pair<std::size_t, string_type>> comments;
    std::istringstream input(file.text);
    string_type line;
    for (std::size_t number = 1; std::getline(input, line); ++number) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      const auto text = trimmed(line);
      const bool top = !line.empty() && line.front() != ' ' && line.front() != '\t';
      if (!top)
        continue;
      if (text.front() == '#') {
        comments.emplace_back(number, trimmed(text.substr(1)));
        continue;
      }
      const auto shape = header_of(text);
      for (const auto &[at, comment] : std::exchange(comments, {}))
        if (const auto written = example_of(comment))
          found.push_back({at, *written, shape});
    }
    if (found.empty())
      continue;
    // The examples alone, each on the line it has in the file, so a problem
    // cites the line the author wrote; the file stays to define the phrases.
    const auto shown = file.name.empty() ? file.path.filename().string() : file.name;
    string_type probe_text;
    std::size_t written = 1;
    for (const auto &item : found) {
      for (; written < item.line; ++written)
        probe_text += '\n';
      probe_text += item.text + '\n';
      ++written;
    }
    auto probe = file;
    probe.path.clear();
    probe.name = shown + " (examples)";
    probe.module = string_type(core::main_module);
    probe.text = probe_text;
    probe.lock = {};
    auto files = p_files;
    files.push_back(std::move(probe));
    auto options = p_options;
    options.sources_written = false;
    const auto result = compile_phrase_program(files, nullptr, options);
    for (const auto &problem : result.problems)
      if (problem.starts_with(shown + " (examples)"))
        problems.push_back(problem);
    for (const auto &item : found)
      if (item.shape && !reads_to(*item.shape, words_of(item.text)))
        problems.push_back(shown + ", line " + std::to_string(item.line) + ": the example `" +
                           item.text + "` doesn't use the words of the phrase below it.");
  }
  return problems;
}

namespace {

auto html_escaped(const string_type &p_text) -> string_type {
  string_type result;
  for (const auto letter : p_text) {
    switch (letter) {
    case '&': result += "&amp;"; break;
    case '<': result += "&lt;"; break;
    case '>': result += "&gt;"; break;
    case '"': result += "&quot;"; break;
    default: result += letter;
    }
  }
  return result;
}

// A page's file name: a topic's, after its package's when it has one.
auto page_name(const manual_entry &p_entry) -> string_type {
  string_type name = p_entry.package.empty() ? p_entry.topic : p_entry.package + "-" + p_entry.topic;
  for (auto &letter : name)
    if (!std::isalnum(static_cast<unsigned char>(letter)) && letter != '-' && letter != '_')
      letter = '_';
  return name + ".html";
}

auto html_page(const string_type &p_title, const string_type &p_body) -> string_type {
  return "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
         "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>" +
         html_escaped(p_title) +
         "</title>\n<style>body{font:16px/1.5 system-ui,sans-serif;max-width:46rem;"
         "margin:2rem auto;padding:0 1rem}code,pre{font-family:ui-monospace,monospace}"
         "pre{background:#f4f4f4;padding:.5rem .75rem;overflow-x:auto}"
         "h2{margin-top:2rem}.also{color:#555}</style>\n</head>\n<body>\n" +
         p_body + "</body>\n</html>\n";
}

// The attribute values `p_name="…"` of a page.
auto attribute_values(const string_type &p_page, const string_type &p_name)
    -> std::vector<string_type> {
  std::vector<string_type> values;
  const auto opening = " " + p_name + "=\"";
  for (auto at = p_page.find(opening); at != string_type::npos; at = p_page.find(opening, at)) {
    at += opening.size();
    const auto end = p_page.find('"', at);
    if (end == string_type::npos)
      break;
    values.push_back(p_page.substr(at, end - at));
    at = end;
  }
  return values;
}

auto read_text_file(const std::filesystem::path &p_path) -> string_type {
  std::ifstream stream(p_path, std::ios::binary);
  std::stringstream text;
  text << stream.rdbuf();
  return text.str();
}

} // namespace

auto write_manual_html(const std::vector<manual_entry> &p_entries,
                       const std::filesystem::path &p_directory)
    -> std::vector<string_type> {
  std::filesystem::create_directories(p_directory);
  // The entries of each page, in the order they came (the library's order
  // keeps a topic together).
  std::vector<string_type> pages;
  std::map<string_type, std::vector<const manual_entry *>> by_page;
  for (const auto &entry : p_entries) {
    const auto name = page_name(entry);
    if (!by_page.contains(name))
      pages.push_back(name);
    by_page[name].push_back(&entry);
  }
  const auto heading_of = [](const manual_entry &p_entry) {
    return p_entry.package.empty() ? p_entry.topic : p_entry.topic + ", from " + p_entry.package;
  };
  string_type index = "<h1>The Ovis manual</h1>\n<p>" + std::to_string(p_entries.size()) +
                      " phrases in " + std::to_string(pages.size()) + " topics.</p>\n";
  for (const auto &name : pages) {
    const auto &entries = by_page[name];
    index += "<h2><a href=\"" + name + "\">" + html_escaped(heading_of(*entries.front())) +
             "</a></h2>\n<ul>\n";
    string_type body = "<p><a href=\"index.html\">The Ovis manual</a></p>\n<h1>" +
                       html_escaped(heading_of(*entries.front())) + "</h1>\n";
    for (std::size_t number = 0; number < entries.size(); ++number) {
      const auto &entry = *entries[number];
      const auto anchor = "e" + std::to_string(number + 1);
      index += "<li><a href=\"" + name + "#" + anchor + "\">" + html_escaped(entry.pattern) +
               "</a></li>\n";
      body += "<h2 id=\"" + anchor + "\"><code>" + html_escaped(entry.pattern) + "</code></h2>\n";
      if (!entry.about.empty())
        body += "<p>" + html_escaped(entry.about) + "</p>\n";
      if (!entry.aliases.empty()) {
        body += "<p class=\"also\">Also: ";
        for (std::size_t other = 0; other < entry.aliases.size(); ++other)
          body += (other ? ", " : "") + ("<code>" + html_escaped(entry.aliases[other]) + "</code>");
        body += "</p>\n";
      }
      if (!entry.examples.empty()) {
        body += "<pre>";
        for (const auto &example : entry.examples)
          body += html_escaped(example) + "\n";
        body += "</pre>\n";
      }
    }
    index += "</ul>\n";
    std::ofstream(p_directory / name, std::ios::binary) << html_page(heading_of(*entries.front()), body);
  }
  std::ofstream(p_directory / "index.html", std::ios::binary) << html_page("The Ovis manual", index);

  // Check what was written, not what was meant: every link goes to a page
  // that is there and, with `#`, to an id on it.
  std::vector<string_type> problems;
  std::vector<string_type> written = {"index.html"};
  written.insert(written.end(), pages.begin(), pages.end());
  std::map<string_type, string_type> texts;
  for (const auto &name : written)
    texts[name] = read_text_file(p_directory / name);
  for (const auto &name : written)
    for (const auto &link : attribute_values(texts[name], "href")) {
      const auto hash = link.find('#');
      const auto target = link.substr(0, hash);
      const auto found = texts.find(target);
      if (found == texts.end())
        problems.push_back(name + ": the link " + link + " goes to a page that isn't there.");
      else if (hash != string_type::npos) {
        const auto ids = attribute_values(found->second, "id");
        if (std::ranges::find(ids, link.substr(hash + 1)) == ids.end())
          problems.push_back(name + ": the link " + link + " goes to an id that isn't there.");
      }
    }
  return problems;
}

} // namespace ovis::phrase

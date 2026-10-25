#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <numeric>
#include <optional>
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
                     .pattern = spoken_pattern(p_item, p_item.patterns.front()),
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
  std::vector<written> found;
  std::vector<use_line> uses;
  for (std::size_t index = 0; index < p_files.size(); ++index) {
    const auto &file = p_files[index];
    const auto shown =
        file.name.empty() ? file.path.filename().string() : file.name;
    std::istringstream input(file.text);
    std::vector<string_type> comments;
    string_type line;
    for (std::size_t number = 1; std::getline(input, line); ++number) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      const auto text = trimmed(line);
      const bool top = !line.empty() && line.front() != ' ' && line.front() != '\t';
      if (text.empty()) {
        comments.clear();
      } else if (top && text.front() == '#') {
        comments.push_back(trimmed(text.substr(1)));
      } else if (text.front() == '#' || text.starts_with("@[")) {
        continue;
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
  return result;
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

} // namespace ovis::phrase

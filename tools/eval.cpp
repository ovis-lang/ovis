// Measures the built-in model.
//
// `questions`: the phrase front end's questions (eval/questions.txt). Where
// the ranking puts the right reading, and how often the model picks it, how
// sure it is and how long it takes. `--no-model` measures the ranking only;
// `--compare-cpu` also asks on the CPU and compares the probabilities with
// the GPU's. `--min-first N` and `--min-shown N` fail the run when fewer
// cases have the right reading ranked first, or shown in the first question:
// CI keeps the ranking from getting worse as the library grows.
// `--orders N` asks every question in N orders of its readings (as ranked,
// reversed, then shuffled) and reports the spread: the order alone moves
// 3B's right picks by about 3 of 26, so one order can't judge a change
// smaller than that. `--both-orders` has the front end ask each question
// twice, the second time reversed, and average (`ask_both_orders`).
//
//   ovis_eval questions <cases.txt> [--model-path FILE] [--only N]
//             [--no-model] [--cpu] [--compare-cpu] [--threads N]
//             [--min-first N] [--min-shown N] [--orders N] [--both-orders]
//   ovis_eval questions <cases.txt> --with <program>
//   ovis_eval questions <cases.txt> --package <file.ovis> [...]
//   ovis_eval rewrites <cases.txt> [--model-path FILE] [--cpu] [--threads N]
//             [--only N]
//
// `rewrites`: lines no reading fits (eval/rewrite-questions.txt), each built
// with the model free to write it again in words the compiler reads; judged
// by what the program prints (`expect: none`: the line should be refused).
// So a change to the rewrite prompt is measured, not tried on a few lines.
//
// `--with`: whether a program's own phrases (its `to …:` blocks) make the
// questions worse. Each case is read with and without them, by the ranking
// alone (no model); a case is worse when the right reading loses first
// place, drops out of the readings shown, or the line stops asking because
// it now reads one way (most likely as the new phrase). Exits 1 if any is.
//
// `--package`: every case reads with the file's phrases as a dependency's
// (`use scope`), not its own: they are offered by phrase-scope.md S3's rule.
//   ovis_eval programs <folder> [--model-path FILE] [--no-model] [--cpu]
//             [--threads N]
//   ovis_eval manual <queries.txt> [--model-path FILE] [--no-model] [--cpu]
//             [--threads N] [--only N] [--min-first N] [--min-shown N]
//
// `manual`: queries to `ovis manual` (eval/manual-queries.txt), each with the
// entry meant: where it comes for the words alone and for the words joined
// with embeddings (L4). `--no-model` measures the words alone; with
// `--min-first` and `--min-shown` that fails the run when fewer queries have
// the entry first, or in the first 5 (CI's floor). `--choose-model FILE`: a
// chat model then chooses among the first 5 found (L5), measured against them.
//
// `programs`: the programs of eval/programs (M1.14), each with the readings
// its author meant in `# expect` lines: questions per program, how each was
// settled, whether that was the reading meant, and the time.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <sstream>

#include <llvm/IR/LLVMContext.h>

#include <ovis/codegen/llvm_codegen.hpp>
#include <ovis/english/language_model.hpp>
#include <ovis/phrase/front_end.hpp>
#include <ovis/phrase/phrase_check.hpp>

namespace {

using ovis::string_type;

struct question_case {
  // A reading, or "none" when none fits.
  string_type expected;
  string_type program;
};

auto read_question_cases(const std::filesystem::path &p_path)
    -> std::vector<question_case> {
  std::ifstream file(p_path);
  if (!file)
    throw std::runtime_error("cannot read " + p_path.string());
  std::vector<question_case> result;
  question_case current;
  const auto finish = [&] {
    if (!current.expected.empty() && !current.program.empty())
      result.push_back(current);
    current = {};
  };
  string_type line;
  while (std::getline(file, line)) {
    if (current.expected.empty() && line.starts_with("#"))
      continue;
    if (line.empty())
      finish();
    else if (line.starts_with("expect: "))
      current.expected = line.substr(8);
    else
      current.program += line + "\n";
  }
  finish();
  return result;
}

// One question as the front end asked it: the numbered readings (the last
// is "none of these") and the answer.
struct asked_question {
  std::vector<string_type> options;
  std::vector<double> probabilities;
  double milliseconds = 0;
};

// The order to show `p_count` readings in: 0 as ranked, 1 reversed, any
// other a shuffle seeded by the order and the readings, so a run repeats.
auto reading_order(std::size_t p_order, const std::vector<string_type> &p_readings)
    -> std::vector<std::size_t> {
  std::vector<std::size_t> result(p_readings.size());
  std::iota(result.begin(), result.end(), 0);
  if (p_order == 1) {
    std::ranges::reverse(result);
  } else if (p_order > 1) {
    std::uint32_t seed = 2166136261U ^ static_cast<std::uint32_t>(p_order);
    for (const auto &reading : p_readings)
      for (const auto letter : reading)
        seed = (seed ^ static_cast<unsigned char>(letter)) * 16777619U;
    // mt19937 is the same everywhere; the swaps are written out so no
    // library's shuffle decides.
    std::mt19937 generator(seed);
    for (auto index = result.size(); index > 1; --index)
      std::swap(result[index - 1], result[generator() % index]);
  }
  return result;
}

// Passes the front end's questions on to `inner`, recording them. Without
// `inner` it answers "none of these", so the front end asks only its first
// round, which holds every candidate in ranking order. With `order`, the
// readings reach `inner` in that order (`reading_order`), "none of these"
// still last; what is recorded is in the front end's order.
class recording_model final : public ovis::english::language_model {
public:
  ovis::english::language_model *inner = nullptr;
  std::size_t order = 0;
  std::vector<asked_question> asked;

  auto name() const -> string_type override { return "recording"; }
  auto embed(const std::vector<string_type> &p_texts)
      -> std::vector<std::vector<float>> override {
    if (inner == nullptr)
      return {};
    return inner->embed(p_texts);
  }
  auto chat(const std::vector<ovis::english::chat_message> &,
            ovis::string_view_type) -> string_type override {
    throw std::runtime_error("the phrase front end only chooses");
  }
  auto choose(const std::vector<ovis::english::chat_message> &p_messages,
              std::size_t p_options) -> std::vector<double> override {
    asked_question question;
    // The question with the option lines taken out, to put back reordered.
    std::vector<string_type> lines;
    std::size_t first_option = 0;
    std::istringstream text(p_messages.back().content);
    for (string_type line; std::getline(text, line);) {
      const auto dot = line.find(". ");
      if (dot != string_type::npos && dot > 0 &&
          line.find_first_not_of("0123456789") == dot) {
        if (question.options.empty())
          first_option = lines.size();
        question.options.push_back(line.substr(dot + 2));
      } else {
        lines.push_back(line);
      }
    }
    const auto start = std::chrono::steady_clock::now();
    if (inner != nullptr && order > 0 && question.options.size() > 2) {
      const std::vector<string_type> readings(question.options.begin(),
                                              question.options.end() - 1);
      auto shown = reading_order(order, readings);
      shown.push_back(readings.size());
      string_type content;
      for (std::size_t index = 0; index <= lines.size(); ++index) {
        if (index == first_option)
          for (std::size_t option = 0; option < shown.size(); ++option)
            content += string_type(1, ovis::english::choice_digit(option + 1)) +
                       ". " + question.options[shown[option]] + "\n";
        if (index < lines.size())
          content += lines[index] + (index + 1 < lines.size() ? "\n" : "");
      }
      auto messages = p_messages;
      messages.back().content = content;
      const auto probabilities = inner->choose(messages, p_options);
      question.probabilities.assign(probabilities.size(), 0);
      for (std::size_t option = 0; option < shown.size(); ++option)
        question.probabilities[shown[option]] = probabilities[option];
    } else if (inner != nullptr) {
      question.probabilities = inner->choose(p_messages, p_options);
    } else {
      question.probabilities.assign(p_options, 0);
      question.probabilities.back() = 1;
    }
    question.milliseconds = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - start)
                                .count();
    asked.push_back(question);
    return asked.back().probabilities;
  }
};

// The reading of the build's first decision, or "none".
auto decided_reading(const ovis::phrase::front_end_result &p_result)
    -> string_type {
  return p_result.decisions.empty() ? "none" : p_result.decisions.front().second;
}

// The file `--package` names, read as a dependency of every case.
string_type package_text;

// Reads a case's program, with the `--package` file as a dependency if one
// is given.
auto read_case(const string_type &p_program, ovis::english::language_model *p_model,
               const ovis::phrase::front_end_options &p_options)
    -> ovis::phrase::front_end_result {
  if (package_text.empty())
    return ovis::phrase::compile_phrases(p_program, p_model, p_options);
  // Away from any phrases/ folder.
  const auto folder = std::filesystem::temp_directory_path() / "ovis-eval-package";
  const std::vector<ovis::phrase::phrase_file> files = {
      {.path = folder / "scope" / "scope.ovis",
       .name = "scope@1/scope.ovis",
       .module = "scope@1.scope",
       .text = package_text,
       .locked = true,
       .package = "scope@1",
       .sees = {"scope@1", "scope@1#internal"}},
      {.path = folder / "main.ovis",
       .name = "main.ovis",
       .module = "main",
       .text = "use scope\n" + p_program,
       .sees = {"."},
       .aliases = {{"scope", "scope@1"}}}};
  auto read = ovis::phrase::compile_phrase_program(files, p_model, p_options);
  ovis::phrase::front_end_result result;
  result.problems = std::move(read.problems);
  result.decisions = std::move(read.decisions.back());
  result.timing = read.timing;
  return result;
}

auto percent(double p_value) -> string_type {
  return std::to_string(static_cast<int>(std::lround(p_value * 100))) + "%";
}

// The best option of a question, and how far it is ahead of the second.
auto best_of(const asked_question &p_question)
    -> std::pair<std::size_t, double> {
  std::vector<double> sorted = p_question.probabilities;
  std::ranges::sort(sorted, std::ranges::greater{});
  const auto best = static_cast<std::size_t>(
      std::ranges::max_element(p_question.probabilities) -
      p_question.probabilities.begin());
  return {best, sorted[0] - (sorted.size() > 1 ? sorted[1] : 0)};
}

struct question_totals {
  int cases = 0;
  int questioned = 0;
  // Settled by a rule or the ranking shortcut, and of those right.
  int shortcuts = 0;
  int shortcuts_right = 0;
  int ranked_first = 0;
  int ranked_in_question = 0;
  int confident = 0;
  int confident_right = 0;
  int guess_right = 0;
  int right = 0;
  int asks = 0;
  double ask_ms = 0;
  // The last question of each case: its best reading's probability and
  // margin, and whether that reading is right; for other thresholds.
  struct final_answer {
    double probability;
    double margin;
    bool is_reading;
    bool right;
  };
  std::vector<final_answer> finals;
  // --compare-cpu: questions asked on both, the largest difference in a
  // probability, and the cases whose outcome differs.
  int compared = 0;
  double largest_difference = 0;
  int disagreements = 0;
  // --orders: cases right in each order, and per case in how many orders.
  std::vector<int> right_in_order;
  std::vector<int> orders_right;
};

// `--embeddings-only`: every question settled by the model's embeddings
// (the ranking's ties ordered by similarity to the span, the first taken),
// no question asked. Below a margin the answer could be "none" instead
// (asking the author), so picks are also reported by margin.
auto run_embedding_choices(const std::vector<question_case> &p_cases,
                           ovis::english::language_model &p_model, int p_only)
    -> int {
  struct answer {
    bool readable;
    bool right;
    // How far the first reading's similarity is ahead of the second's, when
    // the ranking tied them; else infinite.
    double margin;
  };
  std::vector<answer> answers;
  double total_ms = 0;
  for (std::size_t index = 0; index < p_cases.size(); ++index) {
    if (p_only > 0 && static_cast<int>(index) + 1 != p_only)
      continue;
    const auto &item = p_cases[index];
    string_type logged;
    ovis::phrase::front_end_options options;
    options.model_budget_seconds = 1e9;
    options.choose_by_embedding = true;
    // The choice is measured, not what a rewrite makes of a "none".
    options.rewrite = false;
    options.log = [&](ovis::string_view_type p_line) {
      if (p_line.starts_with("embedding: "))
        logged = string_type(p_line);
    };
    recording_model recorder;
    recorder.inner = &p_model;
    const auto start = std::chrono::steady_clock::now();
    const auto result = ovis::phrase::compile_phrases(item.program, &recorder, options);
    total_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start)
                    .count();
    const auto decided = decided_reading(result);
    const auto at = logged.rfind("margin ");
    const auto margin = at == string_type::npos ? INFINITY
                                                : std::stod(logged.substr(at + 7));
    answers.push_back({item.expected != "none", decided == item.expected, margin});
    std::cout << "#" << index + 1 << " expect " << item.expected << "\n  "
              << (decided == item.expected ? "PASS" : "FAIL") << " " << decided
              << " (margin "
              << (std::isinf(margin) ? string_type("none tied")
                                     : std::to_string(margin).substr(0, 5))
              << ")\n";
  }
  const auto right = std::ranges::count_if(answers, &answer::right);
  std::cout << "\nEmbeddings only: " << right << " of " << answers.size()
            << " right, " << std::fixed << std::setprecision(0)
            << total_ms / std::max<std::size_t>(answers.size(), 1)
            << " ms per case.\n";
  // A pick below the margin goes to the author: right only when no reading
  // fits.
  std::cout << "By margin (at least: picked, right overall):";
  for (const double floor : {0.0, 0.01, 0.02, 0.03, 0.05, 0.08}) {
    int picked = 0;
    int correct = 0;
    for (const auto &item : answers) {
      const bool picks = item.margin >= floor;
      picked += picks;
      correct += picks ? item.right : !item.readable;
    }
    std::cout << std::setprecision(2) << " " << floor << ": " << picked << " "
              << correct << ";";
  }
  std::cout << "\n";
  return 0;
}

// The fewest cases that may have the right reading ranked first, and shown
// in the first question.
struct ranking_floor {
  int first = 0;
  int shown = 0;
};

auto run_questions(const std::vector<question_case> &p_cases,
                   ovis::english::language_model *p_model,
                   ovis::english::language_model *p_cpu_model, int p_only,
                   ranking_floor p_floor, std::size_t p_orders,
                   bool p_both_orders) -> int {
  ovis::phrase::front_end_options options;
  options.ask_both_orders = p_both_orders;
  options.model_budget_seconds = 1e9;
  // The choice is measured, not what a rewrite makes of a "none".
  options.rewrite = false;
  question_totals totals;
  totals.right_in_order.assign(p_orders, 0);
  for (std::size_t index = 0; index < p_cases.size(); ++index) {
    if (p_only > 0 && static_cast<int>(index) + 1 != p_only)
      continue;
    const auto &item = p_cases[index];
    ++totals.cases;
    std::cout << "#" << index + 1 << " expect " << item.expected << "\n";

    recording_model ranking;
    const auto unasked = read_case(item.program, &ranking, options);
    if (unasked.timing.questions == 0) {
      std::cout << "  FAIL no question asked\n";
      for (const auto &problem : unasked.problems)
        std::cout << "    " << problem << "\n";
      continue;
    }
    ++totals.questioned;
    if (ranking.asked.empty()) {
      // Settled by a rule or the ranking shortcut: no model needed.
      const auto answer = decided_reading(unasked);
      const bool right = answer == item.expected;
      ++totals.shortcuts;
      totals.shortcuts_right += right;
      // The shortcut takes the first-ranked reading.
      totals.ranked_first += right;
      totals.ranked_in_question += right;
      totals.right += right;
      // No model asked, so every order decides the same.
      for (auto &count : totals.right_in_order)
        count += right;
      if (p_orders > 1)
        totals.orders_right.push_back(right ? static_cast<int>(p_orders) : 0);
      std::cout << "  " << (right ? "PASS" : "FAIL") << " settled without the "
                << "model: " << answer << "\n";
      continue;
    }
    std::vector<string_type> candidates;
    for (const auto &question : ranking.asked)
      candidates.insert(candidates.end(), question.options.begin(),
                        question.options.end() - 1);
    const auto found = std::ranges::find(candidates, item.expected);
    const auto rank = found - candidates.begin() + 1;
    if (item.expected != "none") {
      totals.ranked_first += rank == 1;
      totals.ranked_in_question +=
          found != candidates.end() &&
          rank < static_cast<long>(ovis::english::max_choice_options);
    }
    std::cout << "  ranking: "
              << (item.expected == "none" ? string_type("-")
                  : found == candidates.end()
                      ? string_type("not a candidate")
                      : "#" + std::to_string(rank))
              << " of " << candidates.size() << ":";
    for (std::size_t shown = 0; shown < candidates.size() && shown < 4; ++shown)
      std::cout << (shown == 0 ? " " : ", ") << candidates[shown];
    std::cout << "\n";
    if (p_model == nullptr) {
      totals.right += item.expected == "none" || rank == 1;
      continue;
    }

    const auto ask = [&](ovis::english::language_model *p_inner,
                         std::size_t p_order = 0) {
      recording_model recorder;
      recorder.inner = p_inner;
      recorder.order = p_order;
      const auto result = read_case(item.program, &recorder, options);
      return std::pair{decided_reading(result), recorder.asked};
    };
    const auto [answer, asked] = ask(p_model);
    for (const auto &question : asked) {
      ++totals.asks;
      totals.ask_ms += question.milliseconds;
    }
    const auto &last = asked.back();
    const auto [best, margin] = best_of(last);
    const auto guess = best + 1 < last.probabilities.size()
                           ? last.options[best]
                           : string_type("none");
    const bool confident = answer != "none";
    const bool right = answer == item.expected;
    totals.confident += confident;
    totals.confident_right += confident && right;
    totals.guess_right += guess == item.expected;
    totals.finals.push_back({last.probabilities[best], margin,
                             best + 1 < last.probabilities.size(),
                             guess == item.expected});
    totals.right += right;
    std::cout << "  " << (right ? "PASS" : "FAIL") << " "
              << (confident ? "picked " + answer
                            : "unsure, best " + guess + " " +
                                  percent(last.probabilities[best]))
              << " (margin " << percent(margin) << ", " << asked.size()
              << " asked, " << static_cast<int>(last.milliseconds) << " ms)\n";

    if (p_orders > 1) {
      int right_orders = right;
      totals.right_in_order[0] += right;
      std::set<string_type> answers = {answer};
      for (std::size_t order = 1; order < p_orders; ++order) {
        const auto [other, other_asked] = ask(p_model, order);
        for (const auto &question : other_asked) {
          ++totals.asks;
          totals.ask_ms += question.milliseconds;
        }
        const bool other_right = other == item.expected;
        right_orders += other_right;
        totals.right_in_order[order] += other_right;
        answers.insert(other);
      }
      totals.orders_right.push_back(right_orders);
      std::cout << "  orders: right in " << right_orders << " of " << p_orders;
      // "none" is also what an unsure answer decides.
      if (answers.size() > 1) {
        std::cout << ", decided";
        for (const auto &decided : answers)
          std::cout << (decided == *answers.begin() ? " " : ", ") << decided;
      }
      std::cout << "\n";
    }

    if (p_cpu_model != nullptr) {
      const auto [cpu_answer, cpu_asked] = ask(p_cpu_model);
      for (std::size_t at = 0; at < asked.size() && at < cpu_asked.size(); ++at) {
        if (asked[at].options != cpu_asked[at].options)
          break;
        ++totals.compared;
        for (std::size_t option = 0; option < asked[at].probabilities.size(); ++option)
          totals.largest_difference =
              std::max(totals.largest_difference,
                       std::abs(asked[at].probabilities[option] -
                                cpu_asked[at].probabilities[option]));
      }
      if (cpu_answer != answer) {
        ++totals.disagreements;
        std::cout << "  the CPU decided " << cpu_answer << "\n";
      }
    }
    std::cout.flush();
  }

  const auto readable = totals.cases - static_cast<int>(std::ranges::count_if(
      p_cases, [](const question_case &p_case) { return p_case.expected == "none"; }));
  std::cout << "\n" << totals.questioned << " of " << totals.cases
            << " cases asked a question.\n"
            << "Ranking: the right reading first in " << totals.ranked_first
            << ", in the first question (top "
            << ovis::english::max_choice_options - 1 << ") in " << totals.ranked_in_question
            << ", of " << readable << " cases with one.\n"
            << "Settled without the model (rules, ranking shortcut): "
            << totals.shortcuts << ", right " << totals.shortcuts_right
            << ".\n";
  // The floor is for the whole set, not one case of it.
  const bool kept_floor = p_only > 0 ||
                          (totals.ranked_first >= p_floor.first &&
                           totals.ranked_in_question >= p_floor.shown);
  if (!kept_floor)
    std::cout << "FAIL the ranking got worse: at least " << p_floor.first
              << " first and " << p_floor.shown << " in the first question "
              << "are expected. If a change makes it better, raise the floor.\n";
  if (p_model == nullptr)
    return totals.questioned == totals.cases && kept_floor ? 0 : 1;
  std::cout << "Model: " << totals.right << " of " << totals.cases
            << " right. Confident in " << totals.confident << ", of those right "
            << totals.confident_right
            << (totals.confident > 0
                    ? " (" + percent(double(totals.confident_right) / totals.confident) + ")"
                    : string_type())
            << ". Best guess right in " << totals.guess_right << ".\n"
            << "Time: " << totals.asks << " questions asked, "
            << std::fixed << std::setprecision(0)
            << (totals.asks > 0 ? totals.ask_ms / totals.asks : 0)
            << " ms each on average.\n";
  if (p_orders > 1) {
    const auto [fewest, most] = std::ranges::minmax(totals.right_in_order);
    const auto all = std::accumulate(totals.right_in_order.begin(),
                                     totals.right_in_order.end(), 0);
    std::cout << "Over " << p_orders << " orders: right in " << fewest << " to "
              << most << " (mean " << std::setprecision(1) << std::fixed
              << double(all) / double(p_orders) << ")";
    std::cout.unsetf(std::ios::fixed);
    std::cout << "; right in every order " << std::ranges::count(totals.orders_right,
                                                       static_cast<int>(p_orders))
              << ", in none " << std::ranges::count(totals.orders_right, 0)
              << ", depends on the order "
              << std::ranges::count_if(totals.orders_right,
                                       [&](int p_count) {
                                         return p_count > 0 &&
                                                p_count < static_cast<int>(p_orders);
                                       })
              << ".\n";
  }
  // The front end takes p >= 0.6 and a margin >= 0.3; M1.14 tunes them.
  std::cout << "Picks at other thresholds (p, margin: picked, right):";
  for (const auto &[probability, margin] :
       std::vector<std::pair<double, double>>{
           {0.4, 0.1}, {0.5, 0.2}, {0.6, 0.3}, {0.7, 0.4}, {0.8, 0.5}}) {
    int picked = 0;
    int right = 0;
    for (const auto &answer : totals.finals)
      if (answer.is_reading && answer.probability >= probability &&
          answer.margin >= margin) {
        ++picked;
        right += answer.right;
      }
    std::cout << " " << percent(probability) << " " << percent(margin) << ": "
              << picked << " " << right << ";";
  }
  std::cout << "\n";
  if (p_cpu_model != nullptr)
    std::cout << "CPU: " << totals.compared << " questions compared, largest "
              << "difference in a probability " << std::setprecision(4)
              << totals.largest_difference << "; " << totals.disagreements
              << " cases decided differently.\n";
  return totals.right == totals.cases && kept_floor ? 0 : 1;
}

// A program of eval/programs and the readings its author meant.
struct labelled_program {
  string_type name;
  string_type text;
  // Span → the readings meant ("none": the author should be asked).
  std::map<string_type, std::vector<string_type>> meant;
};

auto read_programs(const std::filesystem::path &p_folder)
    -> std::vector<labelled_program> {
  std::vector<std::filesystem::path> paths;
  for (const auto &entry : std::filesystem::directory_iterator(p_folder))
    if (entry.path().extension() == ".ovis")
      paths.push_back(entry.path());
  std::ranges::sort(paths);
  std::vector<labelled_program> result;
  for (const auto &path : paths) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    labelled_program program{path.stem().string(), text.str()};
    std::istringstream lines(program.text);
    for (string_type line; std::getline(lines, line);) {
      // # expect "SPAN" is READING | READING
      if (!line.starts_with("# expect \""))
        continue;
      const auto is = line.find("\" is ", 10);
      if (is == string_type::npos)
        continue;
      auto &readings = program.meant[line.substr(10, is - 10)];
      std::istringstream choices(line.substr(is + 5));
      for (string_type choice; std::getline(choices, choice, '|');) {
        while (choice.starts_with(' '))
          choice.erase(0, 1);
        while (choice.ends_with(' '))
          choice.pop_back();
        readings.push_back(choice);
      }
    }
    result.push_back(std::move(program));
  }
  return result;
}

// The spans a failed build left to the author.
auto unsettled_spans(const std::vector<string_type> &p_problems)
    -> std::vector<string_type> {
  std::vector<string_type> result;
  for (const auto &problem : p_problems) {
    const auto open = problem.find(": \"");
    const auto close = problem.find("\" is unclear");
    if (open != string_type::npos && close != string_type::npos && close > open)
      result.push_back(problem.substr(open + 3, close - open - 3));
  }
  return result;
}

auto run_programs(const std::vector<labelled_program> &p_programs,
                  ovis::english::language_model *p_model) -> int {
  ovis::phrase::front_end_options options;
  options.model_budget_seconds = 1e9;
  std::vector<double> questions;
  std::vector<double> times;
  std::size_t by_rule = 0, by_ranking = 0, by_model = 0, to_author = 0;
  std::size_t model_right = 0, rules_right = 0, author_right = 0;
  std::size_t unlabelled = 0, built = 0;
  for (const auto &program : p_programs) {
    const auto start = std::chrono::steady_clock::now();
    const auto result = ovis::phrase::compile_phrases(program.text, p_model, options);
    const auto milliseconds = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
    const auto &timing = result.timing;
    questions.push_back(double(timing.questions));
    times.push_back(milliseconds);
    by_rule += timing.by_rule;
    by_ranking += timing.by_ranking;
    by_model += timing.by_model;
    built += result.succeeded();
    std::cout << program.name << ": " << timing.questions << " unclear ("
              << timing.by_rule << " rule, " << timing.by_ranking
              << " ranking, " << timing.by_model << " model), "
              << timing.asked << " asked, " << int(milliseconds) << " ms"
              << (result.succeeded() ? "" : ", not built") << "\n";
    // Which decisions came from the model isn't recorded per line: those the
    // rules and ranking make are made again without it.
    const auto offline = ovis::phrase::compile_phrases(program.text, nullptr);
    std::set<std::pair<string_type, string_type>> without_model;
    for (const auto &decision : offline.decisions)
      without_model.insert(decision);
    for (const auto &[span, reading] : result.decisions) {
      const auto found = program.meant.find(span);
      const auto by = without_model.contains({span, reading}) ? "rule/ranking"
                                                              : "model";
      if (found == program.meant.end()) {
        ++unlabelled;
        std::cout << "  ?    \"" << span << "\" is " << reading << " (" << by
                  << ", not labelled)\n";
        continue;
      }
      const bool right = std::ranges::find(found->second, reading) !=
                         found->second.end();
      (string_type(by) == "model" ? model_right : rules_right) += right;
      std::cout << "  " << (right ? "PASS" : "FAIL") << " \"" << span
                << "\" is " << reading << " (" << by << ")"
                << (right ? "" : ", meant " + found->second.front()) << "\n";
    }
    for (const auto &span : unsettled_spans(result.problems)) {
      ++to_author;
      const auto found = program.meant.find(span);
      const bool right = found != program.meant.end() &&
                         found->second.front() == "none";
      author_right += right;
      std::cout << "  " << (right ? "PASS" : "FAIL") << " \"" << span
                << "\" left to the author"
                << (found == program.meant.end()
                        ? string_type(" (not labelled)")
                        : right ? string_type()
                                : ", meant " + found->second.front())
                << "\n";
    }
    for (const auto &problem : result.problems)
      if (problem.find("\" is unclear") == string_type::npos)
        std::cout << "  problem: " << problem << "\n";
    std::cout.flush();
  }
  const auto median = [](std::vector<double> p_values) {
    std::ranges::sort(p_values);
    return p_values.empty() ? 0.0 : p_values[p_values.size() / 2];
  };
  std::cout << "\n"
            << p_programs.size() << " programs, " << built << " built. "
            << "Questions per program: median " << median(questions)
            << ", most " << std::ranges::max(questions) << ".\n"
            << "Settled by rules " << by_rule << ", by the ranking shortcut "
            << by_ranking << " (" << rules_right << " of these right), by the model "
            << by_model << " (" << model_right << " right), left to the author "
            << to_author << " (" << author_right << " meant to be).\n"
            << "Not labelled: " << unlabelled << ".\n"
            << "Time per program: median " << int(median(times)) << " ms, most "
            << int(std::ranges::max(times)) << " ms.\n";
  return 0;
}

// What the program prints, built and run.
auto output_of_program(const ovis::core::program &p_program) -> string_type {
  llvm::LLVMContext context;
  auto module = ovis::codegen::generate_module(p_program, context, "rewrite");
  ovis::codegen::optimize(*module);
  const auto executable =
      std::filesystem::temp_directory_path() / "ovis-eval-rewrite";
  ovis::codegen::build_executable(*module, executable,
                                  ovis::codegen::libraries_of(p_program), true,
                                  false, ovis::codegen::c_wrappers_of(p_program));
  string_type output;
  auto *const pipe = popen(executable.string().c_str(), "r");
  if (pipe == nullptr)
    throw std::runtime_error("cannot run the program");
  char buffer[256];
  while (const auto count = std::fread(buffer, 1, sizeof(buffer), pipe))
    output.append(buffer, count);
  pclose(pipe);
  std::filesystem::remove(executable);
  while (!output.empty() && (output.back() == '\n' || output.back() == ' '))
    output.pop_back();
  return output;
}

// Each case built with the model free to rewrite its unclear line.
auto run_rewrites(const std::vector<question_case> &p_cases,
                  ovis::english::language_model &p_model, int p_only) -> int {
  ovis::phrase::front_end_options options;
  options.model_budget_seconds = 1e9;
  std::size_t right = 0, rewritten = 0, refused_right = 0, expected_refusals = 0;
  std::vector<double> times;
  for (std::size_t index = 0; index < p_cases.size(); ++index) {
    if (p_only != 0 && static_cast<int>(index + 1) != p_only)
      continue;
    const auto &item = p_cases[index];
    const auto start = std::chrono::steady_clock::now();
    const auto result =
        ovis::phrase::compile_phrases(item.program, &p_model, options);
    times.push_back(std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count());
    const bool wants_none = item.expected == "none";
    string_type got = "none";
    if (result.succeeded())
      got = output_of_program(result.program);
    // The line as the compiler read it, from the sigil it wrote.
    string_type meaning;
    for (std::size_t at = result.source.find("@[meaning: ");
         at != string_type::npos; at = result.source.find("@[meaning: ", at + 1)) {
      const auto end = result.source.find("]\n", at);
      meaning = result.source.substr(at + 11, end - at - 11);
    }
    rewritten += result.timing.by_rewrite;
    expected_refusals += wants_none;
    const bool ok = got == item.expected;
    right += ok;
    refused_right += ok && wants_none;
    std::cout << "#" << index + 1 << " " << (ok ? "PASS" : "FAIL") << " "
              << int(times.back()) << " ms  expected " << item.expected
              << ", got " << got
              << (meaning.empty() ? string_type() : "  (read as: " + meaning + ")")
              << (result.timing.by_rewrite > 0 ? "  [rewritten]" : "") << "\n";
    std::cout.flush();
  }
  std::cout << "\n" << right << " of " << times.size() << " right ("
            << refused_right << " of " << expected_refusals
            << " refusals kept), " << rewritten << " lines rewritten.\n";
  return 0;
}


// One query's rank (from 1; 0 when absent): the first entry whose pattern, or
// another way of saying it, has `p_expected` in it.
auto manual_rank(const std::vector<ovis::phrase::manual_entry> &p_entries,
                 const string_type &p_expected) -> std::size_t {
  for (std::size_t index = 0; index < p_entries.size(); ++index) {
    const auto &entry = p_entries[index];
    if (entry.pattern.find(p_expected) != string_type::npos ||
        std::ranges::any_of(entry.aliases, [&](const string_type &p_alias) {
          return p_alias.find(p_expected) != string_type::npos;
        }))
      return index + 1;
  }
  return 0;
}

constexpr std::size_t manual_shown = 5;

auto run_manual(const std::vector<question_case> &p_cases,
                ovis::english::language_model *p_model, int p_only,
                ranking_floor p_floor, const string_type &p_query_prefix = {},
                const string_type &p_entry_prefix = {}, double p_lexical_weight = 0.5,
                ovis::english::language_model *p_chooser = nullptr) -> int {
  struct tally {
    int first = 0;
    int shown = 0;
  };
  tally words, meaning, alone;
  // L5, the chat model choosing among the first 5 found with embeddings: its
  // pick always moved first, and only when it is sure (`manual_choose`, as
  // `ovis manual` does).
  tally picked, sure;
  int none_first = 0;
  double choose_milliseconds = 0;
  int total = 0;
  const auto count = [](tally &p_tally, std::size_t p_rank) {
    p_tally.first += p_rank == 1;
    p_tally.shown += p_rank >= 1 && p_rank <= manual_shown;
  };
  const auto cache = ovis::english::model_cache_directory() / "manual-embeddings.bin";
  double milliseconds = 0;
  // Loading isn't part of a query's time.
  if (p_chooser != nullptr)
    p_chooser->choose({{"user", "1. yes\n2. no\nAnswer:"}}, 2);
  for (std::size_t index = 0; index < p_cases.size(); ++index) {
    if (p_only != 0 && static_cast<int>(index) + 1 != p_only)
      continue;
    std::vector<string_type> query;
    string_type current;
    for (const auto letter : p_cases[index].program + ' ')
      if (std::isalpha(static_cast<unsigned char>(letter)))
        current += static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
      else {
        if (!current.empty())
          query.push_back(current);
        current.clear();
      }
    const auto &expected = p_cases[index].expected;
    const auto by_words = manual_rank(
        ovis::phrase::manual_search(query, {}, 0), expected);
    count(words, by_words);
    std::size_t by_meaning = 0, by_alone = 0, by_picked = 0;
    double pick_probability = 0;
    if (p_model != nullptr) {
      const auto start = std::chrono::steady_clock::now();
      ovis::phrase::manual_embeddings options{.model = p_model,
                                              .cache = cache,
                                              .query_prefix = p_query_prefix,
                                              .entry_prefix = p_entry_prefix,
                                              .lexical_weight = p_lexical_weight};
      by_meaning = manual_rank(ovis::phrase::manual_search(query, {}, 0, options),
                               expected);
      milliseconds += std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count();
      count(meaning, by_meaning);
      options.lexical_weight = 0;
      by_alone = manual_rank(ovis::phrase::manual_search(query, {}, 0, options),
                             expected);
      count(alone, by_alone);
      if (p_chooser != nullptr) {
        options.lexical_weight = p_lexical_weight;
        const auto found = ovis::phrase::manual_search(query, {}, 0, options);
        string_type asked;
        for (const auto &word : query)
          asked += (asked.empty() ? "" : " ") + word;
        const auto choose_start = std::chrono::steady_clock::now();
        const auto count_shown = std::min(found.size(), ovis::phrase::manual_choice_count);
        const auto probabilities = ovis::phrase::manual_choice_probabilities(
            asked, {found.begin(), found.begin() + static_cast<std::ptrdiff_t>(count_shown)},
            *p_chooser);
        choose_milliseconds += std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - choose_start)
                                   .count();
        std::vector<std::size_t> all(probabilities.size());
        std::iota(all.begin(), all.end(), 0);
        std::ranges::stable_sort(all, std::ranges::greater{},
                                 [&](std::size_t p_index) { return probabilities[p_index]; });
        none_first += all[0] == count_shown;
        const auto best = static_cast<std::size_t>(
            std::ranges::max_element(probabilities.begin(), probabilities.end() - 1) -
            probabilities.begin());
        auto always = found;
        std::rotate(always.begin(), always.begin() + static_cast<std::ptrdiff_t>(best),
                    always.begin() + static_cast<std::ptrdiff_t>(best) + 1);
        count(picked, manual_rank(always, expected));
        by_picked = manual_rank(ovis::phrase::manual_choose(asked, found, *p_chooser),
                                expected);
        count(sure, by_picked);
        pick_probability = all[0] == count_shown ? -probabilities[all[0]] : probabilities[all[0]];
      }
    }
    ++total;
    std::cout << "#" << index + 1 << " " << expected << ": words "
              << (by_words ? std::to_string(by_words) : "-");
    if (p_model != nullptr)
      std::cout << ", with embeddings "
                << (by_meaning ? std::to_string(by_meaning) : "-")
                << ", embeddings alone " << (by_alone ? std::to_string(by_alone) : "-");
    if (p_chooser != nullptr)
      std::cout << ", chosen " << (by_picked ? std::to_string(by_picked) : "-") << " ("
                << (pick_probability < 0 ? "none " : "")
                << int(std::abs(pick_probability) * 100 + 0.5) << "%)";
    std::cout << "   (" << p_cases[index].program.substr(
                                0, p_cases[index].program.size() - 1)
              << ")\n";
  }
  std::cout << "\nRanking, words: the entry meant first in " << words.first
            << " of " << total << ", in the first " << manual_shown << " in "
            << words.shown << ".\n";
  if (p_model != nullptr)
    std::cout << "Ranking, with embeddings: first in " << meaning.first
              << " of " << total << ", in the first " << manual_shown << " in "
              << meaning.shown << "; " << std::fixed << std::setprecision(0)
              << milliseconds / std::max(1, total) << " ms a query.\n"
              << "Ranking, embeddings alone: first in " << alone.first << " of "
              << total << ", in the first " << manual_shown << " in " << alone.shown
              << ".\n";
  if (p_chooser != nullptr)
    std::cout << "Chosen by the model among the first " << ovis::phrase::manual_choice_count
              << ": its pick always first, first in " << picked.first
              << "; only when sure (ovis manual), first in " << sure.first << " (in the first "
              << manual_shown << ": " << sure.shown << "); \"none\" likeliest "
              << none_first << " times; " << std::fixed << std::setprecision(0)
              << choose_milliseconds / std::max(1, total) << " ms a query.\n";
  // The floor is for the words alone: it needs no model.
  if (words.first < p_floor.first || words.shown < p_floor.shown) {
    std::cout << "FAIL: below the floor (" << p_floor.first << " first, "
              << p_floor.shown << " shown).\n";
    return 1;
  }
  return 0;
}

} // namespace

auto main(int p_count, char **p_arguments) -> int {
  try {
    std::vector<string_type> arguments(p_arguments + 1,
                                       p_arguments + p_count);
    std::optional<std::filesystem::path> model_path;
    // `manual`: the chat model that chooses among the entries found (L5).
    std::optional<std::filesystem::path> choose_model_path;
    int only = 0;
    int threads = 0;
    std::size_t orders = 1;
    bool embeddings_only = false;
    string_type query_prefix, entry_prefix;
    double lexical_weight = ovis::phrase::manual_embeddings{}.lexical_weight;
    bool no_prefixes = false;
    bool no_model = false;
    bool cpu = false;
    bool compare_cpu = false;
    bool both_orders = false;
    ranking_floor floor;
    std::optional<std::filesystem::path> with;
    std::vector<string_type> positional;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      if (arguments[index] == "--model-path" && index + 1 < arguments.size())
        model_path = arguments[++index];
      else if (arguments[index] == "--choose-model" && index + 1 < arguments.size())
        choose_model_path = arguments[++index];
      else if (arguments[index] == "--only" && index + 1 < arguments.size())
        only = std::stoi(arguments[++index]);
      else if (arguments[index] == "--threads" && index + 1 < arguments.size())
        threads = std::stoi(arguments[++index]);
      else if (arguments[index] == "--min-first" && index + 1 < arguments.size())
        floor.first = std::stoi(arguments[++index]);
      else if (arguments[index] == "--min-shown" && index + 1 < arguments.size())
        floor.shown = std::stoi(arguments[++index]);
      else if (arguments[index] == "--orders" && index + 1 < arguments.size())
        orders = static_cast<std::size_t>(std::max(1, std::stoi(arguments[++index])));
      else if (arguments[index] == "--with" && index + 1 < arguments.size())
        with = arguments[++index];
      else if (arguments[index] == "--package" && index + 1 < arguments.size()) {
        std::ifstream file(arguments[++index]);
        std::stringstream text;
        text << file.rdbuf();
        package_text = text.str();
      }
      else if (arguments[index] == "--query-prefix" && index + 1 < arguments.size())
        query_prefix = arguments[++index];
      else if (arguments[index] == "--no-prefixes")
        no_prefixes = true;
      else if (arguments[index] == "--lexical-weight" && index + 1 < arguments.size())
        lexical_weight = std::stod(arguments[++index]);
      else if (arguments[index] == "--entry-prefix" && index + 1 < arguments.size())
        entry_prefix = arguments[++index];
      else if (arguments[index] == "--embeddings-only")
        embeddings_only = true;
      else if (arguments[index] == "--no-model")
        no_model = true;
      else if (arguments[index] == "--cpu")
        cpu = true;
      else if (arguments[index] == "--compare-cpu")
        compare_cpu = true;
      else if (arguments[index] == "--both-orders")
        both_orders = true;
      else
        positional.push_back(arguments[index]);
    }
    if (positional.size() != 2 ||
        (positional[0] != "questions" && positional[0] != "programs" &&
         positional[0] != "rewrites" && positional[0] != "manual")) {
      std::cerr << "usage: ovis_eval questions <cases.txt> [--model-path FILE] "
                   "[--only N] [--no-model] [--cpu] [--compare-cpu] "
                   "[--threads N] [--min-first N] [--min-shown N] [--orders N] "
                   "[--both-orders]\n"
                   "       ovis_eval questions <cases.txt> --embeddings-only "
                   "[--model-path FILE] [--only N]\n"
                   "       ovis_eval questions <cases.txt> --with <program>\n"
                   "       ovis_eval questions <cases.txt> --package <file.ovis> "
                   "[...]\n"
                   "       ovis_eval programs <folder> [--model-path FILE] "
                   "[--no-model] [--cpu] [--threads N]\n"
                   "       ovis_eval manual <queries.txt> [--model-path FILE] "
                   "[--no-model] [--cpu] [--threads N] [--only N] "
                   "[--min-first N] [--min-shown N] [--choose-model FILE]\n"
                   "       ovis_eval rewrites <cases.txt> [--model-path FILE] "
                   "[--only N] [--cpu] [--threads N]\n";
      return 2;
    }
    if (positional[0] == "questions" && with)
    {
      std::ifstream cases_file(positional[1]);
      std::stringstream cases_text;
      cases_text << cases_file.rdbuf();
      return ovis::phrase::check_phrases(
          ovis::phrase::parse_phrase_cases(cases_text.str()),
          ovis::phrase::read_phrase_definitions(*with), std::cout);
    }
    if (positional[0] == "manual" && no_model)
      return run_manual(read_question_cases(positional[1]), nullptr, only, floor);
    if (positional[0] == "programs" && no_model)
      return run_programs(read_programs(positional[1]), nullptr);
    if (positional[0] == "questions" && no_model)
      return run_questions(read_question_cases(positional[1]), nullptr,
                           nullptr, only, floor, 1, false);
    const auto location = ovis::english::find_builtin_model(
        model_path, std::filesystem::path(p_arguments[0]));
    if (!location) {
      std::cerr << "ovis_eval: no model found; run `ovis model pull`.\n";
      return 2;
    }
    ovis::english::builtin_model model(
        {.location = *location, .use_gpu = !cpu, .threads = threads});

    std::cout << "Model: " << location->file.filename().string() << "\n";
    // The manual only embeds: a model made for embedding can't choose. The
    // default embedding model's prefixes unless others are given, as `ovis
    // manual` does.
    const auto &embedder = ovis::english::default_embedding_model();
    if (positional[0] == "manual" && query_prefix.empty() && entry_prefix.empty() &&
        !no_prefixes && location->file.filename() == embedder.file_name) {
      query_prefix = embedder.query_prefix;
      entry_prefix = embedder.entry_prefix;
    }
    if (positional[0] == "manual") {
      std::unique_ptr<ovis::english::builtin_model> chooser;
      if (choose_model_path)
        chooser = std::make_unique<ovis::english::builtin_model>(ovis::english::builtin_options{
            .location = {.file = *choose_model_path}, .use_gpu = !cpu, .threads = threads});
      return run_manual(read_question_cases(positional[1]), &model, only, floor,
                        query_prefix, entry_prefix, lexical_weight, chooser.get());
    }
    // Loading isn't part of a question's time.
    const auto load = [](ovis::english::language_model &p_model) {
      const auto start = std::chrono::steady_clock::now();
      p_model.choose({{"user", "1. yes\n2. no\nAnswer:"}}, 2);
      return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                           start)
          .count();
    };
    std::cout << "Loaded in " << std::fixed << std::setprecision(1)
              << load(model) << " s.\n\n";
    std::unique_ptr<ovis::english::builtin_model> cpu_model;
    if (compare_cpu) {
      cpu_model = std::make_unique<ovis::english::builtin_model>(
          ovis::english::builtin_options{
              .location = *location, .use_gpu = false, .threads = threads});
      load(*cpu_model);
    }
    std::cout.unsetf(std::ios::fixed);
    if (positional[0] == "programs")
      return run_programs(read_programs(positional[1]), &model);
    if (positional[0] == "rewrites")
      return run_rewrites(read_question_cases(positional[1]), model, only);
    if (embeddings_only)
      return run_embedding_choices(read_question_cases(positional[1]), model, only);
    return run_questions(read_question_cases(positional[1]), &model,
                         cpu_model.get(), only, floor, orders, both_orders);
  } catch (const std::exception &p_error) {
    std::cerr << "ovis_eval: " << p_error.what() << "\n";
    return 2;
  }
}

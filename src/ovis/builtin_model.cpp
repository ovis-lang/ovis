#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include <ovis/english/language_model.hpp>

#ifdef OVIS_HAS_BUILTIN_LLM
#include <llama.h>
#endif

namespace ovis::english {

namespace {

// Trailer after weights appended to an executable: offset, size, magic.
constexpr std::array<char_type, 16> embedded_magic = {
    'O', 'V', 'I', 'S', '-', 'E', 'M', 'B', 'E', 'D', 'D', 'E', 'D', '-', '1', '\0'};
constexpr std::uint64_t trailer_size = 8 + 8 + embedded_magic.size();
// Keeps the GGUF data aligned so llama.cpp can map it straight from the file.
constexpr std::uint64_t embedded_alignment = 4096;

auto write_u64(std::ostream &p_output, std::uint64_t p_value) -> void {
  for (int byte = 0; byte < 8; ++byte)
    p_output.put(static_cast<char_type>((p_value >> (8 * byte)) & 0xff));
}

auto read_u64(const char_type *p_bytes) -> std::uint64_t {
  std::uint64_t result = 0;
  for (int byte = 7; byte >= 0; --byte)
    result = (result << 8) | static_cast<unsigned char>(p_bytes[byte]);
  return result;
}

auto find_embedded(const std::filesystem::path &p_executable)
    -> std::optional<model_location> {
  std::ifstream input(p_executable, std::ios::binary | std::ios::ate);
  if (!input)
    return std::nullopt;
  const auto file_size = static_cast<std::uint64_t>(input.tellg());
  if (file_size < trailer_size)
    return std::nullopt;
  std::array<char_type, trailer_size> trailer{};
  input.seekg(static_cast<std::streamoff>(file_size - trailer_size));
  input.read(trailer.data(), trailer.size());
  if (!input || std::memcmp(trailer.data() + 16, embedded_magic.data(),
                            embedded_magic.size()) != 0)
    return std::nullopt;
  const auto offset = read_u64(trailer.data());
  const auto size = read_u64(trailer.data() + 8);
  if (offset + size + trailer_size > file_size)
    return std::nullopt;
  return model_location{p_executable, offset, size, true};
}

auto environment_path(const char_type *p_name)
    -> std::optional<std::filesystem::path> {
  const auto *const value = std::getenv(p_name);
  if (value == nullptr || *value == '\0')
    return std::nullopt;
  return std::filesystem::path(value);
}

auto file_location(const std::filesystem::path &p_path)
    -> std::optional<model_location> {
  std::error_code code;
  if (!std::filesystem::is_regular_file(p_path, code))
    return std::nullopt;
  return model_location{p_path, 0, std::filesystem::file_size(p_path), false};
}

#ifdef OVIS_HAS_BUILTIN_LLM

auto quiet_log(ggml_log_level, const char *, void *) -> void {}

auto verbose_log(ggml_log_level, const char *p_text, void *) -> void {
  std::fputs(p_text, stderr);
}

#endif

} // namespace

auto default_models() -> const std::vector<default_model> & {
  static const std::vector<default_model> models = {
      {"3b", "qwen2.5-coder-3b-instruct-q4_k_m.gguf",
       "https://huggingface.co/Qwen/Qwen2.5-Coder-3B-Instruct-GGUF/"
       "resolve/main/qwen2.5-coder-3b-instruct-q4_k_m.gguf"},
      {"7b", "qwen2.5-coder-7b-instruct-q4_k_m.gguf",
       "https://huggingface.co/Qwen/Qwen2.5-Coder-7B-Instruct-GGUF/"
       "resolve/main/qwen2.5-coder-7b-instruct-q4_k_m.gguf"}};
  return models;
}

auto default_model_for(bool p_gpu) -> const default_model & {
  return default_models()[p_gpu ? 1 : 0];
}

auto default_embedding_model() -> const embedding_model & {
  static const embedding_model model = {
      "embeddinggemma-300M-Q8_0.gguf",
      "https://huggingface.co/ggml-org/embeddinggemma-300M-GGUF/"
      "resolve/main/embeddinggemma-300M-Q8_0.gguf",
      "task: search result | query: ", "title: none | text: "};
  return model;
}

auto find_embedding_model(const std::optional<std::filesystem::path> &p_explicit,
                          const std::filesystem::path &p_executable)
    -> std::optional<model_location> {
  if (p_explicit) {
    if (auto found = file_location(*p_explicit))
      return found;
    throw error("No model file at " + p_explicit->string() + ".");
  }
  const auto directory = p_executable.parent_path();
  const auto &model = default_embedding_model();
  for (const auto &candidate :
       {directory / model.file_name, directory / ".." / "share" / "ovis" / model.file_name,
        model_cache_directory() / model.file_name})
    if (auto found = file_location(candidate))
      return found;
  return std::nullopt;
}

auto model_cache_directory() -> std::filesystem::path {
  if (const auto cache = environment_path("XDG_CACHE_HOME"))
    return *cache / "ovis" / "models";
  if (const auto home = environment_path("HOME"))
    return *home / ".cache" / "ovis" / "models";
  return std::filesystem::temp_directory_path() / "ovis" / "models";
}

auto prompt_cache_directory() -> std::filesystem::path {
  return model_cache_directory().parent_path() / "prompts";
}

auto find_builtin_model(const std::optional<std::filesystem::path> &p_explicit,
                        const std::filesystem::path &p_executable, bool p_gpu)
    -> std::optional<model_location> {
  if (p_explicit) {
    if (auto found = file_location(*p_explicit))
      return found;
    throw error("No model file at " + p_explicit->string() + ".");
  }
  if (const auto from_environment = environment_path("OVIS_MODEL_PATH")) {
    if (auto found = file_location(*from_environment))
      return found;
    throw error("No model file at " + from_environment->string() +
                " (from OVIS_MODEL_PATH).");
  }
  if (auto embedded = find_embedded(p_executable))
    return embedded;
  const auto directory = p_executable.parent_path();
  for (const auto *const model :
       {&default_model_for(p_gpu), &default_model_for(!p_gpu)})
    for (const auto &candidate :
         {directory / model->file_name,
          directory / ".." / "share" / "ovis" / model->file_name,
          model_cache_directory() / model->file_name})
      if (auto found = file_location(candidate))
        return found;
  return std::nullopt;
}

auto embed_model(const std::filesystem::path &p_executable,
                 const std::filesystem::path &p_model,
                 const std::filesystem::path &p_output) -> void {
  std::ifstream executable(p_executable, std::ios::binary);
  std::ifstream model(p_model, std::ios::binary);
  if (!executable || !model)
    throw error("Cannot read " +
                (executable ? p_model : p_executable).string() + ".");

  // Re-embedding replaces earlier weights instead of stacking them.
  auto executable_size = std::filesystem::file_size(p_executable);
  if (const auto existing = find_embedded(p_executable))
    executable_size = existing->offset;

  auto temporary = p_output;
  temporary += ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
      throw error("Cannot write " + temporary.string() + ".");
    std::vector<char_type> chunk(1 << 20);
    auto remaining = executable_size;
    while (remaining > 0) {
      const auto count = std::min<std::uint64_t>(remaining, chunk.size());
      executable.read(chunk.data(), static_cast<std::streamsize>(count));
      output.write(chunk.data(), static_cast<std::streamsize>(count));
      remaining -= count;
    }
    const auto offset = (executable_size + embedded_alignment - 1) /
                        embedded_alignment * embedded_alignment;
    for (auto padding = executable_size; padding < offset; ++padding)
      output.put('\0');
    output << model.rdbuf();
    const auto size = std::filesystem::file_size(p_model);
    write_u64(output, offset);
    write_u64(output, size);
    output.write(embedded_magic.data(), embedded_magic.size());
    if (!output)
      throw error("Cannot write " + temporary.string() + ".");
  }
  std::filesystem::permissions(temporary,
                               std::filesystem::status(p_executable).permissions());
  // Renaming works even when replacing the running executable.
  std::filesystem::rename(temporary, p_output);
}

#ifdef OVIS_HAS_BUILTIN_LLM

auto has_builtin_model_support() -> bool { return true; }

struct builtin_model::state {
  FILE *file = nullptr;
  llama_model *model = nullptr;
  llama_context *context = nullptr;
  // For `embed`, made on first use.
  llama_context *embedding_context = nullptr;
  int threads = 1;
  const llama_vocab *vocabulary = nullptr;
  string_type chat_template;
  // Prompt tokens decoded in full batches, to reuse a shared prompt prefix.
  // Generated tokens are never reused: they were decoded one at a time.
  std::vector<llama_token> cached;
  std::uint32_t batch_size = 512;
  // What the computed state depends on besides the tokens, for the prompt
  // cache: the weights, llama.cpp, the device and the threads.
  string_type identity;

  ~state() {
    if (embedding_context != nullptr)
      llama_free(embedding_context);
    if (context != nullptr)
      llama_free(context);
    if (model != nullptr)
      llama_model_free(model);
    if (file != nullptr)
      std::fclose(file);
  }
};

builtin_model::builtin_model(builtin_options p_options)
    : options(std::move(p_options)) {}

builtin_model::~builtin_model() = default;

auto builtin_model::name() const -> string_type {
  return options.location.is_embedded
             ? "embedded (" + std::to_string(options.location.size) + " bytes)"
             : options.location.file.filename().string();
}

// Size plus a hash of the first and last MiB: cheap, and enough to tell
// different weights apart.
auto builtin_model::fingerprint() -> string_type {
  const auto &location = options.location;
  std::ifstream input(location.file, std::ios::binary);
  std::uint64_t hash = 14695981039346656037ULL;
  const auto mix = [&](std::uint64_t p_from) {
    std::vector<char_type> chunk(1 << 20);
    input.clear();
    input.seekg(static_cast<std::streamoff>(location.offset + p_from));
    input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    for (std::streamsize index = 0; index < input.gcount(); ++index)
      hash = (hash ^ static_cast<unsigned char>(chunk[index])) *
             1099511628211ULL;
  };
  mix(0);
  if (location.size > (1 << 20))
    mix(location.size - (1 << 20));
  char_type text[17];
  std::snprintf(text, sizeof(text), "%016llx",
                static_cast<unsigned long long>(hash));
  return "size " + std::to_string(location.size) + ", fnv " + text;
}

namespace {

// The first GPU llama.cpp can use, or nothing.
auto gpu_description() -> string_type {
  for (std::size_t index = 0; index < ggml_backend_dev_count(); ++index) {
    auto *const device = ggml_backend_dev_get(index);
    const auto type = ggml_backend_dev_type(device);
    if (type == GGML_BACKEND_DEVICE_TYPE_GPU ||
        type == GGML_BACKEND_DEVICE_TYPE_IGPU)
      return string_type("the GPU (") + ggml_backend_dev_description(device) +
             ")";
  }
  return {};
}

// Performance cores only: threads on efficiency cores hold every step back
// to their pace.
auto default_threads() -> int {
#ifdef __APPLE__
  int cores = 0;
  std::size_t size = sizeof(cores);
  if (sysctlbyname("hw.perflevel0.physicalcpu", &cores, &size, nullptr, 0) ==
          0 &&
      cores > 0)
    return cores;
#endif
  return static_cast<int>(std::max(std::thread::hardware_concurrency(), 1U));
}

} // namespace

auto has_gpu(bool p_verbose) -> bool {
  // Finding the devices starts the Metal backend, which logs a page of
  // what it compiled and found: shown only with --verbose.
  llama_log_set(p_verbose ? verbose_log : quiet_log, nullptr);
  llama_backend_init();
  return !gpu_description().empty();
}

auto builtin_model::load() -> state & {
  if (!loaded) {
    llama_log_set(options.verbose ? verbose_log : quiet_log, nullptr);
    llama_backend_init();
    auto state_box = std::make_unique<state>();

    auto model_parameters = llama_model_default_params();
    // The lockfile, not the device, keeps builds reproducible: a GPU may
    // break a near-tie differently from the CPU, but only paragraphs that
    // changed are translated again.
    const auto gpu = options.use_gpu ? gpu_description() : string_type();
    model_parameters.n_gpu_layers = gpu.empty() ? 0 : -1;
    // No weights repacked for the CPU's vector units: repacking 3B takes
    // ~7 s on every load, and a build asks only a few questions, each ~0.3 s
    // slower without it (measured, M1.15).
    model_parameters.use_extra_bufts = false;
    const auto &location = options.location;
    if (location.is_embedded) {
      state_box->file = std::fopen(location.file.string().c_str(), "rb");
      if (state_box->file == nullptr ||
          std::fseek(state_box->file, static_cast<long>(location.offset),
                     SEEK_SET) != 0)
        throw error("Cannot read the model embedded in " +
                    location.file.string() + ".");
      state_box->model =
          llama_model_load_from_file_ptr(state_box->file, model_parameters);
    } else {
      state_box->model = llama_model_load_from_file(
          location.file.string().c_str(), model_parameters);
    }
    if (state_box->model == nullptr)
      throw error("Cannot load the model " + name() + ".");

    const auto threads =
        options.threads > 0 ? options.threads : default_threads();
    auto context_parameters = llama_context_default_params();
    context_parameters.n_ctx = static_cast<std::uint32_t>(options.context_size);
    context_parameters.n_batch = state_box->batch_size;
    context_parameters.n_ubatch = state_box->batch_size;
    context_parameters.n_threads = threads;
    context_parameters.n_threads_batch = threads;
    context_parameters.no_perf = true;
    state_box->context =
        llama_init_from_model(state_box->model, context_parameters);
    if (state_box->context == nullptr)
      throw error("Cannot create a context for the model " + name() + ".");
    state_box->threads = threads;
    state_box->vocabulary = llama_model_get_vocab(state_box->model);
    if (const auto *const chat_template =
            llama_model_chat_template(state_box->model, nullptr))
      state_box->chat_template = chat_template;
    state_box->identity = string_type(OVIS_LLAMA_VERSION) + "\n" +
                          fingerprint() + "\n" +
                          (gpu.empty() ? "CPU" : gpu) + "\n" +
                          std::to_string(threads) + "\n" +
                          llama_print_system_info();
    loaded = std::move(state_box);
    if (options.log)
      options.log("Loaded the model " + name() + " on " +
                  (gpu.empty() ? "the CPU with " + std::to_string(threads) +
                                     " threads"
                               : gpu) +
                  ".");
  }
  return *loaded;
}

auto builtin_model::read_prompt(const std::vector<chat_message> &p_messages,
                                std::size_t p_reply_tokens) -> state & {
  auto &current = load();

  // A prompt in the model's own chat format (ChatML if it has none that
  // llama.cpp knows).
  const auto *const chat_template =
      current.chat_template.empty() ? "chatml" : current.chat_template.c_str();
  const auto format = [&](std::size_t p_count, bool p_reply) {
    std::vector<llama_chat_message> messages;
    for (std::size_t index = 0; index < p_count; ++index)
      messages.push_back({p_messages[index].role.c_str(),
                          p_messages[index].content.c_str()});
    string_type prompt;
    const auto length = llama_chat_apply_template(
        chat_template, messages.data(), messages.size(), p_reply, nullptr, 0);
    if (length < 0) {
      for (std::size_t index = 0; index < p_count; ++index)
        prompt += "<|im_start|>" + p_messages[index].role + "\n" +
                  p_messages[index].content + "<|im_end|>\n";
      if (p_reply)
        prompt += "<|im_start|>assistant\n";
    } else {
      prompt.resize(static_cast<std::size_t>(length));
      llama_chat_apply_template(chat_template, messages.data(), messages.size(),
                                p_reply, prompt.data(), length);
    }
    return prompt;
  };
  const auto tokenize = [&](const string_type &p_text) {
    std::vector<llama_token> tokens(p_text.size() + 16);
    const auto count = llama_tokenize(
        current.vocabulary, p_text.data(), static_cast<int32_t>(p_text.size()),
        tokens.data(), static_cast<int32_t>(tokens.size()), true, true);
    if (count < 0)
      throw error("Cannot tokenize the prompt.");
    tokens.resize(static_cast<std::size_t>(count));
    return tokens;
  };

  const auto prompt = format(p_messages.size(), true);
  auto tokens = tokenize(prompt);
  if (tokens.size() + p_reply_tokens >
      static_cast<std::size_t>(options.context_size))
    throw error("The paragraph and its context are too long for the model's "
                "context of " + std::to_string(options.context_size) +
                " tokens; split the paragraph.");

  // Batches start only at fixed places: multiples of the batch size, and the
  // end of the system message. A reused batch then holds exactly what
  // computing it again would give, so the reply doesn't depend on what was
  // asked before. The second place lets requests sharing a system message
  // shorter than a batch (every question of the phrase front end) reuse it.
  std::vector<std::size_t> starts;
  for (std::size_t start = 0; start < tokens.size();
       start += current.batch_size)
    starts.push_back(start);
  std::size_t system_end = 0;
  if (p_messages.size() > 1 && p_messages.front().role == "system") {
    const auto system = format(1, false);
    const auto system_tokens = tokenize(system);
    if (prompt.starts_with(system) && system_tokens.size() < tokens.size() &&
        std::equal(system_tokens.begin(), system_tokens.end(), tokens.begin())) {
      starts.push_back(system_tokens.size());
      system_end = system_tokens.size();
    }
  }
  // And where the options of a numbered question start: the questions of a
  // run-off share everything before them (the line, the names, the
  // question).
  if (const auto options = prompt.rfind("\n1. "); options != string_type::npos) {
    const auto header_tokens = tokenize(prompt.substr(0, options + 1));
    if (header_tokens.size() < tokens.size() &&
        std::equal(header_tokens.begin(), header_tokens.end(), tokens.begin()))
      starts.push_back(header_tokens.size());
  }
  std::ranges::sort(starts);
  const auto [last, end] = std::ranges::unique(starts);
  starts.erase(last, end);

  // Reuse up to the last start the previous prompt shares, keeping at least
  // one token to decode, since its logits are the ones needed.
  std::size_t common = 0;
  while (common < tokens.size() && common < current.cached.size() &&
         tokens[common] == current.cached[common])
    ++common;
  std::size_t shared = 0;
  for (const auto start : starts)
    if (start <= common && start < tokens.size())
      shared = start;
  auto *const memory = llama_get_memory(current.context);
  llama_memory_seq_rm(memory, 0, static_cast<llama_pos>(shared), -1);
  current.cached.assign(tokens.begin(),
                        tokens.begin() + static_cast<std::ptrdiff_t>(shared));

  // A system message this process hasn't read may have been read by an
  // earlier one: its state is in the cache. It is what decoding the same
  // batch here would give, so replies don't change.
  const auto cache_file = [&] {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&](string_view_type p_text) {
      for (const auto character : p_text)
        hash = (hash ^ static_cast<unsigned char>(character)) * 1099511628211ULL;
    };
    mix(current.identity);
    for (std::size_t index = 0; index < system_end; ++index)
      mix(std::to_string(tokens[index]) + ",");
    char_type name[32];
    std::snprintf(name, sizeof(name), "%016llx.state",
                  static_cast<unsigned long long>(hash));
    return prompt_cache_directory() / name;
  };
  if (options.cache_prompts && system_end > 0 && shared == 0) {
    const auto path = cache_file();
    std::vector<llama_token> stored(system_end);
    std::size_t count = 0;
    std::error_code code;
    if (std::filesystem::exists(path, code) &&
        llama_state_seq_load_file(current.context, path.string().c_str(), 0,
                                  stored.data(), stored.size(), &count) > 0 &&
        count == system_end &&
        std::equal(stored.begin(), stored.end(), tokens.begin())) {
      shared = system_end;
      current.cached = std::move(stored);
    } else {
      llama_memory_seq_rm(memory, 0, 0, -1);
    }
  }

  for (auto start = std::ranges::find(starts, shared); start != starts.end();
       ++start) {
    const auto begin = *start;
    const auto finish = std::next(start) == starts.end() ? tokens.size()
                                                         : *std::next(start);
    if (llama_decode(current.context,
                     llama_batch_get_one(tokens.data() + begin,
                                         static_cast<int32_t>(finish - begin))) != 0)
      throw error("The model failed to read the prompt.");
    current.cached.insert(current.cached.end(), tokens.begin() + begin,
                          tokens.begin() + finish);
    // Saved once the whole system message is read, in one batch or several
    // (the rewrite's lists the phrases): switching between system messages
    // then loads it instead of reading it again.
    if (options.cache_prompts && system_end > 0 && finish == system_end) {
      // Written whole or not at all: a reader never sees half a file.
      std::error_code code;
      const auto path = cache_file();
      std::filesystem::create_directories(path.parent_path(), code);
      auto partial = path;
      partial += ".part";
      if (!code && llama_state_seq_save_file(current.context,
                                             partial.string().c_str(), 0,
                                             tokens.data(), system_end) > 0)
        std::filesystem::rename(partial, path, code);
      else
        std::filesystem::remove(partial, code);
    }
  }
  return current;
}

auto builtin_model::chat(const std::vector<chat_message> &p_messages,
                         string_view_type p_grammar) -> string_type {
  auto &current =
      read_prompt(p_messages, static_cast<std::size_t>(options.max_tokens));

  llama_sampler *grammar = nullptr;
  if (!p_grammar.empty()) {
    const string_type text(p_grammar);
    grammar = llama_sampler_init_grammar(current.vocabulary, text.c_str(),
                                         "root");
    if (grammar == nullptr)
      throw error("Internal compiler error: the translation grammar is "
                  "invalid.");
  }

  const auto vocabulary_size = llama_vocab_n_tokens(current.vocabulary);
  std::vector<llama_token_data> candidates(
      static_cast<std::size_t>(vocabulary_size));
  // Greedy decoding under the grammar: the most likely token the grammar
  // allows. Checking only the top token first gives the same answer as
  // checking the whole vocabulary, which is only needed when it is refused.
  const auto next_token = [&]() -> llama_token {
    const auto *const logits = llama_get_logits_ith(current.context, -1);
    const auto *const best = std::max_element(logits, logits + vocabulary_size);
    const auto token = static_cast<llama_token>(best - logits);
    if (grammar == nullptr)
      return token;
    llama_token_data top{token, *best, 0};
    llama_token_data_array single{&top, 1, -1, false};
    llama_sampler_apply(grammar, &single);
    if (top.logit != -INFINITY)
      return token;
    for (llama_token index = 0; index < vocabulary_size; ++index)
      candidates[static_cast<std::size_t>(index)] = {index, logits[index], 0};
    llama_token_data_array all{candidates.data(), candidates.size(), -1, false};
    llama_sampler_apply(grammar, &all);
    const auto allowed = std::max_element(
        candidates.begin(), candidates.end(),
        [](const auto &p_left, const auto &p_right) {
          return p_left.logit < p_right.logit;
        });
    if (allowed->logit == -INFINITY)
      throw error("the grammar allows no further token");
    return allowed->id;
  };

  string_type reply;
  try {
    for (int generated = 0; generated < options.max_tokens; ++generated) {
      auto token = next_token();
      if (grammar != nullptr)
        llama_sampler_accept(grammar, token);
      if (llama_vocab_is_eog(current.vocabulary, token))
        break;
      char_type piece[256];
      const auto piece_length = llama_token_to_piece(
          current.vocabulary, token, piece, sizeof(piece), 0, false);
      if (piece_length > 0)
        reply.append(piece, static_cast<std::size_t>(piece_length));
      if (llama_decode(current.context, llama_batch_get_one(&token, 1)) != 0)
        throw error("The model failed while writing its reply.");
    }
  } catch (const std::exception &p_error) {
    if (grammar != nullptr)
      llama_sampler_free(grammar);
    throw error(string_type("The model stopped: ") + p_error.what());
  }
  if (grammar != nullptr)
    llama_sampler_free(grammar);
  return reply;
}

auto builtin_model::choose(const std::vector<chat_message> &p_messages,
                           std::size_t p_options) -> std::vector<double> {
  choice_grammar(p_options); // checks the number of options
  auto &current = read_prompt(p_messages, 1);
  const auto *const logits = llama_get_logits_ith(current.context, -1);
  std::vector<double> scores;
  for (std::size_t option = 1; option <= p_options; ++option) {
    const auto digit = string_type(1, choice_digit(option));
    llama_token token = 0;
    if (llama_tokenize(current.vocabulary, digit.data(),
                       static_cast<int32_t>(digit.size()), &token, 1, false,
                       false) != 1)
      throw error("The model " + name() + " has no single token for the "
                  "option " + digit + ".");
    scores.push_back(logits[token]);
  }
  return softmax(scores);
}

auto builtin_model::embed(const std::vector<string_type> &p_texts)
    -> std::vector<std::vector<float>> {
  auto &current = load();
  // A model made for embedding says how to pool in its file (`bert.pooling_type`)
  // and needs its special tokens (`[CLS]`, `<eos>`); a chat model has neither.
  char_type architecture[64] = {};
  char_type pooling[16] = {};
  const bool dedicated =
      llama_model_meta_val_str(current.model, "general.architecture", architecture,
                               sizeof architecture) > 0 &&
      llama_model_meta_val_str(current.model,
                               (string_type(architecture) + ".pooling_type").c_str(),
                               pooling, sizeof pooling) > 0;
  std::vector<std::vector<llama_token>> texts;
  std::size_t total = 0;
  for (const auto &text : p_texts) {
    const auto size = static_cast<int32_t>(text.size());
    std::vector<llama_token> tokens(text.size() + 3);
    const auto count =
        llama_tokenize(current.vocabulary, text.data(), size, tokens.data(),
                       static_cast<int32_t>(tokens.size()), dedicated, false);
    if (count <= 0)
      throw error("The model " + name() + " can't read \"" + text + "\".");
    tokens.resize(static_cast<std::size_t>(count));
    total += tokens.size();
    texts.push_back(std::move(tokens));
  }
  if (texts.empty())
    return {};
  // Up to 64 short readings, as one batch of sequences that don't see each
  // other.
  constexpr std::uint32_t max_texts = 64;
  constexpr std::uint32_t max_tokens = 4096;
  if (texts.size() > max_texts || total > max_tokens)
    throw error("Too much text to compare at once.");
  if (current.embedding_context == nullptr) {
    auto parameters = llama_context_default_params();
    parameters.n_ctx = max_tokens;
    parameters.n_batch = max_tokens;
    parameters.n_ubatch = max_tokens;
    parameters.n_seq_max = max_texts;
    parameters.kv_unified = true;
    parameters.n_threads = current.threads;
    parameters.n_threads_batch = current.threads;
    parameters.embeddings = true;
    // The last token has read the whole text. Measured on
    // eval/questions.txt, it ranked better than the mean of all tokens.
    parameters.pooling_type =
        dedicated ? LLAMA_POOLING_TYPE_UNSPECIFIED : LLAMA_POOLING_TYPE_LAST;
    parameters.no_perf = true;
    current.embedding_context = llama_init_from_model(current.model, parameters);
    if (current.embedding_context == nullptr)
      throw error("Cannot create a context for the model " + name() + ".");
  }
  auto *const context = current.embedding_context;
  llama_memory_clear(llama_get_memory(context), true);
  auto batch = llama_batch_init(static_cast<int32_t>(total), 0, 1);
  for (std::size_t sequence = 0; sequence < texts.size(); ++sequence)
    for (std::size_t index = 0; index < texts[sequence].size(); ++index) {
      const auto at = batch.n_tokens++;
      batch.token[at] = texts[sequence][index];
      batch.pos[at] = static_cast<llama_pos>(index);
      batch.n_seq_id[at] = 1;
      batch.seq_id[at][0] = static_cast<llama_seq_id>(sequence);
      batch.logits[at] = index + 1 == texts[sequence].size();
    }
  const auto status = llama_decode(context, batch);
  llama_batch_free(batch);
  if (status != 0)
    throw error("The model " + name() + " failed to read the readings.");
  const auto size = static_cast<std::size_t>(llama_model_n_embd(current.model));
  std::vector<std::vector<float>> result;
  for (std::size_t sequence = 0; sequence < texts.size(); ++sequence) {
    const auto *const values = llama_get_embeddings_seq(
        context, static_cast<llama_seq_id>(sequence));
    if (values == nullptr)
      throw error("The model " + name() + " gave no embedding.");
    std::vector<float> vector(values, values + size);
    double length = 0;
    for (const auto value : vector)
      length += double(value) * value;
    length = std::sqrt(length);
    for (auto &value : vector)
      value = length > 0 ? float(value / length) : 0;
    result.push_back(std::move(vector));
  }
  return result;
}

#else

auto has_builtin_model_support() -> bool { return false; }

auto has_gpu(bool) -> bool { return false; }

struct builtin_model::state {};

builtin_model::builtin_model(builtin_options p_options)
    : options(std::move(p_options)) {}

builtin_model::~builtin_model() = default;

auto builtin_model::name() const -> string_type { return "unavailable"; }

auto builtin_model::fingerprint() -> string_type { return {}; }

auto builtin_model::chat(const std::vector<chat_message> &, string_view_type)
    -> string_type {
  throw error("This build of Ovis has no built-in model (it was configured "
              "with -DOVIS_BUILTIN_LLM=OFF). Use --endpoint or --offline.");
}

auto builtin_model::choose(const std::vector<chat_message> &p_messages,
                           std::size_t p_options) -> std::vector<double> {
  return language_model::choose(p_messages, p_options);
}

auto builtin_model::embed(const std::vector<string_type> &)
    -> std::vector<std::vector<float>> {
  return {};
}

#endif

} // namespace ovis::english

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

// ovisup (tools/ovisup, docs/deployment.md): setting up, updating and
// removing Ovis, against releases served from a folder (OVISUP_DIST_SERVER)
// holding packages whose ovis is a small script.

using Catch::Matchers::ContainsSubstring;

namespace {

struct outcome {
  int status = 0;
  std::string output;
};

auto read_text(const std::filesystem::path &p_path) -> std::string {
  std::ifstream input(p_path);
  std::stringstream text;
  text << input.rdbuf();
  return text.str();
}

auto write_text(const std::filesystem::path &p_path, const std::string &p_text)
    -> void {
  std::filesystem::create_directories(p_path.parent_path());
  std::ofstream(p_path) << p_text;
}

const std::string ovisup = OVIS_SOURCE_DIR "/tools/ovisup";

// A home folder, a release server and the commands that use them.
class setup {
public:
  std::filesystem::path root;
  std::filesystem::path home;
  std::filesystem::path server;

  explicit setup(const std::string &p_name)
      : root(std::filesystem::temp_directory_path() / ("ovis-ovisup-" + p_name)),
        home(root / "home"), server(root / "server") {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(home);
    std::filesystem::create_directories(server);
  }
  ~setup() { std::filesystem::remove_all(root); }

  // Runs a command with this home and server, standard error included.
  auto run(const std::string &p_command) const -> outcome {
    const auto command = "cd '" + root.string() + "' && HOME='" + home.string() +
                         "' OVISUP_DIST_SERVER='file://" + server.string() +
                         "' XDG_CACHE_HOME= OVIS_HOME= OVIS_TOOLCHAIN= " +
                         "PATH=\"$PATH\" " + p_command + " 2>&1 < /dev/null";
    outcome result;
    auto *const pipe = popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buffer[256];
    while (const auto count = std::fread(buffer, 1, sizeof(buffer), pipe))
      result.output.append(buffer, count);
    result.status = pclose(pipe);
    return result;
  }

  // Publishes a package under TAG whose ovis says WHAT when run, and whose
  // --version fails unless it runs.
  auto publish(const std::string &p_tag, const std::string &p_what,
               bool p_runs = true) const -> void {
    const auto stage = root / "stage" / p_tag;
    std::filesystem::remove_all(stage);
    write_text(stage / "ovis" / "bin" / "ovis",
               "#!/bin/sh\n" + std::string(p_runs ? "" : "exit 1\n") +
                   "[ \"$1\" = --version ] && { echo 'ovis 0.1.0'; exit 0; }\n"
                   "echo \"" + p_what + ": $*\"\n");
    write_text(stage / "ovis" / "VERSION", "ovis 0.1.0 (" + p_what + ")\n");
    const auto directory = server / p_tag;
    std::filesystem::create_directories(directory);
    const auto made = run(
        "chmod +x '" + (stage / "ovis" / "bin" / "ovis").string() +
        "' && file=ovis-$(uname -s | sed 's/Linux/linux/;s/Darwin/macos/')-"
        "$(uname -m | sed 's/amd64/x86_64/;s/arm64/aarch64/').tar.gz && "
        "tar -C '" + stage.string() + "' -czf '" + directory.string() +
        "'/$file ovis && cd '" + directory.string() + "' && "
        "{ sha256sum $file 2>/dev/null || shasum -a 256 $file; } > $file.sha256"
        " && cp '" + ovisup + "' ovisup");
    INFO(made.output);
    REQUIRE(made.status == 0);
  }

  auto ovis() const -> std::string { return (home / ".ovis" / "bin" / "ovis").string(); }
  auto tool() const -> std::string {
    return "sh '" + (home / ".ovis" / "bin" / "ovisup").string() + "'";
  }
};

} // namespace

TEST_CASE("ovisup sets up Ovis, nightly while there is no stable release.",
          "[ovisup]") {
  setup space("init");
  write_text(space.home / ".bashrc", "export FOO=1");
  space.publish("nightly", "night one");

  const auto installed = space.run("sh '" + ovisup + "' -y");
  INFO(installed.output);
  REQUIRE(installed.status == 0);
  CHECK_THAT(installed.output, ContainsSubstring("no stable release yet"));
  CHECK_THAT(installed.output, ContainsSubstring("installed nightly"));

  // The shim runs the default toolchain, and ovisup is in bin/.
  const auto ran = space.run(space.ovis() + " build x.ovis");
  CHECK(ran.status == 0);
  CHECK_THAT(ran.output, ContainsSubstring("night one: build x.ovis"));
  CHECK(read_text(space.home / ".ovis" / "settings") == "default=nightly\n");
  CHECK_THAT(space.run(space.tool() + " list").output,
             ContainsSubstring("nightly (default): ovis 0.1.0 (night one)"));

  // One line sourcing env in each profile, on a line of its own.
  const auto bashrc = read_text(space.home / ".bashrc");
  CHECK(bashrc == "export FOO=1\n. \"" + (space.home / ".ovis").string() +
                      "/env\" # added by ovisup\n");
  REQUIRE(space.run("sh '" + ovisup + "' init -y").status == 0);
  CHECK(read_text(space.home / ".bashrc") == bashrc);
  CHECK_THAT(read_text(space.home / ".ovis" / "env"),
             ContainsSubstring((space.home / ".ovis" / "bin").string()));

  // A toolchain not installed is named, not run.
  const auto missing = space.run(space.ovis() + " +stable --version");
  CHECK(missing.status != 0);
  CHECK_THAT(missing.output, ContainsSubstring("ovisup install stable"));
}

TEST_CASE("ovisup installs, chooses, updates and removes toolchains.",
          "[ovisup]") {
  setup space("toolchains");
  space.publish("latest", "stable one");
  space.publish("v0.1.0", "pinned");
  REQUIRE(space.run("sh '" + ovisup + "' -y --no-modify-path").status == 0);
  CHECK_THAT(space.run(space.ovis() + " run a").output,
             ContainsSubstring("stable one: run a"));
  CHECK_FALSE(std::filesystem::exists(space.home / ".profile"));

  // A version installed beside it, run once with +, then made the default.
  REQUIRE(space.run(space.tool() + " install 0.1.0").status == 0);
  CHECK_THAT(space.run(space.ovis() + " +0.1.0 run a").output,
             ContainsSubstring("pinned: run a"));
  CHECK_THAT(space.run("OVIS_TOOLCHAIN=0.1.0 " + space.ovis() + " run a").output,
             ContainsSubstring("pinned: run a"));
  REQUIRE(space.run(space.tool() + " default 0.1.0").status == 0);
  CHECK_THAT(space.run(space.ovis() + " run a").output, ContainsSubstring("pinned"));
  CHECK_THAT(space.run(space.tool() + " which").output,
             ContainsSubstring("/toolchains/0.1.0/bin/ovis"));
  REQUIRE(space.run(space.tool() + " default stable").status == 0);

  // A project's ovis.toml names its toolchain, from any folder inside it;
  // +T and OVIS_TOOLCHAIN still come first.
  write_text(space.root / "app" / "ovis.toml",
             "[build]\ntoolchain = \"stable\"\n\n[project]\nname = \"app\"\n"
             "toolchain = '0.1.0'\n");
  std::filesystem::create_directories(space.root / "app" / "src" / "deep");
  CHECK_THAT(space.run("cd app/src/deep && " + space.ovis() + " run a").output,
             ContainsSubstring("pinned: run a"));
  CHECK_THAT(space.run("cd app && " + space.ovis() + " +stable run a").output,
             ContainsSubstring("stable one: run a"));
  CHECK_THAT(space.run("cd app && OVIS_TOOLCHAIN=stable " + space.ovis() + " run a").output,
             ContainsSubstring("stable one: run a"));
  CHECK_THAT(space.run("cd app && " + space.tool() + " show").output,
             ContainsSubstring("active:  0.1.0 (ovis.toml)"));
  CHECK_THAT(space.run(space.ovis() + " run a").output, ContainsSubstring("stable one"));
  write_text(space.root / "app" / "ovis.toml", "[project]\ntoolchain = \"0.9.0\"\n");
  CHECK_THAT(space.run("cd app && " + space.ovis() + " run a").output,
             ContainsSubstring("toolchain '0.9.0' (ovis.toml) is not installed"));

  // Unchanged packages are left; a new stable replaces the old one.
  auto updated = space.run(space.tool() + " update --no-self-update");
  CHECK_THAT(updated.output, ContainsSubstring("stable unchanged"));
  space.publish("latest", "stable two");
  updated = space.run(space.tool() + " update --no-self-update");
  INFO(updated.output);
  CHECK(updated.status == 0);
  CHECK_THAT(updated.output, ContainsSubstring("installed stable"));
  CHECK_THAT(space.run(space.ovis() + " run a").output,
             ContainsSubstring("stable two: run a"));
  // Versions are pinned: update leaves them.
  CHECK_THAT(updated.output, !ContainsSubstring("0.1.0 unchanged") &&
                                 !ContainsSubstring("installed 0.1.0"));

  // A damaged download, and a package that doesn't run here, are refused
  // and the installed toolchain stays.
  space.publish("latest", "stable three");
  for (const auto &entry :
       std::filesystem::directory_iterator(space.server / "latest"))
    if (entry.path().extension() == ".gz")
      std::ofstream(entry.path(), std::ios::app) << "junk";
  const auto damaged = space.run(space.tool() + " update --no-self-update");
  CHECK(damaged.status != 0);
  CHECK_THAT(damaged.output, ContainsSubstring("does not match its checksum"));
  space.publish("latest", "stable four", false);
  const auto broken = space.run(space.tool() + " update --no-self-update");
  CHECK(broken.status != 0);
  CHECK_THAT(broken.output, ContainsSubstring("does not run here"));
  CHECK_THAT(space.run(space.ovis() + " run a").output,
             ContainsSubstring("stable two: run a"));

  // Unknown names and missing releases fail before anything changes.
  CHECK_THAT(space.run(space.tool() + " install bogus").output,
             ContainsSubstring("unknown toolchain 'bogus'"));
  CHECK_THAT(space.run(space.tool() + " install 9.9.9").output,
             ContainsSubstring("no ovis-"));

  REQUIRE(space.run(space.tool() + " uninstall 0.1.0").status == 0);
  CHECK_FALSE(std::filesystem::exists(space.home / ".ovis" / "toolchains" / "0.1.0"));
  CHECK(space.run(space.tool() + " list").output.find("0.1.0:") ==
        std::string::npos);
}

TEST_CASE("ovisup self update and self uninstall.", "[ovisup]") {
  setup space("self");
  write_text(space.home / ".bashrc", "export FOO=1\n");
  write_text(space.home / ".cache" / "ovis" / "models" / "m.gguf", "weights");
  space.publish("latest", "stable one");
  REQUIRE(space.run("sh '" + ovisup + "' -y").status == 0);

  // A newer ovisup in the release replaces the installed one.
  CHECK_THAT(space.run(space.tool() + " self update").output,
             ContainsSubstring("ovisup unchanged"));
  {
    std::ofstream(space.server / "latest" / "ovisup", std::ios::app)
        << "# a later ovisup\n";
  }
  CHECK_THAT(space.run(space.tool() + " self update").output,
             ContainsSubstring("updated ovisup"));
  CHECK_THAT(read_text(space.home / ".ovis" / "bin" / "ovisup"),
             ContainsSubstring("# a later ovisup"));

  // Everything goes, the profiles are as they were, the models stay unless
  // purged.
  const auto removed = space.run(space.tool() + " self uninstall -y");
  INFO(removed.output);
  REQUIRE(removed.status == 0);
  CHECK_FALSE(std::filesystem::exists(space.home / ".ovis"));
  CHECK(read_text(space.home / ".bashrc") == "export FOO=1\n");
  CHECK(read_text(space.home / ".profile").empty());
  CHECK(std::filesystem::exists(space.home / ".cache" / "ovis" / "models" / "m.gguf"));

  REQUIRE(space.run("sh '" + ovisup + "' -y").status == 0);
  REQUIRE(space.run(space.tool() + " self uninstall -y --purge").status == 0);
  CHECK_FALSE(std::filesystem::exists(space.home / ".ovis"));
  CHECK_FALSE(std::filesystem::exists(space.home / ".cache" / "ovis"));
}

TEST_CASE("ovisup and the compiler have one version.", "[ovisup]") {
  // One version for the toolchain: release.yml refuses a tag that isn't the
  // compiler's, and ovisup names the version it installs by its own.
  const auto after = [](const std::string &p_text, const std::string &p_key) {
    const auto at = p_text.find(p_key);
    REQUIRE(at != std::string::npos);
    const auto start = at + p_key.size();
    return p_text.substr(start, p_text.find_first_of(" \n", start) - start);
  };
  const auto cmake = read_text(OVIS_SOURCE_DIR "/CMakeLists.txt");
  const auto script = read_text(ovisup);
  CHECK(after(script, "ovisup_version=") ==
        after(cmake, "ovis\n    VERSION "));
}

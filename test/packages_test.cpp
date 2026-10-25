#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <ovis/project/packages.hpp>

// Packages hosted on git (proposals/packages.md H1–H4): versions as tags,
// minimal version selection, ovis.lock, the cache, path dependencies and
// overrides, and a package's files read locked with the program. The
// repositories are made by the test and reached through file:// URLs.

using Catch::Matchers::ContainsSubstring;

namespace {

struct outcome {
  int status = 0;
  std::string output;
};

const std::string ovis_program = OVIS_EXECUTABLE;

auto shell(const std::string &p_command) -> outcome {
  outcome result;
  auto *const pipe = popen((p_command + " 2>&1").c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buffer[256];
  while (const auto count = std::fread(buffer, 1, sizeof(buffer), pipe))
    result.output.append(buffer, count);
  result.status = pclose(pipe);
  return result;
}

auto read_text(const std::filesystem::path &p_path) -> std::string {
  std::ifstream input(p_path);
  std::stringstream text;
  text << input.rdbuf();
  return text.str();
}

// Repositories, a cache and a project, all in one folder.
class packages {
public:
  std::filesystem::path root;

  explicit packages(const std::string &p_name)
      : root(std::filesystem::temp_directory_path() / ("ovis-packages-" + p_name)) {
    remove();
    std::filesystem::create_directories(root / "app");
  }
  ~packages() { remove(); }

  auto remove() const -> void {
    // The cache's files are read-only; their folders aren't.
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  auto remove_cache() const -> void {
    std::error_code ignored;
    std::filesystem::remove_all(root / "cache", ignored);
  }

  auto write(const std::string &p_name, const std::string &p_text) const -> void {
    std::filesystem::create_directories((root / p_name).parent_path());
    std::ofstream(root / p_name) << p_text;
  }

  auto url(const std::string &p_repository) const -> std::string {
    return "file://" + (root / p_repository).string();
  }

  auto git(const std::string &p_repository, const std::string &p_arguments) const
      -> void {
    const auto ran = shell("cd '" + (root / p_repository).string() +
                           "' && git -c user.name=ovis -c user.email=ovis@example.com "
                           "-c init.defaultBranch=main " +
                           p_arguments);
    INFO(ran.output);
    REQUIRE(ran.status == 0);
  }

  // Commits the repository's files as they are and tags them.
  auto release(const std::string &p_repository, const std::string &p_version) const
      -> void {
    if (!std::filesystem::exists(root / p_repository / ".git"))
      git(p_repository, "init -q");
    git(p_repository, "add -A");
    git(p_repository, "commit -q --allow-empty -m " + p_version);
    git(p_repository, "tag -a v" + p_version + " -m " + p_version);
  }

  // A package `p_name` at `p_version` saying `p_word` when greeting, with
  // the dependencies' lines given.
  auto package(const std::string &p_name, const std::string &p_version,
               const std::string &p_body, const std::string &p_dependencies = "") const
      -> void {
    write(p_name + "/ovis.toml", "[package]\nname = \"" + p_name + "\"\nversion = \"" +
                                     p_version + "\"\n" +
                                     (p_dependencies.empty()
                                          ? std::string()
                                          : "\n[dependencies]\n" + p_dependencies));
    write(p_name + "/" + p_name + ".ovis", p_body);
    release(p_name, p_version);
  }

  auto ovis(const std::string &p_arguments, const std::string &p_folder = "app") const
      -> outcome {
    // An identity for `ovis release`'s tag, which CI's runners don't have.
    return shell("cd '" + (root / p_folder).string() + "' && GIT_COMMITTER_NAME=ovis "
                 "GIT_COMMITTER_EMAIL=ovis@example.com OVIS_PACKAGE_CACHE='" +
                 (root / "cache").string() + "' '" + ovis_program + "' " + p_arguments);
  }
};

} // namespace

TEST_CASE("Version tags and tree hashes.", "[packages]") {
  packages space("tags");
  space.package("greet", "1.0.0", "to greet (name):\n    show \"Hello,\", name\n");
  space.write("greet/ovis.toml", "[package]\nname = \"greet\"\nversion = \"1.2.0\"\n");
  space.release("greet", "1.2.0");
  space.git("greet", "tag not-a-version");
  space.git("greet", "tag v3");
  const auto tags = ovis::project::list_version_tags(space.url("greet"));
  REQUIRE(tags.size() == 2);
  CHECK(tags.begin()->first == ovis::project::version{1, 0, 0});
  CHECK(tags.rbegin()->first == ovis::project::version{1, 2, 0});
  CHECK(tags.begin()->second.size() == 40);

  CHECK(ovis::project::normalized_url("https://x.org/a/b.git/") == "https://x.org/a/b");
  space.write("tree/a.txt", "one");
  space.write("tree/sub/b.txt", "two");
  const auto hash = ovis::project::tree_hash(space.root / "tree");
  CHECK(hash.starts_with("sha256:"));
  CHECK(hash.size() == 7 + 64);
  space.write("tree/.git/ignored", "x");
  CHECK(ovis::project::tree_hash(space.root / "tree") == hash);
  space.write("tree/sub/b.txt", "two!");
  CHECK(ovis::project::tree_hash(space.root / "tree") != hash);
}

TEST_CASE("One version per line: the least that satisfies everyone.", "[packages]") {
  packages space("mvs");
  space.package("greet", "1.0.0", "to greet (name):\n    show \"Hello,\", name\n");
  space.package("greet", "1.1.0", "to greet (name):\n    show \"Hello there,\", name\n");
  space.package("greet", "1.2.0", "to greet (name):\n    show \"Hi,\", name\n");
  space.package("shout", "1.0.0", "use greet\nto shout at (name):\n    greet name\n    show \"!!!\"\n",
                "greet = { git = \"" + space.url("greet") + "\", version = \"1.1\" }\n");
  space.write("app/ovis.toml", "[project]\nname = \"app\"\n\n[dependencies]\n"
                               "greet = { git = \"" + space.url("greet") +
                                   "\", version = \"1.0\" }\n"
                                   "shout = { git = \"" + space.url("shout") +
                                   "\", version = \"1.0\" }\n");
  space.write("app/main.ovis", "use greet\nuse shout\ngreet \"Ana\"\nshout at \"Bo\"\n");

  // app asks greet 1.0, shout 1.1: one greet, 1.1.0, not the newest 1.2.0.
  auto ran = space.ovis("run --offline --quiet");
  INFO(ran.output);
  REQUIRE(ran.status == 0);
  CHECK(ran.output == "Hello there, Ana\nHello there, Bo\n!!!\n");
  const auto lock = read_text(space.root / "app" / "ovis.lock");
  CHECK_THAT(lock, ContainsSubstring("\"version\": \"1.1.0\""));
  CHECK_THAT(lock, ContainsSubstring("\"dependencies\": [\n        \"greet@1\"\n"));
  CHECK_THAT(lock, ContainsSubstring("\"tree\": \"sha256:"));

  const auto tree = space.ovis("tree");
  CHECK_THAT(tree.output, ContainsSubstring("greet 1.1.0"));
  CHECK_THAT(tree.output, ContainsSubstring("shout 1.0.0"));
  CHECK_THAT(tree.output, ContainsSubstring("  greet 1.1.0"));

  // With the lock and the cache, the repositories aren't needed.
  std::filesystem::rename(space.root / "greet", space.root / "greet-moved");
  std::filesystem::rename(space.root / "shout", space.root / "shout-moved");
  ran = space.ovis("run --offline --quiet");
  INFO(ran.output);
  CHECK(ran.output == "Hello there, Ana\nHello there, Bo\n!!!\n");
  CHECK(read_text(space.root / "app" / "ovis.lock") == lock);
  std::filesystem::rename(space.root / "greet-moved", space.root / "greet");
  std::filesystem::rename(space.root / "shout-moved", space.root / "shout");

  // update moves to the newest version on the line, and says so.
  const auto updated = space.ovis("update greet");
  INFO(updated.output);
  CHECK(updated.status == 0);
  CHECK_THAT(updated.output, ContainsSubstring("greet 1.1.0 -> 1.2.0"));
  CHECK(space.ovis("run --offline --quiet").output == "Hi, Ana\nHi, Bo\n!!!\n");
  CHECK_THAT(space.ovis("update").output,
             ContainsSubstring("every dependency is where ovis.lock has it"));
  CHECK_THAT(space.ovis("update nothing").output,
             ContainsSubstring("no package nothing is in the program"));

  // A build that would change ovis.lock fails under --locked.
  std::filesystem::remove(space.root / "app" / "ovis.lock");
  CHECK_THAT(space.ovis("build --offline --quiet --locked").output,
             ContainsSubstring("ovis.lock would change"));
}

TEST_CASE("Exact versions, missing ones, and a tag that disagrees.", "[packages]") {
  packages space("exact");
  space.package("greet", "1.0.0", "to greet (name):\n    show \"Hello,\", name\n");
  space.package("greet", "1.1.0", "to greet (name):\n    show \"Hello there,\", name\n");
  space.package("shout", "1.0.0", "use greet\nto shout at (name):\n    greet name\n",
                "greet = { git = \"" + space.url("greet") + "\", version = \"1.1\" }\n");
  space.write("app/main.ovis", "use greet\ngreet \"Ana\"\n");
  const auto project = [&](const std::string &p_greet, const std::string &p_more = "") {
    space.write("app/ovis.toml", "[project]\n[dependencies]\ngreet = { git = \"" +
                                     space.url("greet") + "\", " + p_greet + " }\n" + p_more);
    std::filesystem::remove(space.root / "app" / "ovis.lock");
  };

  project("version = \"=1.0.0\"");
  CHECK(space.ovis("run --offline --quiet").output == "Hello, Ana\n");
  // An exact version older than another package needs is said plainly.
  project("version = \"=1.0.0\"",
          "shout = { git = \"" + space.url("shout") + "\", version = \"1\" }\n");
  const auto older = space.ovis("run --offline --quiet");
  CHECK(older.status != 0);
  CHECK_THAT(older.output, ContainsSubstring("ovis.toml asks =1.0.0"));
  CHECK_THAT(older.output, ContainsSubstring("shout 1.0.0 asks 1.1.0"));
  project("version = \"1.5\"");
  CHECK_THAT(space.ovis("run --offline --quiet").output,
             ContainsSubstring("has no version 1.5.0 or later on its 1 line"));

  // A tag whose ovis.toml says another version is refused.
  space.write("greet/ovis.toml", "[package]\nname = \"greet\"\nversion = \"1.9.0\"\n");
  space.release("greet", "1.3.0");
  project("version = \"1.3\"");
  CHECK_THAT(space.ovis("run --offline --quiet").output,
             ContainsSubstring("says it is version 1.9.0"));
  // Something that isn't a package.
  space.write("plain/readme.txt", "no ovis.toml here");
  space.release("plain", "1.0.0");
  space.write("app/ovis.toml", "[project]\n[dependencies]\nplain = { git = \"" +
                                   space.url("plain") + "\", version = \"1\" }\n");
  CHECK_THAT(space.ovis("run --offline --quiet").output,
             ContainsSubstring("has no ovis.toml, so it is not an Ovis package"));
}

TEST_CASE("Path dependencies, and overrides in the root.", "[packages]") {
  packages space("paths");
  space.package("greet", "1.0.0", "to greet (name):\n    show \"Hello,\", name\n");
  space.write("local/greet/ovis.toml", "[package]\nname = \"greet\"\nversion = \"1.4.0\"\n");
  space.write("local/greet/greet.ovis", "to greet (name):\n    show \"Local hello,\", name\n");
  space.write("app/main.ovis", "use greet\ngreet \"Ana\"\n");

  space.write("app/ovis.toml", "[project]\n[dependencies]\n"
                               "greet = { path = \"../local/greet\" }\n");
  CHECK(space.ovis("run --offline --quiet").output == "Local hello, Ana\n");
  CHECK_THAT(read_text(space.root / "app" / "ovis.lock"), ContainsSubstring("\"path\": \""));

  // An override replaces a package everywhere: by its name, or its URL.
  space.write("app/ovis.toml", "[project]\n[dependencies]\ngreet = { git = \"" +
                                   space.url("greet") + "\", version = \"1.0\" }\n\n"
                                   "[overrides]\ngreet = { path = \"../local/greet\" }\n");
  CHECK(space.ovis("run --offline --quiet").output == "Local hello, Ana\n");
  space.write("app/ovis.toml", "[project]\n[dependencies]\ngreet = { git = \"" +
                                   space.url("greet") + "\", version = \"1.0\" }\n\n"
                                   "[overrides]\n\"" + space.url("greet") +
                                   ".git\" = { path = \"../local/greet\" }\n");
  CHECK(space.ovis("run --offline --quiet").output == "Local hello, Ana\n");
}

TEST_CASE("A package's lines are its author's to settle.", "[packages]") {
  packages space("locked");
  space.package("vague", "1.0.0", "to tell about (x):\n    show square root of x + 7\n");
  space.write("app/ovis.toml", "[project]\n[dependencies]\nvague = { git = \"" +
                                   space.url("vague") + "\", version = \"1\" }\n");
  space.write("app/main.ovis", "use vague\ntell about 9\n");
  const auto ran = space.ovis("run --quiet --endpoint http://127.0.0.1:9/v1");
  CHECK(ran.status != 0);
  CHECK_THAT(ran.output, ContainsSubstring("vague@1/vague.ovis, line 2"));
  CHECK_THAT(ran.output, ContainsSubstring("in the package vague@1, whose author must "
                                           "settle it"));

  // Settled in the package (a meaning above the line), it builds, and the
  // cached files are never written.
  space.write("vague/vague.ovis", "to tell about (x):\n"
                                  "    @[meaning: show (square root of x) + 7]\n"
                                  "    show square root of x + 7\n");
  space.write("vague/ovis.toml", "[package]\nname = \"vague\"\nversion = \"1.0.1\"\n");
  space.release("vague", "1.0.1");
  space.ovis("update");
  const auto settled = space.ovis("run --quiet --offline");
  INFO(settled.output);
  CHECK(settled.status == 0);
  CHECK(settled.output == "10.0\n");
}

TEST_CASE("Two lines of one package are two packages.", "[packages]") {
  packages space("lines");
  space.package("greet", "1.0.0", "to greet (name):\n    show \"Hello,\", name\n");
  space.package("greet", "2.0.0", "to greet (name):\n    show \"Greetings,\", name\n");
  space.package("shout", "1.0.0", "use greet\nto shout at (name):\n    greet name\n    show \"!!!\"\n",
                "greet = { git = \"" + space.url("greet") + "\", version = \"1\" }\n");
  space.write("app/ovis.toml", "[project]\n[dependencies]\n"
                               "greet = { git = \"" + space.url("greet") +
                                   "\", version = \"2\" }\n"
                                   "shout = { git = \"" + space.url("shout") +
                                   "\", version = \"1\" }\n");
  // Each file reads with its own dependencies' phrases: the program's
  // greet is 2.0.0's, shout's is 1.0.0's.
  space.write("app/main.ovis", "use greet\nuse shout\ngreet \"Ana\"\nshout at \"Bo\"\n");
  const auto ran = space.ovis("run --offline --quiet");
  INFO(ran.output);
  REQUIRE(ran.status == 0);
  CHECK(ran.output == "Greetings, Ana\nHello, Bo\n!!!\n");
  CHECK_THAT(space.ovis("tree").output, ContainsSubstring("(2 copies: 1.0.0 2.0.0)"));

  // A phrase of a package the program doesn't use directly isn't there.
  space.write("app/ovis.toml", "[project]\n[dependencies]\nshout = { git = \"" +
                                   space.url("shout") + "\", version = \"1\" }\n");
  space.write("app/main.ovis", "use shout\nshout at \"Bo\"\ngreet \"Ana\"\n");
  const auto hidden = space.ovis("run --offline --quiet");
  CHECK(hidden.status != 0);
  CHECK_THAT(hidden.output, ContainsSubstring("main.ovis, line 3"));
  space.write("app/main.ovis", "use greet\nshout at \"Bo\"\n");
  CHECK_THAT(space.ovis("run --offline --quiet").output,
             ContainsSubstring("There is no package called 'greet' in ovis.toml's "
                               "[dependencies]"));
}

TEST_CASE("Two lines' records are different types, said with their versions.",
          "[packages]") {
  packages space("records");
  const auto log = [](const std::string &p_fields) {
    return "an entry has:\n    words (text)\n" + p_fields +
           "\nto note (t: text):\n    e = an entry with words t\n    give back e\n";
  };
  space.package("log", "1.0.0", log(""));
  space.package("log", "2.0.0", log("    level (whole number, 1)\n"));
  space.package("csv", "1.0.0", "use log\nto read the row (t: text):\n    give back note t\n",
                "log = { git = \"" + space.url("log") + "\", version = \"1\" }\n");
  space.package("report", "1.0.0",
                "use log\nto report (e: entry):\n    show \"Report:\", e's words, e's level\n",
                "log = { git = \"" + space.url("log") + "\", version = \"2\" }\n");
  space.write("app/ovis.toml", "[project]\n[dependencies]\n"
                               "csv = { git = \"" + space.url("csv") + "\", version = \"1\" }\n"
                               "report = { git = \"" + space.url("report") +
                                   "\", version = \"1\" }\n");
  space.write("app/main.ovis",
              "use csv\nuse report\nrow = read the row \"a,b\"\nshow row\nreport row\n");
  const auto mixed = space.ovis("run --offline --quiet");
  INFO(mixed.output);
  CHECK(mixed.status != 0);
  CHECK_THAT(mixed.output,
             ContainsSubstring("main.ovis, line 5: Argument 'e' of 'report (e: entry)' must "
                               "be a log 2.0.0 entry but is a log 1.0.0 entry."));
  CHECK_THAT(mixed.output, ContainsSubstring("the program has 2 versions of log"));
  CHECK_THAT(mixed.output, ContainsSubstring("1.0.0 for csv"));
  CHECK_THAT(mixed.output, ContainsSubstring("2.0.0 for report"));

  // Each kept to its own side, both build; a record prints as written.
  space.write("app/main.ovis", "use csv\nrow = read the row \"a,b\"\nshow row\n");
  const auto apart = space.ovis("run --offline --quiet");
  INFO(apart.output);
  CHECK(apart.status == 0);
  CHECK(apart.output == "an entry with words \"a,b\"\n");
}

TEST_CASE("What a package's internal/ files define is its own.", "[packages]") {
  packages space("internal");
  space.write("tools/internal/helpers.ovis", "to add the bang to (t: text):\n"
                                             "    give back t + \"!\"\n");
  space.package("tools", "1.0.0", "to cheer (t: text):\n    s = add the bang to t\n"
                                  "    at the end, free s\n    show s\n");
  space.write("app/ovis.toml", "[project]\n[dependencies]\ntools = { git = \"" +
                                   space.url("tools") + "\", version = \"1\" }\n");
  space.write("app/main.ovis", "use tools\ncheer \"go\"\n");
  const auto ran = space.ovis("run --offline --quiet");
  INFO(ran.output);
  CHECK(ran.output == "go!\n");
  space.write("app/main.ovis", "use tools\ns = add the bang to \"go\"\nshow s\n");
  const auto hidden = space.ovis("run --offline --quiet");
  CHECK(hidden.status != 0);
  CHECK_THAT(hidden.output, ContainsSubstring("main.ovis, line 2"));
}

TEST_CASE("A program's X/internal/ phrases are seen under X/ alone.", "[packages]") {
  packages space("program-internal");
  space.write("app/ovis.toml", "[project]\n");
  space.write("app/pricing/internal/rates.ovis", "to add the tax to (n: whole number):\n"
                                                 "    give back n + 10\n");
  space.write("app/pricing/tax.ovis", "to price of (n: whole number):\n"
                                      "    give back add the tax to n\n");
  space.write("app/main.ovis", "show price of 100\n");
  const auto ran = space.ovis("run --offline --quiet");
  INFO(ran.output);
  CHECK(ran.status == 0);
  CHECK(ran.output == "110\n");

  // main.ovis isn't under pricing/: told why, never asked.
  space.write("app/main.ovis", "show add the tax to 100\n");
  const auto hidden = space.ovis("run --offline --quiet");
  INFO(hidden.output);
  CHECK(hidden.status != 0);
  CHECK_THAT(hidden.output, ContainsSubstring("main.ovis, line 1"));
  CHECK_THAT(hidden.output, ContainsSubstring("only the files under pricing/ see"));
}

TEST_CASE("A package's nested internal/ is its parent folder's.", "[packages]") {
  packages space("nested-internal");
  for (const std::string name : {"tools", "loud"})
    space.write(name + "/text/internal/marks.ovis", "to add the bang to (t: text):\n"
                                                    "    give back t + \"!\"\n");
  space.write("tools/text/cheer.ovis", "to cheer (t: text):\n    s = add the bang to t\n"
                                       "    at the end, free s\n    show s\n");
  space.package("tools", "1.0.0", "to wave:\n    show \"~\"\n");
  // loud's top file isn't under text/.
  space.package("loud", "1.0.0", "to shout (t: text):\n    s = add the bang to t\n"
                                 "    at the end, free s\n    show s\n");
  space.write("app/ovis.toml", "[project]\n[dependencies]\ntools = { git = \"" +
                                   space.url("tools") + "\", version = \"1\" }\n");
  space.write("app/main.ovis", "use tools\ncheer \"go\"\nwave\n");
  const auto ran = space.ovis("run --offline --quiet");
  INFO(ran.output);
  CHECK(ran.status == 0);
  CHECK(ran.output == "go!\n~\n");

  space.write("app/ovis.toml", "[project]\n[dependencies]\nloud = { git = \"" +
                                   space.url("loud") + "\", version = \"1\" }\n");
  space.write("app/main.ovis", "use loud\nshout \"go\"\n");
  const auto hidden = space.ovis("run --offline --quiet");
  INFO(hidden.output);
  CHECK(hidden.status != 0);
  CHECK_THAT(hidden.output, ContainsSubstring("loud.ovis, line 2"));
  CHECK_THAT(hidden.output, ContainsSubstring("only the files under text/ see"));
}

TEST_CASE("A phrase two dependencies have is said with whose it is.", "[packages]") {
  packages space("qualified");
  space.package("alpha", "1.0.0", "to say hi:\n    show \"hi from alpha\"\n");
  space.package("beta", "1.0.0", "to say hi:\n    show \"hi from beta\"\n");
  space.write("app/ovis.toml", "[project]\n[dependencies]\n"
                               "alpha = { git = \"" + space.url("alpha") + "\", version = \"1\" }\n"
                               "beta = { git = \"" + space.url("beta") + "\", version = \"1\" }\n");
  // Both read the line: a question, its readings saying whose they are.
  space.write("app/main.ovis", "use alpha\nuse beta\nsay hi\n");
  const auto asked = space.ovis("run --offline --quiet");
  INFO(asked.output);
  CHECK(asked.status != 0);
  CHECK_THAT(asked.output, ContainsSubstring("alpha's say hi"));
  CHECK_THAT(asked.output, ContainsSubstring("beta's say hi"));

  // Saying whose, in the line or in its meaning, settles it, with or
  // without `use` lines: the line names the package.
  space.write("app/main.ovis", "beta's say hi\nalpha's say hi\n");
  auto ran = space.ovis("run --offline --quiet");
  INFO(ran.output);
  CHECK(ran.output == "hi from beta\nhi from alpha\n");
  space.write("app/main.ovis", "@[meaning: beta's say hi]\nsay hi\n");
  ran = space.ovis("run --offline --quiet");
  INFO(ran.output);
  CHECK(ran.output == "hi from beta\n");
}

TEST_CASE("A file reads with a dependency only where it says `use`.", "[packages]") {
  packages space("use");
  space.package("stats", "1.0.0", "to spread of (xs: list of decimals):\n"
                                  "    give back largest of xs - smallest of xs\n");
  space.write("app/ovis.toml", "[project]\n[dependencies]\nstats = { git = \"" +
                                   space.url("stats") + "\", version = \"1\" }\n");
  space.write("app/helper.ovis", "to report on (xs: list of decimals):\n"
                                 "    show spread of xs\n");
  space.write("app/main.ovis", "# Spreads.\nxs = [1.0, 4.0]\nreport on xs\n");
  // The line reads only with stats's phrases: said, never asked about.
  const auto unused = space.ovis("run --quiet --endpoint http://127.0.0.1:9/v1");
  CHECK(unused.status != 0);
  CHECK_THAT(unused.output, ContainsSubstring("helper.ovis, line 2: This line reads with "
                                              "the phrases of stats, which this file "
                                              "doesn't use: write 'use stats'"));
  // `ovis fix` writes the line into the file that needs it, not main.ovis.
  const auto fixed = space.ovis("fix --quiet --offline < /dev/null");
  INFO(fixed.output);
  CHECK(fixed.status == 0);
  CHECK_THAT(fixed.output, ContainsSubstring("wrote `use stats` into"));
  CHECK(read_text(space.root / "app" / "helper.ovis") ==
        "use stats\nto report on (xs: list of decimals):\n"
        "    show spread of xs\n");
  CHECK(read_text(space.root / "app" / "main.ovis") ==
        "# Spreads.\nxs = [1.0, 4.0]\nreport on xs\n");
  const auto ran = space.ovis("run --offline --quiet");
  INFO(ran.output);
  CHECK(ran.output == "3.0\n");
  // Only at the top level.
  space.write("app/main.ovis", "if 1 < 2:\n    use stats\n");
  CHECK_THAT(space.ovis("run --offline --quiet").output,
             ContainsSubstring("Write 'use' lines at the top level."));
}

TEST_CASE("check-phrases --dependencies: the lines a package's phrases change.",
          "[packages]") {
  packages space("check-dependencies");
  space.package("util", "1.0.0", "to show the total of (xs: list of decimals):\n"
                                 "    show 0.0\n\n"
                                 "to greet (name: text):\n"
                                 "    show \"Hi,\", name\n");
  space.write("app/ovis.toml", "[project]\n");
  space.write("app/main.ovis", "scores = [1.0, 2.0]\nshow total of scores\n"
                               "greet \"Ana\"\n");
  // `add` says which lines the new phrases change; the line written for
  // them (`greet`) is a use, not a change.
  const auto added = space.ovis("add util --git " + space.url("util"));
  INFO(added.output);
  REQUIRE(added.status == 0);
  CHECK_THAT(added.output,
             ContainsSubstring("util's phrases change 1 of the program's lines "
                               "(2 lines read with them):\n"
                               "  main.ovis, line 3: show total of scores\n"
                               "    asks now, offering show sum of scores, "
                               "show the total of scores\n"));
  const auto checked = space.ovis("check-phrases --dependencies");
  INFO(checked.output);
  CHECK(checked.status != 0);
  CHECK_THAT(checked.output, ContainsSubstring("line 3: show total of scores"));
  // A meaning settles the line whatever the package offers.
  space.write("app/main.ovis", "use util\nscores = [1.0, 2.0]\n"
                               "@[meaning: show sum of scores]\nshow total of scores\n"
                               "greet \"Ana\"\n");
  const auto settled = space.ovis("check-phrases --dependencies");
  INFO(settled.output);
  CHECK(settled.status == 0);
  CHECK_THAT(settled.output, ContainsSubstring("util's phrases change none of the "
                                               "program's other lines"));
  CHECK(space.ovis("run --offline --quiet").output == "3.0\nHi, Ana\n");
}

TEST_CASE("A trait for a type needs one of them to be the package's own.",
          "[packages]") {
  packages space("orphans");
  space.package("shapes", "1.0.0",
                "a point has:\n    x (whole number)\n\n"
                "a hashable can:\n    hash of (it) gives a whole number\n\n"
                "a shape can:\n    area of (it) gives a whole number\n\n"
                "a point is a shape:\n    area of (it):\n        give back 1\n");
  space.write("app/ovis.toml", "[project]\n[dependencies]\nshapes = { git = \"" +
                                   space.url("shapes") + "\", version = \"1\" }\n");
  // Its own record with the package's trait: fine.
  space.write("app/main.ovis", "use shapes\n\na circle has:\n    r (whole number)\n\n"
                               "a circle is a shape:\n    area of (it):\n        give back it's r\n\n"
                               "c = a circle with r 3\nshow area of c\n"
                               "p = a point with x 1\nshow area of p\n");
  auto ran = space.ovis("run --offline --quiet");
  INFO(ran.output);
  CHECK(ran.output == "3\n1\n");

  // The package's trait for a number, or a built-in trait for its record:
  // neither is the program's.
  space.write("app/main.ovis", "use shapes\na whole number is hashable:\n    hash of (it):\n"
                               "        give back it\n\nshow 1\n");
  ran = space.ovis("run --offline --quiet");
  CHECK(ran.status != 0);
  CHECK_THAT(ran.output, ContainsSubstring("neither is declared in this program"));
  space.write("app/main.ovis", "use shapes\na point is comparable:\n    to (a: point) comes before "
                               "(b: point):\n        give back a's x < b's x\n\nshow 1\n");
  CHECK_THAT(space.ovis("run --offline --quiet").output,
             ContainsSubstring("a package may only give its own types a trait"));
}

TEST_CASE("A package's own C is built with the program.", "[packages]") {
  if (std::system("pkg-config --exists zlib") != 0)
    SKIP("zlib's pkg-config file is not installed");
  packages space("native");
  space.write("zipper/include/zipper.h", "long packed_size(int count);\n");
  space.write("zipper/c/zipper.c",
              "#include <string.h>\n#include <zlib.h>\n#include \"zipper.h\"\n"
              "long packed_size(int count) {\n  char text[601] = \"\";\n"
              "  for (int i = 0; i < count && i < 100; ++i) strcat(text, \"Ovis! \");\n"
              "  unsigned char out[1200]; uLongf size = sizeof out;\n"
              "  compress(out, &size, (const unsigned char *)text, strlen(text));\n"
              "  return (long)size;\n}\n");
  space.write("zipper/ovis.toml", "[package]\nname = \"zipper\"\nversion = \"1.0.0\"\n\n"
                                  "[build]\ninclude = [\"include\"]\nc = [\"c/*.c\"]\n\n"
                                  "[c.zlib]\npkg-config = \"zlib\"\n");
  space.write("zipper/zipper.ovis", "use \"zipper.h\"\n\nto pack (n: whole number) shouts:\n"
                                    "    give back packed_size(n)\n");
  space.release("zipper", "1.0.0");
  space.write("app/ovis.toml", "[project]\n[dependencies]\nzipper = { git = \"" +
                                   space.url("zipper") + "\", version = \"1\" }\n");
  space.write("app/main.ovis", "use zipper\nshow pack 100 shouts\n");
  const auto ran = space.ovis("run --offline --quiet");
  INFO(ran.output);
  CHECK(ran.status == 0);
  CHECK(ran.output == "20\n");
  CHECK(std::filesystem::exists(space.root / "app" / "build" / "packages" / "zipper-1" /
                                "c" / "c_zipper.c.o"));
}

TEST_CASE("A program has one of each C library.", "[packages][cmake]") {
  if (std::system("command -v cmake > /dev/null") != 0)
    SKIP("cmake is not installed");
  packages space("one-c");
  space.write("adder/CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(adder C)\n"
                                      "add_library(adder STATIC adder.c)\n"
                                      "target_include_directories(adder PUBLIC include)\n");
  space.write("adder/include/adder.h", "int add_some(int n);\n");
  space.write("adder/adder.c", "#include \"adder.h\"\nint add_some(int n) { return n + 1; }\n");
  space.release("adder", "1.0.0");
  space.write("adder/adder.c", "#include \"adder.h\"\nint add_some(int n) { return n + 2; }\n");
  space.release("adder", "1.1.0");
  const auto user = [&](const std::string &p_name, const std::string &p_tag) {
    space.write(p_name + "/ovis.toml", "[package]\nname = \"" + p_name +
                                           "\"\nversion = \"1.0.0\"\n\n[c.adder]\ngit = \"" +
                                           space.url("adder") + "\"\ntag = \"" + p_tag +
                                           "\"\ntargets = [\"adder\"]\n");
    space.write(p_name + "/" + p_name + ".ovis", "use \"adder.h\"\n\nto " + p_name +
                                                     " (n: whole number):\n"
                                                     "    give back add_some(n)\n");
    space.release(p_name, "1.0.0");
  };
  user("left", "v1.0.0");
  user("right", "v1.1.0");
  space.write("app/main.ovis", "use left\nuse right\nshow left 1, right 1\n");
  const auto project = [&](const std::string &p_more) {
    space.write("app/ovis.toml", "[project]\nname = \"app\"\n[dependencies]\n"
                                 "left = { git = \"" + space.url("left") + "\", version = \"1\" }\n"
                                 "right = { git = \"" + space.url("right") +
                                     "\", version = \"1\" }\n" + p_more);
  };
  project("");
  const auto clash = space.ovis("run --offline --quiet");
  CHECK(clash.status != 0);
  CHECK_THAT(clash.output, ContainsSubstring("Two versions of the C library"));
  CHECK_THAT(clash.output, ContainsSubstring("left 1.0.0 wants v1.0.0, right 1.0.0 wants "
                                             "v1.1.0"));
  // The program chooses one for everyone.
  project("\n[overrides]\n\"c.adder\" = { tag = \"v1.1.0\" }\n");
  const auto chosen = space.ovis("run --offline --quiet");
  INFO(chosen.output);
  CHECK(chosen.status == 0);
  CHECK(chosen.output == "3 3\n");
  CHECK_THAT(read_text(space.root / "app" / "ovis.lock"), ContainsSubstring("\"adder\""));
}

TEST_CASE("ovis add, remove, fetch and vendor.", "[packages]") {
  packages space("commands");
  space.package("greet", "1.0.0", "to greet (name):\n    show \"Hello,\", name\n");
  space.package("greet", "1.3.0", "to greet (name):\n    show \"Hi,\", name\n");
  space.write("app/ovis.toml", "[project]\nname = \"app\"  # my program\n");
  space.write("app/main.ovis", "# Greets.\ngreet \"Ana\"\n");

  // add takes the newest version as the oldest the project works with.
  auto added = space.ovis("add greet --git " + space.url("greet"));
  INFO(added.output);
  REQUIRE(added.status == 0);
  CHECK(read_text(space.root / "app" / "ovis.toml") ==
        "[project]\nname = \"app\"  # my program\n\n[dependencies]\ngreet = { git = \"" +
            space.url("greet") + "\", version = \"1.3.0\" }\n");
  // And main.ovis says it uses greet.
  CHECK(read_text(space.root / "app" / "main.ovis") == "# Greets.\nuse greet\ngreet \"Ana\"\n");
  CHECK(space.ovis("run --offline --quiet").output == "Hi, Ana\n");
  CHECK_THAT(space.ovis("add greet --git " + space.url("greet")).output,
             ContainsSubstring("already depends on `greet`"));
  REQUIRE(space.ovis("add c zlib --pkg-config zlib").status == 0);
  CHECK_THAT(read_text(space.root / "app" / "ovis.toml"),
             ContainsSubstring("\n[c.zlib]\npkg-config = \"zlib\"\n"));
  REQUIRE(space.ovis("remove c zlib").status == 0);
  CHECK(read_text(space.root / "app" / "ovis.toml").find("[c.zlib]") == std::string::npos);

  // vendor/ builds with neither the repository nor the cache.
  REQUIRE(space.ovis("fetch").status == 0);
  const auto vendored = space.ovis("vendor");
  INFO(vendored.output);
  REQUIRE(vendored.status == 0);
  CHECK(std::filesystem::exists(space.root / "app" / "vendor" / "greet-1" / "greet.ovis"));
  std::filesystem::rename(space.root / "greet", space.root / "greet-gone");
  space.remove_cache();
  CHECK(space.ovis("run --offline --quiet").output == "Hi, Ana\n");
  std::filesystem::rename(space.root / "greet-gone", space.root / "greet");

  // remove takes the line out, and ovis.lock forgets the package.
  REQUIRE(space.ovis("remove greet").status == 0);
  CHECK(read_text(space.root / "app" / "ovis.toml").find("greet") == std::string::npos);
  CHECK(read_text(space.root / "app" / "ovis.lock").find("greet") == std::string::npos);
  CHECK(read_text(space.root / "app" / "main.ovis") == "# Greets.\ngreet \"Ana\"\n");
  CHECK_THAT(space.ovis("remove greet").output, ContainsSubstring("has no dependency `greet`"));
}

TEST_CASE("ovis release checks a package, then tags it.", "[packages]") {
  packages space("release");
  space.package("greet", "1.0.0", "to greet (name):\n    show \"Hello,\", name\n");
  // Not this version yet; then not committed.
  CHECK_THAT(space.ovis("release 1.1.0", "greet").output,
             ContainsSubstring("ovis.toml says version 1.0.0"));
  space.write("greet/ovis.toml", "[package]\nname = \"greet\"\nversion = \"1.1.0\"\n");
  CHECK_THAT(space.ovis("release 1.1.0", "greet").output,
             ContainsSubstring("Commit or stash the changes first"));
  // A line its users couldn't read.
  space.write("greet/greet.ovis", "to greet (x):\n    show square root of x + 7\n");
  space.git("greet", "commit -qam 1.1.0");
  const auto unsettled = space.ovis("release 1.1.0", "greet");
  CHECK(unsettled.status != 0);
  CHECK_THAT(unsettled.output, ContainsSubstring("doesn't build as its users would"));
  space.write("greet/greet.ovis", "to greet (name):\n    show \"Hello there,\", name\n");
  space.git("greet", "commit -qam settled");
  const auto released = space.ovis("release 1.1.0", "greet");
  INFO(released.output);
  CHECK(released.status == 0);
  CHECK_THAT(released.output, ContainsSubstring("tagged v1.1.0"));
  CHECK(ovis::project::list_version_tags(space.url("greet")).contains({1, 1, 0}));
  CHECK_THAT(space.ovis("release 1.1.0", "greet").output, ContainsSubstring("already exists"));
}

TEST_CASE("A package's # example: lines are checked by check and release.", "[packages]") {
  packages space("examples");
  space.package("greet", "1.0.0", "to greet (name):\n    show \"Hello,\", name\n");
  space.write("greet/ovis.toml", "[package]\nname = \"greet\"\nversion = \"1.1.0\"\n");
  // An example that reads, and one that doesn't.
  space.write("greet/greet.ovis",
              "# Greets someone.\n# example: greet \"Ana\"\nto greet (name):\n"
              "    show \"Hello,\", name\n");
  CHECK(space.ovis("check", "greet").status == 0);
  space.write("greet/greet.ovis",
              "# Greets someone.\n# example: greet \"Ana\"\n# example: greet 7 times\n"
              "to greet (name):\n    show \"Hello,\", name\n");
  const auto broken = space.ovis("check", "greet");
  CHECK(broken.status != 0);
  CHECK_THAT(broken.output, ContainsSubstring("greet.ovis (examples), line 3"));
  space.git("greet", "commit -qam 1.1.0");
  const auto refused = space.ovis("release 1.1.0", "greet");
  CHECK(refused.status != 0);
  CHECK_THAT(refused.output, ContainsSubstring("doesn't build as its users would"));
  // One that is about other words than the phrase's.
  space.write("greet/greet.ovis",
              "# example: show a blank line\nto greet (name):\n    show \"Hello,\", name\n");
  CHECK_THAT(space.ovis("check", "greet").output,
             ContainsSubstring("doesn't use the words of the phrase below it"));
  space.write("greet/greet.ovis", "# example: greet \"Ana\"\nto greet (name):\n"
                                  "    show \"Hello,\", name\n");
  space.git("greet", "commit -qam fixed");
  CHECK(space.ovis("release 1.1.0", "greet").status == 0);
}

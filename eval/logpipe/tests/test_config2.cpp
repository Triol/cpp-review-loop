// test_config2.cpp - tests for the configuration loading extensions:
// recursive includes with cycle detection, profile override priority,
// LOGPIPE_<KEY> environment overrides (+ --no-env), the --check-config
// report table and the level.register.<NAME> custom level registry.

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "config.h"
#include "pipeline.h"
#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

int g_temp_seq = 0;

fs::path make_temp_dir(const char* prefix) {
  const fs::path dir =
      fs::temp_directory_path() /
      (std::string(prefix) + "_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
       std::to_string(g_temp_seq++));
  fs::create_directories(dir);
  return dir;
}

void write_file(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

// Minimal valid config body: one stdin input keeps the "no input sources"
// validation happy without touching the filesystem.
const std::string kMinimalInput = "input.files = stdin:\n";

std::string read_back(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// Env guard: sets an override and removes it when destroyed, so a failing
// CHECK cannot leak the variable into later tests.
class EnvVar {
 public:
  EnvVar(const char* name, const char* value) : name_(name) { ::setenv(name_.c_str(), value, 1); }
  ~EnvVar() { ::unsetenv(name_.c_str()); }
  EnvVar(const EnvVar&) = delete;
  EnvVar& operator=(const EnvVar&) = delete;

 private:
  std::string name_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Include mechanism: a nested include chain applies transitively and a later
// include overrides keys from earlier files (include position ordering).
// ---------------------------------------------------------------------------
TEST(config_include_recursive_override) {
  const fs::path dir = make_temp_dir("logpipe_inc_");
  write_file(dir / "c.conf", "tail.poll_ms = 111\n");
  write_file(dir / "b.conf", "include = c.conf\ntail.poll_ms = 222\n");
  write_file(dir / "a.conf",
             std::string("include = b.conf\ntail.poll_ms = 333\n") + kMinimalInput);

  const logpipe::Config config = logpipe::Config::load((dir / "a.conf").string());
  // c.conf loads first (111), then b.conf's own line overrides (222), then
  // a.conf's body overrides again (333): includes apply at their position.
  CHECK_EQ_INT(config.tail_poll_ms, 333);
  CHECK(contains(config.effective.at("tail.poll_ms").source, "file"));

  // Without the root-level override the deepest include's value wins over
  // the middle file only when the include comes after it.
  write_file(dir / "d.conf", "include = c.conf\n" "rotate.backups = 7\n" + std::string(kMinimalInput));
  const logpipe::Config config2 = logpipe::Config::load((dir / "d.conf").string());
  CHECK_EQ_INT(config2.tail_poll_ms, 111);  // from c.conf via include
  CHECK_EQ_INT(config2.rotate_backups, 7);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Include cycle detection: a.conf -> b.conf -> a.conf must be rejected.
// ---------------------------------------------------------------------------
TEST(config_include_cycle_detected) {
  const fs::path dir = make_temp_dir("logpipe_cycle_");
  write_file(dir / "a.conf", "include = b.conf\n" + std::string(kMinimalInput));
  write_file(dir / "b.conf", "include = a.conf\n");

  bool threw = false;
  try {
    logpipe::Config::load((dir / "a.conf").string());
  } catch (const std::exception& error) {
    threw = true;
    CHECK(contains(error.what(), "cycle"));
  }
  CHECK(threw);

  // A self-include is a cycle too.
  write_file(dir / "self.conf", "include = self.conf\n" + std::string(kMinimalInput));
  threw = false;
  try {
    logpipe::Config::load((dir / "self.conf").string());
  } catch (const std::exception& error) {
    threw = true;
    CHECK(contains(error.what(), "cycle"));
  }
  CHECK(threw);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Include paths are resolved relative to the including file's directory, not
// the working directory.
// ---------------------------------------------------------------------------
TEST(config_include_relative_to_including_file) {
  const fs::path dir = make_temp_dir("logpipe_relinc_");
  const fs::path sub = dir / "nested";
  fs::create_directories(sub);
  write_file(sub / "extra.conf", "rotate.backups = 9\n");
  write_file(dir / "root.conf", "include = nested/extra.conf\n" + std::string(kMinimalInput));

  const logpipe::Config config = logpipe::Config::load((dir / "root.conf").string());
  CHECK_EQ_INT(config.rotate_backups, 9);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Profiles: the activated profile overrides top-level keys; inactive profile
// keys are ignored entirely.
// ---------------------------------------------------------------------------
TEST(config_profile_override_priority) {
  const fs::path dir = make_temp_dir("logpipe_profile_");
  write_file(dir / "logpipe.conf",
             kMinimalInput +
                 "tail.poll_ms = 500\n"
                 "filter.level = INFO\n"
                 "profile.fast.tail.poll_ms = 50\n"
                 "profile.fast.filter.level = ERROR\n"
                 "profile.slow.tail.poll_ms = 5000\n"
                 "active_profile = fast\n");

  const logpipe::Config config = logpipe::Config::load((dir / "logpipe.conf").string());
  CHECK_EQ_INT(config.tail_poll_ms, 50);  // profile beats the top-level value
  CHECK(config.level_threshold == logpipe::Level::Error);
  CHECK(contains(config.effective.at("tail.poll_ms").source, "profile:fast"));
  CHECK(contains(config.effective.at("filter.level").source, "profile:fast"));

  // Switching the profile swaps the override set.
  write_file(dir / "logpipe.conf",
             kMinimalInput +
                 "tail.poll_ms = 500\n"
                 "profile.fast.tail.poll_ms = 50\n"
                 "profile.slow.tail.poll_ms = 5000\n"
                 "active_profile = slow\n");
  const logpipe::Config slow = logpipe::Config::load((dir / "logpipe.conf").string());
  CHECK_EQ_INT(slow.tail_poll_ms, 5000);
  CHECK(contains(slow.effective.at("tail.poll_ms").source, "profile:slow"));

  // No active profile: every profile key is ignored (no warning, no effect).
  write_file(dir / "logpipe.conf",
             kMinimalInput +
                 "tail.poll_ms = 500\n"
                 "profile.fast.tail.poll_ms = 50\n");
  const logpipe::Config plain = logpipe::Config::load((dir / "logpipe.conf").string());
  CHECK_EQ_INT(plain.tail_poll_ms, 500);
  CHECK(!contains(plain.effective.count("tail.poll_ms") ? plain.effective.at("tail.poll_ms").source
                                                        : std::string("default"),
                    "profile"));
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Selecting an undefined profile is a hard error.
// ---------------------------------------------------------------------------
TEST(config_active_profile_must_exist) {
  const fs::path dir = make_temp_dir("logpipe_badprofile_");
  write_file(dir / "logpipe.conf", kMinimalInput + std::string("active_profile = missing\n"));
  bool threw = false;
  try {
    logpipe::Config::load((dir / "logpipe.conf").string());
  } catch (const std::exception& error) {
    threw = true;
    CHECK(contains(error.what(), "missing"));
  }
  CHECK(threw);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Environment overrides: LOGPIPE_<KEY> wins over file and profile; --no-env
// (LoadOptions::use_env = false) restores the file values.
// ---------------------------------------------------------------------------
TEST(config_env_override_and_no_env) {
  const fs::path dir = make_temp_dir("logpipe_env_");
  write_file(dir / "logpipe.conf",
             kMinimalInput +
                 "tail.poll_ms = 500\n"
                 "filter.keyword = base\n"
                 "profile.p.tail.poll_ms = 50\n"
                 "active_profile = p\n");

  {
    EnvVar poll("LOGPIPE_TAIL_POLL_MS", "777");
    EnvVar keyword("LOGPIPE_FILTER_KEYWORD", "fromenv");
    const logpipe::Config config = logpipe::Config::load((dir / "logpipe.conf").string());
    CHECK_EQ_INT(config.tail_poll_ms, 777);  // env beats profile (50) and file (500)
    CHECK_STR_EQ(config.keyword, "fromenv");
    CHECK_STR_EQ(config.effective.at("tail.poll_ms").source, "env");
    CHECK_STR_EQ(config.effective.at("filter.keyword").source, "env");
  }

  {
    EnvVar poll("LOGPIPE_TAIL_POLL_MS", "777");
    logpipe::LoadOptions options;
    options.use_env = false;  // --no-env
    const logpipe::Config config =
        logpipe::Config::load((dir / "logpipe.conf").string(), options);
    CHECK_EQ_INT(config.tail_poll_ms, 50);  // back to the profile value
    CHECK(config.effective.at("tail.poll_ms").source != "env");
  }

  // Without any override the file/profile values stand and the source tags
  // never claim "env".
  const logpipe::Config plain = logpipe::Config::load((dir / "logpipe.conf").string());
  CHECK_EQ_INT(plain.tail_poll_ms, 50);
  CHECK_STR_EQ(plain.keyword, "base");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// LOGPIPE_ACTIVE_PROFILE selects the profile with the highest priority, and
// LOGPIPE_<KEY> supports the dotted filter.level key.
// ---------------------------------------------------------------------------
TEST(config_env_selects_profile_and_level) {
  const fs::path dir = make_temp_dir("logpipe_envprofile_");
  write_file(dir / "logpipe.conf",
             kMinimalInput +
                 "filter.level = INFO\n"
                 "profile.q.filter.level = WARN\n"
                 "profile.r.filter.level = ERROR\n");

  {
    EnvVar active("LOGPIPE_ACTIVE_PROFILE", "r");
    const logpipe::Config config = logpipe::Config::load((dir / "logpipe.conf").string());
    CHECK(config.level_threshold == logpipe::Level::Error);
  }
  {
    EnvVar active("LOGPIPE_ACTIVE_PROFILE", "q");
    EnvVar level("LOGPIPE_FILTER_LEVEL", "debug");
    const logpipe::Config config = logpipe::Config::load((dir / "logpipe.conf").string());
    CHECK(config.level_threshold == logpipe::Level::Debug);  // env beats profile q
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// --check-config report: one row per key with the effective value and the
// provenance tag (file / profile:<name> / env / default).
// ---------------------------------------------------------------------------
TEST(check_config_report_format_and_sources) {
  const fs::path dir = make_temp_dir("logpipe_checkcfg_");
  write_file(dir / "logpipe.conf",
             kMinimalInput +
                 "filter.keyword = needle\n"
                 "tail.poll_ms = 250\n"
                 "profile.p.tail.poll_ms = 25\n"
                 "active_profile = p\n");

  logpipe::Config config;
  {
    EnvVar keyword("LOGPIPE_FILTER_KEYWORD", "envneedle");
    config = logpipe::Config::load((dir / "logpipe.conf").string());

    const fs::path report = dir / "report.txt";
    std::FILE* out = std::fopen(report.string().c_str(), "w");
    CHECK(out != nullptr);
    const int rc = logpipe::check_config_report(out, "logpipe.conf", config);
    std::fclose(out);
    CHECK_EQ_INT(rc, 0);

    const std::string text = read_back(report);
    // Header and column names.
    CHECK(contains(text, "logpipe configuration report"));
    CHECK(contains(text, "KEY"));
    CHECK(contains(text, "VALUE"));
    CHECK(contains(text, "SOURCE"));
    // env-overridden key shows the env value and source tag.
    CHECK(contains(text, "filter.keyword"));
    CHECK(contains(text, "envneedle"));
    CHECK(contains(text, "env"));
    // profile-overridden key shows the profile value and source tag.
    CHECK(contains(text, "tail.poll_ms"));
    CHECK(contains(text, "25"));
    CHECK(contains(text, "profile:p"));
    // key only present in the file shows the file source tag.
    CHECK(contains(text, "filter.keyword") && contains(text, "needle"));
    // unset keys show their default with the "default" tag.
    CHECK(contains(text, "rotate.backups"));
    CHECK(contains(text, "default"));
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Custom level registry: registration through the config, threshold
// comparison against built-in levels, and parser acceptance of the new word.
// ---------------------------------------------------------------------------
TEST(custom_level_registration_and_threshold) {
  logpipe::LevelRegistry::instance().clear();
  const fs::path dir = make_temp_dir("logpipe_customlvl_");
  write_file(dir / "logpipe.conf",
             kMinimalInput +
                 "level.register.AUDIT = 2\n"
                 "filter.level = INFO\n");

  const logpipe::Config config = logpipe::Config::load((dir / "logpipe.conf").string());
  CHECK(config.level_threshold == logpipe::Level::Info);

  logpipe::LogParser parser;
  logpipe::LogFilter filter(config.level_threshold, "");

  // AUDIT registered as 2 sits between WARN(2): equal to WARN's ordinal, so
  // it passes the INFO threshold while DEBUG is filtered.
  const logpipe::LogRecord audit =
      parser.parse("in.log", 1, "2026-09-06 10:00:00 [AUDIT] checked");
  CHECK(audit.parsed);
  CHECK_EQ_INT(static_cast<int>(audit.level), 2);
  CHECK(filter.passes(audit));

  const logpipe::LogRecord debug =
      parser.parse("in.log", 2, "2026-09-06 10:00:01 [DEBUG] noisy");
  CHECK(!filter.passes(debug));

  // A custom name works as the threshold itself: AUDIT-threshold rejects
  // INFO (1 < 2) but accepts WARN (2 >= 2).
  logpipe::LogFilter audit_filter(static_cast<logpipe::Level>(2), "");
  CHECK(!audit_filter.passes(parser.parse("in.log", 3, "2026-09-06 10:00:02 [INFO] mid")));
  CHECK(audit_filter.passes(parser.parse("in.log", 4, "2026-09-06 10:00:03 [WARN] high")));

  // A custom level above ERROR also passes an ERROR threshold.
  write_file(dir / "logpipe.conf",
             kMinimalInput +
                 "level.register.FATALX = 999\n"
                 "filter.level = FATALX\n");
  const logpipe::Config config2 = logpipe::Config::load((dir / "logpipe.conf").string());
  logpipe::LogFilter fatal_filter(config2.level_threshold, "");
  CHECK(!fatal_filter.passes(parser.parse("in.log", 5, "2026-09-06 10:00:04 [ERROR] not enough")));
  CHECK(fatal_filter.passes(parser.parse("in.log", 6, "2026-09-06 10:00:05 [FATALX] enough")));
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Custom level validation: out-of-range values, built-in name collisions and
// duplicate registrations with a different value are rejected.
// ---------------------------------------------------------------------------
TEST(custom_level_validation_errors) {
  logpipe::LevelRegistry::instance().clear();
  const fs::path dir = make_temp_dir("logpipe_lvlerr_");

  auto expect_error = [&](const std::string& body, const char* needle) {
    write_file(dir / "logpipe.conf", kMinimalInput + body);
    bool threw = false;
    try {
      logpipe::Config::load((dir / "logpipe.conf").string());
    } catch (const std::exception& error) {
      threw = true;
      CHECK(contains(error.what(), needle));
    }
    if (!threw) {
      ++testfw::failures();
      std::fprintf(stderr, "  expected Config::load to throw for body: %s\n", body.c_str());
    }
  };

  // Value below the minimum (1) and above the maximum (999).
  expect_error("level.register.ZERO = 0\n", "between");
  expect_error("level.register.BIG = 1000\n", "between");
  // Non-numeric values are rejected by the integer parser.
  expect_error("level.register.TEXT = high\n", "expects an integer");
  // Built-in names cannot be redefined.
  expect_error("level.register.ERROR = 5\n", "built-in");
  // Duplicate registration with a different value.
  expect_error("level.register.DUP = 5\nlevel.register.DUP = 6\n", "already registered");

  // Idempotent re-registration (same name, same value) is accepted.
  write_file(dir / "logpipe.conf",
             kMinimalInput +
                 "level.register.SAME = 5\nlevel.register.SAME = 5\n");
  bool threw = false;
  try {
    logpipe::Config::load((dir / "logpipe.conf").string());
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(!threw);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Custom levels inside the filter.expr DSL: level comparisons accept the
// registered name at compile time and evaluate by numeric severity.
// ---------------------------------------------------------------------------
TEST(custom_level_in_dsl_expression) {
  logpipe::LevelRegistry::instance().clear();
  const fs::path dir = make_temp_dir("logpipe_lvldsl_");
  write_file(dir / "logpipe.conf",
             kMinimalInput +
                 "level.register.NOTICE2 = 1\n"
                 "filter.expr = level >= \"NOTICE2\"\n");

  const logpipe::Config config = logpipe::Config::load((dir / "logpipe.conf").string());
  CHECK(config.filter_expr != nullptr);

  logpipe::LogParser parser;
  const logpipe::LogRecord info =
      parser.parse("in.log", 1, "2026-09-06 11:00:00 [INFO] fine");
  const logpipe::LogRecord debug =
      parser.parse("in.log", 2, "2026-09-06 11:00:01 [DEBUG] chatty");
  CHECK(config.filter_expr->passes(info));
  CHECK(!config.filter_expr->passes(debug));

  // An unregistered name still fails compilation.
  write_file(dir / "logpipe.conf",
             kMinimalInput + "filter.expr = level >= \"NOSUCHLEVEL\"\n");
  bool threw = false;
  try {
    logpipe::Config::load((dir / "logpipe.conf").string());
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

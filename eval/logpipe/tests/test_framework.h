// test_framework.h - minimal zero-dependency assertion runner shared by the
// test translation units (test_main.cpp, test_dsl.cpp).
//
// Tests register themselves via the TEST macro into a single registry;
// main() in test_main.cpp runs them in order and exits non-zero when any
// CHECK failed.

#ifndef LOGPIPE_TEST_FRAMEWORK_H_
#define LOGPIPE_TEST_FRAMEWORK_H_

#include <cstdio>
#include <string>
#include <vector>

namespace testfw {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> registry;
  return registry;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int& failures() {
  static int count = 0;
  return count;
}

}  // namespace testfw

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      ++testfw::failures();                                                      \
      std::fprintf(stderr, "  CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,  \
                   #cond);                                                       \
    }                                                                            \
  } while (0)

// Equality check for values printable with c_str() (std::string and const char*).
#define CHECK_STR_EQ(actual, expected)                                           \
  do {                                                                           \
    const std::string& a_ = (actual);                                            \
    const std::string& e_ = (expected);                                          \
    if (a_ != e_) {                                                              \
      ++testfw::failures();                                                      \
      std::fprintf(stderr, "  CHECK_STR_EQ failed at %s:%d\n    actual  : \"%s\"\n" \
                           "    expected: \"%s\"\n",                             \
                   __FILE__, __LINE__, a_.c_str(), e_.c_str());                  \
    }                                                                            \
  } while (0)

// Equality check for integer-like values (sizes, positions, counters).
#define CHECK_EQ_INT(actual, expected)                                           \
  do {                                                                           \
    const long long a_ = static_cast<long long>(actual);                         \
    const long long e_ = static_cast<long long>(expected);                       \
    if (a_ != e_) {                                                              \
      ++testfw::failures();                                                      \
      std::fprintf(stderr, "  CHECK_EQ_INT failed at %s:%d: %lld != %lld\n",     \
                   __FILE__, __LINE__, a_, e_);                                  \
    }                                                                            \
  } while (0)

#define TEST(name)                                                     \
  static void name();                                                  \
  static const testfw::Registrar registrar_##name(#name, &name);       \
  static void name()

#endif  // LOGPIPE_TEST_FRAMEWORK_H_

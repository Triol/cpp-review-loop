# Minimal zero-dependency test runner. The sources under test (tailer, util,
# config, rle, writer) are compiled straight into the test binary.
add_executable(logpipe_tests
  test_main.cpp
  test_dsl.cpp
  test_features.cpp
  test_fanout.cpp
  test_config2.cpp
  ../src/config.cpp
  ../src/dsl.cpp
  ../src/kv_extractor.cpp
  ../src/prom_stats.cpp
  ../src/tailer.cpp
  ../src/util.cpp
  ../src/rle.cpp
  ../src/transform.cpp
  ../src/writer.cpp)

target_include_directories(logpipe_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# GCC < 9 needs an explicit link for std::filesystem.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
  target_link_libraries(logpipe_tests PRIVATE stdc++fs)
endif()
target_link_libraries(logpipe_tests PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(logpipe_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(logpipe_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME logpipe_tests COMMAND logpipe_tests)

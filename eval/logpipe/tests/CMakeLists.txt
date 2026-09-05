# Minimal zero-dependency test runner. The sources under test (tailer, util)
# are compiled straight into the test binary; the rest of logpipe is headers.
add_executable(logpipe_tests
  test_main.cpp
  ../src/config.cpp
  ../src/tailer.cpp
  ../src/util.cpp)

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

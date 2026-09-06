# logpipe executable: public headers live in ../include, sources in this dir.
add_executable(logpipe
  main.cpp
  util.cpp
  rle.cpp
  config.cpp
  tailer.cpp
  stdin_reader.cpp
  writer.cpp)

target_include_directories(logpipe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# GCC < 9 needs an explicit link for std::filesystem.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
  target_link_libraries(logpipe PRIVATE stdc++fs)
endif()
target_link_libraries(logpipe PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(logpipe PRIVATE /W4 /permissive-)
else()
  target_compile_options(logpipe PRIVATE -Wall -Wextra -Wpedantic)
endif()

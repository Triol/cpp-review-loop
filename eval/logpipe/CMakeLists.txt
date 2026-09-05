cmake_minimum_required(VERSION 3.15)

project(logpipe
  VERSION 1.0.0
  DESCRIPTION "logpipe - multi-source log collection pipeline"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# std::thread needs the pthreads backend linked on POSIX toolchains.
find_package(Threads REQUIRED)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)

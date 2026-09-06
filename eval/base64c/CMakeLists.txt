cmake_minimum_required(VERSION 3.15)
project(base64c LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)

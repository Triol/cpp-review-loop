add_library(base64c base64c.cpp)
target_include_directories(base64c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_options(base64c PRIVATE -Wall -Wextra)

add_executable(base64c_demo main.cpp)
target_link_libraries(base64c_demo PRIVATE base64c)
target_compile_options(base64c_demo PRIVATE -Wall -Wextra)

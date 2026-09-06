add_executable(base64c_tests test_base64c.cpp)
target_link_libraries(base64c_tests PRIVATE base64c)
target_compile_options(base64c_tests PRIVATE -Wall -Wextra)

include(CTest)
add_test(NAME base64c_tests COMMAND base64c_tests)

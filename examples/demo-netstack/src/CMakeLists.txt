add_library(netstack
    net/pool.cpp
    db/row.cpp
    util/legacy.cpp)

target_include_directories(netstack PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(netstack PRIVATE ENABLE_TRACE_LOG NOMINMAX)

# fjarr-opsim — the operator simulator (docs/23#fjarr-opsim-the-operator-simulator).
add_executable(fjarr-opsim main.cpp)
target_include_directories(fjarr-opsim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(fjarr-opsim PRIVATE fjarr::fjarr PkgConfig::GST PkgConfig::SOUP)
set_target_properties(fjarr-opsim PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/agent/tools)

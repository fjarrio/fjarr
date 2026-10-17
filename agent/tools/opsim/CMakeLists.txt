# fjarr-opsim — the operator simulator (docs/23#fjarr-opsim-the-operator-simulator).
add_executable(fjarr-opsim main.cpp checks.cpp)
target_include_directories(fjarr-opsim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# RTP header access for the audio timestamp check (an Opus track's RTP clock, M3.5); opsim's alone.
pkg_check_modules(GSTRTP REQUIRED IMPORTED_TARGET gstreamer-rtp-1.0)
target_link_libraries(fjarr-opsim PRIVATE fjarr::fjarr PkgConfig::GST PkgConfig::GSTRTP PkgConfig::SOUP)
set_target_properties(fjarr-opsim PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/agent/tools)

# Unit + loop tests (GoogleTest, docs/15). `make agent-test`; ASan/TSan presets.
find_package(GTest REQUIRED)
add_executable(fjarr-tests
  test_main.cpp
  test_backoff.cpp
  test_loop.cpp
  test_protocol.cpp
  test_config.cpp
  test_frame_hub.cpp
  test_channels.cpp
  test_blob.cpp
  test_introspect_capability.cpp
  test_session.cpp
  test_loop_media.cpp
  test_introspect.cpp
  test_sources.cpp
  test_camera.cpp
)
target_include_directories(fjarr-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(fjarr-tests PRIVATE fjarr::fjarr GTest::gtest PkgConfig::GST PkgConfig::SOUP)
target_compile_definitions(fjarr-tests PRIVATE FJARR_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/protocol/fixtures")
include(GoogleTest)
gtest_discover_tests(fjarr-tests DISCOVERY_TIMEOUT 60)

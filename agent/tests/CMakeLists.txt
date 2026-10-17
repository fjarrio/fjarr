# Unit + loop tests (GoogleTest, docs/15). `make agent-test`; ASan/TSan presets.
find_package(GTest REQUIRED)
add_executable(fjarr-tests
  test_main.cpp
  test_backoff.cpp
  test_loop.cpp
  test_protocol.cpp
  test_config.cpp
  test_desktop_modules.cpp
  test_helper_protocol.cpp
  test_monitor_identity.cpp
  test_keymap.cpp
  test_clipboard_types.cpp
  test_edid.cpp
  test_x11_layout.cpp
  test_ghost_primary.cpp
  test_default_sink.cpp
  test_cursor_image.cpp
  test_capture_memory.cpp
  test_encoder_memory.cpp
  ${CMAKE_SOURCE_DIR}/agent/modules/desktop-mutter/capture_memory.cpp
  ${CMAKE_SOURCE_DIR}/agent/src/desktop/monitor_identity.cpp
  ${CMAKE_SOURCE_DIR}/agent/src/desktop/keymap.cpp
  ${CMAKE_SOURCE_DIR}/agent/src/desktop/helper_protocol.cpp
  ${CMAKE_SOURCE_DIR}/agent/src/desktop/cursor_image.cpp
  ${CMAKE_SOURCE_DIR}/agent/src/desktop/edid.cpp
  ${CMAKE_SOURCE_DIR}/agent/src/desktop/x11_layout.cpp
  ${CMAKE_SOURCE_DIR}/agent/src/desktop/ghost_primary.cpp
  test_opsim_checks.cpp
  ${CMAKE_SOURCE_DIR}/agent/tools/opsim/checks.cpp
  test_frame_hub.cpp
  test_channels.cpp
  test_blob.cpp
  test_nice_options.cpp
  test_system_profile.cpp
  test_rate_estimator.cpp
  test_supervision.cpp
  test_terminal_capability.cpp
  test_audio_capability.cpp
  test_net.cpp
  test_sctp_watch.cpp
  test_tier_policy.cpp
  test_passthrough.cpp
  test_introspect_capability.cpp
  test_session.cpp
  test_control_domains.cpp
  test_control.cpp
  test_loop_media.cpp
  test_consumer_negotiation.cpp
  test_introspect.cpp
  test_sources.cpp
  test_camera.cpp
)
# ../tools: the parts of fjarr-opsim's checks that need no session (opsim/checks.hpp), tested like the agent's own.
# ../modules: module E's capture memory (ADR-0041), tested against hand-made PipeWire buffers.
target_include_directories(fjarr-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../tools ${CMAKE_CURRENT_SOURCE_DIR}/../modules)
target_link_libraries(fjarr-tests PRIVATE fjarr::fjarr GTest::gtest PkgConfig::GST PkgConfig::GSTALLOC PkgConfig::LIBPIPEWIRE PkgConfig::SOUP PkgConfig::XKBCOMMON)
target_compile_definitions(fjarr-tests PRIVATE FJARR_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/protocol/fixtures")
# The supervision probe spawns the real daemon under a fake systemd notify socket (docs/15).
target_compile_definitions(fjarr-tests PRIVATE FJARR_AGENT_BINARY="$<TARGET_FILE:fjarr-agent>")
add_dependencies(fjarr-tests fjarr-agent)
include(GoogleTest)
gtest_discover_tests(fjarr-tests DISCOVERY_TIMEOUT 60)

# A desktop backend module, built exactly as a shipped one is (ADR-0021): its own shared object,
# discovered and dlopened by the loader. Kept out of the test binary on purpose — linking it in
# would prove nothing about loading it.
add_library(fjarr-desktop-stub MODULE desktop_module_stub.cpp)
target_include_directories(fjarr-desktop-stub PRIVATE ${CMAKE_SOURCE_DIR}/agent/src ${CMAKE_SOURCE_DIR}/agent/include)
target_link_libraries(fjarr-desktop-stub PRIVATE PkgConfig::GST nlohmann_json::nlohmann_json)
set_target_properties(fjarr-desktop-stub PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/desktop-modules" PREFIX "lib")
add_dependencies(fjarr-tests fjarr-desktop-stub)
target_compile_definitions(fjarr-tests PRIVATE FJARR_STUB_MODULE_DIR="${CMAKE_CURRENT_BINARY_DIR}/desktop-modules")

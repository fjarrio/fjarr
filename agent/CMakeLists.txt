# libfjarr — the robot-side library (public headers = the docs/09 surface),
# the fjarr-agent reference daemon, fjarr-opsim and the tests.
# spec: docs/23-agent-core-architecture.md · docs/14-dependencies.md
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0>=1.24
  gstreamer-app-1.0
  gstreamer-video-1.0
  gstreamer-webrtc-1.0
  gstreamer-sdp-1.0)
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0>=3.4)
pkg_check_modules(SYSTEMD IMPORTED_TARGET libsystemd)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(tomlplusplus 3.4 REQUIRED)

# nlohmann json-schema-validator (MIT): no Ubuntu package — pinned FetchContent (docs/14).
include(FetchContent)
set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json_schema_validator
  GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
  GIT_TAG 2.3.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(json_schema_validator)

add_library(fjarr STATIC
  src/version.cpp
  src/agent.cpp
  src/core/glib/raii.cpp
  src/core/log.cpp
  src/core/backoff.cpp
  src/core/loop.cpp
  src/core/protocol.cpp
  src/core/config.cpp
  src/core/signaling_client.cpp
  src/core/blob.cpp
  src/core/blob_pump.cpp
  src/core/session.cpp
  src/core/session_manager.cpp
  src/media/frame_stamp.cpp
  src/media/sources.cpp
  src/media/frame_hub.cpp
  src/media/encoder.cpp
  src/media/producer.cpp
  src/media/consumer.cpp
  src/media/media_plane.cpp
  src/media/probe.cpp
  src/media/rate_estimator.cpp
  src/media/tier_policy.cpp
  src/media/pad_counter.cpp
  src/introspect/introspector.cpp
  src/introspect/memory.cpp
  src/introspect/bundle.cpp
  src/introspect/diagnostics.cpp
  src/introspect/server.cpp
  src/capabilities/test_capability.cpp
  src/capabilities/camera_capability.cpp
  src/capabilities/introspect_capability.cpp
  src/capabilities/terminal_capability.cpp
  src/capabilities/net_capability.cpp
  src/capabilities/net_addressing.cpp
  src/capabilities/desktop_capability.cpp
  src/desktop/module_loader.cpp
)
add_library(fjarr::fjarr ALIAS fjarr)
target_include_directories(fjarr
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fjarr
  PUBLIC nlohmann_json::nlohmann_json PkgConfig::GST
  PRIVATE PkgConfig::SOUP tomlplusplus::tomlplusplus nlohmann_json_schema_validator util ${CMAKE_DL_LIBS}) # util: forkpty (fjarr.terminal); dl: desktop modules (ADR-0021)
target_compile_definitions(fjarr PUBLIC GST_USE_UNSTABLE_API)
if(SYSTEMD_FOUND)
  target_link_libraries(fjarr PRIVATE PkgConfig::SYSTEMD)
  target_compile_definitions(fjarr PRIVATE FJARR_HAVE_SYSTEMD=1)
endif()

add_executable(fjarr-agent daemon/main.cpp)
target_link_libraries(fjarr-agent PRIVATE fjarr::fjarr PkgConfig::GST)
if(SYSTEMD_FOUND)
  target_link_libraries(fjarr-agent PRIVATE PkgConfig::SYSTEMD)
  target_compile_definitions(fjarr-agent PRIVATE FJARR_HAVE_SYSTEMD=1)
endif()
set_target_properties(fjarr-agent PROPERTIES RUNTIME_OUTPUT_DIRECTORY
  ${CMAKE_BINARY_DIR}/agent/daemon)

add_subdirectory(tools/opsim)
add_subdirectory(tests)

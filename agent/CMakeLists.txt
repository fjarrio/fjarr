# libfjarr — the robot-side library (public headers = the docs/09 surface),
# the fjarr-agent reference daemon, fjarr-opsim and the tests.
# spec: docs/23-agent-core-architecture.md · docs/14-dependencies.md
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0>=1.24
  gstreamer-app-1.0
  gstreamer-video-1.0
  gstreamer-webrtc-1.0
  gstreamer-sdp-1.0
  nice) # the local addresses libnice gathers from, without the tunnel interface (docs/23, #34)
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
  src/core/sctp_watch.cpp
  src/core/backoff.cpp
  src/core/loop.cpp
  src/core/protocol.cpp
  src/core/config.cpp
  src/core/signaling_client.cpp
  src/core/blob.cpp
  src/core/blob_pump.cpp
  src/core/session.cpp
  src/core/session_manager.cpp
  src/core/control_domains.cpp
  src/media/frame_stamp.cpp
  src/media/sources.cpp
  src/media/frame_hub.cpp
  src/media/encoder.cpp
  src/media/producer.cpp
  src/media/consumer.cpp
  src/media/nice_options.cpp
  src/core/system_profile.cpp
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
  src/desktop/keycodes.cpp
  src/desktop/clipboard_types.cpp
)
add_library(fjarr::fjarr ALIAS fjarr)
target_include_directories(fjarr
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fjarr
  PUBLIC nlohmann_json::nlohmann_json PkgConfig::GST
  PRIVATE PkgConfig::SOUP tomlplusplus::tomlplusplus nlohmann_json_schema_validator util ${CMAKE_DL_LIBS}) # util: forkpty (fjarr.terminal); dl: desktop modules (ADR-0021)
target_compile_definitions(fjarr PUBLIC GST_USE_UNSTABLE_API PRIVATE FJARR_VERSION="${PROJECT_VERSION}")
if(SYSTEMD_FOUND)
  target_link_libraries(fjarr PRIVATE PkgConfig::SYSTEMD)
  target_compile_definitions(fjarr PRIVATE FJARR_HAVE_SYSTEMD=1)
endif()

# fjarr-desktop-session (ADR-0028, M3): the desktop user's side of the handover — mutter over the
# session bus, descriptors to the agent's module over /run/fjarr/desktop.sock. It links nothing of
# libfjarr: it runs as another account and knows only the helper protocol.
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
# libpipewire: the helper narrows each handed connection to its capture's node (docs/23).
pkg_check_modules(LIBPIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
add_executable(fjarr-desktop-session desktop-session/main.cpp desktop-session/mutter.cpp desktop-session/pipewire.cpp src/desktop/helper_protocol.cpp
  src/desktop/clipboard_types.cpp)
target_include_directories(fjarr-desktop-session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fjarr-desktop-session PRIVATE PkgConfig::GIO PkgConfig::LIBPIPEWIRE nlohmann_json::nlohmann_json)

# Backend module E (ADR-0006, ADR-0021): mutter through the session helper, as its own shared object
# in desktop-modules/ — where FJARR_DESKTOP_MODULE_DIR points in the lab, and what the
# fjarr-desktop-wayland package installs to /usr/lib/fjarr/desktop (M3 3.3).
# Input is libei on the EIS socket the helper hands over; text is typed through mutter's keymap with
# xkbcommon; the stream reader (frames and the cursor) is libpipewire (docs/23#desktop-helper-protocol). All are the module's
# dependencies, not libfjarr's.
pkg_check_modules(LIBEI REQUIRED IMPORTED_TARGET libei-1.0)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
add_library(fjarr-desktop-mutter MODULE modules/desktop-mutter/backend.cpp modules/desktop-mutter/eis_input.cpp modules/desktop-mutter/stream_reader.cpp
  src/desktop/helper_protocol.cpp src/desktop/monitor_identity.cpp src/desktop/keymap.cpp src/desktop/clipboard_types.cpp src/desktop/cursor_image.cpp)
target_include_directories(fjarr-desktop-mutter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fjarr-desktop-mutter PRIVATE PkgConfig::GST PkgConfig::GIO PkgConfig::LIBEI PkgConfig::XKBCOMMON PkgConfig::LIBPIPEWIRE nlohmann_json::nlohmann_json)
set_target_properties(fjarr-desktop-mutter PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/desktop-modules" PREFIX "lib" CXX_VISIBILITY_PRESET hidden)

add_executable(fjarr-agent daemon/main.cpp)
target_link_libraries(fjarr-agent PRIVATE fjarr::fjarr PkgConfig::GST)
# The reference daemon, unlike the demos, may use libfjarr's internals for installation concerns an
# embedder never calls: the system profile check behind --check (docs/26#the-system-profile).
target_include_directories(fjarr-agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(SYSTEMD_FOUND)
  target_link_libraries(fjarr-agent PRIVATE PkgConfig::SYSTEMD)
  target_compile_definitions(fjarr-agent PRIVATE FJARR_HAVE_SYSTEMD=1)
endif()
set_target_properties(fjarr-agent PROPERTIES RUNTIME_OUTPUT_DIRECTORY
  ${CMAKE_BINARY_DIR}/agent/daemon)

add_subdirectory(tools/opsim)
# Package builds (debian/rules) turn the tests off: they need GoogleTest and a GStreamer runtime the
# builder has, but a .deb must not depend on either to exist.
option(FJARR_BUILD_TESTS "Build the agent's unit and loop tests" ON)
if(FJARR_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# What the packages install (docs/26#packages): fjarr-agent, and libfjarr-dev — the static library,
# its headers and a CMake package. The json-schema-validator is a private dependency fetched at build
# time with no Ubuntu package, so its archive travels in fjarr's own export set, kept in a private
# directory rather than beside the system's libraries.
install(TARGETS fjarr-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# fjarr-desktop-wayland: the module where the agent looks for it (ADR-0021) and the session helper.
install(TARGETS fjarr-desktop-mutter LIBRARY DESTINATION lib/fjarr/desktop)
install(TARGETS fjarr-desktop-session RUNTIME DESTINATION lib/fjarr)
install(TARGETS fjarr EXPORT fjarrTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
# Its header is private to libfjarr (no embedder includes it); its PUBLIC_HEADER property names it
# relative to its own directory, which does not resolve from here, so it is cleared, not installed.
set_target_properties(nlohmann_json_schema_validator PROPERTIES PUBLIC_HEADER "")
install(TARGETS nlohmann_json_schema_validator EXPORT fjarrTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/fjarr)
install(DIRECTORY include/fjarr DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT fjarrTargets NAMESPACE fjarr:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fjarr)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/fjarrConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/fjarrConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fjarr)
# 0.x: a minor release may break the API (docs/26#releases), so only the same minor is compatible.
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/fjarrConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/fjarrConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/fjarrConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fjarr)

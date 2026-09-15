# libfjarr — the robot-side library (public headers = the docs/09 surface),
# plus the fjarr-agent reference daemon.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0>=1.24
  gstreamer-app-1.0
  gstreamer-video-1.0
  gstreamer-webrtc-1.0
  gstreamer-sdp-1.0)
find_package(nlohmann_json 3.11 REQUIRED)

add_library(fjarr STATIC
  src/version.cpp
  src/agent.cpp
)
add_library(fjarr::fjarr ALIAS fjarr)
target_include_directories(fjarr PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(fjarr
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE PkgConfig::GST)
target_compile_definitions(fjarr PRIVATE GST_USE_UNSTABLE_API)

add_executable(fjarr-agent daemon/main.cpp)
target_link_libraries(fjarr-agent PRIVATE fjarr::fjarr PkgConfig::GST)
set_target_properties(fjarr-agent PROPERTIES RUNTIME_OUTPUT_DIRECTORY
  ${CMAKE_BINARY_DIR}/agent/daemon)

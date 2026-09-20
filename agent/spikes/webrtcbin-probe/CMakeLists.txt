# Standalone spike — NOT referenced by the root build.
# Build inside the dev container, see README.md.
cmake_minimum_required(VERSION 3.22)
project(webrtcbin-probe CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0>=1.24
  gstreamer-webrtc-1.0
  gstreamer-sdp-1.0)
# JSON lines over stdio for --answerer=stdio (nlohmann-json3-dev, MIT; spike only).
find_package(nlohmann_json 3.2 REQUIRED)

add_executable(webrtcbin-probe main.cpp)
target_link_libraries(webrtcbin-probe PRIVATE PkgConfig::GST nlohmann_json::nlohmann_json)
target_compile_definitions(webrtcbin-probe PRIVATE GST_USE_UNSTABLE_API)
target_compile_options(webrtcbin-probe PRIVATE -Wall -Wextra)

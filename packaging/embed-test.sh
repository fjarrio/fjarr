#!/bin/sh
# The embedding channel (docs/26#packages): on a clean Ubuntu 26.04 with only libfjarr-dev installed
# from its .deb, build demo-robot — the demo that embeds libfjarr through its public API alone — out of
# tree with find_package(fjarr). If this links, a customer's CMake project will too.
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
cp /debs/libfjarr-dev_*.deb /tmp/ && apt-get install -y -qq /tmp/libfjarr-dev_*.deb cmake ninja-build g++ >/tmp/apt.log 2>&1 \
  || { tail -20 /tmp/apt.log; echo "FAIL: libfjarr-dev did not install"; exit 1; }
mkdir -p /tmp/emb && cp /src/main.cpp /tmp/emb/
cat > /tmp/emb/CMakeLists.txt <<'CM'
cmake_minimum_required(VERSION 3.25)
project(embedded-demo-robot CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(fjarr 0.0 REQUIRED)
add_executable(demo-robot main.cpp)
target_link_libraries(demo-robot PRIVATE fjarr::fjarr)
CM
cmake -S /tmp/emb -B /tmp/emb/b -G Ninja >/tmp/cmake.log 2>&1 || { cat /tmp/cmake.log; echo "FAIL: find_package(fjarr)"; exit 1; }
cmake --build /tmp/emb/b >/tmp/build.log 2>&1 || { tail -30 /tmp/build.log; echo "FAIL: demo-robot did not build against the package"; exit 1; }
echo "ok   demo-robot built and linked from libfjarr-dev alone ($(stat -c %s /tmp/emb/b/demo-robot) bytes)"
echo "deb-embed-test: embedding works"

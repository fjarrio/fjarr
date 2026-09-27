cmake_minimum_required(VERSION 3.25)
project(fjarr VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)
# A function that reaches its end without returning is never intentional and is undefined behaviour,
# so it is an error rather than a warning. It was a warning when the tunnel's packet pump fell off
# the end of a `-> bool` callback, and the crash it caused — glibc aborting on a corrupted heap
# seconds into any bulk transfer — reached a shipped build with the diagnosis already in the log
# (docs/18 #28). Other warnings stay warnings for now; see docs/18.
add_compile_options(-Werror=return-type)

enable_testing() # before the subdirectories, or ctest sees none of their tests (GoogleTest — docs/15)

add_subdirectory(agent)
add_subdirectory(demos/demo-robot)

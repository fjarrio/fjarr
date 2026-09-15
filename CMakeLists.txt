cmake_minimum_required(VERSION 3.25)
project(fjarr VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)

add_subdirectory(agent)
add_subdirectory(demos/demo-robot)

enable_testing() # populated from M1 (GoogleTest — docs/15)

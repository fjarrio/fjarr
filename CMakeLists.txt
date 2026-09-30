cmake_minimum_required(VERSION 3.25)
project(fjarr VERSION 0.1.2 LANGUAGES CXX)

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

# Every target this repository ships or tests builds warning-free, and stays that way: a warning
# is an error on those targets. Per target rather than a global -Werror, because a global one also
# applies to the FetchContent'd json-schema-validator, whose warnings are not ours to fix — which is
# how -Werror gets reverted a week after it is added. The eight warnings that stood when this went in
# were fixed at their cause (slice 4.5e follow-up), not silenced.
function(fjarr_warnings_are_errors)
  foreach(target IN LISTS ARGN)
    if(TARGET ${target})
      set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
    endif()
  endforeach()
endfunction()

enable_testing() # before the subdirectories, or ctest sees none of their tests (GoogleTest — docs/15)

add_subdirectory(agent)
add_subdirectory(demos/demo-robot)
fjarr_warnings_are_errors(fjarr fjarr-agent fjarr-opsim fjarr-tests fjarr-desktop-stub demo-robot)

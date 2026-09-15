# demo-robot — the "customer's robot software" embedding libfjarr.
# DEMO RULE: may consume ONLY the public fjarr::fjarr API (docs/02).
add_executable(demo-robot main.cpp)
target_link_libraries(demo-robot PRIVATE fjarr::fjarr)
set_target_properties(demo-robot PROPERTIES RUNTIME_OUTPUT_DIRECTORY
  ${CMAKE_BINARY_DIR}/demos/demo-robot)

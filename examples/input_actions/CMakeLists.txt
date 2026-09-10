# CMakeLists.txt for the Catalyst input actions example.
# Headless: exercises the device registry, gamepad polling and the action layer with no window, so it builds and runs
# without the platform module. Links catalyst::input alone.
# License: CDDL-1.0 (see LICENSE).

add_executable(catalyst_input_actions
  main.cpp
)

target_link_libraries(catalyst_input_actions PRIVATE catalyst::input)

target_include_directories(catalyst_input_actions PRIVATE ${PROJECT_SOURCE_DIR}/include)

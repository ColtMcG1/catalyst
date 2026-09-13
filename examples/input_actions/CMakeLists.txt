# CMakeLists.txt for the Catalyst input actions example.
# Headless: exercises the device registry, gamepad polling and the action layer with no window, so it builds and runs
# without the platform module. Links catalyst::input and catalyst::logging, which is what it reports through.
# License: MIT (see LICENSE).

add_executable(catalyst_input_actions
  main.cpp
)

target_link_libraries(catalyst_input_actions PRIVATE catalyst::input catalyst::logging)

target_include_directories(catalyst_input_actions PRIVATE ${PROJECT_SOURCE_DIR}/include)

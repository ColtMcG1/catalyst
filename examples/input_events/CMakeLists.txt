# CMakeLists.txt for the Catalyst input events example application.
# Creates a window and echoes keyboard, text, mouse and gamepad events, and demonstrates the polled input_state tracker
# and cursor capture, and reports everything through the logging module. Links the monolithic library when available,
# otherwise the platform, input, core and logging modules.
# License: CDDL-1.0 (see LICENSE).

add_executable(catalyst_input_events
  main.cpp
)

if(TARGET catalyst)
  target_link_libraries(catalyst_input_events PRIVATE catalyst)
else()
  target_link_libraries(catalyst_input_events PRIVATE catalyst_platform catalyst_input catalyst_core catalyst::logging)
endif()

target_include_directories(catalyst_input_events PRIVATE ${PROJECT_SOURCE_DIR}/include)

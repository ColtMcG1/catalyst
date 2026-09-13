# CMakeLists.txt for the Catalyst window polling example application.
# This example demonstrates how to set up a simple application that links against the Catalyst platform library to create a window and poll for events. It includes the main source file and links against either the monolithic catalyst library or the platform module, depending on how the project is configured. The example also ensures that the include directories are set up correctly to access the Catalyst headers.
# License: MIT (see LICENSE).

add_executable(catalyst_window_polling
  main.cpp
)

# Prefer the monolithic library if enabled; otherwise link the platform and logging modules.
if(TARGET catalyst)
  target_link_libraries(catalyst_window_polling PRIVATE catalyst)
else()
  target_link_libraries(catalyst_window_polling PRIVATE catalyst_platform catalyst::logging)
endif()

target_include_directories(catalyst_window_polling PRIVATE ${PROJECT_SOURCE_DIR}/include)

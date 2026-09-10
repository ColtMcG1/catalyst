# CMakeLists.txt for the Catalyst rendering_basics example.
# Opens a window, creates a rendering device and swapchain for it, and records/submits a clearing render pass each
# frame. Demonstrates the intended per-frame flow of the rendering API together with the platform module.
# License: CDDL-1.0 (see LICENSE).

add_executable(catalyst_rendering_basics
  main.cpp
)

if(TARGET catalyst)
  target_link_libraries(catalyst_rendering_basics PRIVATE catalyst)
else()
  target_link_libraries(catalyst_rendering_basics PRIVATE catalyst::rendering catalyst::platform catalyst::logging)
endif()

target_include_directories(catalyst_rendering_basics PRIVATE ${PROJECT_SOURCE_DIR}/include)

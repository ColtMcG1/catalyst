# CMakeLists.txt for the Catalyst rendering benchmarks.
# The benchmarks are written against the public rendering API and need a window to present into, so they are only built
# when both the rendering and platform modules are enabled.
# License: CDDL-1.0 (see LICENSE).

if(TARGET catalyst::rendering AND TARGET catalyst::platform)
  add_subdirectory(vulkan)
else()
  message(STATUS "Catalyst rendering benchmarks skipped: they need CATALYST_BUILD_RENDERING and CATALYST_BUILD_PLATFORM")
endif()

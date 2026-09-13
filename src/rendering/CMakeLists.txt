add_library(catalyst_rendering STATIC
  rendering.cpp
  error.cpp
  device.cpp
  timeline.cpp
  queue.cpp
  buffer.cpp
  shader.cpp
  texture.cpp
  pipeline.cpp
  swapchain.cpp
  command.cpp
  frame.cpp
  transfer.cpp
)

add_library(catalyst::rendering ALIAS catalyst_rendering)

target_include_directories(catalyst_rendering
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Diagnostics go through catalyst::logging rather than to stderr (src/rendering/detail_log.hpp).
# PRIVATE: no public rendering header includes a logging one.
target_link_libraries(catalyst_rendering PRIVATE catalyst::logging)

set(CATALYST_RENDERING_BACKEND "auto" CACHE STRING "Rendering backend (auto|d3d12|vulkan|null)")
set_property(CACHE CATALYST_RENDERING_BACKEND PROPERTY STRINGS auto d3d12 vulkan null)

# `auto` prefers Vulkan on every platform, which is the backend Catalyst targets for Windows and
# Linux. It used to resolve to d3d12 on Windows, and that was a trap: the d3d12 and metal backends
# are identity stubs whose resource layer is the null bookkeeping implementation, so an `auto` build
# on Windows silently produced a renderer that recorded state and drew nothing. Both stay selectable
# by name, and stay opt-in until they are real.
#
# `auto` *prefers* rather than *resolves to* Vulkan because the SDK is not everywhere. Resolving to
# it unconditionally meant `cmake -S . -B build` -- the command in the README -- failed at configure
# time on any machine without a Vulkan SDK, which is most machines on a first clone. So `auto`
# probes, and falls back to the null backend with a message saying what happened. Naming `vulkan`
# explicitly still requires it and still fails loudly, because someone who asked for Vulkan by name
# wants to hear that it is missing rather than get a renderer that draws nothing.
set(_catalyst_rendering_backend "${CATALYST_RENDERING_BACKEND}")
if(_catalyst_rendering_backend STREQUAL "auto")
  find_package(Vulkan QUIET)

  if(Vulkan_FOUND)
    set(_catalyst_rendering_backend "vulkan")
    message(STATUS "Catalyst: rendering backend 'auto' resolved to 'vulkan' (${Vulkan_VERSION})")
  else()
    set(_catalyst_rendering_backend "null")
    message(STATUS
      "Catalyst: no Vulkan SDK found, so rendering backend 'auto' resolved to 'null'. "
      "The null backend records state and draws nothing; it builds and tests everywhere. "
      "For a renderer that draws, install a Vulkan SDK (1.3+) and reconfigure, or set "
      "-DCATALYST_RENDERING_BACKEND=vulkan to make a missing SDK an error.")
  endif()
endif()

# CPU-side bookkeeping implementation of the resource contract (src/rendering/detail_backend.hpp).
# It is the resource layer of the null backend and stands in for backends whose GPU resource layer
# has not been written yet, so the public API links and is testable on every configuration.
set(_catalyst_rendering_null_resources ${CMAKE_CURRENT_SOURCE_DIR}/null/null_resources.cpp)

if(_catalyst_rendering_backend STREQUAL "d3d12")
  if(NOT WIN32)
    message(FATAL_ERROR "CATALYST_RENDERING_BACKEND=d3d12 is only supported on Windows")
  endif()

  add_library(catalyst_rendering_backend_d3d12 STATIC
    d3d12/rendering_backend_d3d12.cpp
    # TODO(d3d12): replace with a real resource implementation.
    ${_catalyst_rendering_null_resources}
  )

  target_include_directories(catalyst_rendering_backend_d3d12
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/include
  )

  target_link_libraries(catalyst_rendering_backend_d3d12 PRIVATE catalyst::logging)

  target_link_libraries(catalyst_rendering PRIVATE catalyst_rendering_backend_d3d12)
elseif(_catalyst_rendering_backend STREQUAL "vulkan")
  # Headers and the loader import library come from the Vulkan SDK (VULKAN_SDK env var) or the system package.
  find_package(Vulkan REQUIRED)

  add_library(catalyst_rendering_backend_vulkan STATIC
    vulkan/rendering_backend_vulkan.cpp
    vulkan/vulkan_device.cpp
    vulkan/vulkan_memory.cpp
    vulkan/vulkan_buffer.cpp
    vulkan/vulkan_shader.cpp
    vulkan/vulkan_texture.cpp
    vulkan/vulkan_pipeline.cpp
    vulkan/vulkan_swapchain.cpp
    vulkan/vulkan_command.cpp
    vulkan/vulkan_transfer.cpp
  )

  target_include_directories(catalyst_rendering_backend_vulkan
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/include
  )

  target_link_libraries(catalyst_rendering_backend_vulkan PRIVATE catalyst::logging)

  target_link_libraries(catalyst_rendering_backend_vulkan PUBLIC Vulkan::Vulkan)

  target_link_libraries(catalyst_rendering PRIVATE catalyst_rendering_backend_vulkan)
elseif(_catalyst_rendering_backend STREQUAL "null")
  add_library(catalyst_rendering_backend_null STATIC
    null/rendering_backend_null.cpp
    ${_catalyst_rendering_null_resources}
  )

  target_include_directories(catalyst_rendering_backend_null
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/include
  )

  target_link_libraries(catalyst_rendering_backend_null PRIVATE catalyst::logging)

  target_link_libraries(catalyst_rendering PRIVATE catalyst_rendering_backend_null)
else()
  message(FATAL_ERROR "Unknown CATALYST_RENDERING_BACKEND: ${_catalyst_rendering_backend}")
endif()

# The window backends buffer events in catalyst::core's event_queue and window.hpp exposes core event types, so the
# platform module cannot be built without the core module.
if(NOT TARGET catalyst::core)
  message(FATAL_ERROR "Catalyst platform requires CATALYST_BUILD_CORE to be enabled")
endif()

add_library(catalyst_platform STATIC
  platform.cpp
  monitor.cpp
  window.cpp
)

add_library(catalyst::platform ALIAS catalyst_platform)

target_include_directories(catalyst_platform
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(catalyst_platform PUBLIC catalyst::core)

set(CATALYST_PLATFORM_BACKEND "auto" CACHE STRING "Platform backend (auto|win32|null)")
set_property(CACHE CATALYST_PLATFORM_BACKEND PROPERTY STRINGS auto win32 null)

set(_catalyst_platform_backend "${CATALYST_PLATFORM_BACKEND}")
if(_catalyst_platform_backend STREQUAL "auto")
  if(WIN32)
    set(_catalyst_platform_backend "win32")
  else()
    set(_catalyst_platform_backend "null")
  endif()
endif()

if(_catalyst_platform_backend STREQUAL "win32")
  if(NOT WIN32)
    message(FATAL_ERROR "CATALYST_PLATFORM_BACKEND=win32 is only supported on Windows")
  endif()

  add_library(catalyst_platform_backend_win32 STATIC
    win32/platform_backend_win32.cpp
    win32/win32_helpers.cpp
    win32/monitor_backend_win32.cpp
    win32/window_backend_win32.cpp
  )

  target_include_directories(catalyst_platform_backend_win32
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/include
  )

  # dwmapi: DwmGetCompositionTimingInfo supplies the display refresh period used to pace frames during a modal resize
  # loop, and DwmSetWindowAttribute selects the dark window frame.
  target_link_libraries(catalyst_platform_backend_win32 PUBLIC dwmapi catalyst::core)

  # UTF-8 conversion, run-time entry-point lookup and the <windows.h> include, shared with the input
  # and audio backends. PUBLIC because win32_helpers.hpp re-exports the string helpers.
  target_link_libraries(catalyst_platform_backend_win32 PUBLIC catalyst::win32_support)

  target_link_libraries(catalyst_platform PRIVATE catalyst_platform_backend_win32)
elseif(_catalyst_platform_backend STREQUAL "null")
  add_library(catalyst_platform_backend_null STATIC
    null/platform_backend_null.cpp
    null/monitor_backend_null.cpp
    null/window_backend_null.cpp
  )

  target_include_directories(catalyst_platform_backend_null
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/include
  )

  target_link_libraries(catalyst_platform_backend_null PUBLIC catalyst::core)

  target_link_libraries(catalyst_platform PRIVATE catalyst_platform_backend_null)
else()
  message(FATAL_ERROR "Unknown CATALYST_PLATFORM_BACKEND: ${_catalyst_platform_backend}")
endif()

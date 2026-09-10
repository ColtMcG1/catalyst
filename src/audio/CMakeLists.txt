# The null backend is always compiled, so the module builds, links and tests on every platform, and
# the offline renderer needs no backend at all. Platform backends are added conditionally and
# announce themselves to the registry through CATALYST_AUDIO_HAS_* definitions. All backends live in
# one target: the registry is the only definition of create_backend(), so there is nothing left to
# isolate and a single target removes any link-order sensitivity between it and its factories.
add_library(catalyst_audio STATIC
  audio.cpp
  backend_registry.cpp
  device.cpp
  error.cpp
  mixer.cpp
  offline.cpp
  stream.cpp
  null/audio_backend_null.cpp
)

add_library(catalyst::audio ALIAS catalyst_audio)

target_include_directories(catalyst_audio
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Device and stream events are published to a catalyst::events::bus, which is header-only, so this
# is an include dependency rather than a link one - it comes in with the include directory above.

set(CATALYST_AUDIO_BACKEND "auto" CACHE STRING "Audio backend (auto|win32|null)")
set_property(CACHE CATALYST_AUDIO_BACKEND PROPERTY STRINGS auto win32 null)

set(_catalyst_audio_backend "${CATALYST_AUDIO_BACKEND}")
if(_catalyst_audio_backend STREQUAL "auto")
  if(WIN32)
    set(_catalyst_audio_backend "win32")
  else()
    set(_catalyst_audio_backend "null")
  endif()
endif()

if(_catalyst_audio_backend STREQUAL "win32")
  if(NOT WIN32)
    message(FATAL_ERROR "CATALYST_AUDIO_BACKEND=win32 is only supported on Windows")
  endif()

  # Win32 audio builds always include ASIO (SDK-free minimal loader).
  target_sources(catalyst_audio PRIVATE
    wasapi/wasapi_backend_win32.cpp
    asio/asio_backend_win32.cpp
    asio/asio_loader.cpp
  )

  target_compile_definitions(catalyst_audio
    PRIVATE
      CATALYST_AUDIO_HAS_WASAPI
      CATALYST_AUDIO_HAS_ASIO
  )

  # Avrt provides MMCSS ("Pro Audio") scheduling for the WASAPI render thread.
  target_link_libraries(catalyst_audio PRIVATE Ole32 Uuid Avrt)

  # UTF-8 conversion and the <windows.h> include, shared with the platform and input backends.
  target_link_libraries(catalyst_audio PRIVATE catalyst::win32_support)
elseif(_catalyst_audio_backend STREQUAL "null")
  # Nothing to add: the always-compiled null and offline backends cover this configuration.
else()
  message(FATAL_ERROR "Unknown CATALYST_AUDIO_BACKEND: ${_catalyst_audio_backend}")
endif()

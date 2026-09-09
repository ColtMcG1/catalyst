# Module targets live in subdirectories

# Win32 plumbing shared by the platform, input and audio backends. Not a module: no build switch,
# and it is added before them so every backend can link it.
if(WIN32)
  add_subdirectory(win32)
endif()

if(CATALYST_BUILD_ANIMATION)
  add_subdirectory(animation)
endif()

if(CATALYST_BUILD_AUDIO)
  add_subdirectory(audio)
endif()

if(CATALYST_BUILD_CORE)
  add_subdirectory(core)
endif()

if(CATALYST_BUILD_INPUT)
  add_subdirectory(input)
endif()

if(CATALYST_BUILD_LOGGING)
  add_subdirectory(logging)
endif()

if(CATALYST_BUILD_MATH)
  add_subdirectory(math)
endif()

if(CATALYST_BUILD_NET)
  add_subdirectory(net)
endif()

if(CATALYST_BUILD_PHYSICS)
  add_subdirectory(physics)
endif()

if(CATALYST_BUILD_PLATFORM)
  add_subdirectory(platform)
endif()

if(CATALYST_BUILD_RENDERING)
  add_subdirectory(rendering)
endif()

if(CATALYST_BUILD_RESOURCE)
  add_subdirectory(resource)
endif()

if(CATALYST_BUILD_SCENE)
  add_subdirectory(scene)
endif()

if(CATALYST_BUILD_UI)
  add_subdirectory(ui)
endif()

if(CATALYST_BUILD_UTILS)
  add_subdirectory(utils)
endif()

# Monolithic library that consumers can link against.
# It links whatever modules are enabled.
if(CATALYST_BUILD_ALL)
  add_library(catalyst STATIC
    catalyst.cpp
  )

  add_library(catalyst::catalyst ALIAS catalyst)
  add_library(catalyst::all ALIAS catalyst)

  target_include_directories(catalyst
    PUBLIC
      $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
  )

  if(TARGET catalyst_rendering)
    target_link_libraries(catalyst PUBLIC catalyst_rendering)
  endif()

  if(TARGET catalyst_resource)
    target_link_libraries(catalyst PUBLIC catalyst_resource)
  endif()

  if(TARGET catalyst_scene)
    target_link_libraries(catalyst PUBLIC catalyst_scene)
  endif()

  if(TARGET catalyst_ui)
    target_link_libraries(catalyst PUBLIC catalyst_ui)
  endif()

  if(TARGET catalyst_animation)
    target_link_libraries(catalyst PUBLIC catalyst_animation)
  endif()

  if(TARGET catalyst_audio)
    target_link_libraries(catalyst PUBLIC catalyst_audio)
  endif()

  if(TARGET catalyst_core)
    target_link_libraries(catalyst PUBLIC catalyst_core)
  endif()

  if(TARGET catalyst_input)
    target_link_libraries(catalyst PUBLIC catalyst_input)
  endif()

  if(TARGET catalyst_logging)
    target_link_libraries(catalyst PUBLIC catalyst_logging)
  endif()

  if(TARGET catalyst_math)
    target_link_libraries(catalyst PUBLIC catalyst_math)
  endif()

  if(TARGET catalyst_net)
    target_link_libraries(catalyst PUBLIC catalyst_net)
  endif()

  if(TARGET catalyst_physics)
    target_link_libraries(catalyst PUBLIC catalyst_physics)
  endif()

  if(TARGET catalyst_platform)
    target_link_libraries(catalyst PUBLIC catalyst_platform)
  endif()

  if(TARGET catalyst_utils)
    target_link_libraries(catalyst PUBLIC catalyst_utils)
  endif()
endif()

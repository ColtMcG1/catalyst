if(NOT TARGET catalyst::core)
  message(FATAL_ERROR "Catalyst benchmarks require CATALYST_BUILD_CORE to be enabled")
endif()

add_subdirectory(events)
add_subdirectory(json)
add_subdirectory(render)

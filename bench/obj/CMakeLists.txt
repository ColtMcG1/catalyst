# CMakeLists.txt for the Catalyst OBJ benchmarks.
# Needs the resource target for the parser itself; catalyst::text is header-only and comes in on the
# shared include path, the same way catalyst::resource::csv picks up its scans.
# License: MIT (see LICENSE).

if(NOT TARGET catalyst::resource)
  message(STATUS "Catalyst OBJ benchmarks skipped: they need CATALYST_BUILD_RESOURCE")
  return()
endif()

add_executable(catalyst_bench_obj
  obj.cpp
)

target_link_libraries(catalyst_bench_obj
  PRIVATE
    catalyst::resource
)

target_include_directories(catalyst_bench_obj
  PRIVATE
    ${PROJECT_SOURCE_DIR}/bench
)

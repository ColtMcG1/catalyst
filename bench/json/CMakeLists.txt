# CMakeLists.txt for the Catalyst JSON benchmarks.
# The benchmarks are written against the public catalyst::resource::json API, which is header-only inside
# the resource module, so they only need the resource target to exist.
# License: MIT (see LICENSE).

if(NOT TARGET catalyst::resource)
  message(STATUS "Catalyst JSON benchmarks skipped: they need CATALYST_BUILD_RESOURCE")
  return()
endif()

add_executable(catalyst_bench_json
  json.cpp
)

target_link_libraries(catalyst_bench_json
  PRIVATE
    catalyst::resource
)

target_include_directories(catalyst_bench_json
  PRIVATE
    ${PROJECT_SOURCE_DIR}/bench
)

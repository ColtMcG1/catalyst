# CMakeLists.txt for the Catalyst rendering benchmarks.
# One executable covering four suites: pipeline construction, resource creation and transfers, per-frame overhead and
# quad throughput. The sources only use the public rendering API, so the binary measures whichever backend
# CATALYST_RENDERING_BACKEND selected; the embedded SPIR-V in shaders.hpp limits the drawing suites to backends that
# consume it. Regenerate that header with scripts/embed_spirv.py after editing anything in shaders/.
# License: CDDL-1.0 (see LICENSE).

add_executable(catalyst_bench_render_vulkan
  main.cpp
  render_bench.cpp
  bench_pipeline.cpp
  bench_resources.cpp
  bench_frame.cpp
  bench_quads.cpp
)

if(TARGET catalyst)
  target_link_libraries(catalyst_bench_render_vulkan PRIVATE catalyst)
else()
  target_link_libraries(catalyst_bench_render_vulkan PRIVATE catalyst::rendering catalyst::platform)
endif()

target_include_directories(catalyst_bench_render_vulkan
  PRIVATE
    ${PROJECT_SOURCE_DIR}/bench
    ${PROJECT_SOURCE_DIR}/include
)

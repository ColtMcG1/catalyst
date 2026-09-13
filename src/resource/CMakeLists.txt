add_library(catalyst_resource STATIC
  resource.cpp

  blob.cpp
  error.cpp
  image.cpp
  image_dds.cpp
  image_decode.cpp
  image_ktx2.cpp
  image_mips.cpp
  image_stb.cpp
  loader.cpp
  source.cpp
  vfs.cpp

  csv/error.cpp
  csv/parser.cpp
  csv/serializer.cpp
  csv/table.cpp
  csv/tape.cpp

  json/document.cpp
  json/error.cpp
  json/parser.cpp
  json/serializer.cpp
  json/tape.cpp
  json/value.cpp

  obj/parser.cpp

  uri/error.cpp
  uri/parser.cpp
  uri/percent.cpp
  uri/reference.cpp
  uri/resolver.cpp
)

add_library(catalyst::resource ALIAS catalyst_resource)

target_include_directories(catalyst_resource
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# The asset system reuses catalyst::rendering's `format`, `resource_handle` and `extent3d` rather
# than defining its own. <catalyst/rendering/types.hpp> is constexpr enums and templates with no
# external linkage, so this is an include-path dependency and NOT a link dependency: a build with
# CATALYST_BUILD_RENDERING=OFF still compiles every line of this target, and a headless tool that
# only wants JSON/CSV/URI does not drag in Vulkan or D3D12.
#
# The GPU bridge that turns a resource::image into a rendering::texture is Tier 3 and will live in a
# separate catalyst_resource_gpu target, which *does* link catalyst::rendering. See docs/resource.md.

# ---------------------------------------------------------------------------------------------
# stb_image -- the source-format decoders behind resource::load_image.
#
# PRIVATE, and that is the whole point: <catalyst/resource/image.hpp> never mentions stb, so a
# consumer of this library gets `load_image` without stb's 8k lines landing in their translation
# unit and without stb's include directory on their search path. The dependency stops at the
# library boundary.
#
# It switches off less than its name suggests. image_stb.cpp is the only translation unit that
# includes stb at all; the KTX2 and DDS readers are this module's own code with no codec behind
# them, so a CATALYST_RESOURCE_STB=OFF build still reads every cooked container, still generates mip
# chains, and still has a working loader<image>. What it loses is PNG, JPEG and the rest of the
# source formats -- the ones an editor reads and a shipped build should not be reading anyway.
# ---------------------------------------------------------------------------------------------
if(CATALYST_RESOURCE_STB)
  include(${PROJECT_SOURCE_DIR}/cmake/stb.cmake)

  target_sources(catalyst_resource PRIVATE detail/stb_image_impl.cpp)
  target_link_libraries(catalyst_resource PRIVATE catalyst::stb)
  target_compile_definitions(catalyst_resource PRIVATE CATALYST_RESOURCE_HAS_STB=1)

  # Third-party code compiled verbatim at a pinned commit. Its warnings are not ours to fix and a
  # patched copy would make the next re-pin a merge, so they are off for this one source file
  # rather than for the target.
  #
  # Not for MSVC: it compiles stb_image silently at the default warning level, and adding /w there
  # only earns a D9025 for overriding the /W flag CMake already put on the command line. If this
  # project ever raises MSVC's warning level, this is where the exemption goes -- along with
  # whatever it takes to add it without re-triggering D9025.
  if(NOT MSVC)
    set_source_files_properties(detail/stb_image_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
  endif()
else()
  target_compile_definitions(catalyst_resource PRIVATE CATALYST_RESOURCE_HAS_STB=0)
endif()

cmake_minimum_required(VERSION 3.16)

project(Catalyst VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# Fail here, with one sentence, rather than several hundred lines of template errors partway
# through the build. See cmake/CatalystCompilerSupport.cmake for what is checked and why.
include(CatalystCompilerSupport)
catalyst_require_cxx23_support()

# When using MSVC-style debug info (/Zi) with parallel builds, multiple compiler
# processes can contend for the shared PDB and appear to hang. /FS forces
# serialized PDB writes.
if(MSVC)
	add_compile_options(/FS)
endif()

option(CATALYST_BUILD_ALL "Build monolithic Catalyst library (links enabled modules)" ON)
option(CATALYST_BUILD_ANIMATION "Build Catalyst animation module" ON)
option(CATALYST_BUILD_AUDIO "Build Catalyst audio module" ON)
option(CATALYST_BUILD_CORE "Build Catalyst core module" ON)
option(CATALYST_BUILD_INPUT "Build Catalyst input module" ON)
option(CATALYST_BUILD_LOGGING "Build Catalyst logging module" ON)
option(CATALYST_BUILD_MATH "Build Catalyst math module" ON)
option(CATALYST_BUILD_NET "Build Catalyst net module" ON)
option(CATALYST_BUILD_PHYSICS "Build Catalyst physics module" ON)
option(CATALYST_BUILD_PLATFORM "Build Catalyst platform module" ON)
option(CATALYST_BUILD_RENDERING "Build Catalyst rendering module" ON)
option(CATALYST_BUILD_RESOURCE "Build Catalyst resource module" ON)
option(CATALYST_BUILD_SCENE "Build Catalyst scene module" ON)
option(CATALYST_BUILD_UI "Build Catalyst UI module" ON)
option(CATALYST_BUILD_UTILS "Build Catalyst utils module" ON)
# Image decoding in catalyst::resource. ON fetches stb_image at a pinned commit (cmake/stb.cmake),
# which needs network access the first time a build tree is configured. OFF keeps the resource
# module dependency-free and makes load_image report unsupported_format instead of decoding.
option(CATALYST_RESOURCE_STB "Fetch stb_image and build the resource module's image decoders" ON)

option(CATALYST_BUILD_EXAMPLES "Build Catalyst examples" ON)
option(CATALYST_BUILD_TESTS "Build Catalyst tests" ON)
option(CATALYST_BUILD_BENCHMARKS "Build Catalyst benchmarks" ON)

add_subdirectory(src)

if(CATALYST_BUILD_EXAMPLES)
	add_subdirectory(examples)
endif()

if(CATALYST_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()

if(CATALYST_BUILD_BENCHMARKS)
	add_subdirectory(bench)
endif()
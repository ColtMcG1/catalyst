add_library(catalyst_core STATIC
  core.cpp
)

add_library(catalyst::core ALIAS catalyst_core)

target_include_directories(catalyst_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

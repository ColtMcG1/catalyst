add_library(catalyst_math STATIC
  math.cpp
)

add_library(catalyst::math ALIAS catalyst_math)

target_include_directories(catalyst_math
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

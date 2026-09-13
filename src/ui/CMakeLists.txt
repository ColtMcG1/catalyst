add_library(catalyst_ui STATIC
  ui.cpp
  tree.cpp
  layout.cpp
)

add_library(catalyst::ui ALIAS catalyst_ui)

target_include_directories(catalyst_ui
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Geometry and layout are expressed with the vector and rectangle types from catalyst::math.
if(TARGET catalyst_math)
  target_link_libraries(catalyst_ui PUBLIC catalyst_math)
endif()

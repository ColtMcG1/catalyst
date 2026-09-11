add_library(catalyst_resource STATIC
  resource.cpp

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

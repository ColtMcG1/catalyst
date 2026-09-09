add_library(catalyst_logging STATIC
  logging.cpp
  format.cpp
  event_ring.cpp
  filter.cpp
  middleware.cpp
  sink.cpp
  router.cpp
  log.cpp
  sinks/console.cpp
  sinks/terminal.cpp
  sinks/file.cpp
  sinks/ring_buffer.cpp
  sinks/callback.cpp
  sinks/async.cpp
  sinks/queued.cpp
)

add_library(catalyst::logging ALIAS catalyst_logging)

target_include_directories(catalyst_logging
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# The floor below which log calls are compiled out. It is defined on the public interface because
# the level functions are templates: two translation units that disagree about the floor would be
# two definitions of the same function.
set(CATALYST_LOG_COMPILED_LEVEL "trace" CACHE STRING "Lowest log level compiled in (trace|debug|info|warn|error|fatal|critical)")
set_property(CACHE CATALYST_LOG_COMPILED_LEVEL PROPERTY STRINGS trace debug info warn error fatal critical)

if(NOT CATALYST_LOG_COMPILED_LEVEL MATCHES "^(trace|debug|info|warn|error|fatal|critical)$")
  message(FATAL_ERROR "CATALYST_LOG_COMPILED_LEVEL must be a level name, got: ${CATALYST_LOG_COMPILED_LEVEL}")
endif()

target_compile_definitions(catalyst_logging
  PUBLIC
    CATALYST_LOG_COMPILED_LEVEL=${CATALYST_LOG_COMPILED_LEVEL}
)

# Sinks are called from every thread that logs.
find_package(Threads REQUIRED)
target_link_libraries(catalyst_logging PUBLIC Threads::Threads)

# The console sink turns on the console's virtual terminal processing before it writes colour, which
# needs <windows.h> -- pulled in the one way Catalyst pulls it in anywhere.
if(WIN32)
  target_link_libraries(catalyst_logging PRIVATE catalyst::win32_support)
endif()

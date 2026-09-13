add_executable(catalyst_bench_events_dispatcher
  dispatcher.cpp
)

target_include_directories(catalyst_bench_events_dispatcher
  PRIVATE
    ${PROJECT_SOURCE_DIR}/bench
    ${PROJECT_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(catalyst_bench_events_dispatcher PRIVATE Threads::Threads)
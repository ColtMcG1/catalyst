find_package(Threads REQUIRED)

add_executable(catalyst_tests_events_bus
	test_bus.cpp
)

# The event bus is header-only, so there is no module target to link; the
# include directory and a threading implementation are all it needs.
target_include_directories(catalyst_tests_events_bus
	PRIVATE
		${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(catalyst_tests_events_bus
	PRIVATE
		Threads::Threads
)

add_test(
	NAME catalyst.events.bus
	COMMAND catalyst_tests_events_bus
)

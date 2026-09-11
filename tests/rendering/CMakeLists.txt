add_executable(catalyst_tests_rendering_resources
	test_resources.cpp
)

target_link_libraries(catalyst_tests_rendering_resources
	PRIVATE
		catalyst::rendering
)

add_test(
	NAME catalyst.rendering.resources
	COMMAND catalyst_tests_rendering_resources
)

add_executable(catalyst_tests_rendering_diagnostics
	test_diagnostics.cpp
)

target_link_libraries(catalyst_tests_rendering_diagnostics
	PRIVATE
		catalyst::rendering
)

add_test(
	NAME catalyst.rendering.diagnostics
	COMMAND catalyst_tests_rendering_diagnostics
)

add_executable(catalyst_tests_rendering_timeline
	test_timeline.cpp
)

target_link_libraries(catalyst_tests_rendering_timeline
	PRIVATE
		catalyst::rendering
)

add_test(
	NAME catalyst.rendering.timeline
	COMMAND catalyst_tests_rendering_timeline
)

add_executable(catalyst_tests_rendering_frames
	test_frames.cpp
)

target_link_libraries(catalyst_tests_rendering_frames
	PRIVATE
		catalyst::rendering
)

add_test(
	NAME catalyst.rendering.frames
	COMMAND catalyst_tests_rendering_frames
)

add_executable(catalyst_tests_rendering_transfer
	test_transfer.cpp
)

target_link_libraries(catalyst_tests_rendering_transfer
	PRIVATE
		catalyst::rendering
)

add_test(
	NAME catalyst.rendering.transfer
	COMMAND catalyst_tests_rendering_transfer
)

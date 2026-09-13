add_executable(catalyst_tests_ui_measurement
	test_measurement.cpp
)

target_link_libraries(catalyst_tests_ui_measurement
	PRIVATE
		catalyst::ui
)

add_test(
	NAME catalyst.ui.measurement
	COMMAND catalyst_tests_ui_measurement
)

add_executable(catalyst_tests_ui_geometry
	test_geometry.cpp
)

target_link_libraries(catalyst_tests_ui_geometry
	PRIVATE
		catalyst::ui
)

add_test(
	NAME catalyst.ui.geometry
	COMMAND catalyst_tests_ui_geometry
)

add_executable(catalyst_tests_ui_tree
	test_tree.cpp
)

target_link_libraries(catalyst_tests_ui_tree
	PRIVATE
		catalyst::ui
)

add_test(
	NAME catalyst.ui.tree
	COMMAND catalyst_tests_ui_tree
)

add_executable(catalyst_tests_ui_layout
	test_layout.cpp
)

target_link_libraries(catalyst_tests_ui_layout
	PRIVATE
		catalyst::ui
)

add_test(
	NAME catalyst.ui.layout
	COMMAND catalyst_tests_ui_layout
)

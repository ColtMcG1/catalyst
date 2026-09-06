add_executable(catalyst_tests_resource_json
	test_json.cpp
)

target_link_libraries(catalyst_tests_resource_json
	PRIVATE
		catalyst::resource
)

add_test(
	NAME catalyst.resource.json
	COMMAND catalyst_tests_resource_json
)

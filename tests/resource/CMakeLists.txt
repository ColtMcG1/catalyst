foreach(suite csv json uri)
	add_executable(catalyst_tests_resource_${suite}
		test_${suite}.cpp
	)

	target_link_libraries(catalyst_tests_resource_${suite}
		PRIVATE
			catalyst::resource
	)

	add_test(
		NAME catalyst.resource.${suite}
		COMMAND catalyst_tests_resource_${suite}
	)
endforeach()

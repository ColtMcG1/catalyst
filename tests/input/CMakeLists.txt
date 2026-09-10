# One executable per test file, named catalyst.input.<name> in CTest.
foreach(_test keyboard registry state action gamepad)
	add_executable(catalyst_tests_input_${_test}
		test_${_test}.cpp
	)

	target_link_libraries(catalyst_tests_input_${_test}
		PRIVATE
			catalyst::input
	)

	add_test(
		NAME catalyst.input.${_test}
		COMMAND catalyst_tests_input_${_test}
	)
endforeach()

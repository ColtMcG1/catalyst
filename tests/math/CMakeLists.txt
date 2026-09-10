foreach(suite vector matrix quaternion transform)
	add_executable(catalyst_tests_math_${suite}
		test_${suite}.cpp
	)

	target_link_libraries(catalyst_tests_math_${suite}
		PRIVATE
			catalyst::math
	)

	add_test(
		NAME catalyst.math.${suite}
		COMMAND catalyst_tests_math_${suite}
	)
endforeach()

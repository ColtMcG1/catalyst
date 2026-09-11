foreach(suite scan utf8)
	add_executable(catalyst_tests_text_${suite}
		test_${suite}.cpp
	)

	# catalyst::text is header-only, so there is no module target to link; the include
	# directory is all it needs.
	target_include_directories(catalyst_tests_text_${suite}
		PRIVATE
			${PROJECT_SOURCE_DIR}/include
	)

	add_test(
		NAME catalyst.text.${suite}
		COMMAND catalyst_tests_text_${suite}
	)
endforeach()

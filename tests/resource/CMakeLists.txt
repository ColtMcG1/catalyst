# `container`, `loader` and `mips` are unconditional along with the Tier 1 and document suites.
# None of them needs a third-party codec: the KTX2 and DDS readers are this module's own code, the
# mip filter works on images the test builds itself, and the loader suite loads a DDS.
set(CATALYST_RESOURCE_TEST_SUITES assets container csv json loader mips uri)

# The source-format suite asserts on texels stb decoded, so it only exists where stb is. A
# CATALYST_RESOURCE_STB=OFF build still runs every other resource test.
if(CATALYST_RESOURCE_STB)
	list(APPEND CATALYST_RESOURCE_TEST_SUITES image)
endif()

foreach(suite ${CATALYST_RESOURCE_TEST_SUITES})
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

if(CATALYST_BUILD_AUDIO)
	add_subdirectory(audio)
endif()

add_subdirectory(events)
add_subdirectory(math)
add_subdirectory(input)
add_subdirectory(rendering)
add_subdirectory(text)

if(CATALYST_BUILD_UI)
	add_subdirectory(ui)
endif()

if(CATALYST_BUILD_RESOURCE)
	add_subdirectory(resource)
endif()

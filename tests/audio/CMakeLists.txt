add_executable(catalyst_tests_audio_stream
	test_stream.cpp
)

target_link_libraries(catalyst_tests_audio_stream
	PRIVATE
		catalyst::audio
)

add_test(
	NAME catalyst.audio.stream
	COMMAND catalyst_tests_audio_stream
)

add_executable(catalyst_tests_audio_offline
	test_offline.cpp
)

target_link_libraries(catalyst_tests_audio_offline
	PRIVATE
		catalyst::audio
)

add_test(
	NAME catalyst.audio.offline
	COMMAND catalyst_tests_audio_offline
)

add_executable(catalyst_tests_audio_events
	test_events.cpp
)

target_link_libraries(catalyst_tests_audio_events
	PRIVATE
		catalyst::audio
)

add_test(
	NAME catalyst.audio.events
	COMMAND catalyst_tests_audio_events
)

# The shared conversion table and the GUID text helpers are reached by more than one backend and by
# no public header, so this test reaches into src/ for them.
add_executable(catalyst_tests_audio_convert
	test_convert.cpp
)

target_link_libraries(catalyst_tests_audio_convert
	PRIVATE
		catalyst::audio
)

target_include_directories(catalyst_tests_audio_convert
	PRIVATE
		${PROJECT_SOURCE_DIR}/src
)

if(WIN32)
	target_link_libraries(catalyst_tests_audio_convert PRIVATE catalyst::win32_support Ole32)
endif()

add_test(
	NAME catalyst.audio.convert
	COMMAND catalyst_tests_audio_convert
)

# The ring and the command channel are the only part of the module whose contract is about two
# threads, so this is the one audio test that starts one.
find_package(Threads REQUIRED)

add_executable(catalyst_tests_audio_command
	test_command.cpp
)

target_link_libraries(catalyst_tests_audio_command
	PRIVATE
		catalyst::audio
		Threads::Threads
)

add_test(
	NAME catalyst.audio.command
	COMMAND catalyst_tests_audio_command
)

add_executable(catalyst_tests_audio_mixer
	test_mixer.cpp
)

target_link_libraries(catalyst_tests_audio_mixer
	PRIVATE
		catalyst::audio
		Threads::Threads
)

add_test(
	NAME catalyst.audio.mixer
	COMMAND catalyst_tests_audio_mixer
)

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

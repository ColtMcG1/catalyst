# CMakeLists.txt for the Catalyst audio playback example application.
# Initializes the audio engine, enumerates the available backends and devices, and renders a short 440Hz test tone
# through the render callback before reporting the stream's health counters. Links the monolithic library when
# available, otherwise the audio module.
# License: CDDL-1.0 (see LICENSE).

add_executable(catalyst_audio_playback
  main.cpp
)

if(TARGET catalyst)
  target_link_libraries(catalyst_audio_playback PRIVATE catalyst)
else()
  target_link_libraries(catalyst_audio_playback PRIVATE catalyst_audio)
endif()

target_include_directories(catalyst_audio_playback PRIVATE ${PROJECT_SOURCE_DIR}/include)

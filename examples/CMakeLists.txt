# CMakeLists.txt for the Catalyst example applications.
# This CMakeLists.txt file sets up the build configuration for the example applications included with the Catalyst project. It adds subdirectories for each example, which contain their own CMakeLists.txt files that define the specific build instructions for those examples. This structure allows for easy organization and modularity of the example applications, making it straightforward to build and run them as needed. Each example demonstrates different aspects of using the Catalyst libraries, such as window management, event handling, and audio playback.
# License: MIT (see LICENSE).

add_subdirectory(sandbox)
add_subdirectory(window_polling)
add_subdirectory(window_waiting)
add_subdirectory(input_events)
add_subdirectory(input_actions)
add_subdirectory(rendering_basics)
add_subdirectory(audio_playback)

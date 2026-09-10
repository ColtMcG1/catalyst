# CMakeLists.txt for the Catalyst sandbox application.
# The sandbox is an empty scratch target for experimenting with the Catalyst libraries: its main function does nothing, and it links against either the monolithic catalyst library or the individual modules, depending on how the project is configured. The focused examples alongside it demonstrate the individual modules.
# License: CDDL-1.0 (see LICENSE).

add_executable(catalyst_sandbox
  main.cpp
)

# Prefer the monolithic library if enabled; otherwise link whichever modules exist.
if(TARGET catalyst)
  target_link_libraries(catalyst_sandbox PRIVATE catalyst)
else()
  if(TARGET catalyst_rendering)
    target_link_libraries(catalyst_sandbox PRIVATE catalyst_rendering)
  endif()
  if(TARGET catalyst_ui)
    target_link_libraries(catalyst_sandbox PRIVATE catalyst_ui)
  endif()
  if(TARGET catalyst_logging)
    target_link_libraries(catalyst_sandbox PRIVATE catalyst_logging)
  endif()
endif()

target_include_directories(catalyst_sandbox PRIVATE ${PROJECT_SOURCE_DIR}/include)

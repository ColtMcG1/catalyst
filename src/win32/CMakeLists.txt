# Win32 plumbing shared by the platform, input and audio backends: the <windows.h> include, UTF-8
# conversion and run-time entry-point lookup.
#
# This is deliberately not a Catalyst module and carries no CATALYST_BUILD_* switch. The modules are
# independent of one another by design -- catalyst_audio links neither catalyst_core nor
# catalyst_platform -- so folding this code into an existing module would have created exactly the
# dependency edges that separation avoids. A standalone target lets any backend link it without
# implying anything about which modules are enabled.
add_library(catalyst_win32_support STATIC
  strings.cpp
  module.cpp
)

add_library(catalyst::win32_support ALIAS catalyst_win32_support)

# Consumers include these as <win32/strings.hpp>. Angle brackets matter: several modules have their
# own win32/ subdirectory, and only the angled form skips the includer's directory and so resolves
# unambiguously to this one.
target_include_directories(catalyst_win32_support
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)

find_package(tomlplusplus CONFIG REQUIRED)

add_executable(lwforge
    main.cpp
    stamp.cpp
)

target_link_libraries(lwforge PRIVATE lwi_core tomlplusplus::tomlplusplus)

lwi_apply_warnings(lwforge)

# NO_EHCONT: vcpkg's prebuilt tomlplusplus carries no EH continuation metadata,
# and /guard:ehcont fails the link outright if any object lacks it. lwforge is a
# build-time tool that never runs elevated on a customer machine, so the
# mitigation buys nothing here. The stub keeps it.
lwi_apply_hardening(lwforge NO_EHCONT)

# wmain rather than main: paths reach this tool from the shell and must survive
# a round trip through the ANSI code page unharmed.
set_target_properties(lwforge PROPERTIES LINK_FLAGS "/ENTRY:wmainCRTStartup")

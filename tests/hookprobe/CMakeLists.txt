# A payload binary for the end-to-end fixtures. Not a unit test, so it is not
# registered with CTest: it is a thing the demo payload ships so a hook has
# something real to run and something checkable to leave behind.
#
# Console subsystem, because a hook is launched hidden and never shows a window
# either way, and a console binary is what most real hooks are.
add_executable(hookprobe main.cpp)
lwi_apply_warnings(hookprobe)
lwi_apply_hardening(hookprobe)

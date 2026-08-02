configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/stub.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/stub.rc"
    @ONLY
)

add_executable(lwstub WIN32
    main.cpp
    ui.cpp
    theme.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/stub.rc"
)

target_include_directories(lwstub PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# All in-box. The stub links nothing from vcpkg and carries no third-party code,
# which is the property that keeps its parse surface to the container alone.
target_link_libraries(lwstub PRIVATE
    lwi_core
    d2d1
    dwrite
    dwmapi
    windowscodecs
    shell32
    ole32
    shlwapi
    comctl32
)

lwi_apply_warnings(lwstub)
lwi_apply_hardening(lwstub)
lwi_embed_manifest(lwstub "${CMAKE_CURRENT_SOURCE_DIR}/resources/stub.manifest")

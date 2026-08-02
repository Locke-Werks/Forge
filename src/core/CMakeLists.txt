add_library(lwi_core STATIC
    error.cpp
    win_file.cpp
    hash.cpp
    compress.cpp
    pe_layout.cpp
    container.cpp
    config.cpp
)

target_include_directories(lwi_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# bcrypt: SHA-256 through CNG pseudo-handles.
# cabinet: the in-box Compression API (LZMS), which is why the payload path
#          needs no third-party compressor.
target_link_libraries(lwi_core PUBLIC bcrypt cabinet)

lwi_apply_warnings(lwi_core)
lwi_apply_hardening(lwi_core)

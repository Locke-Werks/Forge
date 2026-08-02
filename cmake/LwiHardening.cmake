# Exploit mitigations for a signed binary that runs elevated and parses bytes an
# attacker controls. No project in the portfolio currently sets any of these, so
# this module is new rather than copied.
#
# /Qspectre is conditional. It needs the "MSVC v143 - VS 2022 C++ x64/x86
# Spectre-mitigated libs" component, which is NOT installed by default in
# Visual Studio Community. Requiring it unconditionally makes a fresh clone fail
# to configure for a reason that reads like a code error, so it is detected and
# reported instead.

set(LWI_SPECTRE_LIBS_FOUND FALSE)
if(MSVC AND DEFINED CMAKE_CXX_COMPILER)
    get_filename_component(_lwi_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    # .../VC/Tools/MSVC/<ver>/bin/Hostx64/x64 -> .../VC/Tools/MSVC/<ver>
    get_filename_component(_lwi_msvc_root "${_lwi_msvc_bin}/../../.." ABSOLUTE)
    if(EXISTS "${_lwi_msvc_root}/lib/spectre/x64")
        set(LWI_SPECTRE_LIBS_FOUND TRUE)
    endif()
endif()

if(LWI_HARDENING AND NOT LWI_SPECTRE_LIBS_FOUND)
    message(STATUS
        "lwi: Spectre-mitigated libs not found, building without /Qspectre.\n"
        "     To enable: Visual Studio Installer > Modify > Individual components >\n"
        "     \"MSVC v143 - VS 2022 C++ x64/x86 Spectre-mitigated libs (Latest)\"")
endif()

# Pass NO_EHCONT for a target that links a third-party static library.
#
# /guard:ehcont is all-or-nothing at link time: LNK1386 rejects the whole image
# if any object lacks the metadata, and a prebuilt vcpkg library will not have
# it. That is fine to give up on build-time tooling, which never runs elevated
# on a customer machine. The stub and the uninstaller, which do, keep it.
function(lwi_apply_hardening target)
    if(NOT MSVC OR NOT LWI_HARDENING)
        return()
    endif()

    set(_ehcont TRUE)
    if("NO_EHCONT" IN_LIST ARGN)
        set(_ehcont FALSE)
    endif()

    target_compile_options(${target} PRIVATE
        /GS                       # stack buffer overrun detection
        /guard:cf                 # Control Flow Guard
        /sdl                      # additional security checks, promotes some warnings
        /Gy /Gw                   # function and data COMDATs, lets /OPT:REF do its job
        $<$<BOOL:${_ehcont}>:/guard:ehcont>
        $<$<BOOL:${LWI_SPECTRE_LIBS_FOUND}>:/Qspectre>
    )

    target_link_options(${target} PRIVATE
        /GUARD:CF
        $<$<BOOL:${_ehcont}>:/GUARD:EHCONT>
        /DYNAMICBASE              # ASLR
        /HIGHENTROPYVA            # 64-bit ASLR entropy
        /NXCOMPAT                 # DEP
        /CETCOMPAT                # shadow stack
        # LOAD_LIBRARY_SEARCH_SYSTEM32 for implicit imports. Closes DLL planting
        # against an installer run from a user-writable Downloads folder.
        # Covers implicit imports only, which is why SetDefaultDllDirectories is
        # still the first call in wWinMain.
        /DEPENDENTLOADFLAG:0x800
        /OPT:REF /OPT:ICF
        /INCREMENTAL:NO
    )
endfunction()

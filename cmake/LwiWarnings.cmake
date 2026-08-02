# The house warning set, matching DeadLetter/cmake/CompilerFlags.cmake and the
# near-identical blocks in Creechr and Parse.
#
# /WX is off by default and on in CI via -DLWI_WERROR=ON. Creechr states the
# policy: /W4 because we like warnings, no /WX because we like shipping more.
# CI is where a new warning should stop the line, not a developer's machine.

function(lwi_apply_warnings target)
    if(NOT MSVC)
        return()
    endif()

    target_compile_options(${target} PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /Zc:inline
        /Zc:throwingNew
        /utf-8
        /EHsc
        $<$<BOOL:${LWI_WERROR}>:/WX>
    )

    target_compile_definitions(${target} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
endfunction()

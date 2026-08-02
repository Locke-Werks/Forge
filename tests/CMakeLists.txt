find_package(doctest CONFIG REQUIRED)

# One executable per test file, registered with CTest, so `ctest
# --output-on-failure` works with no extra wiring. This is Reliquary's pattern,
# which is the cleanest in the portfolio.
file(GLOB LWI_TEST_SOURCES CONFIGURE_DEPENDS "test_*.cpp")

foreach(src IN LISTS LWI_TEST_SOURCES)
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE lwi_core doctest::doctest)
    lwi_apply_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endforeach()

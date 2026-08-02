#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "lwi/version.h"

using namespace lwi;

TEST_CASE("versions compare numerically, not lexicographically")
{
    // The motivating case. String comparison puts "10.0.2" before "9.0.1",
    // which makes an installer offer a downgrade as an upgrade.
    CHECK(compare_versions("10.0.2", "9.0.1") > 0);
    CHECK(compare_versions("9.0.1", "10.0.2") < 0);

    CHECK(compare_versions("1.2.3", "1.2.4") < 0);
    CHECK(compare_versions("1.3.0", "1.2.9") > 0);
    CHECK(compare_versions("2.0.0", "1.99.99") > 0);
    CHECK(compare_versions("0.9.3", "0.9.3") == 0);
}

TEST_CASE("missing trailing components are zero")
{
    CHECK(compare_versions("1.2", "1.2.0") == 0);
    CHECK(compare_versions("1", "1.0.0.0") == 0);
    CHECK(compare_versions("1.2", "1.2.1") < 0);
}

TEST_CASE("a pre-release sorts before its release")
{
    CHECK(compare_versions("1.0.0-rc1", "1.0.0") < 0);
    CHECK(compare_versions("1.0.0", "1.0.0-rc1") > 0);
    CHECK(compare_versions("1.0.0-alpha", "1.0.0-beta") < 0);
    CHECK(compare_versions("1.0.0-rc1", "1.0.0-rc1") == 0);

    // The tag does not outrank the numbers.
    CHECK(compare_versions("1.0.1-alpha", "1.0.0") > 0);
}

TEST_CASE("parsing stops at a component that is not a number")
{
    const Version v = parse_version("1.2.x.4");
    REQUIRE(v.parts.size() == 2);
    CHECK(v.parts[0] == 1);
    CHECK(v.parts[1] == 2);

    // Treating "x" as zero would make 1.2.x equal 1.2.0, which hides a typo.
    CHECK(compare_versions("1.2.x.4", "1.2") == 0);
}

TEST_CASE("round-trips through to_string")
{
    CHECK(parse_version("0.9.3").to_string() == "0.9.3");
    CHECK(parse_version("1.0.0-rc1").to_string() == "1.0.0-rc1");
    CHECK(parse_version("").to_string().empty());
}

TEST_CASE("an empty version is empty rather than zero")
{
    CHECK(parse_version("").empty());
    CHECK_FALSE(parse_version("0").empty());
}

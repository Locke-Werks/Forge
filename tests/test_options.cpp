#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "lwi/options.h"

using namespace lwi;

// The `when` grammar is read twice: by the stub, which decides whether a
// declaration fires, and by the forge, which decides whether the config is
// buildable. They share this function precisely so they cannot disagree, and
// these cases are what "agree" means.

TEST_CASE("an absent condition yields no terms")
{
    CHECK(when_terms("").empty());
    CHECK(when_terms("   ").empty());
    CHECK(when_terms("\t").empty());
}

TEST_CASE("a single term")
{
    const auto terms = when_terms("extras");
    REQUIRE(terms.size() == 1);
    CHECK(terms[0].id == "extras");
    CHECK(terms[0].negated == false);
}

TEST_CASE("negation")
{
    const auto terms = when_terms("!extras");
    REQUIRE(terms.size() == 1);
    CHECK(terms[0].id == "extras");
    CHECK(terms[0].negated == true);
}

TEST_CASE("a space after the bang means the same thing")
{
    const auto terms = when_terms("!  extras");
    REQUIRE(terms.size() == 1);
    CHECK(terms[0].id == "extras");
    CHECK(terms[0].negated == true);
}

TEST_CASE("several terms, whitespace ignored")
{
    const auto terms = when_terms("  extras ,\t!portable,shortcut ");
    REQUIRE(terms.size() == 3);
    CHECK(terms[0].id == "extras");
    CHECK(terms[0].negated == false);
    CHECK(terms[1].id == "portable");
    CHECK(terms[1].negated == true);
    CHECK(terms[2].id == "shortcut");
    CHECK(terms[2].negated == false);
}

TEST_CASE("an empty term survives to be reported rather than being dropped")
{
    // The forge fails the build on one of these. Silently discarding it here
    // would make "extras,," build and mean something slightly different from
    // what it says, which is the whole class of bug this grammar avoids by
    // being too small to have corners.
    const auto trailing = when_terms("extras,");
    REQUIRE(trailing.size() == 2);
    CHECK(trailing[0].id == "extras");
    CHECK(trailing[1].id.empty());

    const auto leading = when_terms(",extras");
    REQUIRE(leading.size() == 2);
    CHECK(leading[0].id.empty());
    CHECK(leading[1].id == "extras");

    const auto bang_only = when_terms("!");
    REQUIRE(bang_only.size() == 1);
    CHECK(bang_only[0].id.empty());
    CHECK(bang_only[0].negated == true);
}

TEST_CASE("terms are views into the expression, not copies")
{
    const std::string expression = "extras, !portable";
    const auto terms = when_terms(expression);
    REQUIRE(terms.size() == 2);
    CHECK(terms[0].id.data() >= expression.data());
    CHECK(terms[1].id.data() + terms[1].id.size() <= expression.data() + expression.size());
}

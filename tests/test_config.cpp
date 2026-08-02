#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "lwi/config.h"

using namespace lwi;

TEST_CASE("config round-trips through its binary encoding")
{
    Config original;
    original.set("product.name", "DeadLetter");
    original.set("product.version", "0.9.3");
    original.set("ui.license_text", "line one\nline two\n");

    std::vector<uint8_t> blob;
    REQUIRE(original.encode(blob).is_ok());

    Config decoded;
    REQUIRE(decoded.decode(blob).is_ok());

    CHECK(decoded.get("product.name") == "DeadLetter");
    CHECK(decoded.get("ui.license_text") == "line one\nline two\n");
    CHECK(decoded.entries().size() == original.entries().size());
}

TEST_CASE("set replaces rather than appends")
{
    Config config;
    config.set("a", "1");
    config.set("a", "2");
    CHECK(config.entries().size() == 1);
    CHECK(config.get("a") == "2");
}

TEST_CASE("array_size counts distinct indices under a prefix")
{
    // Exactly the shape lwforge produces from [[actions]] in TOML, including
    // the sibling "actions.count" key, which must not be mistaken for an index.
    Config config;
    config.set("actions.0.type", "shortcut");
    config.set("actions.0.target", "{InstallDir}\\bin\\app.exe");
    config.set("actions.0.where", "common_programs");
    config.set("actions.1.type", "arp");
    config.set("actions.count", "2");

    CHECK(config.array_size("actions") == 2);

    // A prefix that is not present is zero, not a match on a longer key.
    CHECK(config.array_size("action") == 0);
    CHECK(config.array_size("hooks") == 0);

    // Nested arrays keep their own numbering.
    config.set("hooks.post_install.0.run", "payload:daemon.exe");
    config.set("hooks.post_install.0.args.0", "--register");
    config.set("hooks.post_install.count", "1");
    CHECK(config.array_size("hooks.post_install") == 1);
    CHECK(config.array_size("hooks.post_install.0.args") == 1);
}

TEST_CASE("array_size ignores keys that only share a prefix")
{
    Config config;
    config.set("actions.0.type", "shortcut");
    config.set("actionsomething.0.type", "noise");
    config.set("actions_extra.0.type", "noise");
    CHECK(config.array_size("actions") == 1);
}

TEST_CASE("typed getters fall back rather than guessing")
{
    Config config;
    config.set("n", "17763");
    config.set("bad", "not-a-number");
    config.set("yes", "true");
    config.set("no", "0");

    CHECK(config.get_int("n") == 17763);
    CHECK(config.get_int("bad", 42) == 42);
    CHECK(config.get_int("missing", 7) == 7);
    CHECK(config.get_bool("yes"));
    CHECK_FALSE(config.get_bool("no"));
    CHECK(config.get_bool("missing", true));
}

TEST_CASE("colours parse as #rrggbb and #aarrggbb")
{
    uint32_t out = 0;

    REQUIRE(parse_color("#00cccc", out));
    CHECK(out == 0xFF00CCCCu); // six digits are opaque

    REQUIRE(parse_color("#1400E6E6", out));
    CHECK(out == 0x1400E6E6u);

    // A mistyped brand colour must fail rather than silently render black.
    CHECK_FALSE(parse_color("00cccc", out));
    CHECK_FALSE(parse_color("#00ccc", out));
    CHECK_FALSE(parse_color("#gggggg", out));
    CHECK_FALSE(parse_color("", out));
}

TEST_CASE("decode rejects a malformed blob instead of trusting its lengths")
{
    Config config;
    config.set("k", "v");
    std::vector<uint8_t> blob;
    REQUIRE(config.encode(blob).is_ok());

    SUBCASE("bad magic")
    {
        std::vector<uint8_t> bad = blob;
        bad[0] ^= 0xFF;
        Config out;
        CHECK_FALSE(out.decode(bad).is_ok());
    }

    SUBCASE("truncated")
    {
        std::vector<uint8_t> bad(blob.begin(), blob.begin() + blob.size() / 2);
        Config out;
        CHECK_FALSE(out.decode(bad).is_ok());
    }

    SUBCASE("length field points past the end")
    {
        std::vector<uint8_t> bad = blob;
        bad[8] = 0xFF;
        bad[9] = 0xFF;
        Config out;
        CHECK_FALSE(out.decode(bad).is_ok());
    }
}

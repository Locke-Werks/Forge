#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "lwi/win_file.h"

using namespace lwi;

// long_path prefixes \\?\, which turns off the normalisation Win32 would
// otherwise do. Its header stated the resulting precondition on the caller and
// nothing enforced it, so an absolute path carrying a .. reached CreateFileW
// intact and came back ERROR_INVALID_NAME. That read as a bad path in a config
// rather than as a path the caller was never told to tidy, and it cost two other
// projects an hour each before it was written up.

TEST_CASE("normalize_path resolves .. lexically")
{
    CHECK(normalize_path(L"C:\\repro\\installer\\..\\assets\\app.ico") ==
          L"C:\\repro\\assets\\app.ico");
    CHECK(normalize_path(L"C:\\a\\b\\..\\..\\c") == L"C:\\c");

    // The exact shape from the issue: an absolute config directory joined with
    // a relative icon path that walks up one level.
    CHECK(normalize_path(L"C:\\repro\\installer\\..\\assets") == L"C:\\repro\\assets");
}

TEST_CASE("normalize_path drops . segments and doubled separators")
{
    CHECK(normalize_path(L"C:\\a\\.\\b") == L"C:\\a\\b");
    CHECK(normalize_path(L"C:\\a\\\\b") == L"C:\\a\\b");
    CHECK(normalize_path(L"C:\\.") == L"C:\\");
}

TEST_CASE("normalize_path folds forward slashes")
{
    // The second face of the same bug. A forward-slash absolute path is
    // perfectly ordinary to type, and \\?\C:/x is not a path at all.
    CHECK(normalize_path(L"C:/Users/x/config.toml") == L"C:\\Users\\x\\config.toml");
    CHECK(normalize_path(L"C:/a/../b") == L"C:\\b");
}

TEST_CASE("normalize_path never pops past a root")
{
    CHECK(normalize_path(L"C:\\..") == L"C:\\");
    CHECK(normalize_path(L"C:\\..\\..\\x") == L"C:\\x");
    CHECK(normalize_path(L"\\\\server\\share\\..\\x") == L"\\\\server\\share\\x");
}

TEST_CASE("normalize_path leaves relative paths relative")
{
    CHECK(normalize_path(L"installer\\app.toml") == L"installer\\app.toml");
    CHECK(normalize_path(L"a\\..\\b") == L"b");

    // A leading .. has nowhere to go and is not a root to protect, so it stays.
    CHECK(normalize_path(L"..\\b") == L"..\\b");
    CHECK(normalize_path(L"..\\..\\b") == L"..\\..\\b");
}

TEST_CASE("normalize_path leaves empty and collapsed paths honest")
{
    // Empty stays empty. "." would hand CreateFileW the working directory,
    // which succeeds and is never what a caller with an empty path meant.
    CHECK(normalize_path(L"") == L"");
    CHECK(normalize_path(L".") == L".");
    CHECK(normalize_path(L"a\\..") == L".");
}

TEST_CASE("normalize_path leaves an already-prefixed path alone")
{
    // \\?\ and \\.\ are literal by definition. Rewriting one would change what
    // it names.
    CHECK(normalize_path(L"\\\\?\\C:\\a\\..\\b") == L"\\\\?\\C:\\a\\..\\b");
    CHECK(normalize_path(L"\\\\.\\PhysicalDrive0") == L"\\\\.\\PhysicalDrive0");
}

TEST_CASE("long_path prefixes a normalised absolute path")
{
    CHECK(long_path(L"C:\\a\\b") == L"\\\\?\\C:\\a\\b");
    CHECK(long_path(L"C:\\repro\\installer\\..\\assets\\app.ico") ==
          L"\\\\?\\C:\\repro\\assets\\app.ico");
    CHECK(long_path(L"C:/repro/assets/app.ico") == L"\\\\?\\C:\\repro\\assets\\app.ico");
}

TEST_CASE("long_path spells a UNC path the way the prefix requires")
{
    CHECK(long_path(L"\\\\server\\share\\x") == L"\\\\?\\UNC\\server\\share\\x");
    CHECK(long_path(L"\\\\server\\share\\a\\..\\x") == L"\\\\?\\UNC\\server\\share\\x");
}

TEST_CASE("long_path refuses to prefix what it cannot make absolute")
{
    // The prefix is what disables normalisation, so a path that still needs
    // resolving must not get one. Failing visibly at the API beats producing a
    // path that means something else.
    CHECK(long_path(L"relative\\x") == L"relative\\x");
    CHECK(long_path(L"C:relative") == L"C:relative");
    CHECK(long_path(L"\\rooted") == L"\\rooted");
    CHECK(long_path(L"\\\\?\\C:\\already") == L"\\\\?\\C:\\already");
}

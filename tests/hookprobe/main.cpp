// A hook that can be proved to have run.
//
// The end-to-end fixtures used to run copies of system binaries as their hooks:
// where.exe, which exits 0 and leaves nothing behind, and notepad.exe, which
// never exits at all and was terminated by its own timeout on every install
// while the suite called that a pass. Neither proved a hook ran, and the second
// spent a minute per install proving the opposite.
//
// This writes a value under HKCU that a test can read back, which is what makes
// the assertions real. HKCU rather than a file on purpose: an elevated
// installer's HKCU is the administrator's, so a value landing in the invoking
// user's hive is the evidence that `as = "user"` did what it says.
//
// Dependencies are the OS and the static CRT, nothing else, because a
// pre_install hook runs before the rest of the payload is on disk and a
// payload-supplied DLL would not be there to load.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>

namespace
{

constexpr const wchar_t* kKeyPath = L"Software\\LockeWerks\\ForgeHookProbe";

bool write_value(const wchar_t* name, const wchar_t* data)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
    {
        return false;
    }

    const DWORD bytes = static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t));
    const LSTATUS rc = RegSetValueExW(key, name, 0, REG_SZ,
                                      reinterpret_cast<const BYTE*>(data), bytes);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    // argv[1] names the value and argv[2] is its data, both optional, so the
    // probe stands in for a real hook whose arguments were written to read as
    // documentation rather than as a test fixture.
    const wchar_t* name = argc > 1 ? argv[1] : L"hook";
    const wchar_t* data = argc > 2 ? argv[2] : L"ok";

    int exit_code = 0;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (wcscmp(argv[i], L"--exit") == 0)
        {
            exit_code = _wtoi(argv[i + 1]);
        }
    }

    if (!write_value(name, data))
    {
        std::fwprintf(stderr, L"hookprobe: could not write HKCU\\%ls\\%ls\n", kKeyPath, name);
        return 2;
    }

    // What the environment block says, which is the other half of the as="user"
    // question: a hook can run as the right account and still see the elevated
    // installer's profile paths, and that failure looks exactly like success.
    wchar_t local_appdata[MAX_PATH]{};
    const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local_appdata,
                                              static_cast<DWORD>(std::size(local_appdata)));
    if (got != 0 && got < std::size(local_appdata))
    {
        wchar_t env_name[256]{};
        _snwprintf_s(env_name, std::size(env_name), _TRUNCATE, L"%ls.localappdata", name);
        write_value(env_name, local_appdata);
    }

    return exit_code;
}

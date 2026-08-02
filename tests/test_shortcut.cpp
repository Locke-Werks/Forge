#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <span>
#include <string>

#include "lwi/win_file.h"

using namespace lwi;

// Isolates shell link creation from the installer.
//
// The stub could not save a .lnk anywhere, with any flags, while PowerShell
// could write one to the same directory. This narrows that down: if these pass,
// the COM sequence is right and the difference lives in how the stub's process
// is configured. If they fail, the sequence itself is wrong.

namespace
{

std::wstring temp_dir()
{
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    return std::wstring(buf, n);
}

struct ComScope
{
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope()
    {
        if (SUCCEEDED(hr))
        {
            CoUninitialize();
        }
    }
};

} // namespace

TEST_CASE("a shell link can be created and saved")
{
    ComScope com;
    REQUIRE(SUCCEEDED(com.hr));

    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                  reinterpret_cast<void**>(&link));
    REQUIRE(SUCCEEDED(hr));
    REQUIRE(link != nullptr);

    // A real, well-formed target so nothing can be blamed on the payload.
    wchar_t system_dir[MAX_PATH]{};
    GetSystemDirectoryW(system_dir, MAX_PATH);
    const std::wstring target = std::wstring(system_dir) + L"\\notepad.exe";

    CHECK(SUCCEEDED(link->SetPath(target.c_str())));
    CHECK(SUCCEEDED(link->SetWorkingDirectory(system_dir)));

    IPersistFile* file = nullptr;
    hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
    REQUIRE(SUCCEEDED(hr));

    const std::wstring link_path = temp_dir() + L"lwi-test-shortcut.lnk";
    DeleteFileW(link_path.c_str());

    hr = file->Save(link_path.c_str(), TRUE);
    INFO("Save returned 0x" << std::hex << static_cast<unsigned long>(hr));
    CHECK(SUCCEEDED(hr));

    file->Release();
    link->Release();

    CHECK(GetFileAttributesW(link_path.c_str()) != INVALID_FILE_ATTRIBUTES);
    DeleteFileW(link_path.c_str());
}

TEST_CASE("saving a link to a malformed .exe fails, and that is worth knowing")
{
    // This cost an afternoon. IPersistFile::Save resolves the target while
    // writing the link, and a file whose extension is .exe but whose contents
    // are not a valid PE makes Save fail with a bare E_FAIL. Nothing else in
    // the sequence complains: SetPath returns S_OK, the object looks healthy,
    // and the same process can save a link to notepad.exe a line earlier.
    //
    // It stayed hidden because a target that does not exist AT ALL saves fine,
    // so the failure only appeared once the payload started containing a file
    // at the path the shortcut pointed to. The test fixture was the bug.
    //
    // Pinned here so that if a future Windows makes Save tolerant, this test
    // fails and tells us the workaround is no longer needed.
    ComScope com;
    REQUIRE(SUCCEEDED(com.hr));

    const std::wstring fake = temp_dir() + L"lwi-not-really-an.exe";
    {
        // Size matters. A handful of bytes is rejected as "obviously not an
        // image" and the link still saves; a few kilobytes behind an MZ magic
        // gets far enough into PE parsing to fail the way the real payload did.
        std::string junk = "MZ";
        while (junk.size() < 7200)
        {
            junk += "payload ";
        }
        REQUIRE(write_whole_file(
                    fake, std::span<const uint8_t>(
                              reinterpret_cast<const uint8_t*>(junk.data()), junk.size()))
                    .is_ok());
    }

    IShellLinkW* link = nullptr;
    REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_IShellLinkW, reinterpret_cast<void**>(&link))));
    CHECK(SUCCEEDED(link->SetPath(fake.c_str())));

    IPersistFile* file = nullptr;
    REQUIRE(SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))));

    const std::wstring link_path = temp_dir() + L"lwi-test-malformed.lnk";
    DeleteFileW(link_path.c_str());
    const HRESULT hr = file->Save(link_path.c_str(), TRUE);

    file->Release();
    link->Release();

    INFO("Save returned 0x" << std::hex << static_cast<unsigned long>(hr));
    CHECK(FAILED(hr));

    DeleteFileW(link_path.c_str());
    DeleteFileW(fake.c_str());
}

TEST_CASE("a shell link survives a target that does not exist")
{
    // An installer creates shortcuts before the user has ever run the product,
    // and on a repair the target may be missing entirely. Neither should stop
    // the link being written.
    ComScope com;
    REQUIRE(SUCCEEDED(com.hr));

    IShellLinkW* link = nullptr;
    REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_IShellLinkW, reinterpret_cast<void**>(&link))));

    CHECK(SUCCEEDED(link->SetPath(L"C:\\this\\path\\does\\not\\exist\\app.exe")));

    IPersistFile* file = nullptr;
    REQUIRE(SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))));

    const std::wstring link_path = temp_dir() + L"lwi-test-missing.lnk";
    DeleteFileW(link_path.c_str());

    const HRESULT hr = file->Save(link_path.c_str(), TRUE);
    INFO("Save returned 0x" << std::hex << static_cast<unsigned long>(hr));
    CHECK(SUCCEEDED(hr));

    file->Release();
    link->Release();
    DeleteFileW(link_path.c_str());
}

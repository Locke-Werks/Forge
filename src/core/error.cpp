#include "lwi/error.h"

#include <windows.h>

namespace lwi
{
namespace
{

std::string system_text(unsigned long code)
{
    LPSTR buffer = nullptr;
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                   reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    if (n == 0 || buffer == nullptr)
    {
        return {};
    }
    std::string text(buffer, n);
    LocalFree(buffer);

    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' '))
    {
        text.pop_back();
    }
    return text;
}

std::string hex32(unsigned long v)
{
    static const char* digits = "0123456789abcdef";
    std::string out = "0x";
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        out.push_back(digits[(v >> shift) & 0xF]);
    }
    return out;
}

} // namespace

std::string win32_message(std::string_view what, unsigned long last_error)
{
    std::string out(what);
    out += " failed: ";
    const std::string text = system_text(last_error);
    if (!text.empty())
    {
        out += text;
        out += " ";
    }
    out += "(";
    out += hex32(last_error);
    out += ")";
    return out;
}

std::string hresult_message(std::string_view what, long hr)
{
    return win32_message(what, static_cast<unsigned long>(hr));
}

} // namespace lwi

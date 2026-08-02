#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace lwi
{

enum class Code : uint32_t
{
    Ok = 0,

    // Caller error.
    InvalidArgument,
    OutOfRange,
    Unsupported,

    // Image parsing. Distinguished from generic parse errors because the stub
    // reports them differently: a malformed PE means the file was truncated in
    // transit, a malformed container means it was tampered with.
    NotPeImage,
    MalformedPe,
    NoSignature,

    // Container.
    NoContainer,
    MalformedContainer,
    UnsupportedVersion,
    HashMismatch,

    // Platform.
    IoError,
    CryptoError,
    CompressionError,
};

/// Return type for anything that can fail. Carries a message because a bare
/// code forces every call site to reconstruct the context that produced it,
/// which is exactly the information a support ticket needs.
class Status
{
  public:
    Status() = default;

    static Status ok() { return Status{}; }
    static Status error(Code code, std::string message)
    {
        Status s;
        s.code_ = code;
        s.message_ = std::move(message);
        return s;
    }

    [[nodiscard]] bool is_ok() const { return code_ == Code::Ok; }
    [[nodiscard]] explicit operator bool() const { return is_ok(); }

    [[nodiscard]] Code code() const { return code_; }
    [[nodiscard]] const std::string& message() const { return message_; }

  private:
    Code code_ = Code::Ok;
    std::string message_;
};

/// Formats a Win32 error the way a user can act on: the symbolic value plus the
/// system's own text. GetLastError alone reaches a support inbox as a bare
/// number nobody can look up without the context of which call produced it.
std::string win32_message(std::string_view what, unsigned long last_error);

/// As above for an HRESULT.
std::string hresult_message(std::string_view what, long hr);

} // namespace lwi

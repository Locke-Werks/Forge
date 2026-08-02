#include "lwi/compress.h"

#include <windows.h>

#include <compressapi.h>

namespace lwi
{
namespace
{

/// A ceiling on what we will allocate for a single decompressed buffer. The
/// container index is signed, so a value larger than this means the packaging
/// step produced something absurd, not that an attacker is probing us. Either
/// way, refusing beats a multi-gigabyte allocation inside an elevated process.
constexpr uint64_t kMaxBuffer = 2ull * 1024 * 1024 * 1024;

bool to_win_algo(CompressAlgo algo, DWORD& out)
{
    switch (algo)
    {
    case CompressAlgo::Lzms:
        out = COMPRESS_ALGORITHM_LZMS;
        return true;
    case CompressAlgo::XpressHuff:
        out = COMPRESS_ALGORITHM_XPRESS_HUFF;
        return true;
    case CompressAlgo::None:
    default:
        return false;
    }
}

class Compressor
{
  public:
    ~Compressor()
    {
        if (h_ != nullptr)
        {
            CloseCompressor(h_);
        }
    }
    bool create(DWORD algo) { return CreateCompressor(algo, nullptr, &h_) != FALSE; }
    COMPRESSOR_HANDLE get() const { return h_; }

  private:
    COMPRESSOR_HANDLE h_ = nullptr;
};

class Decompressor
{
  public:
    ~Decompressor()
    {
        if (h_ != nullptr)
        {
            CloseDecompressor(h_);
        }
    }
    bool create(DWORD algo) { return CreateDecompressor(algo, nullptr, &h_) != FALSE; }
    DECOMPRESSOR_HANDLE get() const { return h_; }

  private:
    DECOMPRESSOR_HANDLE h_ = nullptr;
};

} // namespace

const char* algo_name(CompressAlgo algo)
{
    switch (algo)
    {
    case CompressAlgo::None:
        return "none";
    case CompressAlgo::Lzms:
        return "lzms";
    case CompressAlgo::XpressHuff:
        return "xpress-huff";
    }
    return "unknown";
}

Status compress_buffer(CompressAlgo algo, std::span<const uint8_t> input,
                       std::vector<uint8_t>& output)
{
    if (algo == CompressAlgo::None)
    {
        output.assign(input.begin(), input.end());
        return Status::ok();
    }

    DWORD win_algo = 0;
    if (!to_win_algo(algo, win_algo))
    {
        return Status::error(Code::Unsupported, "unknown compression algorithm");
    }

    Compressor c;
    if (!c.create(win_algo))
    {
        return Status::error(Code::CompressionError,
                             win32_message("CreateCompressor", GetLastError()));
    }

    // Two-call sizing: passing a zero-length output buffer fails with
    // ERROR_INSUFFICIENT_BUFFER and reports the size that is guaranteed to
    // succeed. Anything smaller is a guess.
    SIZE_T needed = 0;
    if (!Compress(c.get(), input.data(), input.size(), nullptr, 0, &needed))
    {
        const DWORD err = GetLastError();
        if (err != ERROR_INSUFFICIENT_BUFFER)
        {
            return Status::error(Code::CompressionError, win32_message("Compress (sizing)", err));
        }
    }

    output.assign(needed, 0);

    SIZE_T produced = 0;
    if (!Compress(c.get(), input.data(), input.size(), output.data(), output.size(), &produced))
    {
        return Status::error(Code::CompressionError, win32_message("Compress", GetLastError()));
    }

    output.resize(produced);
    return Status::ok();
}

Status decompress_buffer(CompressAlgo algo, std::span<const uint8_t> input,
                         uint64_t expected_raw_size, std::vector<uint8_t>& output)
{
    if (expected_raw_size > kMaxBuffer)
    {
        return Status::error(Code::OutOfRange, "decompressed size exceeds the allowed maximum");
    }

    if (algo == CompressAlgo::None)
    {
        if (input.size() != expected_raw_size)
        {
            return Status::error(Code::MalformedContainer,
                                 "stored entry size does not match its recorded size");
        }
        output.assign(input.begin(), input.end());
        return Status::ok();
    }

    DWORD win_algo = 0;
    if (!to_win_algo(algo, win_algo))
    {
        return Status::error(Code::Unsupported, "unknown compression algorithm");
    }

    Decompressor d;
    if (!d.create(win_algo))
    {
        return Status::error(Code::CompressionError,
                             win32_message("CreateDecompressor", GetLastError()));
    }

    // Sized from the signed index, never from the length inside the compressed
    // buffer. If the data claims to expand to something else, Decompress fails
    // here instead of us honouring an attacker-chosen allocation.
    output.assign(static_cast<size_t>(expected_raw_size), 0);

    SIZE_T produced = 0;
    if (!Decompress(d.get(), input.data(), input.size(), output.data(), output.size(), &produced))
    {
        return Status::error(Code::CompressionError, win32_message("Decompress", GetLastError()));
    }

    if (produced != expected_raw_size)
    {
        return Status::error(Code::MalformedContainer,
                             "decompressed size does not match the size recorded in the index");
    }

    return Status::ok();
}

} // namespace lwi

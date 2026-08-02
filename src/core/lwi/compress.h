#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "lwi/error.h"

namespace lwi
{

/// Algorithm ids as stored in the container header. These are our own values,
/// deliberately not the COMPRESS_ALGORITHM_* constants, so the on-disk format
/// does not inherit whatever Microsoft does to that enum.
enum class CompressAlgo : uint32_t
{
    None = 0,
    Lzms = 1,
    XpressHuff = 2,
};

/// Compresses a buffer with the in-box Windows Compression API (cabinet.dll).
///
/// Buffer mode, not COMPRESS_RAW. Microsoft's own Decompress remarks say "It is
/// recommended that compressors and decompressors not use the COMPRESS_RAW
/// flag", and buffer mode stores the uncompressed length in the output so a
/// decompressor cannot be handed a length that disagrees with the data. The
/// cost is that we do not get solid LZMS blocks, so the ratio is worse than
/// wimlib-style solid compression. That trade is deliberate for v1: correctness
/// and a smaller parse surface over a few percent of payload size.
Status compress_buffer(CompressAlgo algo, std::span<const uint8_t> input,
                       std::vector<uint8_t>& output);

/// Decompresses a buffer produced by compress_buffer.
///
/// expected_raw_size is the size recorded in the container index, which lives
/// inside the Authenticode-covered region and is therefore trusted. The output
/// buffer is sized from it, never from the length embedded in the compressed
/// data: Microsoft documents that embedded length as untrusted and says it
/// "should be treated as untrusted and tested against reasonable limits". A
/// mismatch fails rather than allocating whatever the buffer asks for.
Status decompress_buffer(CompressAlgo algo, std::span<const uint8_t> input,
                         uint64_t expected_raw_size, std::vector<uint8_t>& output);

const char* algo_name(CompressAlgo algo);

} // namespace lwi

# Container format

The payload and configuration live in a blob appended to the installer
executable, after the last section and before the Authenticode certificate
table.

## Why appended, and why signed afterwards

Data appended past the last section but before the certificate table is covered
by the Authenticode digest. The signing whitepaper's step 14 defines the extra
data region as beginning at `SUM_OF_BYTES_HASHED` and running for

```
file_size - (cert_table_size + SUM_OF_BYTES_HASHED)
```

This was verified rather than assumed: the Authenticode SHA-256 of a real signed
217 MB binary was recomputed two ways, and only the digest that included the
trailing region appeared in the embedded PKCS#7.

The consequence is an ordering rule. Everything that rewrites the image happens
before signing:

1. Copy the stub, strip any existing signature.
2. `UpdateResource` for the icon, version resource and manifest.
3. Append the container.
4. Pad the file to an 8-byte multiple.
5. Sign.

Signing first and appending second would leave the payload outside the digest.
That is the CVE-2013-3900 shape, and it is what this design must not do.

Step 2 must precede step 3 for a second reason: `EndUpdateResource` rewrites the
whole image and can move section raw offsets, so any layout computed before it
is stale. `lwforge` re-reads and re-parses the file after stamping.

## Layout

```
[ PE headers and sections                        ]
[ LWIC container                                 ]  <- footer.container_offset
[ 0 to 7 padding bytes to 8-byte alignment       ]
[ Footer, 72 bytes                               ]
[ Attribute certificate table (added by signtool)]
```

Padding to alignment is deliberate. `signtool` aligns the start of the
certificate table to 8 bytes and inserts up to 7 zero bytes if the file does not
already end there. Padding ourselves means it inserts none. The locator still
scans backwards defensively, because depending on a tool's current behaviour is
how this breaks quietly on a future SDK.

### Footer

72 bytes, a multiple of 8 so an aligned container leaves the whole file aligned.

| Field | Type | Notes |
|---|---|---|
| `magic` | `uint64` | `LWIFOOT\0` |
| `footer_version` | `uint32` | 1 |
| `flags` | `uint32` | bit 0: unsigned development build |
| `container_offset` | `uint64` | absolute file offset |
| `container_size` | `uint64` | |
| `container_sha256` | `uint8[32]` | over the container, excluding padding and footer |
| `footer_crc32` | `uint32` | over every preceding byte of the footer |
| `reserved` | `uint32` | |

### Container

A 128-byte header followed by four regions, each compressed independently:
config, strings, data, index.

The index is written **last**, after the file data, because every index entry
carries the container-relative offset of its data. Writing it earlier means
either reserving a slot and patching it, or compressing it twice, and the second
compression can come out larger than the first: filling in real offsets replaces
a run of zeroes with high-entropy values, and the reserved slot overflows. This
is not hypothetical; it was the first bug the test suite caught.

## Locating the container at runtime

```
GetModuleFileNameW(NULL) -> read the image
e_lfanew at 0x3C, OptionalHeader at e_lfanew + 0x18
DataDirectory at OptionalHeader + 112 (PE32+) or + 96 (PE32)
guard NumberOfRvaAndSizes > 4
sec = DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY]
boundary = sec.Size ? sec.VirtualAddress : file_size
for back in 0..8: try footer at boundary - back - 72, accept on magic + CRC
verify container_sha256 before parsing any header or index field
```

`sec.VirtualAddress` is a **file offset**, not an RVA. PE/COFF states this
verbatim: "These certificates are not loaded into memory as part of the image.
As such, the first field of this entry, which is normally an RVA, is a file
pointer instead." Reading it as an RVA is the usual bug in self-extracting code.

The whole-container hash is checked before any structural field is believed,
because the index is attacker-reachable data being parsed inside an elevated
process. Per-file digests are then checked after decompression and before the
bytes become a file: a successful `Decompress` return is not integrity.

## Limits

The finished installer must stay below 4 GB. Signing a PE at or above that size
fails with `0x80080057`, and Microsoft documents that the hash may be wrong
above it even when signing appears to succeed. `lwforge` refuses earlier, with a
clearer message.

Above roughly 500 MB, signing has to hash the whole file and gets slow. Inno
Setup's own documentation steers large signed setups toward external payload
slices rather than one enormous PE. If a product ever needs that, the footer's
`flags` field is where a sidecar mode would be signalled.

## Compression

`COMPRESS_ALGORITHM_LZMS` through the in-box Compression API, in buffer mode
rather than `COMPRESS_RAW`. Microsoft's own `Decompress` remarks say "It is
recommended that compressors and decompressors not use the `COMPRESS_RAW` flag",
and buffer mode stores the uncompressed length so a decompressor cannot be
handed a length that disagrees with the data.

The cost is no solid blocks, so the ratio is worse than a wimlib-style solid
LZMS stream. That trade is deliberate for v1: a smaller parse surface inside an
elevated binary is worth more than a few percent of payload size. If it ever
stops being worth it, block mode with `COMPRESS_RAW` and a block size at or
below 64 MiB is the upgrade path. Above 64 MiB, Microsoft's LZMS is documented
as incompatible, so that ceiling is a format constraint and not just an API one.

Decompression sizes its output buffer from the size recorded in the signed
index, never from the length embedded in the compressed data, which Microsoft
documents as untrusted.

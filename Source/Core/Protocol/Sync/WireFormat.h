// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The little-endian scalar layer under every byte-serialized payload (Sync frame
// bodies, workspace captures). The Reader is a bounded cursor that latches !ok on
// the first out-of-range read and returns zeros thereafter, so a malformed buffer
// surfaces as one failed flag, never an out-of-bounds access.
//
// Strings are not here: Sync speaks u16 length prefixes and the
// workspace capture u32; each module owns its dialect next to what it persists.

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

inline void putU8(std::vector<std::uint8_t>& b, std::uint8_t v) { b.push_back(v); }

inline void putU16(std::vector<std::uint8_t>& b, std::uint16_t v)
{
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

inline void putU32(std::vector<std::uint8_t>& b, std::uint32_t v)
{
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

inline void putI32(std::vector<std::uint8_t>& b, std::int32_t v) { putU32(b, static_cast<std::uint32_t>(v)); }

inline void putU64(std::vector<std::uint8_t>& b, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
}

inline void putF32(std::vector<std::uint8_t>& b, float v)
{
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits)); // IEEE-754 bit pattern; both ends agree
    putU32(b, bits);
}

inline void putF64(std::vector<std::uint8_t>& b, double v)
{
    std::uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits)); // IEEE-754 bit pattern; both ends agree
    putU64(b, bits);
}

inline void putBytes(std::vector<std::uint8_t>& b, const void* p, std::size_t n)
{
    const std::uint8_t* s = static_cast<const std::uint8_t*>(p);
    b.insert(b.end(), s, s + n);
}

// Bounded little-endian reader over a payload span; every read is length-checked.
// Callers guarding recursive work (not just memory) check `ok` between reads.
struct Reader {
    const std::uint8_t* p;
    std::size_t length;
    std::size_t off = 0;
    bool ok = true;

    // The pointer and the length arrive as one value, so a caller cannot hand
    // over a length that disagrees with the buffer it describes. They are stored
    // apart because every read below indexes `p` against `length` directly, and
    // this is the per-byte decode path.
    explicit Reader(std::span<const std::uint8_t> bytes)
        : p(bytes.data())
        , length(bytes.size())
    {
    }

    [[nodiscard]] std::size_t remaining() const { return ok ? length - off : 0; }

    [[gnu::always_inline]] inline bool need(std::size_t n)
    {
        if (!ok || n > length - off) {
            ok = false;
            return false;
        }
        return true;
    }

    [[gnu::always_inline]] inline std::uint8_t u8()
    {
        if (!need(1))
            return 0;
        return p[off++];
    }
    [[gnu::always_inline]] inline std::uint16_t u16()
    {
        if (!need(2))
            return 0;
        std::uint16_t v = static_cast<std::uint16_t>(p[off]) | static_cast<std::uint16_t>(p[off + 1] << 8);
        off += 2;
        return v;
    }
    [[gnu::always_inline]] inline std::uint32_t u32()
    {
        if (!need(4))
            return 0;
        std::uint32_t v = static_cast<std::uint32_t>(p[off]) | (static_cast<std::uint32_t>(p[off + 1]) << 8) | (static_cast<std::uint32_t>(p[off + 2]) << 16) | (static_cast<std::uint32_t>(p[off + 3]) << 24);
        off += 4;
        return v;
    }
    [[gnu::always_inline]] inline std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32()
    {
        std::uint32_t bits = u32();
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    [[gnu::always_inline]] inline std::uint64_t u64()
    {
        if (!need(8))
            return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(p[off + i]) << (i * 8);
        off += 8;
        return v;
    }
    double f64()
    {
        std::uint64_t bits = u64();
        double v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    // Returns a bounded view of `n` bytes and advances, or an empty span
    // (latching !ok) if short.
    std::span<const std::uint8_t> bytes(std::size_t n)
    {
        if (!need(n))
            return {};
        const std::span<const std::uint8_t> view(p + off, n);
        off += n;
        return view;
    }
};

} // namespace Ubiquitty::Protocol::Sync

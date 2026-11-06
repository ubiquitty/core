// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The socket leaf the daemon a ubiquitty-host binary starts binds.
//
// A header of its own, and inline, because the host's own launcher and every
// test that drives a real daemon compute it with no Core library linked, while
// the frontends that connect to that daemon reach it through
// Session::localDaemonSocketPath (SessionShare.h). One definition is what keeps
// the two sides naming the same socket.

#include "Core/Crypto/SHA256.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>

namespace Ubiquitty::Session {

/// The socket leaf the daemon a ubiquitty-host binary starts binds.
///
/// `host-<16 hex>.sock`, the first eight bytes of the SHA-256 of the binary's
/// contents: the same bytes find the same daemon, and an updated binary starts
/// one of its own beside the old, which keeps its sessions and exits when its
/// last one ends.
///
/// - Parameter binaryPath: The binary, NUL-terminated.
/// - Returns: The leaf, or empty when the binary cannot be read — never a
///   shared well-known name, which any binary that failed to hash would bind.
inline std::string socketLeafForBinary(const char* binaryPath)
{
    if (!binaryPath || binaryPath[0] == '\0')
        return std::string();
    std::FILE* file = std::fopen(binaryPath, "rb");
    if (!file)
        return std::string();

    // The contents, not the size and modification time: two builds mirrored
    // between machines can agree on both and still differ in every byte.
    // Streamed through the hasher a chunk at a time: every launcher, attach
    // and token command computes this, and a debug host is hundreds of
    // megabytes that holding whole would cost each of them.
    Ubiquitty::Crypto::SHA256Hasher hasher;
    std::array<unsigned char, 65536> chunk {};
    std::size_t got = 0;
    std::size_t total = 0;
    bool isHashed = true;
    while (isHashed && (got = std::fread(chunk.data(), 1, chunk.size(), file)) > 0) {
        isHashed = hasher.update(std::span<const unsigned char>(chunk.data(), got)).has_value();
        total += got;
    }
    const bool hasReadError = std::ferror(file) != 0;
    std::fclose(file);
    std::array<unsigned char, 32> digest {};
    if (!isHashed || hasReadError || total == 0 || !hasher.finalize(digest))
        return std::string();

    static constexpr char hexDigits[] = "0123456789abcdef";
    std::string leaf = "host-";
    for (std::size_t index = 0; index < 8; ++index) {
        leaf.push_back(hexDigits[(digest[index] >> 4) & 0x0F]);
        leaf.push_back(hexDigits[digest[index] & 0x0F]);
    }
    leaf += ".sock";
    return leaf;
}

} // namespace Ubiquitty::Session

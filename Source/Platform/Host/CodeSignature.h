// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Reading a code signature: the platform half of the signing policy
// (`SignaturePolicy.h`). macOS reads it with the Security framework from the
// kernel's account of the running code; a platform that signs no code
// answers `Signer::Unsupported` for every process, and its host trusts by the
// file alone. Defined beside the handover's other code checks
// (HandoverAdmission.cpp).

#include "Platform/Host/SignaturePolicy.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace Ubiquitty::Host {

/// The policy this build was configured with: `SigningPolicy::Release` where
/// UBIQUITTY_RELEASE_SIGNING is on, as a release's host is built.
///
/// - Returns: The policy.
[[nodiscard]] SigningPolicy buildSigningPolicy();

/// This process's own signature.
///
/// - Returns: The signature; `Signer::None` when it cannot be read.
[[nodiscard]] CodeSignature signatureOfSelf();

/// The signature of the code the process at the other end of a local stream
/// runs, validated against that running code.
///
/// The process is read by the socket's own account of it, so a process that
/// exits and whose number is reused is not the one judged. The check reads
/// what the signature covers, so a caller judges each connection once, and a
/// host remembers each binary's verdict (`SignatureMemo`).
///
/// - Parameter channel: A connected local stream.
/// - Returns: Its peer's signature; `Signer::None` when it cannot be read.
[[nodiscard]] CodeSignature signatureOfPeer(int channel);

/// The file a verdict is remembered for: the one a peer runs, with its size
/// and its times, which a write in place moves.
struct SignatureKey {
    dev_t device = 0;
    ino_t inode = 0;
    off_t size = 0;
    std::int64_t modifiedSeconds = 0;
    std::int64_t modifiedNanoseconds = 0;
    std::int64_t changedSeconds = 0;
    std::int64_t changedNanoseconds = 0;

    friend bool operator==(const SignatureKey&, const SignatureKey&) = default;
};

/// The key of the file at `path`.
///
/// - Parameter path: An absolute path, as the kernel named a peer's binary.
/// - Returns: The key, or nothing when no regular file is there.
[[nodiscard]] inline std::optional<SignatureKey> signatureKeyOf(const std::string& path)
{
    struct stat info {};
    if (path.empty() || ::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return std::nullopt;
#if defined(__APPLE__)
    const auto& modified = info.st_mtimespec;
    const auto& changed = info.st_ctimespec;
#else
    const auto& modified = info.st_mtim;
    const auto& changed = info.st_ctim;
#endif
    return SignatureKey {
        .device = info.st_dev,
        .inode = info.st_ino,
        .size = info.st_size,
        .modifiedSeconds = static_cast<std::int64_t>(modified.tv_sec),
        .modifiedNanoseconds = static_cast<std::int64_t>(modified.tv_nsec),
        .changedSeconds = static_cast<std::int64_t>(changed.tv_sec),
        .changedNanoseconds = static_cast<std::int64_t>(changed.tv_nsec),
    };
}

/// The signatures of the binaries a host's local peers ran, each read once.
///
/// Only a definite verdict is kept: a signature that could not be read may
/// read at the next connection. The oldest verdict gives way past the bound.
class SignatureMemo {
public:
    /// The most binaries remembered.
    static constexpr std::size_t maxEntries = 16;

    /// The verdict remembered for a file.
    ///
    /// - Parameter key: The file.
    /// - Returns: The verdict, or nothing when none is kept for it.
    [[nodiscard]] const CodeSignature* find(const SignatureKey& key) const
    {
        for (const auto& [held, signature] : m_entries) {
            if (held == key)
                return &signature;
        }
        return nullptr;
    }

    /// Remembers the verdict for a file, replacing any it held.
    ///
    /// - Parameters:
    ///   - key: The file.
    ///   - signature: What its signature said.
    void remember(const SignatureKey& key, CodeSignature signature)
    {
        if (signature.signer == Signer::None || signature.signer == Signer::Unsupported)
            return;
        std::erase_if(m_entries, [&key](const auto& entry) { return entry.first == key; });
        if (m_entries.size() >= maxEntries)
            m_entries.erase(m_entries.begin());
        m_entries.emplace_back(key, std::move(signature));
    }

    /// How many verdicts are kept.
    [[nodiscard]] std::size_t size() const { return m_entries.size(); }

private:
    std::vector<std::pair<SignatureKey, CodeSignature>> m_entries;
};

} // namespace Ubiquitty::Host

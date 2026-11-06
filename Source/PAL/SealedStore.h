// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>

namespace Ubiquitty::PAL {

/// The authenticated domain of one private stored record. The principal is a
/// digest of the already verified identity, never an untrusted display label.
/// A record ID distinguishes windows; purposes separate resume from history.
struct SealScope {
    std::array<std::uint8_t, 16> hostInstance {};
    std::array<std::uint8_t, 32> principal {};
    std::array<std::uint8_t, 16> recordID {};
    /// Separates resume and history keys even for the same window identity.
    enum class Purpose : std::uint8_t {
        Resume = 1,
        History = 2
    };
    Purpose purpose = Purpose::Resume;

    /// Stable bytes used for key lookup and authenticated associated data.
    ///
    /// - Returns: All scope fields in their fixed, padding-free order.
    [[nodiscard]] std::array<std::uint8_t, 65> bytes() const
    {
        std::array<std::uint8_t, 65> result {};
        for (std::size_t index = 0; index < hostInstance.size(); ++index)
            result[index] = hostInstance[index];
        for (std::size_t index = 0; index < principal.size(); ++index)
            result[16 + index] = principal[index];
        for (std::size_t index = 0; index < recordID.size(); ++index)
            result[48 + index] = recordID[index];
        result.back() = static_cast<std::uint8_t>(purpose);
        return result;
    }
};

/// Why encryption or authenticated opening produced no output.
enum class SealError : std::uint8_t {
    Unavailable,
    TooLarge,
    AllocationFailed,
    Refused
};

/// A bounded owner returned only after a whole sealing operation succeeds.
struct SealedStoreBytes {
    std::unique_ptr<std::uint8_t[]> bytes;
    std::size_t size = 0;
    /// Borrows the complete successful output.
    ///
    /// - Returns: Bytes valid while this owner remains alive and unchanged.
    [[nodiscard]] std::span<const std::uint8_t> view() const { return { bytes.get(), size }; }
};

/// Platform cryptography with no plaintext fallback or process-global install.
/// The owner injects the platform's stateless C driver; a default store refuses.
/// Seal and open callbacks must write only after successful authentication and
/// must return the exact output length, or -1. Callbacks are synchronous.
class SealedStore {
public:
    /// Version byte, 12-byte random nonce, and 16-byte authentication tag.
    static constexpr std::size_t overhead = 29;

    /// Stateless synchronous platform entry points, with no fallback entries.
    struct Driver {
        std::ptrdiff_t (*seal)(const std::uint8_t* scope, std::size_t scopeSize, const std::uint8_t* input, std::size_t inputSize, std::uint8_t* output, std::size_t outputCapacity) = nullptr;
        std::ptrdiff_t (*open)(const std::uint8_t* scope, std::size_t scopeSize, const std::uint8_t* input, std::size_t inputSize, std::uint8_t* output, std::size_t outputCapacity) = nullptr;
        bool (*forget)(const std::uint8_t* scope, std::size_t scopeSize) = nullptr;
    };

    /// Creates an unavailable store that refuses every operation.
    SealedStore() = default;

    /// Uses the explicitly supplied platform cryptography.
    ///
    /// - Parameter driver: Callbacks whose code outlives this store.
    explicit SealedStore(Driver driver)
        : m_driver(driver)
    {
    }

    /// Seals within the caller's plaintext cap; no driver means no output.
    ///
    /// - Parameters:
    ///   - scope: The exact authenticated identity and storage purpose.
    ///   - plaintext: Bytes to encrypt, borrowed for this call.
    ///   - maxPlaintextBytes: The owner's plaintext budget.
    /// - Returns: Owned ciphertext, or a refusal without partial output.
    [[nodiscard]] std::expected<SealedStoreBytes, SealError> seal(const SealScope& scope, std::span<const std::uint8_t> plaintext, std::size_t maxPlaintextBytes) const
    {
        if (!m_driver.seal)
            return std::unexpected(SealError::Unavailable);
        if (plaintext.size() > maxPlaintextBytes || plaintext.size() > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) - overhead)
            return std::unexpected(SealError::TooLarge);
        return invoke(m_driver.seal, scope, plaintext, plaintext.size() + overhead);
    }

    /// Authenticates before returning plaintext. Truncation and excess size
    /// are refused before allocation or a platform callback.
    ///
    /// - Parameters:
    ///   - scope: The expected authenticated identity and storage purpose.
    ///   - ciphertext: The complete encrypted envelope.
    ///   - maxPlaintextBytes: The owner's plaintext budget.
    /// - Returns: Owned authenticated plaintext, or a refusal.
    [[nodiscard]] std::expected<SealedStoreBytes, SealError> open(const SealScope& scope, std::span<const std::uint8_t> ciphertext, std::size_t maxPlaintextBytes) const
    {
        if (!m_driver.open)
            return std::unexpected(SealError::Unavailable);
        if (ciphertext.size() < overhead)
            return std::unexpected(SealError::Refused);
        const auto size = ciphertext.size() - overhead;
        if (size > maxPlaintextBytes || size > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
            return std::unexpected(SealError::TooLarge);
        return invoke(m_driver.open, scope, ciphertext, size);
    }

    /// Forgets only this exact record's key. Failure must not be reported as
    /// cryptographic erasure; a caller still removes its encrypted file.
    ///
    /// - Parameter scope: The exact key to retire.
    /// - Returns: Whether the platform confirms the key is absent.
    [[nodiscard]] bool forget(const SealScope& scope) const
    {
        const auto encoded = scope.bytes();
        return m_driver.forget && m_driver.forget(encoded.data(), encoded.size());
    }

private:
    /// Allocates one exact output and accepts only a whole driver result.
    ///
    /// - Parameters:
    ///   - callback: The checked platform entry point.
    ///   - scope: Authenticated identity bytes.
    ///   - input: The borrowed operation input.
    ///   - size: The exact expected output length.
    /// - Returns: A complete owner, or a refusal.
    template<class Callback>
    static std::expected<SealedStoreBytes, SealError> invoke(Callback callback, const SealScope& scope, std::span<const std::uint8_t> input, std::size_t size)
    {
        SealedStoreBytes output { std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[size ? size : 1]), size };
        if (!output.bytes)
            return std::unexpected(SealError::AllocationFailed);
        const auto encoded = scope.bytes();
        const auto written = callback(encoded.data(), encoded.size(), input.data(), input.size(), output.bytes.get(), size);
        if (written < 0 || static_cast<std::size_t>(written) != size)
            return std::unexpected(SealError::Refused);
        return output;
    }

    Driver m_driver {};
};

} // namespace Ubiquitty::PAL

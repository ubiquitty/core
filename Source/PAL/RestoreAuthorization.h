// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/SealedStore.h"

namespace Ubiquitty::PAL {

/// Protected authorization is independent of a record's encryption key. A
/// blocked record survives key-retirement failure and process replacement.
class RestoreAuthorization {
public:
    /// Missing is not permission to read an existing ciphertext file.
    enum class State : std::uint8_t {
        Unavailable = 0,
        Missing = 1,
        Blocked = 2,
        Active = 3
    };
    /// The exact authenticated plaintext authorized by the last durable commit.
    struct Record {
        State state = State::Unavailable;
        std::array<std::uint8_t, 32> digest {};
    };
    /// Synchronous, non-reentrant protected-storage operations. Successful
    /// writes must survive process replacement. The caller serializes a scope.
    struct Driver {
        std::uint8_t (*read)(const std::uint8_t*, std::size_t, std::uint8_t*, std::size_t) = nullptr;
        bool (*write)(const std::uint8_t*, std::size_t, std::uint8_t, const std::uint8_t*, std::size_t) = nullptr;
    };

    /// Creates an unavailable owner; no unprotected fallback exists.
    RestoreAuthorization() = default;
    /// Captures callbacks whose code outlives this owner.
    ///
    /// - Parameter driver: Protected storage callbacks with no fallback.
    explicit RestoreAuthorization(Driver driver)
        : m_driver(driver)
    {
    }

    /// Reads an exact scope without creating it.
    ///
    /// - Parameter scope: Independently verified identity, not file contents.
    /// - Returns: Protected state, or Unavailable on malformed/refused storage.
    [[nodiscard]] Record read(const SealScope& scope) const
    {
        Record record;
        if (!m_driver.read)
            return record;
        const auto bytes = scope.bytes();
        const auto state = m_driver.read(bytes.data(), bytes.size(), record.digest.data(), record.digest.size());
        if (state > static_cast<std::uint8_t>(State::Active))
            return {};
        record.state = static_cast<State>(state);
        return record;
    }

    /// Records deny intent before changing ciphertext or retiring its key.
    ///
    /// - Parameter scope: The exact independently verified record scope.
    /// - Returns: Whether protected storage confirmed the blocked state.
    [[nodiscard]] bool block(const SealScope& scope) const { return write(scope, State::Blocked, {}); }

    /// Authorizes only the plaintext whose ciphertext is already durable.
    ///
    /// - Parameters:
    ///   - scope: The exact scope serialized by this caller.
    ///   - digest: SHA-256 of the complete authenticated plaintext.
    /// - Returns: Whether protected storage confirmed this exact authorization.
    [[nodiscard]] bool activate(const SealScope& scope, const std::array<std::uint8_t, 32>& digest) const { return write(scope, State::Active, digest); }

private:
    /// Persists one fixed authorization value under the caller's scope lock.
    ///
    /// - Parameters:
    ///   - scope: The exact independently verified identity.
    ///   - active: Whether the digest identifies an already durable record.
    ///   - digest: The complete plaintext digest, zero for blocked state.
    /// - Returns: Whether protected storage confirms the entire write.
    [[nodiscard]] bool write(const SealScope& scope, State active, const std::array<std::uint8_t, 32>& digest) const
    {
        if (!m_driver.write)
            return false;
        const auto bytes = scope.bytes();
        return m_driver.write(bytes.data(), bytes.size(), static_cast<std::uint8_t>(active), digest.data(), digest.size());
    }
    Driver m_driver {};
};

} // namespace Ubiquitty::PAL

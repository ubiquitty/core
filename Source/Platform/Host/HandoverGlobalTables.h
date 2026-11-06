// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Preferences/HandoverValues.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Session/BackgroundEvents.h"
#include "Core/Session/EventLog.h"

namespace Ubiquitty::Host {

/// Host-global state that remains observable after all pane owners are carried.
/// Listener/token/PID paths and current executable trust are separate admission
/// bindings, never installed as authority from these bytes.
struct HandoverGlobalTables {
    Preferences::Values configuration;
    std::string shellPath;
    std::string paneHostName;
    std::string paneHomeDirectory;
    int columns = 80;
    int rows = 24;
    Ubiquitty::Session::EventLog::Continuation events;
    Ubiquitty::Session::BackgroundEvents::Continuation backgroundEvents;
    std::uint64_t backgroundInventoryRevision = 1;
    std::string backgroundEventPath;
    std::uint64_t backgroundEventWriteDue = 0;
    std::uint64_t writtenBackgroundEventRevision = 0;
    bool isBackgroundEventStorageAvailable = true;
    std::uint64_t eventDeliveries = 0;
    std::uint64_t nextScrollbackFoldMilliseconds = 0;
    std::uint32_t lastScrollbackFoldSessionID = 0;
    std::vector<std::uint32_t> closuresOwed;
    std::vector<std::pair<std::uint32_t, Protocol::Sync::ID128>> terminationsOwed;
    bool hasHadSession = false;
};

/// Receiver-selected bounds for global state and its existing configuration codec.
struct HandoverGlobalLimits {
    std::size_t maxSectionBytes = 0;
    std::size_t maxEvents = 0;
    std::size_t maxClosures = 0;
    std::size_t maxTerminations = 0;
    std::size_t maxStringBytes = 0;
    Preferences::HandoverValuesLimits configuration;
};

/// Encodes exact global state without running defaults, pruning or owed actions.
/// - Parameters:
///   - state: The frozen predecessor's global owners.
///   - limits: Independent successor bounds.
/// - Returns: A complete section or refusal without changing source state.
[[nodiscard]] std::expected<Protocol::Sync::HandoverBytes, Protocol::Sync::HandoverBodyError> encodeHandoverGlobals(
    const HandoverGlobalTables& state, const HandoverGlobalLimits& limits);

/// Reconstructs private global owners without posting events or executing closures.
/// The outer transaction must verify path bindings against the admitted host root,
/// and all owed session/workspace identities against its other candidate tables.
/// - Parameters:
///   - bytes: Exactly one authenticated predecessor section.
///   - limits: Independent resource ceilings, including the configuration decoder.
/// - Returns: Owned state or refusal without live mutation.
[[nodiscard]] std::expected<HandoverGlobalTables, Protocol::Sync::HandoverBodyError> decodeHandoverGlobals(
    std::span<const std::uint8_t> bytes, const HandoverGlobalLimits& limits);

} // namespace Ubiquitty::Host

// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Assertions.h"
#include "Core/Protocol/Sync/QUICBootstrap.h"
#include "Core/Protocol/Sync/ResumeProof.h"
#include "Core/Screen/Addressing.h"
#include "Core/TerminalOperations.h"
#include "Host/QUICDial.h"

#include <charconv>
#include <cstring>
#include <memory>
#include <random>

namespace UbiquittyTest {

inline constexpr unsigned soakFrameCount = 32;

/// Parses a complete unsigned decimal field without accepting a suffix.
/// - Parameter text: The field.
/// - Returns: Its value, or nothing for malformed or overflowing input.
inline std::optional<std::uint32_t> soakNumber(std::string_view text)
{
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size())
        return std::nullopt;
    return value;
}

/// Produces one deterministic full-screen coloured frame from the PTY peer.
/// - Parameters:
///   - seed: The iteration's replay seed.
///   - frame: Its frame number.
/// - Returns: The exact terminal bytes, with a fixed final cursor.
inline std::string soakFrame(std::uint32_t seed, unsigned frame)
{
    std::mt19937 random(seed ^ (frame * 0x9e3779b9u));
    std::string bytes = "\033[0m\033[2J\033[H";
    for (unsigned row = 1; row <= 24; ++row) {
        bytes += "\033[" + std::to_string(row) + ";1H\033[38;5;" + std::to_string(random() % 256) + "m";
        for (unsigned column = 0; column < 79; ++column)
            bytes.push_back(static_cast<char>('!' + random() % 94));
    }
    return bytes + "\033[0m\033[H";
}

/// Starts a uniquely named command whose completion exercises effect replay.
/// - Parameter seed: The command's replay seed.
/// - Returns: Prompt marks and the command text.
inline std::string soakCommandStart(std::uint32_t seed)
{
    const std::string command = "soak-" + std::to_string(seed);
    return "\033[0m\033[2J\033[H\033]133;A\a$ \033]133;B\a" + command
        + "\033]633;E;" + command + "\a\033]133;C\a\r\n";
}

/// Compares the complete visible ASCII-cell representation and cursor.
/// The fixture generates no hyperlinks or pooled clusters, so cell bytes
/// carry the entire glyph and pen, with no process-local IDs to normalize.
/// - Parameters:
///   - actual: The received screen, possibly absent during reconnect.
///   - expected: A separate terminal fed the exact generated PTY stream.
/// - Returns: Whether every cell and the cursor agree.
inline bool isSoakScreenEqual(const Ubiquitty::Terminal* actual, const Ubiquitty::Terminal& expected)
{
    if (!actual || actual->rows != expected.rows || actual->columns != expected.columns
        || actual->screens.active->cursorX != expected.screens.active->cursorX
        || actual->screens.active->cursorY != expected.screens.active->cursorY)
        return false;
    for (int row = 0; row < expected.rows; ++row) {
        for (int column = 0; column < expected.columns; ++column) {
            const auto* left = Ubiquitty::Screen::visibleCell(actual, 0, row, column);
            const auto* right = Ubiquitty::Screen::visibleCell(&expected, 0, row, column);
            if (!left || !right || std::memcmp(left, right, sizeof(Ubiquitty::Cell)))
                return false;
        }
    }
    return true;
}

/// Builds the production resume cursor from the last fully applied picture.
/// - Parameters:
///   - replicas: Pictures and effect receipts kept across the dropped carrier.
///   - pane: The stable pane identity of the admitted attachment.
///   - welcome: The credential's current attachment identity.
///   - input: The last input sequence sent, including one possibly in flight.
/// - Returns: A proof-authenticated request, without a secret in the Greet.
inline Sync::ResumeRequest soakResume(Sync::ReplicaSet& replicas, Sync::ID128 pane, const Sync::Welcome& welcome, std::uint64_t input)
{
    Sync::ResumeRequest request { .attachmentID = welcome.attachmentID, .resumeSecret = std::nullopt, .panes = {} };
    if (const auto* held = replicas.find(pane)) {
        request.panes.push_back({ .paneID = pane, .ackedGeneration = held->replica.generation, .ackedRowHashesDigest = Sync::replicaDigest(held->replica), .historyEpoch = held->replica.historyEpoch, .cachedRanges = {}, .lastInputSeqSent = input, .view = Sync::ViewState::Live, .effectsAcknowledged = replicas.effectReceipt(pane).acknowledged });
    }
    return request;
}

/// Runs repeated real-QUIC interruptions against one fixture-owned daemon.
/// Each iteration checks a new complete picture and one exact completion
/// effect across the two client owners, including receipts retained at a cut.
/// - Parameters:
///   - bootstrap: A fresh single-use token and pin for the private listener.
///   - iterations: Number of attach/flood/drop/resume cycles.
///   - seed: Master seed; each derived iteration seed is also printed.
///   - observerBootstrap: A second token for an independently retained client
///     that stays attached throughout the first client's reconnects.
inline void runSyncReconnectSoak(const Sync::QUICBootstrapInfo& bootstrap, unsigned iterations, std::uint32_t seed,
    const std::optional<Sync::QUICBootstrapInfo>& observerBootstrap = std::nullopt)
{
    std::printf("sync-soak: seed=%u iterations=%u\n", seed, iterations);
    std::fflush(stdout);
    Sync::ReplicaSet replicas;
    auto dial = std::make_unique<Dial>(replicas, std::nullopt);
    REQUIRE(dial->start(bootstrap.port, Transport::BootstrapToken { .token = bootstrap.token }, bootstrap.certificateSHA256,
        { .target = Sync::AdmissionTarget::Session }));
    REQUIRE_MSG(dial->pumpUntil(10'000, [](const ScreenClient& client) {
        return client.welcome().has_value() && client.pane() && client.generation() > 0;
    }),
        "initial private-daemon attachment did not publish a pane");
    auto welcome = dial->welcome();
    REQUIRE(welcome.has_value());
    Sync::ID128 pane;
    {
        const std::scoped_lock held(dial->lock);
        const auto bound = dial->client.connection().boundPanes();
        REQUIRE(bound.size() == 1);
        pane = bound.front().second;
    }
    const Sync::AdmissionRequest target { .target = Sync::AdmissionTarget::Session,
        .sessionID = welcome->selection.sessionID,
        .expectedDaemonInstanceID = welcome->selection.daemonInstanceID,
        .isAttachOnly = true };
    Sync::ReplicaSet observerReplicas;
    std::unique_ptr<Dial> observer;
    std::optional<Sync::Welcome> observerWelcome;
    std::uint64_t observerReceipt = 0;
    if (observerBootstrap) {
        REQUIRE(observerBootstrap->port == bootstrap.port && observerBootstrap->certificateSHA256 == bootstrap.certificateSHA256);
        observer = std::make_unique<Dial>(observerReplicas, std::nullopt, Sync::ID128 { .low = 3, .high = 4 });
        REQUIRE(observer->start(bootstrap.port, Transport::BootstrapToken { .token = observerBootstrap->token },
            bootstrap.certificateSHA256, target));
        REQUIRE_MSG(observer->pumpUntil(10'000, [&](const ScreenClient& client) {
            return client.welcome().has_value() && client.boundPane(1) == pane && client.generation() > 0;
        }),
            "the independent observer did not attach to the same pane");
        observerWelcome = observer->welcome();
        REQUIRE(observerWelcome && observerWelcome->attachmentID != welcome->attachmentID);
    }
    Ubiquitty::Terminal expected;
    Ubiquitty::resize(expected, 24, 80);
    std::mt19937 random(seed);
    std::uint64_t previousReceipt = 0;
    for (unsigned iteration = 1; iteration <= iterations; ++iteration) {
        const auto iterationSeed = static_cast<std::uint32_t>(random());
        const std::size_t cut = 1 + random() % 8192;
        std::printf("sync-soak: begin iteration=%u seed=%u cut=%zu\n", iteration, iterationSeed, cut);
        std::fflush(stdout);
        Ubiquitty::processBytes(&expected, soakCommandStart(iterationSeed));
        for (unsigned frame = 0; frame < soakFrameCount; ++frame)
            Ubiquitty::processBytes(&expected, soakFrame(iterationSeed, frame));
        Ubiquitty::processBytes(&expected, "\033]133;D;0\a");
        dial->cutStreamAfter(cut);
        const std::string command = "FLOOD " + std::to_string(iterationSeed) + "\n";
        {
            const std::scoped_lock held(dial->lock);
            REQUIRE(dial->client.sendRawNumbered(1, iteration, { reinterpret_cast<const std::uint8_t*>(command.data()), command.size() }));
        }
        REQUIRE_MSG(dial->pumpUntil(10'000, [&](const ScreenClient&) { return dial->hasReachedStreamCut.load(); }),
            "flood did not reach the requested carrier byte cut");
        dial->carrier.stop();
        std::vector<std::string> commands;
        {
            const std::scoped_lock held(dial->lock);
            dial->client.storeReplicas();
            commands = dial->client.takeCompletedCommands();
        }
        const auto interruptedReceipt = replicas.effectReceipt(pane);
        CHECK(interruptedReceipt.acknowledged >= previousReceipt);
        CHECK(interruptedReceipt.consumed >= interruptedReceipt.acknowledged);
        Sync::EffectReceiptState observerAtCut;
        if (observer) {
            const std::scoped_lock held(observer->lock);
            observerAtCut = observerReplicas.effectReceipt(pane);
            REQUIRE(!observer->hasClosed.load());
            REQUIRE(observer->client.connection().state() == Sync::ConnectionState::Attached);
            CHECK(observerAtCut.acknowledged >= observerReceipt);
            CHECK(observerAtCut.consumed >= observerAtCut.acknowledged);
        }
        const auto resume = soakResume(replicas, pane, *welcome, iteration);
        REQUIRE(resume.panes.size() == 1);
        const auto credential = *welcome;
        dial.reset();
        dial = std::make_unique<Dial>(replicas, resume);
        Transport::ExportedPreamble proof { .exporterLabel = std::string(Sync::resumeExporterLabel),
            .build = [credential](std::span<const std::uint8_t, 32> exporter) {
                return Sync::resumePreamble(credential.resumeSecret, exporter, credential.attachmentID, credential.hostInstanceID);
            } };
        REQUIRE(dial->start(bootstrap.port, std::move(proof), bootstrap.certificateSHA256, target));
        REQUIRE_MSG(dial->pumpUntil(10'000, [&](const ScreenClient& client) {
            return client.welcome().has_value() && client.boundPane(1) == pane
                && client.connection().state() == Sync::ConnectionState::Attached;
        }),
            "resume did not bind the pane for retained input replay");
        // A cut can precede the queued input's flush. Replaying its same
        // sequence recovers that input and lets the host deduplicate a write
        // that already reached the PTY.
        {
            const std::scoped_lock held(dial->lock);
            REQUIRE(dial->client.sendRawNumbered(1, iteration, { reinterpret_cast<const std::uint8_t*>(command.data()), command.size() }));
        }
        REQUIRE_MSG(dial->pumpUntil(10'000, [&](const ScreenClient& client) {
            return isSoakScreenEqual(client.pane(), expected) && replicas.effectReceipt(pane).acknowledged > previousReceipt;
        }),
            "resumed picture or effect receipt did not converge before the deadline");
        // Flush the receipt and allow any duplicate completion to arrive.
        (void)dial->pumpUntil(100, [](const ScreenClient&) { return false; });
        Sync::EffectReceiptState receipt;
        {
            const std::scoped_lock held(dial->lock);
            welcome = dial->client.welcome();
            REQUIRE(welcome && welcome->attachmentID == credential.attachmentID);
            REQUIRE(welcome->resumed.size() == 1 && welcome->resumed.front().paneID == pane);
            CHECK(isSoakScreenEqual(dial->client.pane(), expected));
            const auto remaining = dial->client.takeCompletedCommands();
            commands.insert(commands.end(), remaining.begin(), remaining.end());
            CHECK_EQ(dial->client.connection().counters().digestMismatches, 0u);
            CHECK_EQ(dial->client.connection().counters().resyncsSent, 0u);
            dial->client.storeReplicas();
            receipt = replicas.effectReceipt(pane);
        }
        if (commands != std::vector<std::string> { "soak-" + std::to_string(iterationSeed) }) {
            std::fprintf(stderr, "sync-soak: completion count=%zu\n", commands.size());
            for (const auto& completed : commands)
                std::fprintf(stderr, "sync-soak: completion text=[%s]\n", completed.c_str());
        }
        REQUIRE_MSG(commands == std::vector<std::string> { "soak-" + std::to_string(iterationSeed) },
            "the completion effect was missing, duplicated, or changed across resume");
        CHECK(receipt.acknowledged >= interruptedReceipt.acknowledged);
        CHECK(receipt.consumed >= interruptedReceipt.consumed);
        CHECK(receipt.consumed == receipt.acknowledged);
        previousReceipt = receipt.acknowledged;
        if (observer) {
            REQUIRE_MSG(observer->pumpUntil(10'000, [&](const ScreenClient& client) {
                return isSoakScreenEqual(client.pane(), expected)
                    && observerReplicas.effectReceipt(pane).acknowledged > observerReceipt;
            }),
                "the uninterrupted observer's picture or receipts did not converge");
            (void)observer->pumpUntil(100, [](const ScreenClient&) { return false; });
            const std::scoped_lock held(observer->lock);
            REQUIRE(!observer->hasClosed.load());
            REQUIRE(observer->client.connection().state() == Sync::ConnectionState::Attached);
            const auto retainedWelcome = observer->client.welcome();
            REQUIRE(retainedWelcome && retainedWelcome->attachmentID == observerWelcome->attachmentID);
            CHECK(observer->client.boundPane(1) == pane);
            CHECK(isSoakScreenEqual(observer->client.pane(), expected));
            CHECK_EQ(observer->client.connection().counters().digestMismatches, 0u);
            CHECK_EQ(observer->client.connection().counters().resyncsSent, 0u);
            const auto observerCommands = observer->client.takeCompletedCommands();
            REQUIRE_MSG(observerCommands == std::vector<std::string> { "soak-" + std::to_string(iterationSeed) },
                "the uninterrupted observer lost, duplicated, or changed a completion effect");
            observer->client.storeReplicas();
            const auto observed = observerReplicas.effectReceipt(pane);
            CHECK(observed.acknowledged >= observerAtCut.acknowledged);
            CHECK(observed.consumed >= observerAtCut.consumed);
            CHECK(observed.acknowledged == receipt.acknowledged && observed.consumed == receipt.consumed);
            observerReceipt = observed.acknowledged;
            std::printf("sync-soak: observer iteration=%u acknowledged=%llu consumed=%llu\n", iteration,
                static_cast<unsigned long long>(observed.acknowledged), static_cast<unsigned long long>(observed.consumed));
        }
        if (failures)
            return;
        std::printf("sync-soak: complete iteration=%u seed=%u cut=%zu acknowledged=%llu consumed=%llu\n",
            iteration, iterationSeed, cut, static_cast<unsigned long long>(receipt.acknowledged), static_cast<unsigned long long>(receipt.consumed));
        std::fflush(stdout);
    }
    {
        const std::scoped_lock held(dial->lock);
        dial->client.detach();
    }
    (void)dial->pumpUntil(100, [](const ScreenClient&) { return false; });
    if (observer) {
        {
            const std::scoped_lock held(observer->lock);
            observer->client.detach();
        }
        (void)observer->pumpUntil(100, [](const ScreenClient&) { return false; });
    }
    std::printf("sync-soak: ok seed=%u iterations=%u\n", seed, iterations);
}

} // namespace UbiquittyTest

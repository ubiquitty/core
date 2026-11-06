// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Platform/Host/LocalControlClient.h"

#include <array>
#include <span>
#include <vector>

using namespace Ubiquitty::Host;

namespace {

constexpr LocalControlEndpoint owner { .hasPrivateOwnerDirectory = true, .hasPrivateOwnerSocket = true, .hasSameUserPeer = true };
constexpr LocalControlReply reply { .correlation = 19, .daemonInstanceID = 27, .status = LocalControlStatus::Reloaded };

void sendWholeRequest(LocalReloadExchange& exchange)
{
    exchange.authorize(owner, 1);
    const auto request = exchange.outgoing(1);
    REQUIRE(request.size() == localControlRequestBytes);
    exchange.didSend(request.size(), 1);
    CHECK(exchange.state(1) == LocalReloadExchange::State::AwaitingReply);
}

} // namespace

TEST(noLocalReloadByteLeavesBeforeAllEndpointAuthorityIsVerified)
{
    for (unsigned missing = 0; missing < 3; ++missing) {
        LocalReloadExchange exchange(19, 100);
        CHECK(exchange.outgoing(1).empty());
        auto endpoint = owner;
        switch (missing) {
        case 0:
            endpoint.hasPrivateOwnerDirectory = false;
            break;
        case 1:
            endpoint.hasPrivateOwnerSocket = false;
            break;
        case 2:
            endpoint.hasSameUserPeer = false;
            break;
        }
        exchange.authorize(endpoint, 1);
        CHECK(exchange.outgoing(1).empty());
        CHECK(exchange.state(1) == LocalReloadExchange::State::Refused);
        exchange.authorize(owner, 2);
        CHECK(exchange.outgoing(2).empty());
    }
    LocalReloadExchange noCorrelation(0, 100);
    noCorrelation.authorize(owner, 1);
    CHECK(noCorrelation.outgoing(1).empty());
    LocalReloadExchange noDeadline(19, 0);
    noDeadline.authorize(owner, 0);
    CHECK(noDeadline.outgoing(0).empty());
}

TEST(localReloadWritesAndRepliesSurviveEveryPacketBoundary)
{
    const auto request = encodeLocalReloadRequest(19);
    const auto bytes = encodeLocalControlReply(reply);
    for (std::size_t split = 1; split <= request.size(); ++split) {
        LocalReloadExchange exchange(19, 100);
        exchange.authorize(owner, 1);
        CHECK(std::ranges::equal(exchange.outgoing(1), request));
        exchange.didSend(split, 1);
        CHECK(std::ranges::equal(exchange.outgoing(2), std::span { request }.subspan(split)));
        if (split < request.size())
            exchange.didSend(request.size() - split, 2);
        CHECK(exchange.outgoing(2).empty());
        exchange.receive(bytes, 3);
        CHECK(exchange.state(3) == LocalReloadExchange::State::Reloaded);
    }
    for (std::size_t split = 0; split <= bytes.size(); ++split) {
        LocalReloadExchange exchange(19, 100);
        sendWholeRequest(exchange);
        exchange.receive(std::span { bytes }.first(split), 2);
        CHECK(exchange.state(2) == (split == bytes.size() ? LocalReloadExchange::State::Reloaded : LocalReloadExchange::State::AwaitingReply));
        exchange.receive(std::span { bytes }.subspan(split), 3);
        CHECK(exchange.state(3) == LocalReloadExchange::State::Reloaded);
    }
}

TEST(aLocalReloadCannotBeAcknowledgedBeforeItsWholeRequestLeft)
{
    const auto bytes = encodeLocalControlReply(reply);
    for (std::size_t sent = 0; sent < localControlRequestBytes; ++sent) {
        LocalReloadExchange exchange(19, 100);
        exchange.authorize(owner, 1);
        if (sent)
            exchange.didSend(sent, 1);
        exchange.receive(bytes, 2);
        CHECK(exchange.state(2) == LocalReloadExchange::State::Refused);
    }
    LocalReloadExchange unauthenticated(19, 100);
    unauthenticated.receive(bytes, 1);
    CHECK(unauthenticated.state(1) == LocalReloadExchange::State::Refused);
}

TEST(localReloadProgressNeverRenewsItsOriginalDeadline)
{
    const auto bytes = encodeLocalControlReply(reply);
    LocalReloadExchange writing(19, 10);
    writing.authorize(owner, 1);
    writing.didSend(1, 9);
    CHECK(writing.outgoing(10).empty());
    CHECK(writing.state(10) == LocalReloadExchange::State::Refused);
    LocalReloadExchange reading(19, 10);
    sendWholeRequest(reading);
    reading.receive(std::span { bytes }.first(bytes.size() - 1), 9);
    reading.receive(std::span { bytes }.last(1), 10);
    CHECK(reading.state(10) == LocalReloadExchange::State::Refused);
    LocalReloadExchange authorizedTooLate(19, 10);
    authorizedTooLate.authorize(owner, 10);
    CHECK(authorizedTooLate.outgoing(10).empty());
    LocalReloadExchange completed(19, 10);
    sendWholeRequest(completed);
    completed.receive(bytes, 9);
    CHECK(completed.state(100) == LocalReloadExchange::State::Reloaded);
}

TEST(onlyAnExactMatchingLocalReloadReplyCountsAsAcknowledged)
{
    const auto bytes = encodeLocalControlReply(reply);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        LocalReloadExchange oldOwner(19, 100);
        sendWholeRequest(oldOwner);
        oldOwner.receive(std::span { bytes }.first(length), 2);
        oldOwner.refuse(); // EOF or an owner that does not speak the local protocol.
        CHECK(oldOwner.state(2) == LocalReloadExchange::State::Refused);
    }
    for (const auto unexpected : { LocalControlReply { .correlation = 20, .daemonInstanceID = 27, .status = LocalControlStatus::Reloaded },
             LocalControlReply { .correlation = 19, .daemonInstanceID = 0, .status = LocalControlStatus::Reloaded },
             LocalControlReply { .correlation = 19, .daemonInstanceID = 27, .status = LocalControlStatus::Refused } }) {
        LocalReloadExchange exchange(19, 100);
        sendWholeRequest(exchange);
        exchange.receive(encodeLocalControlReply(unexpected), 2);
        CHECK(exchange.state(2) == LocalReloadExchange::State::Refused);
    }
    for (std::size_t index = 0; index < localControlPreamble.size() + 2; ++index) {
        auto malformed = bytes;
        malformed[index] ^= 0xff;
        LocalReloadExchange exchange(19, 100);
        sendWholeRequest(exchange);
        exchange.receive(malformed, 2);
        CHECK(exchange.state(2) == LocalReloadExchange::State::Refused);
    }
    std::vector<std::uint8_t> trailing(bytes.begin(), bytes.end());
    trailing.push_back(0);
    LocalReloadExchange oversized(19, 100);
    sendWholeRequest(oversized);
    oversized.receive(trailing, 2);
    CHECK(oversized.state(2) == LocalReloadExchange::State::Refused);
}

TEST(localReloadWriteAccountingRefusesZeroAndImpossibleProgress)
{
    for (const auto count : { std::size_t { 0 }, localControlRequestBytes + 1 }) {
        LocalReloadExchange exchange(19, 100);
        exchange.authorize(owner, 1);
        exchange.didSend(count, 2);
        CHECK(exchange.state(2) == LocalReloadExchange::State::Refused);
        CHECK(exchange.outgoing(2).empty());
    }
    LocalReloadExchange partial(19, 100);
    partial.authorize(owner, 1);
    partial.didSend(1, 2);
    partial.didSend(localControlRequestBytes, 3);
    CHECK(partial.state(3) == LocalReloadExchange::State::Refused);
}

UBIQUITTY_TEST_MAIN()

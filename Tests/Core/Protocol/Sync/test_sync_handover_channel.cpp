// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/HandoverChannel.h"
#include "Harness.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {
constexpr HandoverChannelLimits limits { 1, 64, 1024, 2048 };
const std::array<HandoverMessage, 7> messages {
    HandoverRequest { 1, { 2, 3 } }, HandoverOffer { 1, { 4, 5 }, 2, 200, 3, 1 },
    HandoverPaneFollows { 1, { 6, 7 }, 400, 4242, 77 }, HandoverAccept {},
    HandoverRefuse { HandoverRefuseReason::PaneBusy }, HandoverRelease {}, HandoverServing {}
};
}

TEST(allControlRecordBytesAreFrozen)
{
    const std::array<std::vector<std::uint8_t>, 7> expected {
        std::vector<std::uint8_t> { 1, 1, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0 },
        { 2, 1, 0, 4, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0, 2, 0, 200, 0, 0, 0, 3, 1 },
        { 3, 1, 0, 6, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 144, 1, 0, 0, 146, 16, 0, 0, 77, 0, 0, 0 },
        { 4 }, { 5, 3 }, { 6 }, { 7 }
    };
    for (std::size_t index = 0; index < messages.size(); ++index) {
        CHECK(encodeHandoverMessage(messages[index]) == expected[index]);
        const auto decoded = decodeHandoverMessage(expected[index], limits);
        REQUIRE(decoded);
        CHECK(*decoded == messages[index]);
    }
}

TEST(eachRefusalReasonIsItsOwnByteAndNoneIsPastTheLast)
{
    // The reason reaches the person running the install, so each blocker
    // the holder knows keeps its own byte rather than folding into PaneBusy.
    const std::array<std::pair<HandoverRefuseReason, std::uint8_t>, 11> reasons { {
        { HandoverRefuseReason::NotSuccessor, 1 },
        { HandoverRefuseReason::NewerBody, 2 },
        { HandoverRefuseReason::PaneBusy, 3 },
        { HandoverRefuseReason::Malformed, 4 },
        { HandoverRefuseReason::Geometry, 5 },
        { HandoverRefuseReason::Ancestry, 6 },
        { HandoverRefuseReason::ApprovalPending, 7 },
        { HandoverRefuseReason::GrantPending, 8 },
        { HandoverRefuseReason::OutputUnsent, 9 },
        { HandoverRefuseReason::RequestPending, 10 },
        { HandoverRefuseReason::AdoptPending, 11 },
    } };
    for (const auto& [reason, byte] : reasons) {
        const std::vector<std::uint8_t> bytes { 5, byte };
        CHECK(encodeHandoverMessage(HandoverRefuse { reason }) == bytes);
        const auto decoded = decodeHandoverMessage(bytes, limits);
        REQUIRE(decoded);
        CHECK(*decoded == HandoverMessage { HandoverRefuse { reason } });
    }
    CHECK(static_cast<std::uint8_t>(lastHandoverRefuseReason) == reasons.back().second);
    CHECK(!decodeHandoverMessage(std::vector<std::uint8_t> { 5, 0 }, limits));
    CHECK(!decodeHandoverMessage(std::vector<std::uint8_t> { 5, 12 }, limits));
}

TEST(preambleRequiresEveryByteAndNoSuffix)
{
    REQUIRE(isHandoverPreamble(handoverPreamble));
    for (std::size_t count = 0; count < handoverPreamble.size(); ++count)
        CHECK(!isHandoverPreamble(std::span(handoverPreamble).first(count)));
    for (std::size_t index = 0; index < handoverPreamble.size(); ++index) {
        auto changed = handoverPreamble;
        changed[index] ^= 1;
        CHECK(!isHandoverPreamble(changed));
    }
    std::vector<std::uint8_t> extra(handoverPreamble.begin(), handoverPreamble.end());
    extra.push_back(0);
    CHECK(!isHandoverPreamble(extra));
}

TEST(allControlRecordsRoundTripAndRejectEveryTruncationOrSuffix)
{
    for (const auto& message : messages) {
        auto bytes = encodeHandoverMessage(message);
        const auto decoded = decodeHandoverMessage(bytes, limits);
        REQUIRE(decoded);
        CHECK(*decoded == message);
        for (std::size_t count = 0; count < bytes.size(); ++count)
            CHECK(!decodeHandoverMessage(std::span(bytes).first(count), limits));
        bytes.push_back(0);
        CHECK(!decodeHandoverMessage(bytes, limits));
    }
}

TEST(newerVersionsAreRefusedBeforeTheirUnknownBodyIsRead)
{
    const std::array<std::uint8_t, 3> newerChannel { 1, 2, 0 };
    const auto channel = decodeHandoverMessage(newerChannel, limits);
    REQUIRE(!channel);
    CHECK(channel.error() == HandoverDecodeError::NewerChannel);
    const std::array<std::uint8_t, 3> newerBody { 2, 2, 0 };
    const auto body = decodeHandoverMessage(newerBody, limits);
    REQUIRE(!body);
    CHECK(body.error() == HandoverDecodeError::NewerBody);
}

TEST(announcedResourcesAreBoundedBeforeAnyBodyAllocation)
{
    for (const HandoverMessage& message : std::array<HandoverMessage, 4> {
             HandoverOffer { 1, { 4, 5 }, 65, 1, 1, 0 }, HandoverOffer { 1, { 4, 5 }, 2, 2049, 1, 0 },
             HandoverPaneFollows { 64, { 6, 7 }, 1, 2, 1 }, HandoverPaneFollows { 0, { 6, 7 }, 1025, 2, 1 } }) {
        const auto result = decodeHandoverMessage(encodeHandoverMessage(message), limits);
        REQUIRE(!result);
        CHECK(result.error() == HandoverDecodeError::OverCapacity);
    }
}

TEST(resourceLimitsAreInclusiveAndMayBeZero)
{
    const HandoverMessage offer = HandoverOffer { 1, { 4, 5 }, 64, 2048, maxHandoverListeners, 1 };
    const HandoverMessage pane = HandoverPaneFollows { 63, { 6, 7 }, 1024, 2, 1 };
    CHECK(decodeHandoverMessage(encodeHandoverMessage(offer), limits).has_value());
    CHECK(decodeHandoverMessage(encodeHandoverMessage(pane), limits).has_value());
    const HandoverChannelLimits empty { 1, 0, 0, 0 };
    CHECK(!decodeHandoverMessage(encodeHandoverMessage(offer), empty));
    CHECK(!decodeHandoverMessage(encodeHandoverMessage(pane), empty));
    CHECK(decodeHandoverMessage(encodeHandoverMessage(HandoverOffer { 1, { 4, 5 }, 0, 0, 1, 0 }), empty).has_value());
}

TEST(zeroIdentitiesVersionsAndEmptyPaneBodiesAreMalformed)
{
    for (const HandoverMessage& message : std::array<HandoverMessage, 14> {
             HandoverRequest { 1, {} }, HandoverRequest { 0, { 1, 2 } },
             HandoverOffer { 0, { 1, 2 }, 0, 0, 1, 0 }, HandoverOffer { 1, {}, 0, 0, 1, 0 },
             HandoverOffer { 1, { 1, 2 }, 0, 0, 0, 0 }, HandoverOffer { 1, { 1, 2 }, 0, 0, maxHandoverListeners + 1, 0 },
             HandoverOffer { 1, { 1, 2 }, 0, 0, 1, 2 },
             HandoverPaneFollows { 0, {}, 1, 2, 1 }, HandoverPaneFollows { 0, { 1, 2 }, 0, 2, 1 },
             HandoverPaneFollows { 0, { 1, 2 }, 1, 0, 1 }, HandoverPaneFollows { 0, { 1, 2 }, 1, 1, 1 },
             HandoverPaneFollows { 0, { 1, 2 }, 1, 2, 0 }, HandoverPaneFollows { 0, { 1, 2 }, 1, 0x80000000u, 1 },
             HandoverPaneFollows { 0, { 1, 2 }, 1, 2, 0x80000000u } }) {
        const auto result = decodeHandoverMessage(encodeHandoverMessage(message), limits);
        REQUIRE(!result);
        CHECK(result.error() == HandoverDecodeError::Malformed);
    }
}

TEST(unknownTagsAndRefusalsAreNotAccepted)
{
    for (unsigned tag = 0; tag <= 255; ++tag) {
        if (tag >= 1 && tag <= 7)
            continue;
        const std::array<std::uint8_t, 1> bytes { static_cast<std::uint8_t>(tag) };
        CHECK(!decodeHandoverMessage(bytes, limits));
    }
    for (unsigned reason = 0; reason <= 255; ++reason) {
        const std::array<std::uint8_t, 2> bytes { 5, static_cast<std::uint8_t>(reason) };
        CHECK(decodeHandoverMessage(bytes, limits).has_value() == (reason >= 1 && reason <= static_cast<unsigned>(lastHandoverRefuseReason)));
    }
}

UBIQUITTY_TEST_MAIN()

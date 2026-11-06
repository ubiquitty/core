// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#ifndef UBIQUITTY_TEST_WITHOUT_CORE
#define UBIQUITTY_TEST_WITHOUT_CORE
#endif
#include "Harness.h"

#include "Core/Protocol/Control/Reclaim.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Platform/Host/LocalHandoff.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace {
namespace Local = Ubiquitty::Protocol::Local;
namespace Control = Ubiquitty::Protocol::Control;
namespace Host = Ubiquitty::Host;

Control::ReclaimTarget reclaimTarget()
{
    return { .handle = 7, .sessionID = 19, .daemonInstanceID = 23, .hostInstanceID = { .low = 2, .high = 3 }, .attachmentID = { .low = 5, .high = 7 }, .paneID = { .low = 19, .high = 23 } };
}

Ubiquitty::Codec::JSON::Value json(const char* text)
{
    bool isJSON = false;
    auto value = Ubiquitty::Codec::JSON::parse(text, isJSON);
    CHECK(isJSON);
    return value;
}
}

TEST(aLocalHandoffSelectsItsOwnOperationBeforeAnyRecord)
{
    const auto request = Local::encodeLocalHandoffRequest(11);
    REQUIRE(request.has_value());
    CHECK_EQ(request->size(), Local::localControlRequestBytes);
    CHECK_EQ((*request)[Local::localControlPreamble.size()], 3);
    for (std::size_t cut = 0; cut < request->size(); ++cut) {
        Local::LocalControlReader reader;
        CHECK(reader.consume(std::span(*request).first(cut)) == Local::LocalControlReader::State::Awaiting);
        CHECK(reader.consume(std::span(*request).subspan(cut)) == Local::LocalControlReader::State::Ready);
        const auto decoded = reader.take();
        REQUIRE(decoded.has_value());
        CHECK(decoded->operation == Local::LocalControlOperation::SessionHandoff);
        CHECK_EQ(decoded->correlation, 11u);
    }
    CHECK(!Local::encodeLocalHandoffRequest(0));
    auto trailing = std::vector<std::uint8_t>(request->begin(), request->end());
    trailing.push_back(2);
    Local::LocalControlReader reader;
    CHECK(reader.consume(trailing) == Local::LocalControlReader::State::Refused);
    auto unknown = *request;
    unknown[Local::localControlPreamble.size()] = 0x7F;
    Local::LocalControlReader unknownReader;
    CHECK(unknownReader.consume(unknown) == Local::LocalControlReader::State::Refused);
}

TEST(aLocalHandoffReadinessCannotBeMistakenForReloadOrQUIC)
{
    const Local::HandoffReady expected { .correlation = 11, .daemonInstanceID = 23, .status = Local::HandoffReadyStatus::Ready };
    const auto bytes = Local::encodeLocalHandoffReady(expected);
    CHECK(Local::decodeLocalHandoffReady(bytes) == expected);
    CHECK(!Local::decodeLocalControlReply(bytes));
    CHECK(!Local::decodeLocalQUICTokenReply(bytes));
    for (std::size_t cut = 0; cut < bytes.size(); ++cut)
        CHECK(!Local::decodeLocalHandoffReady(std::span(bytes).first(cut)));
    auto invalid = bytes;
    invalid[Local::localControlPreamble.size() + 1] = 2;
    CHECK(!Local::decodeLocalHandoffReady(invalid));
    CHECK(!Local::decodeLocalHandoffReady(Local::encodeLocalHandoffReady({ .correlation = 0, .daemonInstanceID = 23 })));
    CHECK(!Local::decodeLocalHandoffReady(Local::encodeLocalHandoffReady({ .correlation = 11, .daemonInstanceID = 0 })));
}

TEST(aDescriptorBearingLocalAdoptNeverWidensTheGenericLocalLane)
{
    const Host::LocalControlPeer peer { .isSameUserLocalStream = true, .isTrustedOwnerBinary = true, .hasRemoteGreeting = false, .hasDescriptors = true };
    CHECK(!Host::admitsLocalControl(peer));
    const Host::LocalAdoptAuthority authority { .peer = peer, .isSelectedHandoff = true, .hasVerifiedProcess = true, .hasCurrentProcessInstance = true };
    CHECK(Host::admitsLocalAdopt(authority));
    for (int missing = 0; missing < 6; ++missing) {
        auto invalid = authority;
        switch (missing) {
        case 0:
            invalid.peer.isSameUserLocalStream = false;
            break;
        case 1:
            invalid.peer.isTrustedOwnerBinary = false;
            break;
        case 2:
            invalid.peer.hasRemoteGreeting = true;
            break;
        case 3:
            invalid.isSelectedHandoff = false;
            break;
        case 4:
            invalid.hasVerifiedProcess = false;
            break;
        case 5:
            invalid.hasCurrentProcessInstance = false;
            break;
        }
        CHECK(!Host::admitsLocalAdopt(invalid));
    }
}

TEST(aLocalHandoffDescriptorBelongsOnlyToTheFirstRecordByte)
{
    const Host::LocalHandoffDescriptorPosition first { .isSelectedHandoff = true };
    CHECK(Host::awaitsLocalHandoffDescriptor(first));
    CHECK(Host::admitsLocalHandoffDescriptorPosition(first, true));
    CHECK(!Host::admitsLocalHandoffDescriptorPosition(first, false));
    for (int changed = 0; changed < 4; ++changed) {
        auto later = first;
        switch (changed) {
        case 0:
            later.isSelectedHandoff = false;
            break;
        case 1:
            later.hasReceivedRecord = true;
            break;
        case 2:
            later.hasPendingDescriptor = true;
            break;
        case 3:
            later.pendingRecordBytes = 1;
            break;
        }
        CHECK(!Host::awaitsLocalHandoffDescriptor(later));
        CHECK(!Host::admitsLocalHandoffDescriptorPosition(later, true));
        CHECK(Host::admitsLocalHandoffDescriptorPosition(later, false));
    }
}

TEST(aLocalHandoffDescriptorPositionExhaustsEveryStateCombination)
{
    for (unsigned state = 0; state < 16; ++state) {
        const Host::LocalHandoffDescriptorPosition position {
            .isSelectedHandoff = (state & 1) != 0,
            .hasReceivedRecord = (state & 2) != 0,
            .hasPendingDescriptor = (state & 4) != 0,
            .pendingRecordBytes = (state & 8) != 0 ? 1u : 0u,
        };
        const bool isFirst = state == 1;
        CHECK_EQ(Host::awaitsLocalHandoffDescriptor(position), isFirst);
        CHECK_EQ(Host::admitsLocalHandoffDescriptorPosition(position, true), isFirst);
        CHECK_EQ(Host::admitsLocalHandoffDescriptorPosition(position, false), !isFirst);
    }
}

TEST(aLocalAdoptIsolatesEveryRecordKindIncludingFreshStatusAndAbort)
{
    for (const bool hasReceivedRecord : { false, true }) {
        const Host::LocalHandoffDescriptorPosition boundary { .isSelectedHandoff = true, .hasReceivedRecord = hasReceivedRecord };
        CHECK(Host::awaitsLocalAdoptRecordKind(boundary));
        for (const auto kind : { Local::AdoptWireMessage::Status, Local::AdoptWireMessage::Abort, Local::AdoptWireMessage::Commit }) {
            CHECK(Host::admitsLocalAdoptDescriptorPosition(boundary, static_cast<std::uint8_t>(kind), 0));
            CHECK(!Host::admitsLocalAdoptDescriptorPosition(boundary, static_cast<std::uint8_t>(kind), 1));
        }
        CHECK(Host::admitsLocalAdoptDescriptorPosition(boundary, static_cast<std::uint8_t>(Local::AdoptWireMessage::Prepare), 1));
        CHECK(!Host::admitsLocalAdoptDescriptorPosition(boundary, static_cast<std::uint8_t>(Local::AdoptWireMessage::Prepare), 0));
        auto partial = boundary;
        partial.pendingRecordBytes = 1;
        partial.hasPendingDescriptor = true;
        CHECK(!Host::awaitsLocalAdoptRecordKind(partial));
        CHECK(Host::admitsLocalAdoptDescriptorPosition(partial, 0xFF, 0));
        CHECK(!Host::admitsLocalAdoptDescriptorPosition(partial, 0xFF, 1));
    }
}

TEST(aLocalAdoptDescriptorPolicyExhaustsKindsCountsAndBoundaryStates)
{
    for (unsigned state = 0; state < 8; ++state) {
        for (const std::size_t pendingBytes : { 0u, 1u, 5u, 37u }) {
            const Host::LocalHandoffDescriptorPosition position {
                .isSelectedHandoff = (state & 1) != 0,
                .hasReceivedRecord = (state & 2) != 0,
                .hasPendingDescriptor = (state & 4) != 0,
                .pendingRecordBytes = pendingBytes,
            };
            CHECK_EQ(Host::awaitsLocalAdoptRecordKind(position), position.isSelectedHandoff && !pendingBytes);
            for (unsigned value = 0; value <= 255; ++value) {
                const auto kind = static_cast<Local::AdoptWireMessage>(value);
                for (std::size_t descriptorCount = 0; descriptorCount <= 2; ++descriptorCount) {
                    bool admitted = false;
                    if (position.isSelectedHandoff) {
                        if (pendingBytes)
                            admitted = !descriptorCount;
                        else if (!position.hasPendingDescriptor) {
                            if (kind == Local::AdoptWireMessage::Prepare)
                                admitted = descriptorCount == 1;
                            else if (kind == Local::AdoptWireMessage::Commit || kind == Local::AdoptWireMessage::Abort || kind == Local::AdoptWireMessage::Status)
                                admitted = !descriptorCount;
                        }
                    }
                    CHECK_EQ(Host::admitsLocalAdoptDescriptorPosition(position, static_cast<std::uint8_t>(value), descriptorCount), admitted);
                }
            }
        }
    }
}

TEST(aLocalReclaimRequiresTheExistingAttachmentAndExactAdoptedOwner)
{
    const Host::LocalReclaimAuthority authority { .peer = { .isSameUserLocalStream = true, .isTrustedOwnerBinary = true, .hasRemoteGreeting = true, .hasDescriptors = false },
        .hasLiveAttachment = true,
        .isGrantedHuman = true,
        .hasCurrentAuthority = true,
        .hasExactAdoptedTarget = true };
    CHECK(Host::admitsLocalReclaim(authority));
    for (int missing = 0; missing < 8; ++missing) {
        auto invalid = authority;
        switch (missing) {
        case 0:
            invalid.peer.isSameUserLocalStream = false;
            break;
        case 1:
            invalid.peer.isTrustedOwnerBinary = false;
            break;
        case 2:
            invalid.peer.hasRemoteGreeting = false;
            break;
        case 3:
            invalid.peer.hasDescriptors = true;
            break;
        case 4:
            invalid.hasLiveAttachment = false;
            break;
        case 5:
            invalid.isGrantedHuman = false;
            break;
        case 6:
            invalid.hasCurrentAuthority = false;
            break;
        case 7:
            invalid.hasExactAdoptedTarget = false;
            break;
        }
        CHECK(!Host::admitsLocalReclaim(invalid));
    }
}

TEST(aReclaimRequestContainsSelectorsButNoResumeCredential)
{
    const auto text = Control::reclaimRequest({ .handle = 7, .daemonInstanceID = 23 }, "reclaim:1");
    const auto request = json(text.c_str());
    CHECK(request.stringOr("method") == "pane.reclaim");
    CHECK(request.stringOr("id") == "reclaim:1");
    const auto* parameters = request.find("params");
    REQUIRE(parameters);
    const auto intent = Control::readReclaimRequest(*parameters);
    REQUIRE(intent);
    CHECK_EQ(intent->handle, 7u);
    CHECK_EQ(intent->daemonInstanceID, 23u);
    CHECK_EQ(parameters->members().size(), 2u);
    for (const auto* invalid : { R"({"pane":0,"daemonInstance":"23"})", R"({"pane":7.5,"daemonInstance":"23"})",
             R"({"pane":4294967296,"daemonInstance":"23"})", R"({"pane":7,"daemonInstance":"023"})",
             R"({"pane":7,"daemonInstance":"0"})", R"({"pane":7,"daemonInstance":"18446744073709551616"})",
             R"({"pane":7,"daemonInstance":"23","resumeProof":"not-authority"})" })
        CHECK(!Control::readReclaimRequest(json(invalid)));
}

TEST(aReclaimReleasedResultMatchesOnlyItsWholeOriginalIdentity)
{
    const auto target = reclaimTarget();
    const auto id = Ubiquitty::Codec::JSON::Value::string("reclaim:1");
    const auto answer = Control::reclaimReleasedResult(&id, target);
    CHECK(Control::isReclaimReleasedResult(answer, "reclaim:1", target));
    CHECK(!Control::isReclaimReleasedResult(answer, "reclaim:2", target));
    for (int changed = 0; changed < 6; ++changed) {
        auto other = target;
        switch (changed) {
        case 0:
            ++other.handle;
            break;
        case 1:
            ++other.sessionID;
            break;
        case 2:
            ++other.daemonInstanceID;
            break;
        case 3:
            ++other.hostInstanceID.low;
            break;
        case 4:
            ++other.attachmentID.low;
            break;
        case 5:
            ++other.paneID.low;
            break;
        }
        CHECK(!Control::isReclaimReleasedResult(answer, "reclaim:1", other));
    }
    auto ambiguous = answer;
    ambiguous.set("error", Ubiquitty::Codec::JSON::Value::object());
    CHECK(!Control::isReclaimReleasedResult(ambiguous, "reclaim:1", target));
    auto duplicate = answer;
    duplicate.set("result", *answer.find("result"));
    CHECK(!Control::isReclaimReleasedResult(duplicate, "reclaim:1", target));
    CHECK(!Control::isReclaimReleasedResult(Ubiquitty::Codec::JSON::Value {}, "reclaim:1", target));
}

UBIQUITTY_TEST_MAIN()

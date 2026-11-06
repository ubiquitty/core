// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Protocol/Sync/FrameIngress.h"
#include "Core/Protocol/Sync/Notify.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "PAL/Capability.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace UbiquittyTest::ConsentFuzz {

namespace Control = Ubiquitty::Protocol::Control;
namespace Sync = Ubiquitty::Protocol::Sync;

/// Checks the typed parameters after the actual Call envelope accepts them.
inline void callText(std::string_view text)
{
    if (text.size() > Sync::maxFrameBodyBytes)
        return;
    const auto request = Sync::decodeCallRequest(text);
    if (request.outcome != Control::RequestOutcome::Dispatch)
        return;
    if (request.method == "consent.respond") {
        if (const auto response = Control::readConsentResponse(request.params)) {
            const auto encoded = Control::consentResponseRequest(*response, "fuzz.response");
            assert(!encoded.empty());
            const auto again = Sync::decodeCallRequest(encoded);
            assert(again.outcome == Control::RequestOutcome::Dispatch);
            assert(again.method == request.method);
            assert(Control::readConsentResponse(again.params) == response);
        }
    } else if (request.method == "consent.revoke") {
        if (const auto revocation = Control::readConsentRevocation(request.params)) {
            const auto encoded = Control::consentRevocationRequest(*revocation, "fuzz.revoke");
            assert(!encoded.empty());
            const auto again = Sync::decodeCallRequest(encoded);
            assert(again.outcome == Control::RequestOutcome::Dispatch);
            assert(again.method == request.method);
            assert(Control::readConsentRevocation(again.params) == revocation);
        }
    }
}

/// Reaches the bounded parameter readers without requiring a request prefix.
inline void parameters(std::string_view text)
{
    if (text.size() > Sync::maxFrameBodyBytes)
        return;
    bool isJSON = false;
    const auto value = Ubiquitty::Codec::JSON::parse(std::string(text), isJSON);
    if (!isJSON)
        return;
    (void)Control::readConsentResponse(value);
    (void)Control::readConsentRevocation(value);
}

/// Re-encodes a record its kind's decoder accepted and requires the very same
/// body back: every record has one canonical encoding.
///
/// - Parameters:
///   - body: The whole `Notify` body.
///   - head: The body split at its kind.
///   - kind: The kind this check covers; a body of another kind is ignored.
///   - decode: The kind's record decoder.
///   - encode: The kind's body encoder.
template<typename Decode, typename Encode>
inline void checkNotifyRoundTrip(std::span<const std::uint8_t> body, const Sync::NotifyHead& head, Sync::NotifyKind kind, Decode decode, Encode encode)
{
    if (head.kind != std::to_underlying(kind))
        return;
    const auto decoded = decode(head.record);
    if (!decoded)
        return;
    const auto encoded = encode(*decoded);
    assert(encoded);
    assert(encoded->size() <= Sync::maxFrameBodyBytes);
    assert(std::ranges::equal(*encoded, body));
}

/// Exercises each notify kind's reader and its one canonical encoding.
///
/// - Parameter body: A candidate `Notify` body, ignored past one frame.
inline void notifyBody(std::span<const std::uint8_t> body)
{
    if (body.size() > Sync::maxFrameBodyBytes)
        return;
    const auto head = Sync::decodeNotifyHead(body);
    if (!head)
        return;
    checkNotifyRoundTrip(body, *head, Sync::NotifyKind::ConsentQuestion, Sync::decodeConsentQuestion, Sync::consentQuestionNotify);
    checkNotifyRoundTrip(body, *head, Sync::NotifyKind::ConsentResolved, Sync::decodeConsentResolved, Sync::consentResolvedNotify);
    checkNotifyRoundTrip(body, *head, Sync::NotifyKind::ConsentStatus, Sync::decodeConsentStatus, Sync::consentStatusNotify);
    checkNotifyRoundTrip(body, *head, Sync::NotifyKind::StructureConsent, Sync::decodeStructureConsent, Sync::structureConsentNotify);
    checkNotifyRoundTrip(body, *head, Sync::NotifyKind::Attachments, Sync::decodeAttachments, Sync::attachmentsNotify);
}

/// Reads current Call/Notify bodies without inventing an alternate wire grammar.
///
/// - Parameters:
///   - kind: The frame kind the body arrived under; kinds other than Call and
///     Notify are ignored.
///   - body: The frame body, ignored past the largest body a frame may carry.
inline void frameBody(Sync::FrameKind kind, std::span<const std::uint8_t> body)
{
    if (body.size() > Sync::maxFrameBodyBytes)
        return;
    if (kind == Sync::FrameKind::Call) {
        if (const auto call = Sync::decodeCall(body))
            callText(call->text);
    } else if (kind == Sync::FrameKind::Notify)
        notifyBody(body);
}

/// Reassembles arbitrary split headers/bodies through production bounded ingress.
///
/// - Parameter input: The bytes, fed in pieces whose sizes the bytes choose.
/// - Returns: The ingress state once the input runs out or ingress stops
///   accepting.
inline Sync::FrameIngress::State stream(std::span<const std::uint8_t> input)
{
    Sync::FrameIngress ingress(Sync::maxFrameBodyBytes);
    while (!input.empty()) {
        const auto bytes = input.first(std::min<std::size_t>(input.front() + 1u, input.size()));
        const auto result = ingress.consume(bytes, [](auto headerBytes) {
            Sync::FrameHeader header;
            Sync::EnvelopeError error;
            switch (Sync::readFrameHeader(headerBytes, header, error)) {
            case Sync::FrameHeaderState::NeedsMore:
                return Sync::FrameHeaderDecision {};
            case Sync::FrameHeaderState::Failed:
                return Sync::FrameHeaderDecision { Sync::FrameHeaderDecision::State::Refused, 0 };
            case Sync::FrameHeaderState::Complete:
                return Sync::FrameHeaderDecision { Sync::FrameHeaderDecision::State::Admitted, header.bodyBytes };
            }
            std::unreachable(); }, [](auto headerBytes, auto body) {
            Sync::FrameHeader header;
            Sync::EnvelopeError error;
            const auto state = Sync::readFrameHeader(headerBytes, header, error);
            assert(state == Sync::FrameHeaderState::Complete);
            assert(header.bodyBytes == body.size());
            frameBody(header.kind, body);
            return Sync::FrameDelivery::Continue; });
        assert(result.consumedBytes <= bytes.size());
        assert(ingress.retainedBytes() <= Sync::maxFrameBodyBytes);
        if (result.state != Sync::FrameIngress::State::Accepting) {
            assert(ingress.consume(bytes, [](auto) { return Sync::FrameHeaderDecision {}; }, [](auto, auto) { return Sync::FrameDelivery::Continue; }).consumedBytes == 0);
            return result.state;
        }
        assert(result.consumedBytes == bytes.size());
        input = input.subspan(result.consumedBytes);
    }
    return Sync::FrameIngress::State::Accepting;
}

/// Keeps deep JSON and envelope paths reachable through at most 64 byte flips.
inline void seeded(std::span<const std::uint8_t> input)
{
    const auto selector = input.empty() ? 0 : input.front();
    Sync::FrameKind kind = Sync::FrameKind::Notify;
    std::optional<std::vector<std::uint8_t>> body;
    const auto textBody = [](const std::string& text) { return std::optional { std::vector<std::uint8_t>(text.begin(), text.end()) }; };
    switch (selector % 7) {
    case 0: {
        kind = Sync::FrameKind::Call;
        const auto questionKind = static_cast<Sync::ConsentKind>(selector % 4);
        const auto decision = static_cast<Sync::ConsentChoice>((selector / 4) % (questionKind == Sync::ConsentKind::Structure ? 2 : 4));
        body = textBody(Control::consentResponseRequest({ 7, questionKind, decision }, "fuzz.response"));
        break;
    }
    case 1: {
        kind = Sync::FrameKind::Call;
        const auto action = static_cast<Sync::ConsentRevocationKind>(selector % 4);
        Control::ConsentRevocation revocation { .sessionID = 9, .action = action, .attachmentID = {}, .identity = {} };
        if (action == Sync::ConsentRevocationKind::DisconnectNow)
            revocation.attachmentID = { 11, 13 };
        else if (action != Sync::ConsentRevocationKind::DisconnectAll)
            revocation.identity = "verified grant";
        body = textBody(Control::consentRevocationRequest(revocation, "fuzz.revoke"));
        break;
    }
    case 2: {
        Sync::ConsentQuestion question { .requestID = 7, .sessionID = 9, .peerProcessID = 23, .peerProcess = "/verified/tool", .agentLabel = "claimed label", .action = static_cast<std::uint8_t>(selector % 3) };
        if (question.action == std::to_underlying(Sync::ConsentKind::Capability)) {
            question.capabilityCode = std::to_underlying(Ubiquitty::Capability::ClipboardRead);
            question.capabilityOrigin = std::to_underlying(Ubiquitty::CapabilityOrigin::Remote);
            question.isRecipientsOwnRequest = true;
        }
        body = Sync::consentQuestionNotify(question);
        break;
    }
    case 3:
        body = Sync::consentResolvedNotify({ static_cast<Sync::ConsentKind>(selector % 4), 7, 9 });
        break;
    case 4:
        body = Sync::consentStatusNotify({ { 7, 9 }, static_cast<Sync::ConsentStatusPhase>(selector % 4) });
        break;
    case 5:
        body = Sync::structureConsentNotify({ .requestID = 7, .sessionID = 9, .workspaceID = 3, .revision = 1, .workspaceName = "work", .method = "workspace.rename", .parameters = R"({"workspace":3,"name":"work"})", .agentLabel = "claimed label", .peerProcess = "/verified/tool", .peerProcessID = 23 });
        break;
    case 6: {
        Sync::AttachmentsList list { .revision = 1, .attachments = {} };
        Sync::AttachmentEntry attachment { .attachmentID = { 11, 13 }, .role = Sync::AttachmentRole::Driver, .panes = { { 17, 19 } }, .isSelf = true, .principal = std::nullopt };
        attachment.presence = Sync::AttachmentPresence { .clientID = 5, .processID = 23, .process = "/verified/tool", .claimedLabel = "claimed label", .grantIdentity = "verified grant", .isQUIC = true, .sharedPanes = { { { 17, 19 }, static_cast<Sync::PresenceGrant>(selector % 3) } } };
        list.attachments.push_back(std::move(attachment));
        body = Sync::attachmentsNotify(list);
        break;
    }
    }
    assert(body && !body->empty());
    if (kind == Sync::FrameKind::Call) {
        const auto request = Sync::decodeCallRequest(std::string_view(reinterpret_cast<const char*>(body->data()), body->size()));
        assert(request.outcome == Control::RequestOutcome::Dispatch);
        assert(selector % 7 == 0 ? Control::readConsentResponse(request.params).has_value() : Control::readConsentRevocation(request.params).has_value());
    } else {
        const auto head = Sync::decodeNotifyHead(*body);
        assert(head);
        assert(Sync::notifyKindOf(head->kind));
    }
    const auto exercise = [&](std::span<const std::uint8_t> value) {
        frameBody(kind, value);
        std::vector<std::uint8_t> framed;
        if (Sync::appendFrame(framed, kind, 0, value))
            stream(framed);
    };
    exercise(*body);
    const auto count = std::min<std::size_t>(input.size(), 128);
    for (std::size_t index = 0; index + 1 < count; index += 2)
        (*body)[input[index] % body->size()] ^= input[index + 1];
    exercise(*body);
}

} // namespace UbiquittyTest::ConsentFuzz

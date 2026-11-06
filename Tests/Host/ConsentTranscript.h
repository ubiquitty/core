// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/FrameIngress.h"
#include "Core/Protocol/Sync/Notify.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::HostTest {

/// Pure transcript checks for the socket fixtures. Admission is checked while
/// reading each frame, never inferred from which queued status a caller asks for.
class ConsentTranscript {
public:
    bool feed(std::span<const std::byte> bytes)
    {
        if (m_failed)
            return false;
        using namespace Protocol::Sync;
        const std::span<const std::uint8_t> raw { reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() };
        const auto result = m_ingress.consume(raw, [&](std::span<const std::uint8_t> prefix) {
            FrameHeader header;
            EnvelopeError error = EnvelopeError::None;
            const auto state = readFrameHeader(prefix, header, error);
            if (state == FrameHeaderState::NeedsMore)
                return FrameHeaderDecision {};
            bool isAdmitted = state == FrameHeaderState::Complete && m_state != State::Closed
                && directionOf(header.kind) != Direction::ClientToHost;
            if (m_state == State::BeforeWelcome)
                isAdmitted = isAdmitted && (header.kind == FrameKind::Welcome || header.kind == FrameKind::Bye);
            else if (header.kind == FrameKind::Welcome)
                isAdmitted = false;
            if (m_state == State::AwaitingPending || m_state == State::Pending) {
                isAdmitted = isAdmitted && (header.kind == FrameKind::Bye
                    || (header.kind == FrameKind::Notify && header.bodyBytes <= maxConsentStatusBytes)
                    || (header.kind == FrameKind::Probe && header.bodyBytes == 8)
                    || (header.kind == FrameKind::Echo && header.bodyBytes == 12));
            }
            if (m_state == State::Denied || m_state == State::Expired)
                isAdmitted = isAdmitted && header.kind == FrameKind::Bye;
            return FrameHeaderDecision { .state = isAdmitted ? FrameHeaderDecision::State::Admitted : FrameHeaderDecision::State::Refused,
                .bodyBytes = header.bodyBytes }; }, [&](std::span<const std::uint8_t> prefix, std::span<const std::uint8_t> body) {
            FrameHeader header;
            EnvelopeError error = EnvelopeError::None;
            m_failed = readFrameHeader(prefix, header, error) != FrameHeaderState::Complete
                || !read({ .kind = header.kind, .channel = header.channel, .body = body });
            return m_failed ? Protocol::Sync::FrameDelivery::Stop : Protocol::Sync::FrameDelivery::Continue; });
        m_failed = m_failed || result.state != Protocol::Sync::FrameIngress::State::Accepting;
        return !m_failed;
    }

    [[nodiscard]] bool hasFailed() const { return m_failed; }
    [[nodiscard]] bool isGranted() const { return !m_failed && m_state == State::Admitted; }

    std::optional<Protocol::Sync::ConsentQuestion> takeQuestion(Protocol::Sync::ConsentKind kind)
    {
        if (m_failed)
            return std::nullopt;
        const auto found = std::ranges::find(m_questions, std::to_underlying(kind), &Protocol::Sync::ConsentQuestion::action);
        if (found == m_questions.end())
            return std::nullopt;
        auto result = std::move(*found);
        m_questions.erase(found);
        return result;
    }

    std::optional<Protocol::Sync::ConsentStatus> takeStatus(Protocol::Sync::ConsentStatusPhase phase)
    {
        if (m_failed)
            return std::nullopt;
        const auto found = std::ranges::find(m_statuses, phase, &Protocol::Sync::ConsentStatus::phase);
        if (found == m_statuses.end())
            return std::nullopt;
        auto result = *found;
        m_statuses.erase(found);
        return result;
    }

    std::optional<Protocol::Sync::AttachmentsList> takeRoster()
    {
        return m_failed ? std::nullopt : std::exchange(m_roster, std::nullopt);
    }

    std::optional<Codec::JSON::Value> takeResult(std::string_view id)
    {
        if (m_failed)
            return std::nullopt;
        const auto found = std::ranges::find_if(m_results, [&](const auto& result) { return result.stringOr("id") == id; });
        if (found == m_results.end())
            return std::nullopt;
        auto result = std::move(*found);
        m_results.erase(found);
        return result;
    }

private:
    enum class State : std::uint8_t {
        BeforeWelcome,
        AwaitingPending,
        Pending,
        Admitted,
        Denied,
        Expired,
        Closed,
    };

    bool read(const Protocol::Sync::Frame& frame)
    {
        using namespace Protocol::Sync;
        if (m_state == State::Closed)
            return false;
        if (frame.kind == FrameKind::Bye) {
            const auto bye = decodeBye(frame.body);
            if (!bye || frame.channel || ((m_state == State::Denied || m_state == State::Expired) && bye->reason != ByeReason::NoConsent))
                return false;
            m_state = State::Closed;
            return true;
        }
        if (m_state == State::Denied || m_state == State::Expired)
            return false;
        if (frame.kind == FrameKind::Probe)
            return frame.channel == 0 && decodeProbe(frame.body).has_value();
        if (frame.kind == FrameKind::Echo)
            return frame.channel == 0 && decodeEcho(frame.body).has_value();
        if (frame.kind == FrameKind::Welcome) {
            const auto welcome = decodeWelcome(frame.body);
            if (frame.channel || !welcome || m_state != State::BeforeWelcome)
                return false;
            m_question = welcome->consent;
            m_state = welcome->admission == AttachmentAdmission::Granted ? State::Admitted : State::AwaitingPending;
            return true;
        }
        if (m_state == State::BeforeWelcome)
            return false;
        if (frame.kind == FrameKind::Notify) {
            const auto notify = decodeNotifyHead(frame.body);
            if (!notify || frame.channel)
                return false;
            const bool isStatus = notify->kind == std::to_underlying(NotifyKind::ConsentStatus);
            if (m_state != State::Admitted) {
                const auto status = isStatus ? decodeConsentStatus(notify->record) : std::nullopt;
                if (!status || status->question != m_question)
                    return false;
                if (m_state == State::AwaitingPending) {
                    if (status->phase != ConsentStatusPhase::Pending)
                        return false;
                    m_state = State::Pending;
                } else {
                    switch (status->phase) {
                    case ConsentStatusPhase::Pending:
                        return false;
                    case ConsentStatusPhase::Approved:
                        m_state = State::Admitted;
                        break;
                    case ConsentStatusPhase::Denied:
                        m_state = State::Denied;
                        break;
                    case ConsentStatusPhase::Expired:
                        m_state = State::Expired;
                        break;
                    }
                }
                m_statuses.push_back(*status);
                return true;
            }
            if (isStatus)
                return false;
            if (notify->kind == std::to_underlying(NotifyKind::ConsentQuestion)) {
                auto question = decodeConsentQuestion(notify->record);
                if (!question)
                    return false;
                m_questions.push_back(std::move(*question));
            } else if (notify->kind == std::to_underlying(NotifyKind::Attachments)) {
                auto roster = decodeAttachments(notify->record);
                if (!roster)
                    return false;
                m_roster = std::move(*roster);
            }
            return true;
        }
        // Includes Results and every picture, record, effect, structure, and
        // bulk frame: none is admitted before the exact Approved transition.
        if (m_state != State::Admitted)
            return false;
        if (frame.kind == FrameKind::Result) {
            const auto result = decodeResult(frame.body);
            if (frame.channel || !result || result->transfer)
                return false;
            bool isJSON = false;
            auto answer = Codec::JSON::parse(result->text, isJSON);
            const auto* id = answer.find("id");
            const auto* success = answer.find("result");
            const auto* error = answer.find("error");
            if (!isJSON || !answer.isObject() || answer.stringOr("jsonrpc") != "2.0" || !id || !id->isString() || id->asString().empty() || (success != nullptr) == (error != nullptr)
                || !Protocol::Control::ConsentDetail::exactMembers(answer, { "jsonrpc", "id", success ? "result" : "error" }))
                return false;
            if (error) {
                const auto* code = error->find("code");
                const auto* message = error->find("message");
                if (!error->isObject() || !code || !code->isNumber() || !std::isfinite(code->asNumber())
                    || code->asNumber() < INT32_MIN || code->asNumber() > INT32_MAX || std::trunc(code->asNumber()) != code->asNumber() || !message || !message->isString())
                    return false;
                if (error->find("data") ? !Protocol::Control::ConsentDetail::exactMembers(*error, { "code", "message", "data" })
                                        : !Protocol::Control::ConsentDetail::exactMembers(*error, { "code", "message" }))
                    return false;
            }
            m_results.push_back(std::move(answer));
        }
        return true;
    }

    Protocol::Sync::FrameIngress m_ingress { Protocol::Sync::maxFrameBodyBytes };
    State m_state = State::BeforeWelcome;
    Protocol::Sync::ConsentBinding m_question;
    std::vector<Protocol::Sync::ConsentQuestion> m_questions;
    std::vector<Protocol::Sync::ConsentStatus> m_statuses;
    std::optional<Protocol::Sync::AttachmentsList> m_roster;
    std::vector<Codec::JSON::Value> m_results;
    bool m_failed = false;
};

} // namespace Ubiquitty::HostTest

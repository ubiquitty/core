// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/consent_remote_clipboard.cpp — a program driven from the owner's
// phone copies with OSC 52, and the phone is asked.
//
// A remote principal's clipboard write is host-escaping, and the tier's remote
// default is a hard Deny; the per-capability refinement makes this one a Prompt
// (defaultDecision(Capability, CapabilityOrigin)). That is worth something only
// if the whole loop closes on the daemon: the program's write is refused and
// queued, the question goes to the phone (a remote client, which is the human
// on the session), the phone's answer records a grant for the phone's own
// principal, and that original write passes the gate to the session Terminal's
// clipboard sink. Each step is exercised here through the daemon's own code —
// servicePTY parses the output and drains the queue, the canonical Call takes the
// answer — so a break anywhere in it reads as a failure rather than as a
// quiet session.
//
// The sink is a recording ClipboardClient installed on the session Terminal,
// the seam a frontend installs. This fixture replaces the host sink, so this
// pins the gate and the consent loop, not delivery to the phone's pasteboard.
//
// Exit 0 on success.
//

#include "Platform/Host/Daemon.h"

#include "Assertions.h"
#include "NotifyRead.h"
#include "Platform/StreamDouble.h"
#include "ScreenPlaneAccess.h"

#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Sync/EffectCodec.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Capability.h"
#include "PAL/TerminalClients.h"

#include <array>
#include <cstdio>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace Ubiquitty {
namespace Host {

struct ConsentRemoteClipboardAccess {
    // A session wired the way createSession wires one, under `decisions`: the
    // policy reaches the host-wide grant store and consent queue, and the
    // Terminal routes its checks through it.
    static Daemon::Session& addSession(Daemon& d, std::uint32_t id, const CapabilityDecisionTable& decisions, TestDoubles::StreamDouble** streamOut)
    {
        auto session = std::make_unique<Daemon::Session>(decisions);
        session->id = id;
        session->term = std::make_unique<Terminal>();
        resize(*session->term, 24, 80);
        session->capabilityPolicy.setGrantStore(&d.m_grantStore);
        session->capabilityPolicy.setConsentQueue(&d.m_consentQueue);
        session->term->capabilityPolicy = &session->capabilityPolicy;
        auto stream = std::make_unique<TestDoubles::StreamDouble>();
        *streamOut = stream.get();
        session->stream = std::move(stream);
        session->screenSource = std::make_unique<Protocol::Sync::PaneSource>(*session->term, Protocol::Sync::ID128 { .low = id, .high = 1 });
        Daemon::Session& ref = *session;
        d.m_sessions.push_back(std::move(session));
        return ref;
    }

    // Models the verified interactive-bootstrap authority at the carrier's
    // return boundary, not token minting, TLS or preamble verification. The
    // physical connection has not greeted or attached; canonical admission
    // must still bind the exact fixture-owned session. Shared principals also
    // model an existing attachment's identity on another connection.
    static std::size_t addPhone(Daemon& d, std::uint32_t sessionID)
    {
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.hasToken = true;
        c.hasBootstrapToken = true;
        c.isInteractiveUser = true;
        c.tokenID = "owner-phone";
        c.tokenScope = { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive };
        // This is the fixture-selected target, not an admitted subscription.
        c.sessionID = sessionID;
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Human, Protocol::Sync::Role::Driver);
        const auto& admitted = d.m_clients[index];
        CHECK(admitted.syncConnection && admitted.syncConnection->state() == Protocol::Sync::ConnectionState::Attached);
        CHECK(admitted.helloSeen && admitted.sessionID == sessionID && admitted.subscriptionFor(sessionID));
        CHECK(d.hasLiveStructureAttachment(admitted));
        return index;
    }

    static CapabilityOrigin originOf(const Daemon& d, std::size_t index) { return d.originOf(d.m_clients[index]); }
    static bool isHuman(const Daemon& d, std::size_t index) { return !d.isAgentClient(d.m_clients[index]); }
    static std::string grantKey(const Daemon& d, std::size_t index) { return d.grantKeyFor(d.m_clients[index]); }

    // What `stampInputOrigin` stamps when the phone types.
    static void stampDriver(Daemon& d, Daemon::Session& s, std::size_t index)
    {
        s.capabilityPolicy.setCurrentIdentity(d.grantKeyFor(d.m_clients[index]));
        d.setSessionInputDriver(s, d.m_clients[index].clientID);
    }

    static void service(Daemon& d, Daemon::Session& s) { d.servicePTY(s); }
    static void expire(Daemon& d) { d.expireCapabilityQuestions(std::numeric_limits<std::uint64_t>::max()); }
    static void drop(Daemon& d, std::size_t index) { d.dropClient(index); }
    static void revoke(Daemon& d, std::size_t index)
    {
        const std::string identity = d.grantKeyFor(d.m_clients[index]);
        d.m_grantStore.revokeIdentity(identity);
        d.m_consentQueue.resolveIdentity(identity);
        d.withdrawPendingGrantsFor(identity);
    }
    static void loseDrive(Daemon& d, std::size_t index) { d.m_clients[index].tokenScope = { Protocol::Sync::Scope::Observe }; }
    static void changeIdentity(Daemon& d, std::size_t index) { d.m_clients[index].tokenID = "replacement-identity"; }
    static void expireToken(Daemon& d, std::size_t index) { d.m_clients[index].tokenExpiryUnix = 1; }
    static void makeAgent(Daemon& d, std::size_t index)
    {
        d.m_clients[index].isInteractiveUser = false;
        d.m_clients[index].agentLabel = "agent";
        d.m_clients[index].tokenID = "agent-token";
    }
    static bool hasHeld(Daemon::Session& s) { return s.capabilityPolicy.pendingClipboardWrite() != nullptr; }

    // Every canonical consent question queued since the last call. The shared
    // drain moves the real connection's canonical frames into the physical buffer.
    static std::vector<Protocol::Sync::ConsentQuestion> takeConsentRequests(Daemon& d, std::size_t index)
    {
        std::vector<Protocol::Sync::ConsentQuestion> requests;
        d.drainSyncConnection(index, ScreenPlaneAccess::now());
        const auto out = ScreenPlaneAccess::takeOutput(d, index);
        Protocol::Sync::FrameDecoder decoder;
        decoder.append(out);
        while (const auto frame = decoder.next()) {
            if (frame->kind != Protocol::Sync::FrameKind::Notify)
                continue;
            CHECK_EQ(frame->channel, 0u);
            if (const auto notify = UbiquittyTest::notifyOf(frame->body))
                if (const auto request = UbiquittyTest::consentQuestionOf(*notify))
                    requests.push_back(*request);
        }
        CHECK(decoder.error() == Protocol::Sync::EnvelopeError::None);
        CHECK_EQ(decoder.bufferedBytes(), 0u);
        return requests;
    }

    static void answer(Daemon& d, std::size_t index, std::uint32_t requestID, Protocol::Sync::ConsentChoice decision)
    {
        const auto call = Protocol::Control::consentResponseRequest(
            { requestID, Protocol::Sync::ConsentKind::Capability, decision }, "fixture.answer");
        CHECK(!call.empty());
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, Protocol::Sync::Call { call });
        std::vector<std::uint8_t> stream;
        CHECK(Protocol::Sync::appendFrame(stream, Protocol::Sync::FrameKind::Call, 0, body));
        d.receiveScreenBytes(index, stream);
        d.serviceSyncConnection(index, ScreenPlaneAccess::now());
    }

    static bool isGranted(const Daemon& d, Capability capability, const std::string& identity) { return d.m_grantStore.isGranted(capability, CapabilityOrigin::Remote, identity); }
    static void grantAlways(Daemon& d, Capability capability, const std::string& identity) { d.m_grantStore.grant(capability, CapabilityOrigin::Remote, identity, CapabilityGrantScope::Always); }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Capability;
using Ubiquitty::CapabilityOrigin;
using Ubiquitty::Host::Daemon;
using Ubiquitty::TestDoubles::StreamDouble;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

using A = Ubiquitty::Host::ConsentRemoteClipboardAccess;

// OSC 52 copy of "copied" to the clipboard selection, as vim's osc52 plugin sends it.
constexpr const char* copies = "\033]52;c;Y29waWVk\a";

// The session Terminal's clipboard sink: what the gate lets through, in order.
class RecordingClipboard final : public Ubiquitty::ClipboardClient {
public:
    void writeClipboard(Ubiquitty::ClipboardBuffer buffer, std::span<const char> bytes) override
    {
        writes.emplace_back(bytes.begin(), bytes.end());
        buffers.push_back(buffer);
    }

    std::vector<std::string> writes;
    std::vector<Ubiquitty::ClipboardBuffer> buffers;
};

Ubiquitty::CapabilityDecisionTable decisionsFrom(const std::string& text)
{
    Ubiquitty::Preferences::Values values;
    std::istringstream input(text);
    CHECK_MSG(Ubiquitty::Preferences::parse(input, values, Ubiquitty::Preferences::ParseMode::Strict), text.c_str());
    return Ubiquitty::Preferences::Effective::capabilityDecisions(values);
}

} // namespace

int main()
{
    // ---- 1. The first exact copy waits for the answer; no second OSC 52 is
    // needed, and Allow Once is consumed by that original operation.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, decisionsFrom(""), &stream);
        RecordingClipboard clipboard;
        s.term->clipboardClient = &clipboard;
        const std::size_t phone = A::addPhone(d, 1);
        CHECK(A::originOf(d, phone) == CapabilityOrigin::Remote);
        CHECK(A::isHuman(d, phone));
        A::stampDriver(d, s, phone);

        stream->pushInbound(copies);
        A::service(d, s);
        CHECK(s.term->currentCapabilityOrigin == CapabilityOrigin::Remote);
        CHECK_MSG(clipboard.writes.empty(), "the first remote copy was not held for consent");

        // A program repeating the copy while the question is open asks nothing more.
        stream->pushInbound("\033]52;c;cmVwbGFjZWQ=\a"); // not the operation being approved
        A::service(d, s);
        CHECK(clipboard.writes.empty());

        const std::vector<Sync::ConsentQuestion> asked = A::takeConsentRequests(d, phone);
        CHECK_MSG(asked.size() == 1, "the phone was not asked exactly once");
        if (asked.size() == 1) {
            CHECK(asked[0].action == 2); // a capability grant
            CHECK(asked[0].sessionID == 1);
            CHECK(asked[0].capabilityCode == static_cast<std::uint16_t>(std::to_underlying(Capability::ClipboardWrite)));
            CHECK(asked[0].isRecipientsOwnRequest);
            CHECK(asked[0].agentLabel.empty()); // no claimed label or verified local process on this phone
            CHECK(asked[0].peerProcessID == 0 && asked[0].peerProcess.empty());

            A::answer(d, phone, asked[0].requestID, Sync::ConsentChoice::AllowOnce);
            CHECK(!A::isGranted(d, Capability::ClipboardWrite, A::grantKey(d, phone)));
            CHECK_MSG(clipboard.writes.size() == 1, "approval did not deliver the original copy without another OSC 52");
            if (clipboard.writes.size() == 1)
                CHECK(clipboard.writes[0] == "copied");
            A::answer(d, phone, asked[0].requestID, Sync::ConsentChoice::AllowOnce);
            CHECK(clipboard.writes.size() == 1);
            stream->pushInbound(copies);
            A::service(d, s);
            CHECK(clipboard.writes.size() == 1);
            CHECK(A::takeConsentRequests(d, phone).size() == 1);
        }
        s.term->clipboardClient = nullptr;
    }

    // A complete chunked write asks once, and one permission covers
    // its distinct selectors rather than being spent on the first chunk.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, decisionsFrom(""), &stream);
        RecordingClipboard clipboard;
        s.term->clipboardClient = &clipboard;
        const auto phone = A::addPhone(d, 1);
        A::stampDriver(d, s, phone);
        stream->pushInbound("\033]52;pcpc;Y29w!\a");
        A::service(d, s);
        CHECK(A::takeConsentRequests(d, phone).empty());
        stream->pushInbound("\033]52;pcpc;aWVk\a");
        A::service(d, s);
        const auto asked = A::takeConsentRequests(d, phone);
        CHECK(asked.size() == 1);
        if (asked.size() == 1)
            A::answer(d, phone, asked[0].requestID, Sync::ConsentChoice::AllowOnce);
        CHECK(clipboard.writes == std::vector<std::string>({ "copied", "copied" }));
        CHECK(clipboard.buffers == std::vector<Ubiquitty::ClipboardBuffer>({ Ubiquitty::ClipboardBuffer::Primary, Ubiquitty::ClipboardBuffer::Clipboard }));
        CHECK(!A::isGranted(d, Capability::ClipboardWrite, A::grantKey(d, phone)));
        CHECK(!A::hasHeld(s));
        s.term->clipboardClient = nullptr;
    }

    // A missing data field, incomplete transfer or invalid selector is not
    // an approvable operation. The invalid selector also abandons the chunk.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, decisionsFrom(""), &stream);
        RecordingClipboard clipboard;
        s.term->clipboardClient = &clipboard;
        const auto phone = A::addPhone(d, 1);
        A::stampDriver(d, s, phone);
        for (const char* fragment : { "\033]52;c\a", "\033]52;c;Y29w!\a", "\033]52;x;%%%%\a" }) {
            stream->pushInbound(fragment);
            A::service(d, s);
            CHECK(clipboard.writes.empty());
            CHECK(!A::hasHeld(s));
            CHECK(A::takeConsentRequests(d, phone).empty());
        }
        stream->pushInbound(copies);
        A::service(d, s);
        const auto asked = A::takeConsentRequests(d, phone);
        CHECK(asked.size() == 1);
        if (asked.size() == 1)
            A::answer(d, phone, asked[0].requestID, Sync::ConsentChoice::AllowOnce);
        CHECK(clipboard.writes == std::vector<std::string>({ "copied" }));
        s.term->clipboardClient = nullptr;
    }

    // Non-base64 data clears the named selection, including a malformed
    // completed transfer. Clearing is a write: the phone must consent before
    // an empty payload reaches its clipboard, and denial leaves it untouched.
    for (const char* clear : { "\033]52;c;%%%%\a", "\033]52;c;!\a", "\033]52;c;Y29w!\a\033]52;c;%%%%\a" }) {
        for (const auto choice : { Sync::ConsentChoice::AllowOnce, Sync::ConsentChoice::Deny }) {
            Daemon d;
            StreamDouble* stream = nullptr;
            auto& s = A::addSession(d, 1, decisionsFrom(""), &stream);
            RecordingClipboard clipboard;
            s.term->clipboardClient = &clipboard;
            const auto phone = A::addPhone(d, 1);
            A::stampDriver(d, s, phone);
            stream->pushInbound(clear);
            A::service(d, s);
            CHECK(clipboard.writes.empty());
            CHECK(A::hasHeld(s));
            const auto asked = A::takeConsentRequests(d, phone);
            CHECK(asked.size() == 1);
            if (asked.size() == 1) {
                CHECK(asked[0].capabilityCode == static_cast<std::uint16_t>(std::to_underlying(Capability::ClipboardWrite)));
                CHECK(asked[0].isRecipientsOwnRequest);
                A::answer(d, phone, asked[0].requestID, choice);
                CHECK(!A::hasHeld(s));
                CHECK(!A::isGranted(d, Capability::ClipboardWrite, A::grantKey(d, phone)));
                if (choice == Sync::ConsentChoice::AllowOnce) {
                    CHECK(clipboard.writes == std::vector<std::string>({ "" }));
                    CHECK(clipboard.buffers == std::vector<Ubiquitty::ClipboardBuffer>({ Ubiquitty::ClipboardBuffer::Clipboard }));
                } else
                    CHECK(clipboard.writes.empty());

                const auto writeCount = clipboard.writes.size();
                A::answer(d, phone, asked[0].requestID, Sync::ConsentChoice::AllowOnce);
                CHECK(clipboard.writes.size() == writeCount);
                stream->pushInbound(copies);
                A::service(d, s);
                CHECK(clipboard.writes.size() == writeCount);
                const auto copyRequests = A::takeConsentRequests(d, phone);
                CHECK(copyRequests.size() == 1);
                if (copyRequests.size() == 1)
                    A::answer(d, phone, copyRequests[0].requestID, Sync::ConsentChoice::AllowOnce);
                if (choice == Sync::ConsentChoice::AllowOnce)
                    CHECK(clipboard.writes == std::vector<std::string>({ "", "copied" }));
                else
                    CHECK(clipboard.writes == std::vector<std::string>({ "copied" }));
                CHECK(!A::hasHeld(s));
            }
            s.term->clipboardClient = nullptr;
        }
    }

    // Every invalidation releases the payload. A saved answer cannot
    // revive it, even if the same principal remains on another connection.
    enum class Invalidation {
        Deny,
        Expire,
        Drop,
        Revoke,
        LoseDrive,
        ChangeIdentity,
        ExpireToken,
        HardDeny,
        DisableOSC52,
        ChangePrimary
    };
    for (Invalidation invalidation : { Invalidation::Deny, Invalidation::Expire, Invalidation::Drop, Invalidation::Revoke,
             Invalidation::LoseDrive, Invalidation::ChangeIdentity, Invalidation::ExpireToken, Invalidation::HardDeny, Invalidation::DisableOSC52, Invalidation::ChangePrimary }) {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, decisionsFrom(""), &stream);
        RecordingClipboard clipboard;
        s.term->clipboardClient = &clipboard;
        const auto phone = A::addPhone(d, 1);
        A::stampDriver(d, s, phone);
        stream->pushInbound(copies);
        A::service(d, s);
        const auto asked = A::takeConsentRequests(d, phone);
        CHECK(asked.size() == 1);
        CHECK(A::hasHeld(s));
        switch (invalidation) {
        case Invalidation::Deny:
            break;
        case Invalidation::Expire:
            A::expire(d);
            break;
        case Invalidation::Drop:
            A::drop(d, phone);
            break;
        case Invalidation::Revoke:
            A::revoke(d, phone);
            break;
        case Invalidation::LoseDrive:
            A::loseDrive(d, phone);
            break;
        case Invalidation::ChangeIdentity:
            A::changeIdentity(d, phone);
            break;
        case Invalidation::ExpireToken:
            A::expireToken(d, phone);
            break;
        case Invalidation::HardDeny:
            s.term->osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Deny;
            break;
        case Invalidation::DisableOSC52:
            s.term->isOSC52Allowed = false;
            break;
        case Invalidation::ChangePrimary: {
            const auto other = A::addPhone(d, 1);
            A::stampDriver(d, s, other);
            if (asked.size() == 1)
                A::answer(d, other, asked[0].requestID, Sync::ConsentChoice::AllowOnce);
            CHECK(clipboard.writes.empty());
            break;
        }
        }
        if (asked.size() == 1)
            A::answer(d, phone, asked[0].requestID, invalidation == Invalidation::Deny ? Sync::ConsentChoice::Deny : Sync::ConsentChoice::AllowOnce);
        CHECK(clipboard.writes.empty());
        CHECK(!A::hasHeld(s));
        s.term->clipboardClient = nullptr;
    }

    // Empty means clear, still an operation. It needs no allocation;
    // a nonempty allocation refusal and an over-limit body are fail-closed.
    {
        using Held = Ubiquitty::HostCapabilityPolicy::PendingClipboardWrite;
        const std::array buffers { Ubiquitty::ClipboardBuffer::Clipboard };
        const auto refuse = +[](std::size_t) { return std::unique_ptr<char[]> {}; };
        CHECK(Held::capture(CapabilityOrigin::Remote, "principal", buffers, {}, refuse).has_value());
        CHECK(!Held::capture(CapabilityOrigin::Remote, "principal", buffers, std::span<const char>("x", 1), refuse));
        const std::string maximum(Ubiquitty::Protocol::Sync::maxEffectClipboardWriteBytes, 'x');
        auto exact = Held::capture(CapabilityOrigin::Remote, "principal", buffers, maximum);
        CHECK(exact && exact->byteCount == maximum.size());
        const std::string tooLong(Ubiquitty::Protocol::Sync::maxEffectClipboardWriteBytes + 1, 'x');
        CHECK(!Held::capture(CapabilityOrigin::Remote, "principal", buffers, tooLong));
        CHECK(!Held::capture(CapabilityOrigin::Remote, {}, buffers, maximum));
    }

    // A question deduplicated for one principal never adopts another pane's
    // bytes. That pane can ask afresh once the first operation is resolved.
    {
        Daemon d;
        StreamDouble* firstStream = nullptr;
        StreamDouble* secondStream = nullptr;
        auto& first = A::addSession(d, 1, decisionsFrom(""), &firstStream);
        auto& second = A::addSession(d, 2, decisionsFrom(""), &secondStream);
        RecordingClipboard firstClipboard;
        RecordingClipboard secondClipboard;
        first.term->clipboardClient = &firstClipboard;
        second.term->clipboardClient = &secondClipboard;
        const auto firstPhone = A::addPhone(d, 1);
        const auto secondPhone = A::addPhone(d, 2);
        A::stampDriver(d, first, firstPhone);
        A::stampDriver(d, second, secondPhone);
        firstStream->pushInbound(copies);
        A::service(d, first);
        secondStream->pushInbound("\033]52;c;c2Vjb25k\a");
        A::service(d, second);
        CHECK(!A::hasHeld(second));
        CHECK(A::takeConsentRequests(d, secondPhone).empty());
        const auto firstAsked = A::takeConsentRequests(d, firstPhone);
        CHECK(firstAsked.size() == 1);
        if (firstAsked.size() == 1) {
            A::answer(d, secondPhone, firstAsked[0].requestID, Sync::ConsentChoice::AllowOnce);
            CHECK(firstClipboard.writes.empty());
            A::answer(d, firstPhone, firstAsked[0].requestID, Sync::ConsentChoice::AllowOnce);
        }
        CHECK(firstClipboard.writes == std::vector<std::string>({ "copied" }));
        CHECK(secondClipboard.writes.empty());
        secondStream->pushInbound("\033]52;c;c2Vjb25k\a");
        A::service(d, second);
        const auto secondAsked = A::takeConsentRequests(d, secondPhone);
        CHECK(secondAsked.size() == 1);
        if (secondAsked.size() == 1)
            A::answer(d, secondPhone, secondAsked[0].requestID, Sync::ConsentChoice::AllowOnce);
        CHECK(secondClipboard.writes == std::vector<std::string>({ "second" }));
        first.term->clipboardClient = nullptr;
        second.term->clipboardClient = nullptr;
    }

    // When the source is an agent and the clipboard belongs to a human,
    // either endpoint leaving cancels the exact operation.
    for (const bool shouldDropSource : { false, true }) {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, decisionsFrom(""), &stream);
        RecordingClipboard clipboard;
        s.term->clipboardClient = &clipboard;
        const auto phone = A::addPhone(d, 1);
        const auto agent = A::addPhone(d, 1);
        A::makeAgent(d, agent);
        A::stampDriver(d, s, phone);
        A::stampDriver(d, s, agent);
        stream->pushInbound(copies);
        A::service(d, s);
        const auto asked = A::takeConsentRequests(d, phone);
        CHECK(asked.size() == 1);
        CHECK(A::takeConsentRequests(d, agent).empty());
        A::drop(d, shouldDropSource ? agent : phone);
        CHECK(!A::hasHeld(s));
        if (asked.size() == 1)
            A::answer(d, phone, asked[0].requestID, Sync::ConsentChoice::AllowOnce);
        CHECK(clipboard.writes.empty());
        s.term->clipboardClient = nullptr;
    }

    // Empty copy clears only after approval; Allow Session authorizes later
    // writes without retaining or prompting again.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, decisionsFrom(""), &stream);
        RecordingClipboard clipboard;
        s.term->clipboardClient = &clipboard;
        const auto phone = A::addPhone(d, 1);
        A::stampDriver(d, s, phone);
        stream->pushInbound("\033]52;c;\a");
        A::service(d, s);
        CHECK(clipboard.writes.empty());
        const auto asked = A::takeConsentRequests(d, phone);
        CHECK(asked.size() == 1);
        if (asked.size() == 1)
            A::answer(d, phone, asked[0].requestID, Sync::ConsentChoice::AllowSession);
        CHECK(clipboard.writes == std::vector<std::string>({ "" }));
        stream->pushInbound(copies);
        A::service(d, s);
        CHECK(clipboard.writes == std::vector<std::string>({ "", "copied" }));
        CHECK(A::takeConsentRequests(d, phone).empty());
        CHECK(!A::hasHeld(s));
        s.term->clipboardClient = nullptr;
    }

    // An over-budget operation retires its queued question too: a later
    // ordinary copy must be able to ask, not remain deduplicated forever.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, decisionsFrom(""), &stream);
        RecordingClipboard clipboard;
        s.term->clipboardClient = &clipboard;
        const auto phone = A::addPhone(d, 1);
        A::stampDriver(d, s, phone);
        const std::array buffers { Ubiquitty::ClipboardBuffer::Clipboard };
        const std::string tooLong(Ubiquitty::Protocol::Sync::maxEffectClipboardWriteBytes + 1, 'x');
        CHECK(s.capabilityPolicy.check(Capability::ClipboardWrite, CapabilityOrigin::Remote, {}) == Ubiquitty::CapabilityDecision::Prompt);
        s.capabilityPolicy.holdClipboardWrite(CapabilityOrigin::Remote, buffers, tooLong);
        A::service(d, s);
        CHECK(!A::hasHeld(s));
        CHECK(A::takeConsentRequests(d, phone).empty());
        stream->pushInbound(copies);
        A::service(d, s);
        const auto asked = A::takeConsentRequests(d, phone);
        CHECK(asked.size() == 1);
        if (asked.size() == 1)
            A::answer(d, phone, asked[0].requestID, Sync::ConsentChoice::AllowOnce);
        CHECK(clipboard.writes == std::vector<std::string>({ "copied" }));
        s.term->clipboardClient = nullptr;
    }

    // Either token expiring after the prompt is enough, without waiting for
    // the daemon's next expired-token sweep.
    for (const bool shouldExpireSource : { false, true }) {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, decisionsFrom(""), &stream);
        RecordingClipboard clipboard;
        s.term->clipboardClient = &clipboard;
        const auto phone = A::addPhone(d, 1);
        const auto agent = A::addPhone(d, 1);
        A::makeAgent(d, agent);
        A::stampDriver(d, s, phone);
        A::stampDriver(d, s, agent);
        stream->pushInbound(copies);
        A::service(d, s);
        const auto asked = A::takeConsentRequests(d, phone);
        CHECK(asked.size() == 1);
        A::expireToken(d, shouldExpireSource ? agent : phone);
        if (asked.size() == 1)
            A::answer(d, phone, asked[0].requestID, Sync::ConsentChoice::AllowOnce);
        CHECK(!A::hasHeld(s));
        CHECK(clipboard.writes.empty());
        s.term->clipboardClient = nullptr;
    }

    // ---- 2. The phone denies: nothing is granted and the copy stays refused.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, decisionsFrom(""), &stream);
        RecordingClipboard clipboard;
        s.term->clipboardClient = &clipboard;
        const std::size_t phone = A::addPhone(d, 1);
        A::stampDriver(d, s, phone);

        stream->pushInbound(copies);
        A::service(d, s);
        const std::vector<Sync::ConsentQuestion> asked = A::takeConsentRequests(d, phone);
        CHECK(asked.size() == 1);
        if (asked.size() == 1)
            A::answer(d, phone, asked[0].requestID, Sync::ConsentChoice::Deny);
        CHECK(!A::isGranted(d, Capability::ClipboardWrite, A::grantKey(d, phone)));
        stream->pushInbound(copies);
        A::service(d, s);
        CHECK(clipboard.writes.empty());
        s.term->clipboardClient = nullptr;
    }

    // ---- 3. The owner wrote `host_escaping = "deny"` for remote: the copy is
    // refused, nobody is asked, and a grant persisted before the file said so
    // does not lift it.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, decisionsFrom("[capabilities.remote]\nhost_escaping = \"deny\"\n"), &stream);
        RecordingClipboard clipboard;
        s.term->clipboardClient = &clipboard;
        const std::size_t phone = A::addPhone(d, 1);
        A::stampDriver(d, s, phone);
        A::grantAlways(d, Capability::ClipboardWrite, A::grantKey(d, phone));

        stream->pushInbound("\033]52;c;Y29w!\a");
        A::service(d, s);
        CHECK(clipboard.writes.empty());
        CHECK(!A::hasHeld(s));
        CHECK(A::takeConsentRequests(d, phone).empty());
        stream->pushInbound("\033]52;c;aWVk\a");
        A::service(d, s);
        CHECK(clipboard.writes.empty());
        CHECK(!A::hasHeld(s));
        CHECK(A::takeConsentRequests(d, phone).empty());
        stream->pushInbound(copies);
        A::service(d, s);
        CHECK_MSG(clipboard.writes.empty(), "a configured deny was lifted by a persisted grant");
        CHECK(A::takeConsentRequests(d, phone).empty());
        s.term->clipboardClient = nullptr;
    }

    if (failures == 0)
        printf("consent_remote_clipboard: PASSED — a remote OSC 52 copy asks the phone once, "
               "an allow delivers the original copy once, a deny and a configured deny hold\n");
    else
        printf("consent_remote_clipboard: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}

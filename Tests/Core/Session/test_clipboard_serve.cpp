// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_clipboard_serve.cpp — answering a remote host's clipboard request.
//
// The whole decision, driven against a fake clipboard rather than a platform
// one: which requests may be answered, what a type list carries, which item a
// name selects, and what an absent type reports. Every native frontend routes
// through this, so a case here is a case for all of them — which is the point
// of the function existing rather than one copy per shell.

#include "Core/Session/ClipboardServe.h"

#include <string>
#include <vector>

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;
using Ubiquitty::Clipboard::ServeFacts;
using Ubiquitty::Session::answerClipboardRequest;

// A clipboard holding whatever a case gives it. Owns its bytes, because
// ClipboardItem is a pair of views and the sink is handed views into here.
class FakeClipboard final : public Ubiquitty::ClipboardClient {
public:
    /// What the capture will offer, in order.
    std::vector<std::pair<std::string, std::string>> held;
    /// Whether the frontend can capture at all.
    bool canCapture = true; // bool-ok: names a capability, not a state
    /// Whether a capture that succeeds also calls the sink. A frontend that
    /// answered true and called nothing is the shape the answer must survive.
    bool doesCallSink = true; // bool-ok: names a behaviour of the fake
    /// How many captures were asked for.
    int captureCount = 0;

    [[nodiscard]] bool captureClipboardForRemote(Ubiquitty::ClipboardBuffer,
        const std::function<void(std::span<const Ubiquitty::ClipboardItem>)>& sink) override
    {
        ++captureCount;
        if (!canCapture)
            return false;
        if (!doesCallSink)
            return true;
        std::vector<Ubiquitty::ClipboardItem> views;
        views.reserve(held.size());
        for (const auto& [mime, bytes] : held)
            views.push_back({ mime, { bytes.data(), bytes.size() } });
        sink(views);
        return true;
    }
};

Sync::ClipboardRequestMessage ask(std::uint32_t id, Sync::ClipboardRequestKind kind, std::string mime = "")
{
    Sync::ClipboardRequestMessage request;
    request.requestID = id;
    request.kind = kind;
    request.mime = std::move(mime);
    return request;
}

ServeFacts permitted()
{
    return ServeFacts { .didClaimServe = true, .isClipboardReadDenied = false };
}

} // namespace

/// A type list answers with names and no bytes.
///
/// The asking tool is `xclip -o -t TARGETS`, which throws any payload away, so
/// carrying them would ship the whole clipboard to answer "what have you got".
TEST(testATypeListCarriesNamesAndNoBytes)
{
    FakeClipboard clip;
    clip.held = { { "image/png", "\x89PNG" }, { "text/plain", "hello" } };

    const Sync::ClipboardDataMessage answer
        = answerClipboardRequest(&clip, ask(11, Sync::ClipboardRequestKind::Types), permitted());

    CHECK(answer.requestID == 11);
    CHECK(answer.status == Sync::ClipboardServeStatus::OK);
    CHECK(answer.items.size() == 2);
    if (answer.items.size() == 2) {
        CHECK(answer.items[0].mime == "image/png");
        CHECK(answer.items[0].data.empty());
        CHECK(answer.items[1].mime == "text/plain");
        CHECK(answer.items[1].data.empty());
    }
}

/// One item answers with exactly that payload, matched the way every other
/// reader matches: parameters stripped, case folded.
TEST(testOneItemAnswersTheNamedPayloadWhateverItsSpelling)
{
    FakeClipboard clip;
    // A NUL inside, because a reader treating the payload as a C string
    // truncates here and nowhere else.
    clip.held = { { "Image/PNG", std::string("\x89PNG\0\x1a", 6) }, { "text/plain", "hi" } };

    const Sync::ClipboardDataMessage answer = answerClipboardRequest(
        &clip, ask(12, Sync::ClipboardRequestKind::Item, "IMAGE/PNG;base64"), permitted());

    CHECK(answer.status == Sync::ClipboardServeStatus::OK);
    CHECK(answer.items.size() == 1);
    if (answer.items.size() == 1) {
        // The stored spelling comes back, not the requested one: the program's
        // own type list named it this way.
        CHECK(answer.items[0].mime == "Image/PNG");
        CHECK(answer.items[0].data.size() == 6);
        CHECK(answer.items[0].data[4] == 0x00);
    }
}

/// A type the clipboard does not hold is Empty, not Refused.
///
/// The difference is "there is no PNG here" against "this client will not tell
/// you", and a program can act on the first. An empty clipboard reports the
/// same way, for the same reason.
TEST(testAnAbsentTypeAndAnEmptyClipboardBothReportEmpty)
{
    FakeClipboard held;
    held.held = { { "text/plain", "hi" } };
    const Sync::ClipboardDataMessage missing = answerClipboardRequest(
        &held, ask(13, Sync::ClipboardRequestKind::Item, "image/png"), permitted());
    CHECK(missing.status == Sync::ClipboardServeStatus::Empty);
    CHECK(missing.items.empty());

    FakeClipboard bare;
    const Sync::ClipboardDataMessage nothing
        = answerClipboardRequest(&bare, ask(14, Sync::ClipboardRequestKind::Types), permitted());
    CHECK(nothing.status == Sync::ClipboardServeStatus::Empty);
    CHECK(nothing.items.empty());
}

/// Nothing is captured when the gate refuses, and the refusal is visible.
///
/// The capture is what touches the pasteboard, so a gate that refused after
/// reading would have read a clipboard the user said no to. The count is the
/// assertion that matters here; the status alone would pass either way.
TEST(testARefusedRequestNeverReachesTheClipboard)
{
    FakeClipboard clip;
    clip.held = { { "text/plain", "secret" } };

    const Sync::ClipboardDataMessage unclaimed = answerClipboardRequest(&clip,
        ask(15, Sync::ClipboardRequestKind::Types),
        ServeFacts { .didClaimServe = false, .isClipboardReadDenied = false });
    CHECK(unclaimed.status == Sync::ClipboardServeStatus::Refused);
    CHECK(clip.captureCount == 0);

    const Sync::ClipboardDataMessage denied = answerClipboardRequest(&clip,
        ask(16, Sync::ClipboardRequestKind::Types),
        ServeFacts { .didClaimServe = true, .isClipboardReadDenied = true });
    CHECK(denied.status == Sync::ClipboardServeStatus::Refused);
    CHECK(clip.captureCount == 0);
    CHECK(denied.items.empty());
}

/// A frontend that cannot capture refuses; so does one with no clipboard.
///
/// Both are "this client will not answer" to the program on the far side, and
/// neither may read as an empty clipboard — a program told the clipboard is
/// empty stops asking, and a program told it was refused can say so.
TEST(testAFrontendThatCannotCaptureRefusesRatherThanReportingEmpty)
{
    FakeClipboard cannot;
    cannot.canCapture = false;
    const Sync::ClipboardDataMessage refused
        = answerClipboardRequest(&cannot, ask(17, Sync::ClipboardRequestKind::Types), permitted());
    CHECK(refused.status == Sync::ClipboardServeStatus::Refused);
    CHECK(cannot.captureCount == 1);

    const Sync::ClipboardDataMessage none
        = answerClipboardRequest(nullptr, ask(18, Sync::ClipboardRequestKind::Types), permitted());
    CHECK(none.status == Sync::ClipboardServeStatus::Refused);

    // A frontend that answers true and never calls the sink is a bug in that
    // frontend. Refusal is the safe reading of it: the alternative is a
    // default-constructed answer, whose status is OK.
    FakeClipboard silent;
    silent.doesCallSink = false;
    const Sync::ClipboardDataMessage quiet
        = answerClipboardRequest(&silent, ask(19, Sync::ClipboardRequestKind::Types), permitted());
    CHECK(quiet.status == Sync::ClipboardServeStatus::Refused);
}

/// A payload past what one frame carries is refused whole, by the same rule
/// every client already shares.
///
/// Here rather than only in the protocol's own case because this is the path a
/// pasteboard actually takes: a screenshot is the realistic way to exceed it,
/// and half a PNG is worse than a clipboard the program reads as too large.
TEST(testAnOversizePayloadIsRefusedWholeRatherThanTruncated)
{
    FakeClipboard clip;
    clip.held = { { "image/png", std::string(Sync::maxClipboardServeBytes + 1, 'z') } };

    const Sync::ClipboardDataMessage answer = answerClipboardRequest(
        &clip, ask(20, Sync::ClipboardRequestKind::Item, "image/png"), permitted());

    CHECK(answer.status == Sync::ClipboardServeStatus::TooLarge);
    CHECK(answer.items.empty());
}

UBIQUITTY_TEST_MAIN()

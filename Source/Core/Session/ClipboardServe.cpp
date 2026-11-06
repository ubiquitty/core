// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/ClipboardServe.h"

#include <utility>
#include <vector>

namespace Ubiquitty::Session {

namespace {

using Protocol::Sync::ClipboardDataMessage;
using Protocol::Sync::ClipboardRequestKind;
using Protocol::Sync::ClipboardRequestMessage;
using Protocol::Sync::ClipboardServeItem;
using Protocol::Sync::ClipboardServeStatus;

ClipboardDataMessage refuse(std::uint32_t requestID)
{
    return Protocol::Sync::makeClipboardData(requestID, ClipboardServeStatus::Refused, {});
}

ClipboardDataMessage empty(std::uint32_t requestID)
{
    return Protocol::Sync::makeClipboardData(requestID, ClipboardServeStatus::Empty, {});
}

/// The names the capture holds, with no bytes.
///
/// A type list carrying payloads would ship the whole clipboard to answer
/// "what have you got", and the asking tool — `xclip -o -t TARGETS` — throws
/// any payload away.
std::vector<ClipboardServeItem> namesOf(std::span<const Ubiquitty::ClipboardItem> captured)
{
    std::vector<ClipboardServeItem> items;
    items.reserve(captured.size());
    for (const Ubiquitty::ClipboardItem& item : captured)
        items.push_back({ std::string(item.mime), {} });
    return items;
}

/// The one payload a name selects, or nothing when the capture holds no such
/// type.
std::vector<ClipboardServeItem> payloadFor(
    std::span<const Ubiquitty::ClipboardItem> captured, std::string_view wanted)
{
    for (const Ubiquitty::ClipboardItem& item : captured) {
        if (!Ubiquitty::mimeNamesSameType(item.mime, wanted))
            continue;
        // The stored spelling rides the answer, not the requested one: the
        // program asked for a type and gets back the name the clipboard holds
        // it under, which is what its own type list said.
        std::vector<ClipboardServeItem> items;
        items.push_back({ std::string(item.mime),
            std::vector<std::uint8_t>(item.bytes.begin(), item.bytes.end()) });
        return items;
    }
    return {};
}

} // namespace

ClipboardDataMessage answerClipboardRequest(Ubiquitty::ClipboardClient* clip,
    const ClipboardRequestMessage& request, Clipboard::ServeFacts gate)
{
    if (Clipboard::serveDecisionFor(gate) == Clipboard::ServeDecision::Refuse)
        return refuse(request.requestID);
    // No clipboard installed is a frontend that cannot answer, which is the
    // same fact to the program as one that will not.
    if (clip == nullptr)
        return refuse(request.requestID);

    ClipboardDataMessage answer;
    bool didCallSink = false;
    const bool wantsTypes = request.kind == ClipboardRequestKind::Types;
    // Built inside the sink because ClipboardItem is a pair of views onto
    // storage the frontend owns for the duration of the call. Copying here is
    // what makes the answer outlive it.
    const bool didCaptureSucceed = clip->captureClipboardForRemote(
        Ubiquitty::ClipboardBuffer::Clipboard, [&](std::span<const Ubiquitty::ClipboardItem> items) {
            didCallSink = true;
            if (items.empty()) {
                answer = empty(request.requestID);
                return;
            }
            std::vector<ClipboardServeItem> chosen
                = wantsTypes ? namesOf(items) : payloadFor(items, request.mime);
            // Nothing of that type. Empty rather than Refused: the difference
            // is "the clipboard has no PNG" against "this client will not tell
            // you", and a program can act on the first.
            if (chosen.empty()) {
                answer = empty(request.requestID);
                return;
            }
            answer = Protocol::Sync::makeClipboardData(
                request.requestID, ClipboardServeStatus::OK, std::move(chosen));
        });

    // A frontend that answered false captured nothing and said so; one that
    // answered true without calling the sink is a bug in that frontend, and a
    // refusal is the safe reading of it rather than an empty clipboard.
    if (!didCaptureSucceed || !didCallSink)
        return refuse(request.requestID);
    return answer;
}

} // namespace Ubiquitty::Session

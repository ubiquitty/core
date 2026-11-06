// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Answering a remote host's clipboard request from the client's own clipboard.
//
// A program on the far side that shells out for a clipboard finds nothing — a
// headless host runs no display server, so xclip and wl-paste have nothing to
// talk to and pbpaste is not installed. When the user has opted in, the request
// crosses the session and the client answers from the pasteboard of the machine
// they are sitting at.
//
// The whole decision is here rather than in each frontend because it is the
// same decision everywhere and it is a security one: which requests may be
// answered at all, what a type list carries, which item a name selects, and
// what an absent type reports. A frontend that resolved any of those privately
// would be a second copy that drifts from the first the day either is edited.
// What a frontend owns is the capture — reaching its own platform pasteboard —
// which is exactly what the ClipboardClient seam already abstracts.

#include "Core/Clipboard/Scope.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "PAL/TerminalClients.h"

namespace Ubiquitty::Session {

/// Answers one clipboard request from the frontend's own clipboard.
///
/// Called on the frontend's loop — the thread it posted the session's work to —
/// because that is where ``ClipboardClient/captureClipboardForRemote`` may
/// touch a platform pasteboard. The answer is a message, not a frame: what may
/// ride in it is ``Protocol::Sync::makeClipboardData``'s rule, and this adds
/// only which items the request asked for.
///
/// Refusal is the answer to every uncertainty here, and never silence. The
/// program on the far side is blocked on a reply; a refusal it can act on beats
/// a wait it cannot end, and it beats an empty answer, which it would read as a
/// clipboard that holds nothing.
///
/// - Parameters:
///   - clip: The frontend's clipboard, or nullptr when it installed none.
///   - request: What the host's program asked for.
///   - gate: Whether answering is permitted at all.
/// - Returns: The message to frame.
[[nodiscard]] Protocol::Sync::ClipboardDataMessage answerClipboardRequest(Ubiquitty::ClipboardClient* clip,
    const Protocol::Sync::ClipboardRequestMessage& request, Clipboard::ServeFacts gate);

} // namespace Ubiquitty::Session

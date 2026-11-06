// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/ConsentStatus.h"
//
// The client-facing sentence for a host verdict on an attach: held for consent,
// or consent refused. A refusal's sentence is `Sync::byeNotice`.
//
// A held or refused attach delivers no checkpoint, so the frame that carries the
// verdict is the only thing distinguishing it from a host that is down, a slow
// network, or a shell that has printed nothing. A frontend that decodes those
// frames and says nothing leaves the user with a blank screen and no reason for
// it, which is how a single unanswered permission question reads as the terminal
// being broken.
//
// The mapping is here, not in a frontend, because a reason code is meaningless
// on its own and each frontend would otherwise carry its own table of them —
// and a table keyed by index silently mistranslates every reason added after it
// was written. The switches below name every enumerator with no `default:`, so a
// new reason stops the build instead.

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/NativeAttachment.h"

#include <string_view>

namespace Ubiquitty::Protocol::Sync {

/// The notice for an attach the host is holding pending a human's consent.
///
/// The hold is finite — the daemon denies on its own timeout — so the wording
/// says what is being waited for rather than promising the attach will proceed.
///
/// - Returns: A phrase for a status line, in the present tense.
[[nodiscard]] inline std::string_view attachHeldNotice()
{
    return "waiting for permission—someone attached to this session has to allow this connection";
}

/// The notice for an attachment's exact terminal own-consent status.
///
/// - Parameter reason: The wire reason; a value outside the enum yields the
///   generic refusal rather than an empty string.
/// - Returns: A phrase for a status line.
[[nodiscard]] inline std::string_view consentDeniedNotice(Sync::ConsentStatusPhase reason)
{
    switch (reason) {
    case Sync::ConsentStatusPhase::Denied:
        return "connection declined—the person on this session said no";
    case Sync::ConsentStatusPhase::Expired:
        return "connection declined—nobody answered in time";
    case Sync::ConsentStatusPhase::Pending:
    case Sync::ConsentStatusPhase::Approved:
        break;
    }
    return "connection declined";
}

} // namespace Ubiquitty::Protocol::Sync

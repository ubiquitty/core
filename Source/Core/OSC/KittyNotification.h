// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/OSC/KittyNotification.h — kitty's desktop notification protocol
// (OSC 99), assembled from the chunks a program sends it in.
//
//   OSC 99 ; <key>=<value>:<key>=<value>… ; <payload> ST
//
// One notification is usually several sequences: a title, then a body, each
// possibly split again, tied together by `i=` and ended by the first chunk
// that does not say `d=0`. Read one sequence at a time, the title and the body
// arrive as two notifications and neither says who sent the other.
//
// What is honoured:
//
//   i=  the notification's name. Chunks under one name accumulate; the name
//       travels with the finished notification so a later one under it stands
//       in place of the earlier.
//   d=  `0` holds the notification open for more; anything else finishes it.
//   p=  `title` (the default) and `body` say which field the payload joins.
//       `?` queries supported payload types and notification occasions.
//   e=  `1` marks the payload as base64.
//   o=  `always`, `unfocused` or `invisible`: when the program wants it shown.
//
// What is read and set aside, each for a reason:
//
//   p=close, p=alive  and  a=report, c=1  ask the terminal to answer or
//       to withdraw a banner. Every one is a reply or a reach into the
//       system's list that nothing here makes, so the chunk is consumed and
//       produces nothing; `p=close` also abandons what was open under its name.
//   p=icon, p=buttons, g=, n=  carry pictures and buttons a banner posted
//       through the portable path has no slot for.
//   u=, w=, s=  urgency, expiry and sound are the reader's settings here, not
//       the sender's.
//   f=, t=  the application's name and a type tag; the title a program sends
//       is what names it.
//
// A key this does not list is ignored, as the protocol asks of a terminal that
// predates it.
//
// Everything a program controls is bounded: the names held open at once, the
// bytes of each field and of a name, the encoded text one chunk may decode,
// and how long an unfinished notification is kept.
//

#include "Base/Types.h"
#include "Core/OSC/TabStatus.h"
#include "PAL/TerminalClients.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::OSC {

/// The most notifications one terminal holds unfinished at once.
///
/// A program assembling more than a few at a time is not describing anything a
/// person could read; past this the one begun longest ago is abandoned.
inline constexpr std::size_t maxOpenKittyNotifications = 4;

/// The longest notification name kept, in bytes. Names are short tokens.
inline constexpr std::size_t kittyNotificationIdentifierCapacity = 32;

/// How long an unfinished notification is kept: thirty seconds of the 60 Hz
/// clock. Chunks of one notification are written back to back, so one still
/// open after this was abandoned by its program, and a completion arriving
/// later starts afresh rather than joining words from another time.
inline constexpr std::uint32_t kittyNotificationAssemblyTicks = 30 * 60;

/// One finished notification.
struct KittyNotification {
    /// The program's name for it, reduced to the protocol's alphabet; empty
    /// for one sent without a name.
    std::string identifier;
    /// The title and the body, each at most `attentionTextCapacity` bytes.
    std::string title;
    std::string body;
    /// When the program wants it shown.
    NotificationOccasion occasion = NotificationOccasion::Always;
};

/// Assembles OSC 99 chunks into notifications, for one terminal.
class KittyNotificationAssembler {
public:
    /// Takes one OSC 99 payload and returns the notification it finishes.
    ///
    /// - Parameters:
    ///   - payload: Everything after `99;`: the metadata, and the text after
    ///     its `;` when there is one.
    ///   - now: The terminal's clock, or nothing where it has none, in which
    ///     case nothing expires and the count alone bounds what is open.
    /// - Returns: The finished notification, or nothing while one is still
    ///   open, when the chunk was of a kind set aside, or when what finished
    ///   has no words.
    [[nodiscard]] std::optional<KittyNotification> accept(std::string_view payload, std::optional<Ticks> now);

    /// The notifications begun and not yet finished.
    ///
    /// - Returns: A count no greater than `maxOpenKittyNotifications`.
    [[nodiscard]] std::size_t openCount() const { return m_open.size(); }

    /// Abandons every unfinished notification.
    void clear() { m_open.clear(); }

private:
    /// One notification begun and not finished, and when it was begun.
    struct Open {
        KittyNotification notification;
        std::optional<Ticks> startedAt;
    };

    std::vector<Open> m_open;
};

} // namespace Ubiquitty::OSC

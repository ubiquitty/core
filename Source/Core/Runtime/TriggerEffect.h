// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Base/SwiftInterop.h"
#include "Core/Preferences/Preferences.h"

// What a frontend does with a trigger the engine matched.
//
// The engine classifies and reports (Core/Triggers/Engine.h); what the report
// means — which of the two `[triggers]` gates it has to pass, what argv it
// becomes, what body rides to a webhook, what a banner heads with — is one
// decision, and it is here so that every frontend reaches the same answer. A
// frontend is left with the act itself: spawn this argv, POST this body, put
// this string on the pasteboard.
//
// Here rather than beside the engine because the gates are configuration and
// Core/Triggers is a model module that must not reach up into behaviour; here
// rather than in each frontend because the two gates and the argv rule are the
// containment for attacker-controllable output, and a second spelling of a
// containment is how one arm comes to be missing it.

namespace Ubiquitty::Runtime {

/// The act a matched trigger resolves to.
///
/// Open rather than closed: it crosses into Swift as a raw value, and a closed
/// enumeration traps on a value the importer does not know.
enum class TriggerEffectKind : std::uint8_t {
    /// Nothing to do: the action paints rather than dispatches (Highlight), a
    /// gate is shut, or the rule's payload names no act.
    None UBIQUITTY_SWIFT_NAME(none) = 0,
    /// Raise a desktop banner with the resolved title and body.
    Notify UBIQUITTY_SWIFT_NAME(notify),
    /// Put the matched text on the system clipboard.
    Copy UBIQUITTY_SWIFT_NAME(copy),
    /// Name the window with the resolved title.
    SetTitle UBIQUITTY_SWIFT_NAME(setTitle),
    /// Spawn the resolved argv. Reached only with `allow_run_command` open.
    RunCommand UBIQUITTY_SWIFT_NAME(runCommand),
    /// POST the resolved body to the resolved URL. Reached only with
    /// `allow_webhook` open.
    Webhook UBIQUITTY_SWIFT_NAME(webhook),
};

/// One resolved trigger act, with every string a frontend needs already built.
///
/// Only the fields the kind names are filled. The match is never in `url` and
/// never in a shell string: `arguments` holds it as one element and
/// `requestBody` as a JSON value, which is the whole containment.
struct TriggerEffect {
    /// What to do, `None` when there is nothing to do.
    TriggerEffectKind kind = TriggerEffectKind::None;
    /// A banner's heading for `Notify`, the window's name for `SetTitle`.
    std::string title;
    /// A banner's subtitle, empty for a body-only message.
    std::string body;
    /// The bytes `Copy` puts on the clipboard.
    std::string text;
    /// The webhook's URL, already checked to be http or https.
    std::string url;
    /// The webhook's request body: `{"match":"…"}` with the match escaped.
    std::string requestBody;
    /// `RunCommand`'s explicit argv. Element 0 is the program.
    std::vector<std::string> arguments;

    /// How many arguments the spawn takes, program included.
    ///
    /// Beside `arguments` for a frontend that cannot spell a C++ container —
    /// the Swift arm reads the argv one element at a time through this and
    /// ``argumentAt``.
    ///
    /// - Returns: The element count, zero for every other kind.
    [[nodiscard]] int argumentCount() const { return static_cast<int>(arguments.size()); }

    /// One argument of the spawn.
    ///
    /// - Parameter index: Which element, 0 being the program.
    /// - Returns: The argument, or empty when the index names none.
    [[nodiscard]] std::string argumentAt(int index) const
    {
        if (index < 0 || index >= argumentCount())
            return {};
        return arguments[static_cast<std::size_t>(index)];
    }
};

/// Whether a trigger action reaches the frontend at all.
///
/// Highlight is painted, not dispatched: the match's span is on the terminal's
/// highlight ring before any client hears of it, and the renderer reads it each
/// frame. A queue that carried it would wake a frontend for every line a
/// highlight rule paints, to hand it nothing to do.
///
/// - Parameter action: An `Ubiquitty::Triggers::Action`, widened.
/// - Returns: False for Highlight and for a value that names no action.
[[nodiscard]] bool doesTriggerActionDispatch(std::uint8_t action);

/// Resolves a matched trigger into the act its frontend should perform.
///
/// The two `[triggers]` gates are applied here and nowhere else on this path:
/// `run_command` resolves to `None` unless `allow_run_command` is true, `webhook` to
/// `None` unless `allow_webhook` is true and the URL's scheme is http or https.
/// A frontend therefore cannot spawn or POST by forgetting to ask — there is
/// nothing to spawn or POST in the answer it gets.
///
/// `Highlight` resolves to `None`: the span is already on the terminal's
/// highlight ring and the renderer reads it each frame, so there is nothing for
/// a frontend to dispatch.
///
/// - Parameters:
///   - action: An `Ubiquitty::Triggers::Action`, widened so a caller holding
///     the engine's report need not name the enumeration.
///   - argument: The rule's payload — a title, a command template, a URL.
///   - matchedText: The matched span. Attacker-controllable: it is whatever the
///     program printed.
///   - configuration: The live values the gates are read from.
/// - Returns: The act, whose `kind` is `None` when there is none.
[[nodiscard]] TriggerEffect resolveTriggerEffect(std::uint8_t action, std::string_view argument,
    std::string_view matchedText, const Preferences::Values& configuration);

} // namespace Ubiquitty::Runtime

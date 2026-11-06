// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The one door configuration enters Core through.
//
// A frontend hands Core the TOML document — the bytes of the file a person
// edits, the blob a page supplied, or nothing at all — and Core parses it,
// validates it, replaces the live `Values` wholesale and tells every
// subscriber once. A frontend never assigns a field: a settings surface edits
// one key through ``configureKey``, which rewrites the document text and comes
// back through the same door, so the text and the values cannot disagree.
// (``configureKey`` is declared beside the by-key writer, in EffectiveValue.h.)
//
// An empty document is the default table. That is the rule every other rule
// here leans on: a frontend with no file and a frontend whose file states
// every default configure identically, because both reach `Values` through
// one parse and every reader resolves through `Effective::`.
//
// Main-thread only, like ``values()``.

#include "Base/SwiftInterop.h"
#include "Core/Preferences/Preferences.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Preferences {

/// What a ``configure`` call did with a document.
struct ConfigureResult {
    /// Whether the document replaced the live values.
    ///
    /// False only under `ParseMode::Strict` with at least one diagnostic: a
    /// strict caller — a snapshot case, a fixture — wants a typo to fail the
    /// run rather than draw with the key missing. Under `Permissive` every
    /// document applies, each key on a line it could not read keeping the
    /// value it had, and the lines are listed below.
    bool isApplied = false;
    /// Every line the parse read and did not apply, in document order.
    std::vector<SkippedKey> diagnostics;
    /// The configuration epoch after the call; unchanged when not applied.
    std::uint64_t epoch = 0;
};

/// Parses `document` and, when it validates, makes it the live configuration.
///
/// The document replaces the previous one wholesale — a key it omits reads as
/// its default, whatever an earlier document said — and every
/// ``ConfigurationSubscription`` is told once, after the replacement.
///
/// A key on a line the parse refuses keeps the value the previous document
/// gave it rather than falling back to its default: a value of the wrong kind
/// or outside the key's vocabulary, a value that does not parse, an array that
/// never closes, and every key under a section header that cannot be read. A
/// person saving a file with one bad line loses that line's change and nothing
/// else. A key nothing reads is named and ignored, since a newer build may know
/// it; and a number outside its key's range is clamped, not refused.
///
/// Under `Permissive` the document becomes the live document even when a line
/// is refused, so a settings surface that edits ``liveDocument`` edits the
/// text the person saved rather than an older copy it would write back over
/// their file.
///
/// - Parameters:
///   - document: The TOML text. Empty is the default table, not an error.
///   - mode: `Strict` refuses a document with any diagnostic and leaves the
///     live configuration untouched; `Permissive` applies what it could read.
/// - Returns: What happened, with the diagnostics a frontend can show.
///
/// Takes an owned string rather than a view: a Swift shell hands its bytes
/// across, and a view built over Swift-owned text dangles.
ConfigureResult configure(const std::string& document, ParseMode mode);

/// Resolves a proposed document without changing live values or notifying subscribers.
///
/// Uses the current script output under the same gate as ``configure``, and
/// keeps a refused line's value as ``configure`` would. A reset preview
/// therefore sees inherited rules without running a script or persisting.
[[nodiscard]] Values previewConfiguration(const std::string& document);

/// ``configure`` over the bytes of a file.
///
/// A file that does not exist is the empty document: a first run is the
/// default table, not a failure. Diagnostics are also logged against `path`
/// so a reader without a settings window can find the line.
///
/// - Parameters:
///   - path: The file to read.
///   - mode: As for ``configure``.
/// - Returns: As for ``configure``.
ConfigureResult configureFromFile(const std::string& path, ParseMode mode);

/// ``configure`` over the document the platform keeps for this app.
///
/// The bytes come through the PAL preferences seam (`configurationBlobKey`):
/// on a desktop that is the file a person edits, in a sandboxed app its
/// container's copy, in a browser what the page supplied. An absent document
/// is the empty one, which is the default table.
///
/// - Parameter mode: As for ``configure``.
/// - Returns: As for ``configure``.
ConfigureResult configureFromStoredDocument(ParseMode mode);

/// Hands Core what the `[scripting]` program printed.
///
/// The program is the frontend's to run — Core starts no processes — but its
/// rules are configuration: they are kept beside the live document and merged
/// into it, and into every later document that opens the `[scripting]` gate,
/// so a by-key edit or a reload does not drop them. Moves the live values once
/// and tells every subscriber, like ``configure``.
///
/// - Parameter output: The program's standard output, already bounded to
///   ``scriptingOutputLimit``; empty clears what an earlier run printed.
/// - Returns: How many directives the live configuration merged.
int configureScriptOutput(const std::string& output);

/// The document the live configuration was parsed from.
///
/// Empty until the first ``configure``. A frontend persists what
/// ``configureKey`` returns; this is the same text, for a caller that needs it
/// later.
///
/// - Returns: The live document, by reference; valid until the next configure.
const std::string& liveDocument();

/// The sentence a frontend shows for one diagnostic.
///
/// Names the line and the key, so it is a place to look rather than a claim
/// that something is wrong somewhere; the file's name is the caller's to add.
///
/// - Parameter skipped: The diagnostic.
/// - Returns: One sentence, such as `line 12: no setting named font.sise — ignored`.
[[nodiscard]] std::string diagnosticSentence(const SkippedKey& skipped);

/// The live document's diagnostics, as one line a window can show.
///
/// - Returns: The first diagnostic's sentence and how many follow it; empty
///   when the live document applied whole.
[[nodiscard]] std::string liveDiagnosticSummary();

/// A number that moves every time the live configuration is replaced.
///
/// For a consumer that pulls rather than subscribes — a renderer comparing it
/// at frame assembly re-derives exactly when something moved and never
/// otherwise.
///
/// - Returns: The current epoch; starts at zero.
std::uint64_t configurationEpoch();

/// A registration for the one configuration-changed notification.
///
/// Lives as long as the subscriber: constructing it subscribes, destroying it
/// unsubscribes, so a destroyed subscriber is never called. Moving it moves the
/// registration; the moved-from value subscribes nothing. Not copied, because
/// one subscriber is told once.
///
/// The callback runs on the main thread, after the live values are replaced,
/// once per replacement. It may destroy any subscription, its own included.
class UBIQUITTY_NONCOPYABLE ConfigurationSubscription {
public:
    using Callback = std::function<void(const Values&)>;

    /// Subscribes `callback` until this subscription is destroyed.
    ///
    /// - Parameter callback: Called with the new live values after each
    ///   replacement.
    explicit ConfigurationSubscription(Callback callback);

    /// A subscription whose callback takes no arguments and reads what it
    /// needs from the live configuration.
    ///
    /// The spelling a Swift subscriber reaches: its closure crosses as an
    /// owned `std::function<void()>` (Platform/Apple/OwnedCallback.h).
    ///
    /// - Parameter callback: Called after each replacement.
    /// - Returns: The subscription.
    [[nodiscard]] static ConfigurationSubscription notifying(std::function<void()> callback);

    ConfigurationSubscription(ConfigurationSubscription&&) noexcept;
    ConfigurationSubscription& operator=(ConfigurationSubscription&&) noexcept;
    /// Unsubscribes; the callback is not called after this returns.
    ~ConfigurationSubscription();
    ConfigurationSubscription(const ConfigurationSubscription&) = delete;
    ConfigurationSubscription& operator=(const ConfigurationSubscription&) = delete;

    /// What the registry holds: its address is stable while the handle moves.
    struct Registration {
        Callback callback;
    };

private:
    std::unique_ptr<Registration> m_registration;
};

/// Tells every live subscription that the configuration changed.
///
/// Called once per configure, after the live values are replaced; every path
/// that moves them is a configure, so nothing else needs to.
///
/// - Parameter values: The new live configuration.
void notifyConfigurationSubscribers(const Values& values);

} // namespace Ubiquitty::Preferences

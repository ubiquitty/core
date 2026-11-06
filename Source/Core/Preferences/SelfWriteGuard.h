// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The app's own writes to the preferences file, told apart from a person's.
//
// A frontend watches the file so that a save from an editor reaches every open
// window. The app writes the same file — a menu toggle, a settings row, a theme
// block — and its write raises the same change event an editor's does. Reloading
// on it re-parses what the app already holds and pushes it into every window
// again, and a reload whose re-apply persists anything writes once more and
// loops. So every own write records the bytes it left in the file, and a change
// event that finds exactly those bytes there is the app's own and reloads
// nothing.
//
// The bytes rather than a count of writes. A watcher raises one event for a
// burst of writes that land before its handler runs, and after an atomic
// replace it is watching a file that is gone until the handler re-opens the
// new one; a counter armed once per write is left owing tokens by every such
// burst, and each owed token later swallows a person's save. The bytes answer
// the question the handler asks — is what the file says now the app's — however
// many events the writes raised. A person's save that lands before the handler
// runs holds other bytes and reloads, and once one has, the file is the
// person's: a later save that happens to restore the app's bytes reloads too.
//
// Every write here is atomic — a sibling temporary file renamed over the target
// — so a crash mid-write leaves the old file whole rather than a truncated one.
//
// Main-thread only, as `Preferences::values()` is: the writers and the watcher's
// handler both run on the frontend's event loop.

#include "Core/Preferences/Configuration.h"

#include <optional>
#include <string>
#include <utility>

namespace Ubiquitty::Preferences {

/// Who made the change a file watcher reported.
enum class ChangeOrigin : unsigned char {
    /// The file holds exactly what the app last wrote; nothing to reload.
    OwnWrite,
    /// Anything else: a person's save, a deletion, another program.
    Foreign,
};

/// The bytes the app last left in the preferences file.
class SelfWriteGuard {
public:
    /// Records the bytes an own write left in the file.
    ///
    /// - Parameter bytes: The whole text the write put there.
    void noteOwnWrite(std::string bytes) { m_ownBytes = std::move(bytes); }

    /// Answers a change event from what the file holds now.
    ///
    /// A foreign answer forgets the app's bytes: the file is the person's from
    /// there on, until the app writes it again.
    ///
    /// - Parameter fileBytes: The file's whole text, or nothing when it could
    ///   not be read.
    /// - Returns: `OwnWrite` when the text is exactly the app's last write.
    [[nodiscard]] ChangeOrigin originOf(const std::optional<std::string>& fileBytes)
    {
        if (m_ownBytes && fileBytes && *fileBytes == *m_ownBytes)
            return ChangeOrigin::OwnWrite;
        m_ownBytes.reset();
        return ChangeOrigin::Foreign;
    }

    /// Whether an own write stands unanswered by a foreign change.
    [[nodiscard]] bool hasOwnWrite() const { return m_ownBytes.has_value(); }

private:
    std::optional<std::string> m_ownBytes;
};

/// The process-wide guard every own write records into and the watcher asks.
///
/// - Returns: The one instance, which outlives every caller.
[[nodiscard]] SelfWriteGuard& selfWriteGuard();

/// Answers a change event on the preferences file, `filePath()`.
///
/// - Returns: `OwnWrite` when the file holds exactly the app's last write,
///   read off its bytes now.
[[nodiscard]] ChangeOrigin originOfFileChange();

/// Something that mirrors the live values and re-reads them after an own write.
///
/// A settings window is the case: a menu toggle moves a value the window shows,
/// and the reload that would have told it is the one the guard skips.
using OwnWriteListener = void (*)();

/// Installs the listener every own write notifies.
///
/// - Parameter listener: Called after each own write, whether or not the file
///   took it, since the caller moved the live value either way; null
///   uninstalls it.
void setOwnWriteListener(OwnWriteListener listener);

/// Makes `document` the live configuration and the stored document.
///
/// The settings surface's path for an edit the by-key writer cannot model — a
/// `[[themes]]` entry, a `[[profiles]]` block, a `[[font.range_map]]` block:
/// the caller
/// splices the live document (``liveDocument``) and hands the result back.
/// Every ``ConfigurationSubscription`` hears it once, and the PAL
/// configuration blob — on a desktop the file a person edits — takes the same
/// text as the app's own write, so a watcher does not reload it again.
///
/// - Parameter document: The whole new document.
/// - Returns: What the configure did, under `Permissive`.
ConfigureResult configureOwnDocument(const std::string& document);

/// Sets `section`.`key` in the live document and makes the result live and
/// persisted, as ``configureOwnDocument`` does.
///
/// For a key no `Key` names — a `[macros]` entry, whose names are the
/// reader's own.
///
/// - Parameters:
///   - section: The `[section]` the key belongs to.
///   - key: The key name.
///   - tomlValue: The literal to write, already serialized.
/// - Returns: The new document.
std::optional<std::string> configureOwnSectionKey(const std::string& section, const std::string& key, const std::string& tomlValue);

/// Removes `section`.`key` from the live document and makes the result live
/// and persisted, as ``configureOwnDocument`` does.
///
/// - Parameters:
///   - section: The `[section]` the key belongs to.
///   - key: The key name.
/// - Returns: The new document.
std::optional<std::string> removeConfiguredSectionKey(const std::string& section, const std::string& key);

} // namespace Ubiquitty::Preferences

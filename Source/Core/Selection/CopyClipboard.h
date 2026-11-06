// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Preferences/Preferences.h"
#include "Core/Selection/CopyStyle.h"
#include "Core/Selection/StyledClipboard.h"
#include "Core/Selection/TerminalSelection.h"

namespace Ubiquitty::Selection {

/// The flavour ⌘C carries, as `[selection] copy_style` names it.
///
/// The value is stored as a name and consumed as an enum, and the one shell
/// that resolved it did so itself: the macOS window controller compared the
/// string in three places, so a Swift-shell window carried no flavour at all,
/// and a name the file template documents with hyphens matched nothing. Both
/// spellings are accepted, and an unset, empty or unrecognised name answers
/// `Everything`, which is what ⌘C carries when nothing says otherwise.
///
/// - Parameter values: The configuration fold.
/// - Returns: A concrete flavour; never `Configured`.
[[nodiscard]] CopyStyle copyStyle(const Preferences::Values& values) noexcept;

/// The flavour a picked menu item carries.
///
/// The ⌘C item names `Configured` and takes the file's answer; a Copy Special
/// item names its own flavour and keeps it whatever the file says. One
/// function rather than a conditional at each call site, because the two arms
/// spelling that conditional apart is how a Copy Special item comes to obey a
/// preference it was written to override.
///
/// - Parameters:
///   - values: The configuration fold.
///   - requested: The flavour the item names.
/// - Returns: A concrete flavour; never `Configured`.
[[nodiscard]] CopyStyle copyStyle(const Preferences::Values& values, CopyStyle requested) noexcept;

/// The selection as the flavours a copy writes.
///
/// The whole of the copy decision: which flavour `[selection] copy_style`
/// names when the picked item names none, which palette "Style for Copy"
/// renders it in, the walk that turns cells into RTF and HTML, and the
/// `[unicode]` normalisation the plain text is copied in. A frontend hands
/// over the item that was picked and the face it draws with, and writes the
/// bytes it gets back; it decides nothing. Both macOS shells call this, so a
/// flavour cannot mean one thing on one of them.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal the view shows. Read under the model lock.
///   - values: The configuration fold.
///   - requested: The flavour the picked item names; `Configured` for ⌘C.
///   - font: The face the rich flavours name; the size is the one drawn, zoom
///     included.
/// - Returns: The flavours; `rtf` and `html` empty for a plain copy or an
///   empty selection.
[[nodiscard]] StyledClipboard copyClipboard(Session& session, Terminal& terminal, const Preferences::Values& values, CopyStyle requested, const StyledFont& font);

} // namespace Ubiquitty::Selection

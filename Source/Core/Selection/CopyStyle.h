// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Compiler.h"
#include "Base/SwiftInterop.h"

#include <cstdint>

namespace Ubiquitty::Selection {

/// Which of the resolved colours and attributes survive into the styled output.
///
/// `Full` keeps the foreground, the background and bold, italic and underline.
/// `NoBackground` drops the cell background so a copy reads on the target's own
/// paper. `StylesOnly` keeps bold, italic and underline alone, with neither
/// colour.
enum class StyleColorMode : std::uint8_t {
    Full,
    NoBackground,
    StylesOnly,
};

/// The flavour of a copy, as an Edit-menu item names it.
///
/// `Configured` is the ⌘C item: it names no flavour of its own and defers to
/// `[selection] copy_style`. The other four are the Copy Special items, which
/// name a flavour outright and are not re-decided by the file. Closed and
/// Swift-named because the Swift shell switches over it: a flavour added here
/// is a compile error there rather than a silently plain copy.
enum class UBIQUITTY_CLOSED_ENUM CopyStyle : std::uint8_t {
    Configured UBIQUITTY_SWIFT_NAME(configured),
    PlainText UBIQUITTY_SWIFT_NAME(plainText),
    Everything UBIQUITTY_SWIFT_NAME(everything),
    NoBackground UBIQUITTY_SWIFT_NAME(noBackground),
    StylesOnly UBIQUITTY_SWIFT_NAME(stylesOnly),
};

/// The colour mode a styled flavour of `style` is generated in.
///
/// `Configured` and `PlainText` answer `Full` only so the fold is total: a
/// caller reaching this without first resolving the one and refusing the
/// other writes rich bytes for a copy the reader asked to be plain.
///
/// - Parameter style: The resolved flavour.
/// - Returns: Which colours survive into RTF and HTML.
[[nodiscard]] constexpr StyleColorMode styleColorMode(CopyStyle style) noexcept
{
    switch (style) {
    case CopyStyle::NoBackground:
        return StyleColorMode::NoBackground;
    case CopyStyle::StylesOnly:
        return StyleColorMode::StylesOnly;
    case CopyStyle::Configured:
    case CopyStyle::PlainText:
    case CopyStyle::Everything:
        return StyleColorMode::Full;
    }
    return StyleColorMode::Full;
}

} // namespace Ubiquitty::Selection
